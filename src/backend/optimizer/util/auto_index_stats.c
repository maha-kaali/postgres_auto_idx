#include "postgres.h"
#include "optimizer/auto_index_stats.h"
#include "storage/shmem.h"
#include "nodes/pg_list.h"
#include "utils/guc.h"

AutoIndexShmemState *AutoIndexState = NULL;

/* GUC variable for log toggle - default ON for debugging */
bool auto_index_log_enabled = true;

/* Define GUC for auto_index.log */
void
DefineAutoIndexGUCs(void)
{
    DefineCustomBoolVariable("auto_index.log",
                             "Enable verbose logging for autonomous indexing",
                             NULL,
                             &auto_index_log_enabled,
                             true,  /* default value - enabled by default for debugging */
                             PGC_SUSET,  /* superuser can change, applies to all sessions including bgworker */
                             0,
                             NULL,
                             NULL,
                             NULL);
}

void AutoIndexShmemInit(void) {
    bool found;

    AutoIndexState = (AutoIndexShmemState *) ShmemInitStruct("Auto Index Stats", sizeof(AutoIndexShmemState), &found);
    if (!found) {
        MemSet(AutoIndexState, 0, sizeof(AutoIndexShmemState));
        SpinLockInit(&AutoIndexState->global_lock);
        AUTO_INDEX_LOG("Shared memory initialized (size=%zu bytes)", sizeof(AutoIndexShmemState));
    } else {
        AUTO_INDEX_LOG("Attached to existing shared memory");
    }
}

static int cmp_attr(const void *a, const void *b) {
    return (*(AttrNumber*)a - *(AttrNumber*)b);
}

// check if candidate exists, if so increment, else add new
void TrackIndexCandidate(Oid relid, List *attnums) {
    int num_attrs;
    AttrNumber attrs[MAX_COMPOSITE_ATTRS];
    int i = 0;
    ListCell *lc;
    bool found = false;

    if (AutoIndexState == NULL) {
        elog(WARNING, "AutoIndex: AutoIndexState is NULL in TrackIndexCandidate");
        return;
    }

    num_attrs = list_length(attnums);

    AUTO_INDEX_LOG("TrackIndexCandidate called: relid=%u, num_attrs=%d", relid, num_attrs);

    if (num_attrs > MAX_COMPOSITE_ATTRS || num_attrs == 0) {
        AUTO_INDEX_LOG("TrackIndexCandidate: rejected (num_attrs=%d, max=%d)", num_attrs, MAX_COMPOSITE_ATTRS);
        return;
    }

    foreach(lc, attnums) {
        attrs[i++] = (AttrNumber) lfirst_int(lc);
    }

    // sort to ensure (a,b) == (b,a)
    qsort(attrs, num_attrs, sizeof(AttrNumber), cmp_attr);

    /* Log the sorted attribute numbers */
    if (auto_index_log_enabled) {
        StringInfoData attr_str;
        initStringInfo(&attr_str);
        for (i = 0; i < num_attrs; i++) {
            if (i > 0) appendStringInfoString(&attr_str, ", ");
            appendStringInfo(&attr_str, "%d", attrs[i]);
        }
        AUTO_INDEX_LOG("TrackIndexCandidate: sorted attrs=[%s]", attr_str.data);
        pfree(attr_str.data);
    }

    SpinLockAcquire(&AutoIndexState->global_lock);

    AUTO_INDEX_LOG_DEBUG("TrackIndexCandidate: scanning %d existing candidates", AutoIndexState->count);

    for (int j = 0; j < AutoIndexState->count; j++) {
        IndexCandidate *cand = &AutoIndexState->candidates[j];
        if (cand->relid == relid && cand->num_attrs == num_attrs) {
            if (memcmp(cand->attrs, attrs, num_attrs * sizeof(AttrNumber)) == 0) {
                SpinLockAcquire(&cand->mutex);
                cand->frequency++;
                AUTO_INDEX_LOG("TrackIndexCandidate: EXISTING candidate found, frequency incremented to %d (relid=%u)",
                              cand->frequency, relid);
                SpinLockRelease(&cand->mutex);
                found = true;
                break;
            }
        }
    }

    if (!found && AutoIndexState->count < MAX_CANDIDATES) {
        IndexCandidate *cand = &AutoIndexState->candidates[AutoIndexState->count];
        cand->relid = relid;
        cand->num_attrs = num_attrs;
        memcpy(cand->attrs, attrs, num_attrs * sizeof(AttrNumber));
        cand->frequency = 1;
        SpinLockInit(&cand->mutex);
        AutoIndexState->count++;
        AUTO_INDEX_LOG("TrackIndexCandidate: NEW candidate added (relid=%u, num_attrs=%d, total_candidates=%d)",
                      relid, num_attrs, AutoIndexState->count);
    } else if (!found) {
        AUTO_INDEX_LOG("TrackIndexCandidate: candidate pool FULL (max=%d), cannot add new candidate", MAX_CANDIDATES);
    }

    SpinLockRelease(&AutoIndexState->global_lock);
}
