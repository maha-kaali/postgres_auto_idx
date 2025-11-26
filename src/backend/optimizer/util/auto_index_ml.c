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
    
    // smol tables don't need indexes
    classTup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(classTup)) return -100.0;
    
    classForm = (Form_pg_class) GETSTRUCT(classTup);
    rows = classForm->reltuples;
    ReleaseSysCache(classTup);

    if (rows < 1000) return -100.0; // means it suppresses indexing 
    table_size_score = log(rows > 0 ? rows : 1); 

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
             // stadistinct < 0 means percentage (e.g. -1 = unique). > 0 means absolute count.
             if (stats->stadistinct > 0 && stats->stadistinct < 5 && rows >= 1000) {
                 current_penalty += 100; // accumulate penalty
             } else {
                 has_selective_column = true; // found a good column!
             }
             ReleaseSysCache(statTup);
        } else {
            // if no stats, assume it might be selective (optimistic)
            has_selective_column = true; 
        }
    }

    if (!has_selective_column) {
        selectivity_score -= current_penalty;
    }

    // frequency
    scan_frequency_score = (double) frequency * 10.0;

    // simple linear scoring model
    total_score = scan_frequency_score + table_size_score + selectivity_score;
    
    return total_score;
}
