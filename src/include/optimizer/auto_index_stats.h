#ifndef AUTO_INDEX_STATS_H
#define AUTO_INDEX_STATS_H

#include "postgres.h"
#include "storage/spin.h"
#include "nodes/pg_list.h"
#include "access/attnum.h"

#define MAX_CANDIDATES 100
#define MAX_COMPOSITE_ATTRS 4

/* Log toggle for auto-index debugging - set via GUC auto_index.log */
extern bool auto_index_log_enabled;

/* Macro for conditional logging */
#define AUTO_INDEX_LOG(fmt, ...) \
    do { \
        if (auto_index_log_enabled) \
            elog(LOG, "AutoIndex: " fmt, ##__VA_ARGS__); \
    } while(0)

#define AUTO_INDEX_LOG_DEBUG(fmt, ...) \
    do { \
        if (auto_index_log_enabled) \
            elog(DEBUG1, "AutoIndex: " fmt, ##__VA_ARGS__); \
    } while(0)


/* this is what will be stored in shared memory */
typedef struct {
    Oid relid;
    int num_attrs;
    AttrNumber attrs[MAX_COMPOSITE_ATTRS]; /* to support composite keys */
    int frequency;
    int last_frequency;  /* frequency seen in previous worker cycle */
    int stale_cycles;    /* cycles without frequency change */
    int ideal_freq;      /* threshold frequency before considering this candidate */
    bool freq_set;       /* whether ideal_freq has been calculated */
    slock_t mutex;       /* to support concurrency */
} IndexCandidate;

#define STALE_CYCLE_THRESHOLD 100  /* remove candidate after this many idle cycles */

typedef struct {
    IndexCandidate candidates[MAX_CANDIDATES];
    int count;
    slock_t global_lock;
} AutoIndexShmemState;

// this is the global variable that will point to shared memory
extern AutoIndexShmemState *AutoIndexState;

void AutoIndexShmemInit(void);
void TrackIndexCandidate(Oid relid, List *attnums);
double CalculateIndexScore(Oid relid, AttrNumber *attrs, int num_attrs, int frequency);
void DefineAutoIndexGUCs(void);
double get_relation_rows(Oid relid);
bool IndexExistsForAttrs(Oid relid, AttrNumber *attrs, int num_attrs);
void RemoveIndexCandidate(int index);
void PrintCandidateStateTable(void);

#endif
