/*
 * estore.c -- Expiration Store implementation
 * 
 * Copyright (c) 2011-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "fmacros.h"
#include "estore.h"
#include "zmalloc.h"
#include "redisassert.h"
#include "server.h"
#include <string.h>

/* Forward declaration of the estore structure */
struct _estore {
    int flags;                  /* Flags for configuration options */
    EbucketsType *bucket_type;  /* Type of buckets used in this store */
    ebuckets *ebArray;          /* Array of ebuckets (one per slot in cluster mode, or just one) */
    int num_buckets_bits;       /* Log2 of the number of buckets */
    int num_buckets;            /* Number of buckets (1 << num_buckets_bits) */
    unsigned long long count;   /* Total number of items in this estore */
    fenwickTree *buckets_sizes; /* Binary indexed tree (BIT) that describes cumulative key frequencies */
};

/* Get the appropriate bucket for a given eidx */
ebuckets *estoreGetBuckets(estore *es, int eidx) {
    debugAssert(eidx < es->num_buckets);
    return &(es->ebArray[eidx]);
}

/* Create a new expiration store
 * type             - Pointer to a static EbucketsType defining the bucket behavior.
 * num_buckets_bits - The log2 of the number of buckets (0 for 1 bucket,
 *                    CLUSTER_SLOT_MASK_BITS for CLUSTER_SLOTS buckets)
 * flags - Configuration flags
 */
estore *estoreCreate(EbucketsType *type, int num_buckets_bits) {
    /* We can't support more than 2^16 buckets to be consistent with kvstore */
    assert(num_buckets_bits <= 16);

    estore *es = zmalloc(sizeof(estore));
    /* Store the bucket type */
    es->bucket_type = type;

    /* Calculate number of buckets based on num_buckets_bits */
    es->num_buckets_bits = num_buckets_bits;
    es->num_buckets = 1 << num_buckets_bits;
    es->buckets_sizes = es->num_buckets > 1 ? fwTreeCreate(num_buckets_bits) : NULL;

    /* Allocate the buckets array */
    es->ebArray = zcalloc(sizeof(ebuckets) * es->num_buckets);

    /* Initialize all buckets */
    for (int i = 0; i < es->num_buckets; i++) {
        es->ebArray[i] = ebCreate();
    }

    es->count = 0;
    return es;
}

/* Empty an expiration store (clear all entries but keep the structure) */
void estoreEmpty(estore *es) {
    if (es == NULL) return;

    for (int i = 0; i < es->num_buckets; i++) {
        ebDestroy(&es->ebArray[i], es->bucket_type, NULL);
        es->ebArray[i] = ebCreate();
    }

    if (es->buckets_sizes) fwTreeClear(es->buckets_sizes);
    es->count = 0;
}

/* Check if the expiration store is empty */
int estoreIsEmpty(estore *es) {
    return es->count == 0;
}

/* Get the first non-empty bucket index in the estore */
int estoreGetFirstNonEmptyBucket(estore *es) {
    if (es->num_buckets == 1 || estoreSize(es) == 0)
        return 0;
    return fwTreeFindFirstNonEmpty(es->buckets_sizes);
}

/* Get the next non-empty bucket index after the given index */
int estoreGetNextNonEmptyBucket(estore *es, int eidx) {
    if (es->num_buckets == 1) {
        assert(eidx == 0);
        return -1;
    }
    return fwTreeFindNextNonEmpty(es->buckets_sizes, eidx);
}

/* Release an expiration store (free all memory) */
void estoreRelease(estore *es) {
    if (es == NULL) return;

    for (int i = 0; i < es->num_buckets; i++) {
        if (es->ebArray[i])
            ebDestroy(&es->ebArray[i], es->bucket_type, NULL);
    }
    fwTreeDestroy(es->buckets_sizes);
    zfree(es->ebArray);
    zfree(es);
}

/* Perform active expiration on a specific bucket */
void estoreActiveExpire(estore *es, int eidx, ExpireInfo *info) {
    ebuckets *eb = estoreGetBuckets(es, eidx);
    uint64_t before = ebGetTotalItems(*eb, es->bucket_type);
    ebExpire(eb, es->bucket_type, info);
    /* If items expired (or updated), update the BIT and estore count */
    if (info->itemsExpired) {
        uint64_t diff = before - ebGetTotalItems(*eb, es->bucket_type);
        fwTreeUpdate(es->buckets_sizes, eidx, (long long) diff);
        es->count -= diff;
    }
}

/* Add item to estore with the given expiration time. The item must has
 * expireMeta already allocated. */
void estoreAdd(estore *es, int eidx, eItem item, uint64_t when) {
    debugAssert(es != NULL && item != NULL);

    /* currently only used by hash field expiration. Verify it has expireMeta */
    debugAssert((((robj *)item)->encoding == OBJ_ENCODING_LISTPACK_EX) ||
                ((((robj *)item)->encoding == OBJ_ENCODING_HT) &&
                 ((dict *) ((robj *)item)->ptr)->type == &entryHashDictTypeWithHFE));

    ebuckets *bucket = estoreGetBuckets(es, eidx);
    if (ebAdd(bucket, es->bucket_type, item, when) == 0) {
        es->count++;
        fwTreeUpdate(es->buckets_sizes, eidx, 1);
    }
}

/* Remove an item from the expiration store. Returns the expire time or EB_EXPIRE_TIME_INVALID */
uint64_t estoreRemove(estore *es, int eidx, eItem item) {
    uint64_t expireTime;
    debugAssert(es != NULL && item != NULL);

    /* Currently only used by hash field expiration. gracefully ignore otherwise */
    kvobj *kv = (kvobj *) item;
    if ( (kv->type != OBJ_HASH) ||
         (kv->encoding == OBJ_ENCODING_LISTPACK) ||
         ((kv->encoding == OBJ_ENCODING_HT) && (((dict *)kv->ptr)->type != &entryHashDictTypeWithHFE)))
        return EB_EXPIRE_TIME_INVALID;

    /* If (ExpireMeta of kv) marked as trash, then it is already removed */
    if ((expireTime = ebGetExpireTime(es->bucket_type, item)) == EB_EXPIRE_TIME_INVALID)
        return EB_EXPIRE_TIME_INVALID;

    ebuckets *bucket = estoreGetBuckets(es, eidx);
    serverAssert(ebRemove(bucket, es->bucket_type, item)==1);
    es->count--;
    fwTreeUpdate(es->buckets_sizes, eidx, -1);

    return expireTime;
}

/* Update an item's expiration time in the store */
void estoreUpdate(estore *es, int eidx, eItem item, uint64_t when) {
    debugAssert(es != NULL && item != NULL);

    /* currently only used by hash field expiration. Verify it has expireMeta */
    debugAssert((((robj *)item)->encoding == OBJ_ENCODING_LISTPACK_EX) ||
                ((((robj *)item)->encoding == OBJ_ENCODING_HT) &&
                 ((dict *) ((robj *)item)->ptr)->type == &entryHashDictTypeWithHFE));

    debugAssert(ebGetExpireTime(es->bucket_type, item) != EB_EXPIRE_TIME_INVALID);

    ebuckets *bucket = estoreGetBuckets(es, eidx);

    /* Remove the item from its current position */
    serverAssert(ebRemove(bucket, es->bucket_type, item) != 0);

    /* Add the item back with the new expiration time */
    serverAssert(ebAdd(bucket, es->bucket_type, item, when) == 0);

    /* Note that estore count remain unchanged */
}

/* Get the total number of items in the expiration store */
uint64_t estoreSize(estore *es) {
    return es->count;
}

/* Move ebuckets from one estore to another */
void estoreMoveEbuckets(estore *src, estore *dst, int eidx) {
    serverAssert(src->num_buckets > eidx);
    serverAssert(src->num_buckets == dst->num_buckets);
    serverAssert(ebIsEmpty(dst->ebArray[eidx])); /* If it is NULL */

    /* Adjust source estore */
    ebuckets eb = src->ebArray[eidx];
    if (ebIsEmpty(eb)) return;
    int64_t count = (int64_t)ebGetTotalItems(eb, src->bucket_type);
    src->count -= count;
    fwTreeUpdate(src->buckets_sizes, eidx, -count);
    src->ebArray[eidx] = ebCreate(); /* Set to NULL actually.*/

    /* Move ebuckets to destination estore */
    dst->ebArray[eidx] = eb;
    dst->count += count;
    fwTreeUpdate(dst->buckets_sizes, eidx, count);
}

#ifdef REDIS_TEST
#include <stdio.h>
#include "testhelp.h"

#define TEST(name) printf("test — %s\n", name);

/* Test item structure for estore testing */
typedef struct TestItem {
    kvobj kv;  /* mimic kvobj of type HASH to pass checks in estore */
    ExpireMeta mexpire;
    int index;
} TestItem;

/* Test EbucketsType for estore testing */
ExpireMeta *getTestItemExpireMeta(const eItem item) {
    return &((TestItem *)item)->mexpire;
}

void deleteTestItemCb(eItem item, void *ctx) {
    UNUSED(ctx);
    zfree(item);
}

EbucketsType testEbucketsType = {
    .getExpireMeta = getTestItemExpireMeta,
    .onDeleteItem = deleteTestItemCb,
    .itemsAddrAreOdd = 0,
};

/* Helper function to create a test item */
static TestItem *createTestItem(int index) {
    TestItem *item = zmalloc(sizeof(TestItem));
    item->index = index;
    item->mexpire.trash = 1;
    /* mimic kvobj of type HASH to pass checks in estore */
    item->kv.type = OBJ_HASH;
    item->kv.encoding = OBJ_ENCODING_LISTPACK_EX;
    return item;
}

static ExpireAction activeExpireTestCb(eItem item, void *ctx) {
    UNUSED(ctx);
    zfree(item);
    return 0;
}

int estoreTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Initialize minimal server state needed for testing */
    server.hz = 10;
    server.unixtime = time(NULL);

    TEST("Create and destroy estore") {
        estore *es = estoreCreate(&testEbucketsType, 0);
        assert(es != NULL);
        assert(estoreIsEmpty(es));
        assert(estoreSize(es) == 0);
        estoreRelease(es);
    }

    TEST("Create estore with multiple buckets") {
        estore *es = estoreCreate(&testEbucketsType, 2); /* 4 buckets */
        assert(es != NULL);
        assert(estoreIsEmpty(es));
        assert(estoreSize(es) == 0);

        /* Test bucket access */
        ebuckets *bucket0 = estoreGetBuckets(es, 0);
        ebuckets *bucket1 = estoreGetBuckets(es, 1);
        ebuckets *bucket2 = estoreGetBuckets(es, 2);
        ebuckets *bucket3 = estoreGetBuckets(es, 3);

        assert(bucket0 != NULL);
        assert(bucket1 != NULL);
        assert(bucket2 != NULL);
        assert(bucket3 != NULL);

        /* All buckets should be different */
        assert(bucket0 != bucket1);
        assert(bucket0 != bucket2);
        assert(bucket1 != bucket3);

        estoreRelease(es);
    }

    TEST("Add and remove items") {
        estore *es = estoreCreate(&testEbucketsType, 1); /* 2 buckets */

        /* Test initial state */
        assert(estoreSize(es) == 0);
        assert(estoreIsEmpty(es));

        /* Create test items */
        TestItem *item1 = createTestItem(1);
        TestItem *item2 = createTestItem(2);
        TestItem *item3 = createTestItem(3);

        /* Add items to different buckets */
        estoreAdd(es, 0, item1, 1000);
        assert(estoreSize(es) == 1);
        assert(!estoreIsEmpty(es));

        estoreAdd(es, 1, item2, 2000);
        assert(estoreSize(es) == 2);

        estoreAdd(es, 0, item3, 3000);  /* Add another item to bucket 0 */
        assert(estoreSize(es) == 3);

        /* Verify expiration times are set correctly */
        assert(ebGetMetaExpTime(&item1->mexpire) == 1000);
        assert(ebGetMetaExpTime(&item2->mexpire) == 2000);
        assert(ebGetMetaExpTime(&item3->mexpire) == 3000);

        /* Remove items */
        uint64_t expireTime1 = estoreRemove(es, 0, item1);
        assert(expireTime1 == 1000);
        assert(estoreSize(es) == 2);
        zfree(item1);

        uint64_t expireTime2 = estoreRemove(es, 1, item2);
        assert(expireTime2 == 2000);
        assert(estoreSize(es) == 1);
        zfree(item2);

        uint64_t expireTime3 = estoreRemove(es, 0, item3);
        assert(expireTime3 == 3000);
        assert(estoreSize(es) == 0);
        assert(estoreIsEmpty(es));
        zfree(item3);

        /* Clean up - items are freed by the onDeleteItem callback */
        estoreRelease(es);
    }

    TEST("Update item expiration") {
        estore *es = estoreCreate(&testEbucketsType, 0); /* 1 bucket */

        /* Create and add a test item */
        TestItem *item = createTestItem(1);
        estoreAdd(es, 0, item, 1000);
        assert(estoreSize(es) == 1);

        /* Verify initial expiration time */
        assert(ebGetMetaExpTime(&item->mexpire) == 1000);

        /* Update expiration time */
        estoreUpdate(es, 0, item, 2000);
        assert(estoreSize(es) == 1); /* Size should remain the same */
        assert(ebGetMetaExpTime(&item->mexpire) == 2000);

        /* Update again to a different time */
        estoreUpdate(es, 0, item, 500);
        assert(estoreSize(es) == 1);
        assert(ebGetMetaExpTime(&item->mexpire) == 500);

        /* Clean up */
        estoreRemove(es, 0, item);
        assert(estoreSize(es) == 0);
        zfree(item);

        estoreRelease(es);
    }

    TEST("Non-empty bucket iteration") {
        estore *es = estoreCreate(&testEbucketsType, 2); /* 4 buckets */

        /* Test bucket iteration on empty store */
        assert(estoreGetFirstNonEmptyBucket(es) == 0); /* Returns 0 when empty */
        assert(estoreGetNextNonEmptyBucket(es, 0) == -1); /* No next bucket when empty */

        /* Create test items and add to specific buckets */
        TestItem *item1 = createTestItem(1);
        TestItem *item2 = createTestItem(2);
        TestItem *item3 = createTestItem(3);

        /* Add to bucket 1 and 3 (skip 0 and 2) */
        estoreAdd(es, 1, item1, 1000);
        estoreAdd(es, 3, item2, 2000);
        estoreAdd(es, 3, item3, 3000);  /* Add another item to bucket 3 */

        assert(estoreSize(es) == 3);

        /* Test iteration through non-empty buckets */
        int firstBucket = estoreGetFirstNonEmptyBucket(es);
        assert(firstBucket == 1);

        int nextBucket = estoreGetNextNonEmptyBucket(es, firstBucket);
        assert(nextBucket == 3);

        int lastBucket = estoreGetNextNonEmptyBucket(es, nextBucket);
        assert(lastBucket == -1); /* No more non-empty buckets */

        /* Test iteration from different starting points */
        assert(estoreGetNextNonEmptyBucket(es, 0) == 1);
        assert(estoreGetNextNonEmptyBucket(es, 2) == 3);

        /* Clean up */
        estoreRemove(es, 1, item1);
        zfree(item1);
        estoreRemove(es, 3, item2);
        zfree(item2);
        estoreRemove(es, 3, item3);
        zfree(item3);
        assert(estoreSize(es) == 0);

        estoreRelease(es);
    }

    TEST("Empty estore") {
        estore *es = estoreCreate(&testEbucketsType, 1); /* 2 buckets */

        /* Add some items */
        TestItem *item1 = createTestItem(1);
        TestItem *item2 = createTestItem(2);
        TestItem *item3 = createTestItem(3);

        estoreAdd(es, 0, item1, 1000);
        estoreAdd(es, 1, item2, 2000);
        estoreAdd(es, 0, item3, 3000);
        assert(estoreSize(es) == 3);
        assert(!estoreIsEmpty(es));

        /* Empty the store - this should call onDeleteItem for all items */
        estoreEmpty(es);
        assert(estoreSize(es) == 0);
        assert(estoreIsEmpty(es));

        /* Verify buckets are empty */
        assert(estoreGetFirstNonEmptyBucket(es) == 0); /* Returns 0 when empty */
        assert(estoreGetNextNonEmptyBucket(es, 0) == -1);

        estoreRelease(es);
    }

    TEST("Active expiration") {
        estore *es = estoreCreate(&testEbucketsType, 14); /* 2^14 buckets */

        /* Create items with different expiration times */
        TestItem *expiredItem1 = createTestItem(1);
        TestItem *expiredItem2 = createTestItem(2);
        TestItem *expiredItem3 = createTestItem(3);
        TestItem *futureItem = createTestItem(4);

        estoreAdd(es, 0, expiredItem1, 1023);
        estoreAdd(es, 0, expiredItem2, 2047);
        estoreAdd(es, 1, expiredItem3, 127);
        estoreAdd(es, 0, futureItem, 4095);
        assert(estoreSize(es) == 4);

        /* Perform active expiration */
        ExpireInfo expireInfo = {
            .maxToExpire = UINT64_MAX,
            .onExpireItem = activeExpireTestCb,
            .ctx = NULL,
            .now = 2048,  /* Current time in milliseconds */
            .itemsExpired = 0
        };

        estoreActiveExpire(es, 0, &expireInfo);

        /* The expired items should be removed, future item should remain */
        assert(expireInfo.itemsExpired == 2);
        assert(estoreSize(es) == 2);
        
        estoreActiveExpire(es, 1, &expireInfo);
        assert(expireInfo.itemsExpired == 1);
        assert(estoreSize(es) == 1);

        /* Clean up remaining item */
        estoreRemove(es, 0, futureItem);
        zfree(futureItem);
        assert(estoreSize(es) == 0);

        estoreRelease(es);
    }
    
    return 0;
}
#endif
