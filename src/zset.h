#ifndef __ZSET_H__
#define __ZSET_H__

#include "redis.h"
#include <stdbool.h>

#define ZR_LONG 1
#define ZR_STRING 2
struct zipresult {
    double score;
    union {
        long long v;
        sds s;
    } val;
    double distance; /* distance is in meters */
    char type;       /* access type for the union */
};

/* Redis DB Access */
bool zsetScore(robj *zobj, robj *member, double *score);
list *geozrangebyscore(robj *zobj, double min, double max, int limit);

/* New list operation: append one list to another */
void listJoin(list *join_to, list *join);

/* Helpers for returning zrangebyscore results */
struct zipresult *result_str(double score, unsigned char *str, int len);
struct zipresult *result_long(double score, long long v);
void free_zipresult(struct zipresult *r);

#endif
