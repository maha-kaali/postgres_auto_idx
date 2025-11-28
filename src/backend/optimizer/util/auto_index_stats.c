#include "postgres.h"
#include "optimizer/auto_index_stats.h"
#include "storage/shmem.h"
#include "nodes/pg_list.h"
#include "utils/guc.h"
#include "utils/syscache.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/table.h"
#include "utils/fmgroids.h"

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

/* Get estimated row count for a relation from pg_class */
double
get_relation_rows(Oid relid)
{
    HeapTuple tuple;
    Form_pg_class classForm;
    double rows = 1000.0;  /* default fallback */

    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (HeapTupleIsValid(tuple))
    {
        classForm = (Form_pg_class) GETSTRUCT(tuple);
        rows = classForm->reltuples;
        if (rows < 1.0)
            rows = 1.0;  /* avoid log(0) or negative */
        ReleaseSysCache(tuple);
    }
    return rows;
}

/*
 * Check if an index already exists for the given relation and attributes.
 * Returns true if a matching index is found.
 */
bool
IndexExistsForAttrs(Oid relid, AttrNumber *attrs, int num_attrs)
{
    Relation indexRelation;
    SysScanDesc scan;
    ScanKeyData skey;
    HeapTuple indexTuple;
    bool found = false;
    int indexes_checked = 0;

    AUTO_INDEX_LOG("IndexExistsForAttrs: checking relid=%u, num_attrs=%d", relid, num_attrs);

    /* scan pg_index to see if matching index exists */
    indexRelation = table_open(IndexRelationId, AccessShareLock);

    ScanKeyInit(&skey,
                Anum_pg_index_indrelid,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(relid));

    scan = systable_beginscan(indexRelation, IndexIndrelidIndexId, true,
                              NULL, 1, &skey);

    while ((indexTuple = systable_getnext(scan)) != NULL)
    {
        Form_pg_index indexForm = (Form_pg_index) GETSTRUCT(indexTuple);
        int i;
        bool match = true;

        indexes_checked++;

        /* skip invalid or not-yet-ready indexes */
        if (!indexForm->indisvalid || !indexForm->indisready)
        {
            AUTO_INDEX_LOG("IndexExistsForAttrs: skipping invalid/not-ready index (oid=%u)",
                          indexForm->indexrelid);
            continue;
        }

        /* check if number of key columns matches */
        if (indexForm->indnkeyatts != num_attrs)
            continue;

        /* check if all our attrs exist in index (order independent) */
        for (i = 0; i < num_attrs; i++)
        {
            bool attr_found = false;
            int j;
            /* iterate over INDEX key columns, not num_attrs */
            for (j = 0; j < indexForm->indnkeyatts; j++)
            {
                if (indexForm->indkey.values[j] == attrs[i])
                {
                    attr_found = true;
                    break;
                }
            }
            if (!attr_found)
            {
                match = false;
                break;
            }
        }

        if (match)
        {
            found = true;
            AUTO_INDEX_LOG("IndexExistsForAttrs: MATCH found - index oid=%u for relid=%u",
                          indexForm->indexrelid, relid);
            break;
        }
    }

    systable_endscan(scan);
    table_close(indexRelation, AccessShareLock);

    AUTO_INDEX_LOG("IndexExistsForAttrs: checked %d indexes, found=%s",
                  indexes_checked, found ? "true" : "false");

    return found;
}

/*
 * remove index candidate from shmem
 * need global lock?
 */
void
RemoveIndexCandidate(int index)
{
    int i;

    SpinLockAcquire(&AutoIndexState->global_lock);

    if (AutoIndexState == NULL || index < 0 || index >= AutoIndexState->count)
        return;

    AUTO_INDEX_LOG("RemoveIndexCandidate: removing candidate[%d], relid=%u",
                   index, AutoIndexState->candidates[index].relid);

    /* Shift all subsequent candidates down */
    for (i = index; i < AutoIndexState->count - 1; i++)
    {
        memcpy(&AutoIndexState->candidates[i],
               &AutoIndexState->candidates[i + 1],
               sizeof(IndexCandidate));
    }

    AutoIndexState->count--;
    AUTO_INDEX_LOG("RemoveIndexCandidate: new count=%d", AutoIndexState->count);

    SpinLockRelease(&AutoIndexState->global_lock);
}

/*
 * Print the candidate state table when detailed logging is enabled
 */
void
PrintCandidateStateTable(void)
{
    int i, j;

    if (!auto_index_log_enabled)
        return;

    if (AutoIndexState == NULL) {
        elog(LOG, "AutoIndex: StateTable: AutoIndexState is NULL");
        return;
    }

    elog(LOG, "AutoIndex: ========== CANDIDATE STATE TABLE ==========");
    elog(LOG, "AutoIndex: Total candidates: %d / %d", AutoIndexState->count, MAX_CANDIDATES);
    elog(LOG, "AutoIndex: %-4s | %-10s | %-6s | %-8s | %-8s | %s",
         "Idx", "RelOid", "Freq", "IdealF", "FreqSet", "Attrs");
    elog(LOG, "AutoIndex: -----|------------|--------|----------|----------|--------");

    for (i = 0; i < AutoIndexState->count; i++) {
        IndexCandidate *cand = &AutoIndexState->candidates[i];
        StringInfoData attr_str;

        initStringInfo(&attr_str);
        for (j = 0; j < cand->num_attrs; j++) {
            if (j > 0)
                appendStringInfoString(&attr_str, ",");
            appendStringInfo(&attr_str, "%d", cand->attrs[j]);
        }

        elog(LOG, "AutoIndex: %-4d | %-10u | %-6d | %-8d | %-8s | [%s]",
             i,
             cand->relid,
             cand->frequency,
             cand->ideal_freq,
             cand->freq_set ? "true" : "false",
             attr_str.data);

        pfree(attr_str.data);
    }

    elog(LOG, "AutoIndex: ==============================================");
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

/* check if candidate exists, if so increment, else add new */
void TrackIndexCandidate(Oid relid, List *attnums) {
    int num_attrs;
    AttrNumber attrs[MAX_COMPOSITE_ATTRS];
    int i = 0;
    ListCell *lc;
    bool found = false;
    double rows;

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

    /* fail fast for small tables */
    rows = get_relation_rows(relid);
    if (rows < 1000) {
        AUTO_INDEX_LOG("TrackIndexCandidate: rejected - table too small (rows=%.0f < 1000)", rows);
        return;
    }

    foreach(lc, attnums) {
        attrs[i++] = (AttrNumber) lfirst_int(lc);
    }

    /* sort to ensure (a,b) == (b,a) */
    qsort(attrs, num_attrs, sizeof(AttrNumber), cmp_attr);

    /* fail fast if index already exists */
    if (IndexExistsForAttrs(relid, attrs, num_attrs)) {
        AUTO_INDEX_LOG("TrackIndexCandidate: rejected - index already exists for relid=%u", relid);
        return;
    }

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
