/*
 * streaming_lag.c
 *
 * PostgreSQL extension with custom background worker to measure
 * streaming lag on a replica in seconds instead of bytes
 *
 * Written by Torsten Förtsch <torsten.foertsch@gmx.net>
 *
 * Copyright 2014 Torsten Förtsch. This program is Free
 * Software; see the README.md file for the license conditions.
 */

#include "postgres.h"

/* Following are required for all bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* these headers are used by this particular worker's code */
#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "tcop/utility.h"

#include <sys/time.h>

PG_MODULE_MAGIC;

void _PG_init(void);

/* flags set by signal handlers */
static volatile sig_atomic_t got_sigalrm = false;
static volatile sig_atomic_t got_sigterm = false;
static volatile sig_atomic_t got_sighup  = false;

/* GUC variables */
static char *guc_database = NULL;
static char *guc_schema   = NULL;
static int  guc_precision = 0;

static void
log_info(char *msg) {
  ereport(LOG, (errmsg("%s: %s", MyBgworkerEntry->bgw_name, msg)));
}

static void
sigterm(SIGNAL_ARGS)
{
  int save_errno = errno;

  got_sigterm = true;
  if (MyProc) SetLatch(&MyProc->procLatch);

  errno = save_errno;
}

static void
sighup(SIGNAL_ARGS)
{
  int save_errno = errno;

  got_sighup = true;
  if (MyProc) SetLatch(&MyProc->procLatch);

  errno = save_errno;
}

static void
sigalrm(SIGNAL_ARGS)
{
  int save_errno = errno;

  got_sigalrm = true;
  if (MyProc) SetLatch(&MyProc->procLatch);

  errno = save_errno;
}

/*
 * Initialize objects
 *
 */

static void
initialize_objects(void)
{
  int ret;
  int ntup;
  bool isnull;
  StringInfoData buf;

  guc_schema = (char*)quote_identifier(guc_schema);

  SetCurrentStatementStartTimestamp();
  StartTransactionCommand();
  SPI_connect();
  PushActiveSnapshot(GetTransactionSnapshot());
  pgstat_report_activity(STATE_RUNNING, "Verifying config log objects");

  initStringInfo(&buf);
  appendStringInfo(&buf,
                   "SELECT count(1)"
                   "  FROM pg_catalog.pg_class c"
                   "  JOIN pg_catalog.pg_namespace n ON c.relnamespace=n.oid"
                   " WHERE n.nspname='%s'"
                   "   AND c.relname='streaming_lag_data'"
                   "   AND c.relkind='r'",
                   guc_schema);

  ret = SPI_execute(buf.data, false, 0);
  if (ret != SPI_OK_SELECT) {
    ereport(FATAL, (errmsg("%s: SPI error code %d", MyBgworkerEntry->bgw_name, ret)));
  }

  /* This should never happen */
  if (SPI_processed != 1) {
    ereport(FATAL, (errmsg("%s: got %d rows from a 'SELECT count()'",
                           MyBgworkerEntry->bgw_name, SPI_processed)));
  }

  ntup = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
                                     SPI_tuptable->tupdesc,
                                     1, &isnull));

  /* This should never happen */
  if (isnull) {
    ereport(FATAL, (errmsg("%s: 'SELECT count()' returns NULL",
                           MyBgworkerEntry->bgw_name)));
  }

  if (ntup == 0) {
    ereport(FATAL, (errmsg("%s: table %s.streaming_lag_data not found",
                           MyBgworkerEntry->bgw_name, guc_schema),
                    errhint("'streaming_lag.schema' must match the SCHEMA option "
                            "at CREATE EXTENSION time")));
  }

  resetStringInfo(&buf);
  appendStringInfo(&buf,
                   "DELETE FROM %s.streaming_lag_data",
                   guc_schema);

  ret = SPI_execute(buf.data, false, 0);
  if (ret != SPI_OK_DELETE) {
    ereport(FATAL, (errmsg("%s: SPI error code %d", MyBgworkerEntry->bgw_name, ret)));
  }

  resetStringInfo(&buf);
  appendStringInfo(&buf,
                   "INSERT INTO %s.streaming_lag_data (tstmp) SELECT now()",
                   guc_schema);
  ret = SPI_execute(buf.data, false, 0);
  if (ret != SPI_OK_INSERT) {
    ereport(FATAL, (errmsg("%s: SPI error code %d", MyBgworkerEntry->bgw_name, ret)));
  }

  SPI_finish();
  PopActiveSnapshot();
  CommitTransactionCommand();
  pgstat_report_activity(STATE_IDLE, NULL);

  log_info("initialized, database objects validated");
}

static void
my_main(Datum main_arg)
{
  StringInfoData buf;
  struct itimerval timer;
  int rc;

  timer.it_value.tv_sec = guc_precision/1000;
  timer.it_value.tv_usec = (guc_precision%1000)*1000;
  timer.it_interval.tv_sec = timer.it_value.tv_sec;
  timer.it_interval.tv_usec = timer.it_value.tv_usec;

  pqsignal(SIGTERM, sigterm);
  pqsignal(SIGHUP,  sighup);
  pqsignal(SIGALRM, sigalrm);

  /* We're now ready to receive signals */
  BackgroundWorkerUnblockSignals();

  /* Connect to database */
  BackgroundWorkerInitializeConnection(guc_database, NULL);

  /* Verify expected objects exist */
  initialize_objects();

  SetCurrentStatementStartTimestamp();
  StartTransactionCommand();
  SPI_connect();
  PushActiveSnapshot(GetTransactionSnapshot());
  pgstat_report_activity(STATE_RUNNING, buf.data);

  rc = SPI_execute("SET synchronous_commit TO off", false, 0);
  if (rc != SPI_OK_UTILITY) {
    ereport(FATAL, (errmsg("%s: cannot SET synchronous_commit TO off: error code %d",
                           MyBgworkerEntry->bgw_name, rc)));
  }

  SPI_finish();
  PopActiveSnapshot();
  CommitTransactionCommand();
  pgstat_report_activity(STATE_IDLE, NULL);

  initStringInfo(&buf);
  appendStringInfo(&buf,
                   "UPDATE %s.streaming_lag_data SET tstmp=now()",
                   guc_schema);

  if (setitimer(ITIMER_REAL, &timer, NULL) == -1)
    ereport(FATAL, (errmsg("%s: cannot start timer", MyBgworkerEntry->bgw_name)));

  while (!got_sigterm) {
    rc = WaitLatch(&MyProc->procLatch,
                   WL_LATCH_SET | WL_POSTMASTER_DEATH,
                   -1);
    ResetLatch(&MyProc->procLatch);

    /* emergency bailout if postmaster has died */
    if (rc & WL_POSTMASTER_DEATH) proc_exit(1);

    if (got_sighup) {
      got_sighup = false;
      ProcessConfigFile(PGC_SIGHUP);
      
      timer.it_value.tv_sec = guc_precision/1000;
      timer.it_value.tv_usec = (guc_precision%1000)*1000;
      timer.it_interval.tv_sec = timer.it_value.tv_sec;
      timer.it_interval.tv_usec = timer.it_value.tv_usec;

      if (setitimer(ITIMER_REAL, &timer, NULL) == -1)
        ereport(FATAL, (errmsg("%s: cannot start timer", MyBgworkerEntry->bgw_name)));
    }

    if (got_sigalrm) {
      got_sigalrm = false;

      SetCurrentStatementStartTimestamp();
      StartTransactionCommand();
      SPI_connect();
      PushActiveSnapshot(GetTransactionSnapshot());
      pgstat_report_activity(STATE_RUNNING, buf.data);

      rc = SPI_execute(buf.data, false, 0);
      if (rc != SPI_OK_UPDATE) {
        ereport(FATAL, (errmsg("%s: cannot update timestamp: error code %d",
                               MyBgworkerEntry->bgw_name, rc)));
      }

      SPI_finish();
      PopActiveSnapshot();
      CommitTransactionCommand();
      pgstat_report_activity(STATE_IDLE, NULL);
    }
  }

  proc_exit(0);
}


/*
 * Entrypoint of this module.
 */
void
_PG_init(void)
{
  BackgroundWorker worker;

  /* get GUC settings, if available */

  DefineCustomStringVariable(
                             "streaming_lag.database",
                             "Database used for streaming_lag",
                             "Database used to generate WAL timestamps (default: postgres).",
                             &guc_database,
                             "postgres",
                             PGC_POSTMASTER,
                             0,
                             NULL,
                             NULL,
                             NULL
                             );

  if (!process_shared_preload_libraries_in_progress) return;
  
  DefineCustomStringVariable(
                             "streaming_lag.schema",
                             "Schema used for streaming_lag",
                             "Schema used to generate WAL timestamps (default: public).",
                             &guc_schema,
                             "public",
                             PGC_POSTMASTER,
                             0,
                             NULL,
                             NULL,
                             NULL
                             );

  DefineCustomIntVariable("streaming_lag.precision",
                          "WAL timestamp interval (in milliseconds).",
                          NULL,
                          &guc_precision,
                          5000,
                          0,
                          INT_MAX,
                          PGC_SIGHUP,
                          0,
                          NULL,
                          NULL,
                          NULL);

  /* register the worker processes */
  worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
  worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
  worker.bgw_main = my_main;

  worker.bgw_restart_time = 1;
  worker.bgw_main_arg = (Datum) 0;

  /* this value is shown in the process list */
  snprintf(worker.bgw_name, BGW_MAXLEN, "streaming_lag");

  RegisterBackgroundWorker(&worker);
}
