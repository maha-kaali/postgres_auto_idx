#ifndef AUTO_INDEX_STATS_H
#define AUTO_INDEX_STATS_H

#include "postgres.h"
#include "storage/spin.h"
#include "nodes/pg_list.h"
#include "access/attnum.h"

#define MAX_CANDIDATES 100
#define MAX_COMPOSITE_ATTRS 4


// this is what will be stored in shared memory
typedef struct {
    Oid relid;
    int num_attrs;
    AttrNumber attrs[MAX_COMPOSITE_ATTRS]; // to support composite keys
    int frequency;
    slock_t mutex; // to support concurrency, incase multiple workers try to udpate
} IndexCandidate;

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

#endif
