#include "postgres.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/lsyscache.h"
#include "optimizer/auto_index_stats.h"
#include "utils/snapmgr.h"
#include "access/xact.h"
#include "lib/stringinfo.h"
#include "tcop/utility.h"
#include "utils/wait_event.h"

// declaration of the scoring function from auto_index_ml.c
extern double CalculateIndexScore(Oid relid, AttrNumber *attrs, int num_attrs, int frequency);

void AutoIndexWorkerMain(Datum main_arg);

static volatile sig_atomic_t got_sigterm = false;

static void
auto_index_sigterm(SIGNAL_ARGS)
{
    int save_errno = errno;
    got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

void AutoIndexWorkerMain(Datum main_arg) {
    int cycle_count = 0;

    // connect to database "postgres" for now.
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    // set up signal handlers
    pqsignal(SIGTERM, auto_index_sigterm);
    BackgroundWorkerUnblockSignals();

    elog(LOG, "autoIndexWorker: started and connected to database");
    AUTO_INDEX_LOG("Worker initialized (log_enabled=%s)", auto_index_log_enabled ? "true" : "false");


    for (;;) {
        int i;
        int candidates_evaluated = 0;
        int indexes_created = 0;

        cycle_count++;

        // to fix BUG: it didn't close automatically when server stopped
        // check for shutdown
        if (got_sigterm) {
            elog(LOG, "autoIndexWorker: received SIGTERM, shutting down");
            AUTO_INDEX_LOG("Worker shutting down after %d cycles", cycle_count);
            break;
        }

        CHECK_FOR_INTERRUPTS();

        // sleep for 5 seconds (5000 ms) for testing
        (void) WaitLatch(MyLatch,
                         WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
                         5000L,
                         PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);

        if (got_sigterm) {
            elog(LOG, "autoIndexWorker: received SIGTERM, shutting down");
            AUTO_INDEX_LOG("Worker shutting down after %d cycles", cycle_count);
            break;
        }

        if (AutoIndexState == NULL) {
            AUTO_INDEX_LOG_DEBUG("Worker cycle %d: AutoIndexState is NULL, skipping", cycle_count);
            continue;
        }

        AUTO_INDEX_LOG("Worker cycle %d: BEGIN (total_candidates=%d)", cycle_count, AutoIndexState->count);

        // start transaction for catalog access
        StartTransactionCommand();
        PushActiveSnapshot(GetTransactionSnapshot());
        SPI_connect();

        // scan shared memory
        for (i=0; i < AutoIndexState->count; i++) {
            IndexCandidate *cand = &AutoIndexState->candidates[i];
            double score;
            double creation_threshold = 10.0;

            if (cand->frequency == 0) {
                AUTO_INDEX_LOG_DEBUG("Worker: candidate[%d] skipped (frequency=0)", i);
                continue;
            }

            candidates_evaluated++;

            AUTO_INDEX_LOG("Worker: evaluating candidate[%d] relid=%u, num_attrs=%d, frequency=%d",
                          i, cand->relid, cand->num_attrs, cand->frequency);

            // calculate score
            score = CalculateIndexScore(cand->relid, cand->attrs, cand->num_attrs, cand->frequency);

            AUTO_INDEX_LOG("Worker: candidate[%d] score=%.4f (threshold=%.4f)", i, score, creation_threshold);

            // threshold check
            if (score > creation_threshold) {
                // create index using SPI
                char *relname = get_rel_name(cand->relid);
                if (relname) {
                    StringInfoData buf;
                    StringInfoData col_list;
                    int j;

                    initStringInfo(&buf);
                    initStringInfo(&col_list);

                    appendStringInfo(&buf, "CREATE INDEX auto_idx_%s", relname);

                    for (j=0; j<cand->num_attrs; j++) {
                        char *colname = get_attname(cand->relid, cand->attrs[j], false);
                        appendStringInfo(&buf, "_%s", colname);
                        if (j > 0) appendStringInfoString(&col_list, ", ");
                        appendStringInfoString(&col_list, colname);
                    }

                    appendStringInfo(&buf, " ON %s (", relname);
                    for (j=0; j<cand->num_attrs; j++) {
                        char *colname = get_attname(cand->relid, cand->attrs[j], false);
                        if (j > 0) appendStringInfoString(&buf, ", ");
                        appendStringInfoString(&buf, colname);
                    }
                    appendStringInfoString(&buf, ")");

                    AUTO_INDEX_LOG("Worker: CREATING INDEX on %s(%s) - score=%.4f",
                                  relname, col_list.data, score);
                    elog(LOG, "autoIndexWorker: creating index: %s (score: %f)", buf.data, score);

                    // Execute
                    SPI_execute(buf.data, false, 0);
                    indexes_created++;

                    AUTO_INDEX_LOG("Worker: INDEX CREATED successfully: %s", buf.data);

                    // reset counter
                    SpinLockAcquire(&cand->mutex);
                    cand->frequency = 0;
                    SpinLockRelease(&cand->mutex);

                    AUTO_INDEX_LOG("Worker: candidate[%d] frequency reset to 0", i);

                    pfree(col_list.data);
                } else {
                    AUTO_INDEX_LOG("Worker: candidate[%d] SKIPPED - could not get relation name for relid=%u",
                                  i, cand->relid);
                }
            } else {
                AUTO_INDEX_LOG("Worker: candidate[%d] BELOW threshold (score=%.4f < %.4f)",
                              i, score, creation_threshold);
            }
        }

        SPI_finish();
        PopActiveSnapshot();
        CommitTransactionCommand();

        AUTO_INDEX_LOG("Worker cycle %d: END (evaluated=%d, created=%d)",
                      cycle_count, candidates_evaluated, indexes_created);
    }
}
