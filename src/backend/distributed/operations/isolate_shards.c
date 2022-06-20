/*-------------------------------------------------------------------------
 *
 * split_shards.c
 *
 * This file contains functions to split a shard according to a given
 * distribution column value.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "c.h"
#include "fmgr.h"
#include "libpq-fe.h"

#include "catalog/pg_class.h"
#include "distributed/colocation_utils.h"
#include "distributed/coordinator_protocol.h"
#include "distributed/metadata_cache.h"
#include "distributed/metadata_sync.h"
#include "distributed/multi_join_order.h"
#include "distributed/multi_partitioning_utils.h"
#include "distributed/multi_router_planner.h"
#include "distributed/pg_dist_partition.h"
#include "distributed/pg_dist_shard.h"
#include "distributed/remote_commands.h"
#include "distributed/reference_table_utils.h"
#include "distributed/resource_lock.h"
#include "distributed/worker_manager.h"
#include "distributed/worker_protocol.h"
#include "distributed/worker_transaction.h"
#include "distributed/version_compat.h"
#include "distributed/shard_split.h"
#include "nodes/pg_list.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"


/* declarations for dynamic loading */
PG_FUNCTION_INFO_V1(isolate_tenant_to_new_shard);
PG_FUNCTION_INFO_V1(worker_hash);


/* local function forward declarations */
static uint64 SplitShardByValue(ShardInterval *sourceShard, Datum distributionValueDatum);
static void CreateSplitOffShards(ShardInterval *sourceShard, int hashedValue,
								 List **splitOffShardList, int *isolatedShardId);
static List * ShardTemplateList(ShardInterval *sourceShard, int hashedValue,
								int *isolatedShardIndex);
static ShardInterval * CreateSplitOffShardFromTemplate(ShardInterval *shardTemplate,
													   Oid relationId);
static List * SplitOffCommandList(ShardInterval *sourceShard,
								  ShardInterval *splitOffShard);

/*
 * isolate_tenant_to_new_shard isolates a tenant to its own shard by spliting
 * the current matching shard.
 */
Datum
isolate_tenant_to_new_shard(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);
	EnsureCoordinator();

	Oid relationId = PG_GETARG_OID(0);
	Datum inputDatum = PG_GETARG_DATUM(1);
	text *cascadeOptionText = PG_GETARG_TEXT_P(2);
	ListCell *colocatedTableCell = NULL;

	EnsureTableOwner(relationId);

	CitusTableCacheEntry *cacheEntry = GetCitusTableCacheEntry(relationId);

	char partitionMethod = cacheEntry->partitionMethod;
	if (partitionMethod != DISTRIBUTE_BY_HASH)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot isolate tenant because tenant isolation "
							   "is only support for hash distributed tables")));
	}

	if (PartitionedTable(relationId))
	{
		char *sourceRelationName = get_rel_name(relationId);

		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot isolate shard placement of '%s', because it "
							   "is a partitioned table", sourceRelationName),
						errdetail("Citus does not support isolating placements of "
								  "partitioned tables.")));
	}

	List *colocatedTableList = ColocatedTableList(relationId);
	int colocatedTableCount = list_length(colocatedTableList);

	foreach(colocatedTableCell, colocatedTableList)
	{
		Oid colocatedTableId = lfirst_oid(colocatedTableCell);

		/*
		 * At the moment, Citus does not support copying a shard if that shard's
		 * relation is in a colocation group with a partitioned table or partition.
		 */
		if (colocatedTableId != relationId &&
			PartitionedTable(colocatedTableId))
		{
			char *sourceRelationName = get_rel_name(relationId);
			char *colocatedRelationName = get_rel_name(colocatedTableId);

			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot isolate shard placement of '%s', because it "
								   "is a partitioned table", colocatedRelationName),
							errdetail("In colocation group of '%s', a partitioned "
									  "relation exists: '%s'. Citus does not support "
									  "isolating placements of partitioned tables.",
									  sourceRelationName, colocatedRelationName)));
		}
	}

	Oid inputDataType = get_fn_expr_argtype(fcinfo->flinfo, 1);
	char *tenantIdString = DatumToString(inputDatum, inputDataType);

	char *cascadeOptionString = text_to_cstring(cascadeOptionText);
	if (pg_strncasecmp(cascadeOptionString, "CASCADE", NAMEDATALEN) != 0 &&
		colocatedTableCount > 1)
	{
		char *relationName = get_rel_name(relationId);
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot isolate tenant because \"%s\" has colocated "
							   "tables", relationName),
						errhint("Use CASCADE option to isolate tenants for the "
								"colocated tables too. Example usage: "
								"isolate_tenant_to_new_shard('%s', '%s', 'CASCADE')",
								relationName, tenantIdString)));
	}

	EnsureReferenceTablesExistOnAllNodes();

	Var *distributionColumn = DistPartitionKey(relationId);

	/* earlier we checked that the table was hash partitioned, so there should be a distribution column */
	Assert(distributionColumn != NULL);

	Oid distributionColumnType = distributionColumn->vartype;

	Datum tenantIdDatum = StringToDatum(tenantIdString, distributionColumnType);
	ShardInterval *sourceShard = FindShardInterval(tenantIdDatum, cacheEntry);
	if (sourceShard == NULL)
	{
		ereport(ERROR, (errmsg("tenant does not have a shard")));
	}

	uint64 isolatedShardId = SplitShardByValue(sourceShard, tenantIdDatum);

	PG_RETURN_INT64(isolatedShardId);
}


/*
 * worker_hash returns the hashed value of the given value.
 */
Datum
worker_hash(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	Datum valueDatum = PG_GETARG_DATUM(0);

	/* figure out hash function from the data type */
	Oid valueDataType = get_fn_expr_argtype(fcinfo->flinfo, 0);
	TypeCacheEntry *typeEntry = lookup_type_cache(valueDataType,
												  TYPECACHE_HASH_PROC_FINFO);

	if (typeEntry->hash_proc_finfo.fn_oid == InvalidOid)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot find a hash function for the input type"),
						errhint("Cast input to a data type with a hash function.")));
	}

	FmgrInfo *hashFunction = palloc0(sizeof(FmgrInfo));
	fmgr_info_copy(hashFunction, &(typeEntry->hash_proc_finfo), CurrentMemoryContext);

	/* calculate hash value */
	Datum hashedValueDatum =
		FunctionCall1Coll(hashFunction, PG_GET_COLLATION(), valueDatum);

	PG_RETURN_INT32(hashedValueDatum);
}


/*
 * SplitShardByValue gets a shard and a value which is in the range of
 * distribution column of this shard. Then, it splits this shard and all its
 * colocated shards into three; the lower range, the given value itself, and
 * the upper range. Finally, it returns the id of the shard which is created
 * for the given value.
 */
static uint64
SplitShardByValue(ShardInterval *sourceShard, Datum distributionValueDatum)
{
	Oid relationId = sourceShard->relationId;
	int isolatedShardId = 0;
	List *splitOffShardList = NIL;

	if (XactModificationLevel > XACT_MODIFICATION_NONE)
	{
		ereport(ERROR, (errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
						errmsg("cannot isolate a tenant after other modifications "
							   "in the same transaction")));
	}

	/* sort the tables to avoid deadlocks */
	List *colocatedTableList = ColocatedTableList(relationId);
	colocatedTableList = SortList(colocatedTableList, CompareOids);

	Oid colocatedTableId = InvalidOid;
	foreach_oid(colocatedTableId, colocatedTableList)
	{
		/*
		 * Block concurrent DDL / TRUNCATE commands on the relation. Similarly,
		 * block concurrent citus_move_shard_placement()/isolate_tenant_to_new_shard()
		 * on any shard of the same relation. This is OK for now since
		 * we're executing shard moves/splits sequentially anyway.
		 */
		LockRelationOid(colocatedTableId, ShareUpdateExclusiveLock);
	}

	/* get colocated shard list */
	List *colocatedShardList = ColocatedShardIntervalList(sourceShard);

	/* get locks */
	BlockWritesToShardList(colocatedShardList);

	ErrorIfCannotSplitShard(ISOLATE_TENANT_TO_NEW_SHARD, sourceShard);

	/* get hash function name */
	CitusTableCacheEntry *cacheEntry = GetCitusTableCacheEntry(relationId);
	FmgrInfo *hashFunction = cacheEntry->hashFunction;

	/* get hashed value of the distribution value */
	Datum hashedValueDatum = FunctionCall1(hashFunction, distributionValueDatum);
	int hashedValue = DatumGetInt32(hashedValueDatum);

	/* create a list of nodes with source shard placements */
	List *sourcePlacementList = ActiveShardPlacementList(sourceShard->shardId);

	/* create new shards in a separate transaction and commit them */
	CreateSplitOffShards(sourceShard, hashedValue, &splitOffShardList, &isolatedShardId);

	/*
	 * Drop old shards and delete related metadata. Have to do that before
	 * creating the new shard metadata, because there's cross-checks
	 * preventing inconsistent metadata (like overlapping shards).
	 */
	DropShardList(colocatedShardList);

	/* insert new metadata */
	InsertSplitOffShardMetadata(splitOffShardList, sourcePlacementList);

	/*
	 * Create foreign keys if exists after the metadata changes happening in
	 * DropShardList() and InsertSplitOffShardMetadata() because the foreign
	 * key creation depends on the new metadata.
	 */
	CreateForeignConstraints(splitOffShardList, sourcePlacementList);

	CitusInvalidateRelcacheByRelid(DistShardRelationId());

	return isolatedShardId;
}


/*
 * CreateSplitOffShards gets a shard and a hashed value to pick the split point.
 * First, it creates templates to create new shards. Then, for every colocated
 * shard, it creates new split shards data and physically creates them on the
 * worker nodes. This function returns newly created split off shards and the
 * matching shard id for the source shard and hashed value via passed parameters.
 */
static void
CreateSplitOffShards(ShardInterval *sourceShard, int hashedValue,
					 List **splitOffShardList, int *isolatedShardId)
{
	List *nodeCommandList = NIL;
	ListCell *sourceColocatedShardCell = NULL;
	int isolatedShardIndex = 0;

	List *sourceColocatedShardList = ColocatedShardIntervalList(sourceShard);
	List *shardTemplateList = ShardTemplateList(sourceShard, hashedValue,
												&isolatedShardIndex);

	foreach(sourceColocatedShardCell, sourceColocatedShardList)
	{
		ShardInterval *sourceColocatedShard =
			(ShardInterval *) lfirst(sourceColocatedShardCell);
		Oid relationId = sourceColocatedShard->relationId;
		ListCell *templateShardCell = NULL;
		int currentShardIndex = 0;

		foreach(templateShardCell, shardTemplateList)
		{
			ShardInterval *templateShard = (ShardInterval *) lfirst(templateShardCell);

			ShardInterval *splitOffShard = CreateSplitOffShardFromTemplate(templateShard,
																		   relationId);
			List *splitOffCommandList = SplitOffCommandList(sourceColocatedShard,
															splitOffShard);
			nodeCommandList = list_concat(nodeCommandList, splitOffCommandList);

			/* check if this is the isolated shard for the given table */
			if (splitOffShard->relationId == sourceShard->relationId &&
				currentShardIndex == isolatedShardIndex)
			{
				(*isolatedShardId) = splitOffShard->shardId;
			}

			/* add newly created split off shards to list */
			(*splitOffShardList) = lappend(*splitOffShardList, splitOffShard);

			currentShardIndex++;
		}
	}

	List *sourcePlacementList = ActiveShardPlacementList(sourceShard->shardId);
	ExecuteCommandListOnPlacements(nodeCommandList, sourcePlacementList);
}


/*
 * ShardTemplateList creates shard templates with new min and max values from
 * the given shard and the split point which is the given hashed value.
 * It returns the list of shard templates, and passes the isolated shard index
 * via isolatedShardIndex parameter.
 */
static List *
ShardTemplateList(ShardInterval *sourceShard, int hashedValue, int *isolatedShardIndex)
{
	List *shardTemplateList = NIL;

	/* get min and max values of the source shard */
	int32 shardMinValue = DatumGetInt32(sourceShard->minValue);
	int32 shardMaxValue = DatumGetInt32(sourceShard->maxValue);

	(*isolatedShardIndex) = 0;

	/* add a shard template for lower range if exists */
	if (shardMinValue < hashedValue)
	{
		ShardInterval *lowerRangeShard = CopyShardInterval(sourceShard);

		lowerRangeShard->minValue = Int32GetDatum(shardMinValue);
		lowerRangeShard->maxValue = Int32GetDatum(hashedValue - 1);

		shardTemplateList = lappend(shardTemplateList, lowerRangeShard);
		(*isolatedShardIndex) = 1;
	}

	/* add shard template for the isolated value */
	ShardInterval *isolatedShard = CopyShardInterval(sourceShard);

	isolatedShard->minValue = Int32GetDatum(hashedValue);
	isolatedShard->maxValue = Int32GetDatum(hashedValue);

	shardTemplateList = lappend(shardTemplateList, isolatedShard);

	/* add a shard template for upper range if exists */
	if (shardMaxValue > hashedValue)
	{
		ShardInterval *upperRangeShard = CopyShardInterval(sourceShard);

		upperRangeShard->minValue = Int32GetDatum(hashedValue + 1);
		upperRangeShard->maxValue = Int32GetDatum(shardMaxValue);

		shardTemplateList = lappend(shardTemplateList, upperRangeShard);
	}

	if (list_length(shardTemplateList) == 1)
	{
		char *tableName = get_rel_name(sourceShard->relationId);
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("table \"%s\" has already been isolated for the "
							   "given value", tableName)));
	}

	return shardTemplateList;
}


/*
 * CreateSplitOffShardFromTemplate creates a new split off shard from the given
 * shard template by creating a new shard id and setting the relation id.
 */
static ShardInterval *
CreateSplitOffShardFromTemplate(ShardInterval *shardTemplate, Oid relationId)
{
	ShardInterval *splitOffShard = CopyShardInterval(shardTemplate);

	/* set new shard id and the relation id */
	splitOffShard->shardId = GetNextShardId();
	splitOffShard->relationId = relationId;

	return splitOffShard;
}


/*
 * SplitOffCommandList creates a command list to run on worker nodes to create
 * new split off shard from the source shard.
 */
static List *
SplitOffCommandList(ShardInterval *sourceShard, ShardInterval *splitOffShard)
{
	List *splitOffCommandList = NIL;
	bool includeSequenceDefaults = false;

	Oid relationId = sourceShard->relationId;
	Var *partitionKey = DistPartitionKey(relationId);
	Assert(partitionKey != NULL);
	const char *partitionColumnName = get_attname(relationId,
												  partitionKey->varattno, false);
	const char *quotedPartitionColumnName = quote_identifier(partitionColumnName);

	char *splitOffShardName = ConstructQualifiedShardName(splitOffShard);
	char *sourceShardName = ConstructQualifiedShardName(sourceShard);

	int32 shardMinValue = DatumGetInt32(splitOffShard->minValue);
	int32 shardMaxValue = DatumGetInt32(splitOffShard->maxValue);

	List *tableCreationCommandList =
		GetPreLoadTableCreationCommands(relationId, includeSequenceDefaults, NULL);
	tableCreationCommandList = WorkerApplyShardDDLCommandList(tableCreationCommandList,
															  splitOffShard->shardId);

	splitOffCommandList = list_concat(splitOffCommandList, tableCreationCommandList);

	StringInfo splitOffShardCommand = makeStringInfo();
	appendStringInfo(splitOffShardCommand,
					 "INSERT INTO %s SELECT * FROM %s WHERE "
					 "worker_hash(%s) >= %d AND worker_hash(%s) <= %d",
					 splitOffShardName, sourceShardName, quotedPartitionColumnName,
					 shardMinValue, quotedPartitionColumnName, shardMaxValue);

	splitOffCommandList = lappend(splitOffCommandList, splitOffShardCommand->data);

	List *indexCommandList = GetPostLoadTableCreationCommands(relationId, true, true);
	indexCommandList = WorkerApplyShardDDLCommandList(indexCommandList,
													  splitOffShard->shardId);

	splitOffCommandList = list_concat(splitOffCommandList, indexCommandList);

	return splitOffCommandList;
}
