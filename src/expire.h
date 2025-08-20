#ifndef __EXPIRE_H__
#define __EXPIRE_H__

#include "ebuckets.h"
#include "util.h"

/* resolve circular dependency */
typedef struct redisDb redisDb;
typedef struct redisObject kvobj;

extern EbucketsType estoreBucketsType;

/* Forward declaration of the estore structure */
typedef struct _estore {
    int flags;                  /* Flags for configuration options */
    EbucketsType *bucket_type;  /* Type of buckets used in this store */
    ebuckets *ebArray;          /* Array of ebuckets (one per slot in cluster mode, or just one) */
    int num_buckets_bits;       /* Log2 of the number of buckets */
    int num_buckets;            /* Number of buckets (1 << num_buckets_bits) */
    unsigned long long count;   /* Total number of items in this estore */
    fwTree *buckets_sizes;      /* Binary indexed tree (BIT) that describes cumulative key frequencies */
} estore;

/* Create a new expiration store */
estore *estoreCreate(EbucketsType *type, int num_buckets_bits);

/* Empty an expiration store (clear all entries but keep the structure) */
void estoreEmpty(estore *es);

int estoreIsEmpty(estore *es);

/* Release an expiration store (free all memory) */
void estoreRelease(estore *es);

/* Remove item from estore */
uint64_t estoreRemove(estore *es, int slot, eItem item);

/* Add item to estore with the given expiration time */
void estoreAdd(estore *es, int slot, eItem item, uint64_t when);

/* Update item in estore with new expiration time */
void estoreUpdate(estore *es, int slot, eItem item, uint64_t when);

void estoreGetStats(estore *es, char *buf, size_t bufsize, int full);

/* Get the total number of kv's in estore */
uint64_t estoreSize(estore *es);

ebuckets *estoreGetBuckets(estore *es, int slot);

/* Get the first non-empty bucket index in the estore */
int estoreGetFirstNonEmptyBucket(estore *es);

/* Get the next non-empty bucket index after the given index */
int estoreGetNextNonEmptyBucket(estore *es, int slot);

ExpireMeta *hashGetExpireMeta(const eItem kvobjHash);

#endif 