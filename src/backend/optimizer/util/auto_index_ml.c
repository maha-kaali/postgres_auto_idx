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

    /*
     * IMPROVED SELECTIVITY MODEL (The "Tipping Point" Logic)
     * For composite indexes, we only penalize if ALL columns are low selectivity.
     */
    bool has_selective_column = false;
    double current_penalty = 0;

    for (i = 0; i < num_attrs; i++) {
        HeapTuple statTup = SearchSysCache3(STATRELATTINH,
                                            ObjectIdGetDatum(relid),
                                            Int16GetDatum(attrs[i]),
                                            BoolGetDatum(false));
        if (HeapTupleIsValid(statTup)) {
            Form_pg_statistic stats = (Form_pg_statistic) GETSTRUCT(statTup);
            double num_distinct;
            double selectivity;

            AUTO_INDEX_LOG("CalculateIndexScore: attr[%d]=%d stadistinct=%.4f",
                          i, attrs[i], stats->stadistinct);

            /*
             * Get the actual Number of Distinct Values.
             * Postgres stores distinct count weirdly:
             * > 0 means absolute count (e.g., 5 distinct values).
             * < 0 means ratio (e.g., -0.1 means 10% of rows are distinct).
             */
            if (stats->stadistinct < 0) {
                /* It's a ratio (common for large tables) */
                num_distinct = rows * (-stats->stadistinct);
            } else {
                /* It's an absolute count */
                num_distinct = stats->stadistinct;
            }

            /* Sanity check to avoid division by zero */
            if (num_distinct < 1.0) num_distinct = 1.0;

            /*
             * Calculate Selectivity (Probability of a row matching).
             * Example: Gender (2 distinct) -> Selectivity = 0.5 (50% of rows match)
             * Example: UUID (Unique) -> Selectivity = 0.000...1 (1 row matches)
             */
            selectivity = 1.0 / num_distinct;

            /*
             * Apply Penalties based on Physics.
             * If an index fetches > 20% of table, the planner usually ignores it.
             * So we should heavily penalize it.
             */
            if (selectivity > 0.20) {
                /* Matches >20% of rows (e.g. Gender, Boolean flags).
                 * This index will likely NEVER be used by the planner. */
                current_penalty += 1000.0;
                AUTO_INDEX_LOG("CalculateIndexScore: attr[%d] selectivity %.2f > 20%% (useless) -> Penalty 1000", i, selectivity);
            } else if (selectivity > 0.05) {
                /* Matches 5% - 20% of rows.
                 * Useful ONLY if the table is massive, otherwise SeqScan is often faster.
                 * Small penalty. */
                current_penalty += 50.0;
                AUTO_INDEX_LOG("CalculateIndexScore: attr[%d] selectivity %.2f (mediocre) -> Penalty 50", i, selectivity);
            } else {
                /* Matches < 5% of rows. This is "Golden". No penalty. */
                has_selective_column = true;
                AUTO_INDEX_LOG("CalculateIndexScore: attr[%d] selectivity %.2f (excellent) -> No Penalty", i, selectivity);
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
