
#include "postgres.h"

#include "safe_mem_lib.h"

#include "libpq/pqmq.h"
#include "parser/analyze.h"
#include "pgstat.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/backend_status.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/timeout.h"

#include "distributed/background_jobs.h"
#include "distributed/citus_safe_lib.h"
#include "distributed/listutils.h"
#include "distributed/maintenanced.h"
#include "distributed/metadata_cache.h"
#include "distributed/metadata_utility.h"
#include "distributed/shard_cleaner.h"

bool BackgroundTaskMonitorDebugDelay = false;

/* Table-of-contents constants for our dynamic shared memory segment. */
#define CITUS_BACKGROUND_JOB_MAGIC 0x51028081
#define CITUS_BACKGROUND_JOB_KEY_DATABASE 0
#define CITUS_BACKGROUND_JOB_KEY_USERNAME 1
#define CITUS_BACKGROUND_JOB_KEY_COMMAND 2
#define CITUS_BACKGROUND_JOB_KEY_QUEUE 3
#define CITUS_BACKGROUND_JOB_NKEYS 4

static BackgroundWorkerHandle * StartCitusBackgroundJobExecuter(char *database,
																char *user,
																char *command);
static void ExecuteSqlString(const char *sql);

BackgroundWorkerHandle *
StartCitusBackgroundTaskMonitorWorker(Oid database, Oid extensionOwner)
{
	BackgroundWorker worker = { 0 };
	BackgroundWorkerHandle *handle = NULL;

	/* Configure a worker. */
	memset(&worker, 0, sizeof(worker));
	SafeSnprintf(worker.bgw_name, BGW_MAXLEN,
				 "Citus Background Task Monitor: %u/%u",
				 database, extensionOwner);
	worker.bgw_flags =
		BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;

	/* don't restart, we manage restarts from maintenance daemon */
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	strcpy_s(worker.bgw_library_name, sizeof(worker.bgw_library_name), "citus");
	strcpy_s(worker.bgw_function_name, sizeof(worker.bgw_library_name),
			 "CitusBackgroundTaskMonitorMain");
	worker.bgw_main_arg = ObjectIdGetDatum(MyDatabaseId);
	memcpy_s(worker.bgw_extra, sizeof(worker.bgw_extra), &extensionOwner,
			 sizeof(Oid));
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
	{
		return NULL;
	}

	pid_t pid;
	WaitForBackgroundWorkerStartup(handle, &pid);

	return handle;
}


void
CitusBackgroundTaskMonitorMain(Datum arg)
{
	Oid databaseOid = DatumGetObjectId(arg);

	/* extension owner is passed via bgw_extra */
	Oid extensionOwner = InvalidOid;
	memcpy_s(&extensionOwner, sizeof(extensionOwner),
			 MyBgworkerEntry->bgw_extra, sizeof(Oid));

	BackgroundWorkerUnblockSignals();

	/* connect to database, after that we can actually access catalogs */
	BackgroundWorkerInitializeConnectionByOid(databaseOid, extensionOwner, 0);

	/* TODO get lock to make sure there is only one worker running per databasse */

	/* make worker recognizable in pg_stat_activity */
	pgstat_report_appname("citus background task monitor");

	ereport(LOG, (errmsg("citus background task monitor")));

	if (BackgroundTaskMonitorDebugDelay)
	{
		pg_usleep(30 * 1000 * 1000);
	}

	MemoryContext perTaskContext =
		AllocSetContextCreateExtended(CurrentMemoryContext,
									  "PerTaskContext",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * First we find all jobs that are running, we need to check if they are still running
	 * if not reset their state back to scheduled.
	 */
	{
		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		/* TODO have an actual function to check if the worker is still running */
		ResetRunningBackgroundTasks();

		PopActiveSnapshot();
		CommitTransactionCommand();
	}


	MemoryContext oldContextPerJob = MemoryContextSwitchTo(perTaskContext);
	bool hasJobs = true;
	while (hasJobs)
	{
		MemoryContextReset(perTaskContext);

		CHECK_FOR_INTERRUPTS();

		InvalidateMetadataSystemCache();
		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * We need to load the job into the perTaskContext as we will switch contexts
		 * later due to the committing and starting of new transactions
		 */
		MemoryContext oldContext = MemoryContextSwitchTo(perTaskContext);
		RebalanceJob *job = GetRunableRebalanceJob();
		MemoryContextSwitchTo(oldContext);

		if (!job)
		{
			PopActiveSnapshot();
			CommitTransactionCommand();

			hasJobs = false;
			break;
		}

		PopActiveSnapshot();
		CommitTransactionCommand();

		MemoryContextSwitchTo(perTaskContext);

		/* TODO find the actual database and username */
		BackgroundWorkerHandle *handle =
			StartCitusBackgroundJobExecuter("postgres", "nilsdijk", job->command);

		if (handle == NULL)
		{
			/* TODO something better here */
			ereport(ERROR, (errmsg("unable to start background worker")));
		}

		pid_t pid = 0;
		GetBackgroundWorkerPid(handle, &pid);

		ereport(LOG, (errmsg("found job with jobid: %ld", job->jobid)));

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		/* Update job status to indicate it is running */
		UpdateJobStatus(job->jobid, &pid, BACKGROUND_TASK_STATUS_RUNNING, NULL, NULL);

		PopActiveSnapshot();
		CommitTransactionCommand();

		MemoryContextSwitchTo(perTaskContext);

		/* TODO keep polling the job */
		while (GetBackgroundWorkerPid(handle, &pid) != BGWH_STOPPED)
		{
			int latchFlags = WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH;
			int rc = WaitLatch(MyLatch, latchFlags, (long) 1000, PG_WAIT_EXTENSION);

			/* emergency bailout if postmaster has died */
			if (rc & WL_POSTMASTER_DEATH)
			{
				proc_exit(1);
			}

			if (rc & WL_LATCH_SET)
			{
				ResetLatch(MyLatch);
				CHECK_FOR_INTERRUPTS();
			}
		}

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		/* TODO job can actually also have failed*/
		UpdateJobStatus(job->jobid, NULL, BACKGROUND_TASK_STATUS_DONE, NULL, NULL);

		PopActiveSnapshot();
		CommitTransactionCommand();
	}

	MemoryContextSwitchTo(oldContextPerJob);
	MemoryContextDelete(perTaskContext);
}


static dsm_segment *
StoreArgumentsInDSM(char *database, char *username, char *command)
{
	/*
	 * Create the shared memory that we will pass to the background
	 * worker process.  We use DSM_CREATE_NULL_IF_MAXSEGMENTS so that we
	 * do not ERROR here.  This way, we can mark the job as failed and
	 * keep the launcher process running normally.
	 */
	shm_toc_estimator e = { 0 };
	shm_toc_initialize_estimator(&e);
	shm_toc_estimate_chunk(&e, strlen(database) + 1);
	shm_toc_estimate_chunk(&e, strlen(username) + 1);
	shm_toc_estimate_chunk(&e, strlen(command) + 1);
#define QUEUE_SIZE ((Size) 65536)
	shm_toc_estimate_chunk(&e, QUEUE_SIZE);
	shm_toc_estimate_keys(&e, CITUS_BACKGROUND_JOB_NKEYS);
	Size segsize = shm_toc_estimate(&e);

	dsm_segment *seg = dsm_create(segsize, DSM_CREATE_NULL_IF_MAXSEGMENTS);
	if (seg == NULL)
	{
		ereport(ERROR,
				(errmsg("max number of DSM segments may has been reached")));

		return NULL;
	}

	shm_toc *toc = shm_toc_create(CITUS_BACKGROUND_JOB_MAGIC, dsm_segment_address(seg),
								  segsize);

	Size size = strlen(database) + 1;
	char *databaseTarget = shm_toc_allocate(toc, size);
	strcpy_s(databaseTarget, size, database);
	shm_toc_insert(toc, CITUS_BACKGROUND_JOB_KEY_DATABASE, databaseTarget);

	size = strlen(username) + 1;
	char *usernameTarget = shm_toc_allocate(toc, size);
	strcpy_s(usernameTarget, size, username);
	shm_toc_insert(toc, CITUS_BACKGROUND_JOB_KEY_USERNAME, usernameTarget);

	size = strlen(command) + 1;
	char *commandTarget = shm_toc_allocate(toc, size);
	strcpy_s(commandTarget, size, command);
	shm_toc_insert(toc, CITUS_BACKGROUND_JOB_KEY_COMMAND, commandTarget);

	shm_mq *mq = shm_mq_create(shm_toc_allocate(toc, QUEUE_SIZE), QUEUE_SIZE);
	shm_toc_insert(toc, CITUS_BACKGROUND_JOB_KEY_QUEUE, mq);
	shm_mq_set_receiver(mq, MyProc);

	/*
	 * Attach the queue before launching a worker, so that we'll automatically
	 * detach the queue if we error out.  (Otherwise, the worker might sit
	 * there trying to write the queue long after we've gone away.)
	 */
	MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	shm_mq_attach(mq, seg, NULL);
	MemoryContextSwitchTo(oldcontext);

	return seg;
}


static BackgroundWorkerHandle *
StartCitusBackgroundJobExecuter(char *database, char *username, char *command)
{
	dsm_segment *seg = StoreArgumentsInDSM(database, username, command);

	/* Configure a worker. */
	BackgroundWorker worker = { 0 };
	memset(&worker, 0, sizeof(worker));
	SafeSnprintf(worker.bgw_name, BGW_MAXLEN, "Citus Background Job Executor: %s/%s",
				 database, username);
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;

	/* don't restart, we manage restarts from maintenance daemon */
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	strcpy_s(worker.bgw_library_name, sizeof(worker.bgw_library_name), "citus");
	strcpy_s(worker.bgw_function_name, sizeof(worker.bgw_library_name),
			 "CitusBackgroundJobExecuter");
	worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(seg));
	worker.bgw_notify_pid = MyProcPid;

	BackgroundWorkerHandle *handle = NULL;
	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
	{
		dsm_detach(seg);
		return NULL;
	}

	pid_t pid = { 0 };
	WaitForBackgroundWorkerStartup(handle, &pid);

	return handle;
}


/*
 * Background worker logic.
 *
 * based on the background worker logic in pgcron
 */
void
CitusBackgroundJobExecuter(Datum main_arg)
{
	/*
	 * TODO figure out if we need this signal handler that is in pgcron
	 * pqsignal(SIGTERM, pg_cron_background_worker_sigterm);
	 */
	BackgroundWorkerUnblockSignals();

	/* Set up a memory context and resource owner. */
	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "citus background job");
	CurrentMemoryContext = AllocSetContextCreate(TopMemoryContext,
												 "citus background job execution",
												 ALLOCSET_DEFAULT_MINSIZE,
												 ALLOCSET_DEFAULT_INITSIZE,
												 ALLOCSET_DEFAULT_MAXSIZE);

	/* Set up a dynamic shared memory segment. */
	dsm_segment *seg = dsm_attach(DatumGetInt32(main_arg));
	if (seg == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("unable to map dynamic shared memory segment")));
	}

	shm_toc *toc = shm_toc_attach(CITUS_BACKGROUND_JOB_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("bad magic number in dynamic shared memory segment")));
	}

	char *database = shm_toc_lookup(toc, CITUS_BACKGROUND_JOB_KEY_DATABASE, false);
	char *username = shm_toc_lookup(toc, CITUS_BACKGROUND_JOB_KEY_USERNAME, false);
	char *command = shm_toc_lookup(toc, CITUS_BACKGROUND_JOB_KEY_COMMAND, false);
	shm_mq *mq = shm_toc_lookup(toc, CITUS_BACKGROUND_JOB_KEY_QUEUE, false);

	shm_mq_set_sender(mq, MyProc);
	shm_mq_handle *responseq = shm_mq_attach(mq, seg, NULL);
	pq_redirect_to_shm_mq(seg, responseq);

	BackgroundWorkerInitializeConnection(database, username, 0);

	/* Prepare to execute the query. */
	SetCurrentStatementStartTimestamp();
	debug_query_string = command;
	pgstat_report_activity(STATE_RUNNING, command);
	StartTransactionCommand();
	if (StatementTimeout > 0)
	{
		enable_timeout_after(STATEMENT_TIMEOUT, StatementTimeout);
	}
	else
	{
		disable_timeout(STATEMENT_TIMEOUT, false);
	}

	/* Execute the query. */
	ExecuteSqlString(command);

	/* Post-execution cleanup. */
	disable_timeout(STATEMENT_TIMEOUT, false);
	CommitTransactionCommand();
	pgstat_report_activity(STATE_IDLE, command);
	pgstat_report_stat(true);

	/* Signal that we are done. */
	ReadyForQuery(DestRemote);

	dsm_detach(seg);
	proc_exit(0);
}


/*
 * Execute given SQL string without SPI or a libpq session.
 */
static void
ExecuteSqlString(const char *sql)
{
	/*
	 * Parse the SQL string into a list of raw parse trees.
	 *
	 * Because we allow statements that perform internal transaction control,
	 * we can't do this in TopTransactionContext; the parse trees might get
	 * blown away before we're done executing them.
	 */
	MemoryContext parsecontext = AllocSetContextCreate(CurrentMemoryContext,
													   "query parse/plan",
													   ALLOCSET_DEFAULT_MINSIZE,
													   ALLOCSET_DEFAULT_INITSIZE,
													   ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContext oldcontext = MemoryContextSwitchTo(parsecontext);
	List *raw_parsetree_list = pg_parse_query(sql);
	int commands_remaining = list_length(raw_parsetree_list);
	bool isTopLevel = commands_remaining == 1;
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Do parse analysis, rule rewrite, planning, and execution for each raw
	 * parsetree.  We must fully execute each query before beginning parse
	 * analysis on the next one, since there may be interdependencies.
	 */
	RawStmt *parsetree = NULL;
	foreach_ptr(parsetree, raw_parsetree_list)
	{
		/*
		 * We don't allow transaction-control commands like COMMIT and ABORT
		 * here.  The entire SQL statement is executed as a single transaction
		 * which commits if no errors are encountered.
		 */
		if (IsA(parsetree, TransactionStmt))
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg(
								"transaction control statements are not allowed in background job")));
		}

		/*
		 * Get the command name for use in status display (it also becomes the
		 * default completion tag, down inside PortalRun).  Set ps_status and
		 * do any special start-of-SQL-command processing needed by the
		 * destination.
		 */
		CommandTag commandTag = CreateCommandTag(parsetree->stmt);
		set_ps_display(GetCommandTagName(commandTag));
		BeginCommand(commandTag, DestNone);

		/* Set up a snapshot if parse analysis/planning will need one. */
		bool snapshot_set = false;
		if (analyze_requires_snapshot(parsetree))
		{
			PushActiveSnapshot(GetTransactionSnapshot());
			snapshot_set = true;
		}

		/*
		 * OK to analyze, rewrite, and plan this query.
		 *
		 * As with parsing, we need to make sure this data outlives the
		 * transaction, because of the possibility that the statement might
		 * perform internal transaction control.
		 */
		oldcontext = MemoryContextSwitchTo(parsecontext);

#if PG_VERSION_NUM >= 150000
		List *querytree_list =
			pg_analyze_and_rewrite_fixedparams(parsetree, sql, NULL, 0, NULL);
#else
		List *querytree_list =
			pg_analyze_and_rewrite(parsetree, sql, NULL, 0, NULL);
#endif

		List *plantree_list = pg_plan_queries(querytree_list, sql, 0, NULL);

		/* Done with the snapshot used for parsing/planning */
		if (snapshot_set)
		{
			PopActiveSnapshot();
		}

		/* If we got a cancel signal in analysis or planning, quit */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Execute the query using the unnamed portal.
		 */
		Portal portal = CreatePortal("", true, true);

		/* Don't display the portal in pg_cursors */
		portal->visible = false;
		PortalDefineQuery(portal, NULL, sql, commandTag, plantree_list, NULL);
		PortalStart(portal, NULL, 0, InvalidSnapshot);
		int16 format[] = { 1 };
		PortalSetResultFormat(portal, lengthof(format), format);        /* binary format */

		commands_remaining--;
		DestReceiver *receiver = CreateDestReceiver(DestNone);

		/*
		 * Only once the portal and destreceiver have been established can
		 * we return to the transaction context.  All that stuff needs to
		 * survive an internal commit inside PortalRun!
		 */
		MemoryContextSwitchTo(oldcontext);

		/* Here's where we actually execute the command. */
		QueryCompletion qc = { 0 };
		(void) PortalRun(portal, FETCH_ALL, isTopLevel, true, receiver, receiver, &qc);

		/* Clean up the receiver. */
		(*receiver->rDestroy)(receiver);

		/*
		 * Send a CommandComplete message even if we suppressed the query
		 * results.  The user backend will report these in the absence of
		 * any true query results.
		 */
		EndCommand(&qc, DestRemote, false);

		/* Clean up the portal. */
		PortalDrop(portal, false);
	}

	/* Be sure to advance the command counter after the last script command */
	CommandCounterIncrement();
}
