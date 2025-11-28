#include "postgres.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_class.h"
#include "access/htup_details.h"
#include "optimizer/auto_index_stats.h"
#include <math.h>

// higher is better
double CalculateIndexScore(Oid relid, AttrNumber *attrs, int num_attrs, int frequency) {
    double selectivity_score = 0;
    double table_size_score;
    double scan_frequency_score;
    HeapTuple classTup;
    Form_pg_class classForm;
    double rows;
    int i;
    double total_score;

    AUTO_INDEX_LOG("CalculateIndexScore: START (relid=%u, num_attrs=%d, frequency=%d)",
                   relid, num_attrs, frequency);

    // smol tables don't need indexes
    classTup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(classTup)) {
        AUTO_INDEX_LOG("CalculateIndexScore: REJECTED - invalid relation (relid=%u)", relid);
        return -100.0;
    }

    classForm = (Form_pg_class) GETSTRUCT(classTup);
    rows = classForm->reltuples;
    ReleaseSysCache(classTup);

    AUTO_INDEX_LOG("CalculateIndexScore: table rows=%.0f", rows);

    if (rows < 1000) {
        AUTO_INDEX_LOG("CalculateIndexScore: REJECTED - table too small (rows=%.0f < 1000)", rows);
        return -100.0; // means it suppresses indexing
    }
    table_size_score = log(rows > 0 ? rows : 1);
    AUTO_INDEX_LOG("CalculateIndexScore: table_size_score=%.4f (log of %.0f rows)", table_size_score, rows);

    // selectivity
    // we check pg_statistic. If distinct values are low (2 for Gender), selectivity is poor.
    // for composite indexes, we only penalize if ALL columns are low selectivity.
    bool has_selective_column = false;
    double current_penalty = 0;

    for (i=0; i<num_attrs; i++) {
        HeapTuple statTup = SearchSysCache3(STATRELATTINH,
                                            ObjectIdGetDatum(relid),
                                            Int16GetDatum(attrs[i]),
                                            BoolGetDatum(false));
        if (HeapTupleIsValid(statTup)) {
             Form_pg_statistic stats = (Form_pg_statistic) GETSTRUCT(statTup);

             // stadistinct < 0 means percentage . > 0 means absolute count.

             AUTO_INDEX_LOG("CalculateIndexScore: attr[%d]=%d stadistinct=%.4f",
                           i, attrs[i], stats->stadistinct);

            // very basic so changing
            //  if (stats->stadistinct > 0 && stats->stadistinct < 5 && rows >= 1000) {
            //      current_penalty += 100; 
             double sel = stats->stadistinct / (double)rows;

             if (sel < 0.01){           // less than 1% distinct
                 current_penalty += 100;    
        
                 AUTO_INDEX_LOG("CalculateIndexScore: attr[%d] LOW selectivity , penalty+=100", i);
             } else {
                 has_selective_column = true; 

                 AUTO_INDEX_LOG("CalculateIndexScore: attr[%d] GOOD selectivity", i);
             }
             ReleaseSysCache(statTup);
        } else {
            /* if no stats available, be optimistic */
            has_selective_column = true;
            AUTO_INDEX_LOG("CalculateIndexScore: attr[%d]=%d NO stats found, assuming selective", i, attrs[i]);
        }
    }

    if (!has_selective_column) {
        selectivity_score -= current_penalty;
        AUTO_INDEX_LOG("CalculateIndexScore: NO selective columns, selectivity_score=%.4f", selectivity_score);
    } else {
        AUTO_INDEX_LOG("CalculateIndexScore: has selective column(s), no penalty applied");
    }

    // frequency
    scan_frequency_score = (double) frequency * 10.0;

    AUTO_INDEX_LOG("CalculateIndexScore: scan_frequency_score=%.4f (frequency=%d * 10)",
                   scan_frequency_score, frequency);

    // simple linear scoring model
    total_score = scan_frequency_score + table_size_score + selectivity_score;

    AUTO_INDEX_LOG("CalculateIndexScore: FINAL SCORE=%.4f (freq=%.4f + size=%.4f + sel=%.4f)",
                   total_score, scan_frequency_score, table_size_score, selectivity_score);

    return total_score;
}
