#include "postgres.h"
#include "optimizer/auto_index_stats.h"
#include "storage/shmem.h"
#include "nodes/pg_list.h"

AutoIndexShmemState *AutoIndexState = NULL;

void AutoIndexShmemInit(void) {
    bool found;
    AutoIndexState = (AutoIndexShmemState *) ShmemInitStruct("Auto Index Stats", sizeof(AutoIndexShmemState), &found);
    if (!found) {
        MemSet(AutoIndexState, 0, sizeof(AutoIndexShmemState));
        SpinLockInit(&AutoIndexState->global_lock);
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
    // elog(LOG, "AutoIndex: Tracking candidate with %d attrs", num_attrs); // Verbose, maybe keep commented or use DEBUG1

    if (num_attrs > MAX_COMPOSITE_ATTRS || num_attrs == 0) return;
    
    foreach(lc, attnums) {
        attrs[i++] = (AttrNumber) lfirst_int(lc);
    }

    // sort to ensure (a,b) == (b,a)
    qsort(attrs, num_attrs, sizeof(AttrNumber), cmp_attr);

    SpinLockAcquire(&AutoIndexState->global_lock);

    for (int j = 0; j < AutoIndexState->count; j++) {
        IndexCandidate *cand = &AutoIndexState->candidates[j];
        if (cand->relid == relid && cand->num_attrs == num_attrs) {
            if (memcmp(cand->attrs, attrs, num_attrs * sizeof(AttrNumber)) == 0) {
                SpinLockAcquire(&cand->mutex);
                cand->frequency++;
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
    }

    SpinLockRelease(&AutoIndexState->global_lock);
}
