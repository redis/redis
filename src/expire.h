#ifndef __EXPIRE_H__
#define __EXPIRE_H__

#include "server.h"
#include "ebuckets.h"

/* resolve circular dependency */
typedef struct redisDb redisDb;
typedef struct redisObject kvobj;


/* Forward declaration of the estore structure */
typedef struct _estore {
    int flags;                            /* Flags for configuration options */
    EbucketsType *bucket_type;            /* Type of buckets used in this store */
    ebuckets *buckets;                    /* Array of ebuckets (one per slot in cluster mode, or just one) */
    int num_buckets_bits;                 /* Log2 of the number of buckets */
    int num_buckets;                      /* Number of buckets (1 << num_buckets_bits) */
    unsigned long long count;             /* Total number of kv's in this estore */
} estore;

extern EbucketsType estoreBucketsType;

/* Active expiration cycle for keys */
void activeExpireKeyCycle(int type);

/* Create a new expiration store */
estore *estoreCreate(EbucketsType *type, int num_buckets_bits);

/* Empty an expiration store (clear all entries but keep the structure) */
void estoreEmpty(estore *es);

/* Release an expiration store (free all memory) */
void estoreRelease(estore *es);

/* Remove kv from estore */
void estoreRemove(estore *es, int slot, kvobj *kv);

/* Add kv to estore with the given expiration time */
void estoreAdd(estore *es, kvobj *kv, int slot, long long when);

void estoreIncrementalCascade(estore *es, uint64_t now, uint64_t maxCascade);

void estoreGetStats(estore *es, char *buf, size_t bufsize, int full);

/* Get the total number of kv's in estore */
unsigned long long estoreSize(estore *es);

/* Get the number of kv's in a specific slot of estore */
unsigned long long estoreSlotSize(estore *es, int slot);

/* Check if a specific slot is empty */
int estoreSlotIsEmpty(estore *es, int slot);

/* Perform active expiration on keys using ebuckets */
unsigned int estoreActiveExpire(redisDb *db, unsigned int max_keys);

/* Get the appropriate bucket for a given slot */
ebuckets *estoreGetBucket(estore *es, int slot);

size_t estoreMemUsage(estore *es);

#endif
