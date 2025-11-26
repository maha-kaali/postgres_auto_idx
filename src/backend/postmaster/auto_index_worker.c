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
    // connect to database "postgres" for now. 
    BackgroundWorkerInitializeConnection("postgres", NULL, 0);

    // set up signal handlers
    pqsignal(SIGTERM, auto_index_sigterm);
    BackgroundWorkerUnblockSignals();
    
    elog(LOG, "autoIndexWorker: started and connected to database");


    for (;;) {
        int i;
        // to fix BUG: it didn't close automatically when server stopped
        // check for shutdown
        if (got_sigterm) {
            elog(LOG, "autoIndexWorker: received SIGTERM, shutting down");
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
            break;
        }

        if (AutoIndexState == NULL) continue;

        // start transaction for catalog access
        StartTransactionCommand();
        PushActiveSnapshot(GetTransactionSnapshot());
        SPI_connect();

        // scan shared memory
        for (i=0; i < AutoIndexState->count; i++) {
            IndexCandidate *cand = &AutoIndexState->candidates[i];
            double score;
            double creation_threshold = 10.0;

            if (cand->frequency == 0) continue;

            // calculate score
            score = CalculateIndexScore(cand->relid, cand->attrs, cand->num_attrs, cand->frequency);
            
            // threshold check
            if (score > creation_threshold) {
                // create index using SPI
                char *relname = get_rel_name(cand->relid);
                if (relname) {
                    StringInfoData buf;
                    int j;

                    initStringInfo(&buf);
                    appendStringInfo(&buf, "CREATE INDEX auto_idx_%s", relname);
                    
                    for (j=0; j<cand->num_attrs; j++) {
                        char *colname = get_attname(cand->relid, cand->attrs[j], false);
                        appendStringInfo(&buf, "_%s", colname);
                    }
                    
                    appendStringInfo(&buf, " ON %s (", relname);
                    for (j=0; j<cand->num_attrs; j++) {
                        char *colname = get_attname(cand->relid, cand->attrs[j], false);
                        if (j > 0) appendStringInfoString(&buf, ", ");
                        appendStringInfoString(&buf, colname);
                    }
                    appendStringInfoString(&buf, ")");
                    
                    elog(LOG, "autoIndexWorker: creating index: %s (score: %f)", buf.data, score);
                    
                    // Execute
                    SPI_execute(buf.data, false, 0);
                    
                    // reset counter
                    SpinLockAcquire(&cand->mutex);
                    cand->frequency = 0;
                    SpinLockRelease(&cand->mutex);
                }
            }
        }
        
        SPI_finish();
        PopActiveSnapshot();
        CommitTransactionCommand();
    }
}
