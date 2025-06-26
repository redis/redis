/*
 * Copyright Redis Ltd. 2024 - present
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include "zmalloc.h"
#include "redisassert.h"
#include "config.h"
#include "ebuckets.h"

#define UNUSED(x) (void)(x)

/*** DEBUGGING & VALIDATION
 *
 * To validate DS on add(), remove() and ebExpire()
 * #define EB_VALIDATE_DEBUG 1
 */

#if (REDIS_TEST || EB_VALIDATE_DEBUG) && !defined(EB_TEST_BENCHMARK)
#define EB_VALIDATE_STRUCTURE(eb, type) ebValidate(eb, type)
#else
#define EB_VALIDATE_STRUCTURE(eb, type) // Do nothing
#endif

/*** BENCHMARK
 *
 * To benchmark ebuckets creation and active-expire with 10 million items, apply
 * the following command such that `EB_TEST_BENCHMARK` gets desired distribution
 * of expiration times:
 *
 *   # 0=1msec, 1=1sec, 2=1min, 3=1hour, 4=1day, 5=1week, 6=1month
 *   make REDIS_CFLAGS='-DREDIS_TEST -DEB_TEST_BENCHMARK=3' && ./src/redis-server test ebuckets
 */

/*
 * EB_SEG_MAX_ITEMS - Maximum number of items in rax-segment before trying to
 * split. To simplify, it has the same value as EB_LIST_MAX_ITEMS.
 */
#define EB_SEG_MAX_ITEMS 16
#define EB_LIST_MAX_ITEMS EB_SEG_MAX_ITEMS

/* From expiration time in msec to bucket-key - using the type's bucketPrecision */
#define EB_BUCKET_KEY(exptime, type) ((exptime) >> (type)->ebp.precision)

/* From bucket-key to expiration time in msec */
#define EB_BUCKET_EXP_TIME(bucketKey, type) ((uint64_t)(bucketKey) << (type)->ebp.precision)

/* ebstack macro helper to adjust type before run command on ebuckets L1/2 */
#define EB_STACK_EXEC_L1(type, ebCommand)            \
    do {                                             \
        (type)->isEbStack = 0;                       \
        ebCommand;                                   \
        (type)->isEbStack = 1;                       \
    } while (0)
#define EB_STACK_EXEC_L2(type, ebCommand)            \
    do {                                             \
        (type)->isEbStack = 0;                       \
        EBucketPrecision __ebp_tmp = (type)->ebp;    \
        (type)->ebp = ebpStackL2;                    \
        ebCommand;                                   \
        (type)->ebp = __ebp_tmp;                     \
        (type)->isEbStack = 1;                       \
    } while (0)

#define EB_STACK_L3_SIZEOF(n) (sizeof(ebVector) + sizeof(eItem) * (n))

/*** structs ***/

typedef struct CommonSegHdr {
    eItem head;
} CommonSegHdr;


/* FirstSegHdr - Header of first segment of a bucket.
 *
 * A bucket in rax tree with a single segment will be as follows:
 *
 *            +-------------+     +------------+             +------------+
 *            | FirstSegHdr |     | eItem(1)   |             | eItem(N)   |
 * [rax] -->  | eItem head  | --> | void *next | --> ... --> | void *next | --+
 *            +-------------+     +------------+             +------------+   |
 *                    ^                                                       |
 *                    |                                                       |
 *                    +-------------------------------------------------------+
 *
 * Note that the cyclic references assist to update locally the segment(s) without
 * the need to "heavy" traversal of the rax tree for each change.
 */
typedef struct FirstSegHdr {
    eItem head;          /* first item in the list */
    uint32_t totalItems; /* total items in the bucket, across chained segments */
    uint32_t numSegs;    /* number of segments in the bucket */
} FirstSegHdr;

/* NextSegHdr - Header of next segment in an extended-segment (bucket)
 *
 * Here is the layout of an extended-segment, after adding another item to a single,
 * full (EB_SEG_MAX_ITEMS=16), segment (all items must have same bucket-key value):
 *
 *            +-------------+     +------------+      +------------+     +------------+             +------------+
 *            | FirstSegHdr |     | eItem(17)  |      | NextSegHdr |     | eItem(1)   |             | eItem(16)  |
 * [rax] -->  | eItem head  | --> | void *next | -->  | eItem head | --> | void *next | --> ... --> | void *next | --+
 *            +-------------+     +------------+      +------------+     +------------+             +------------+   |
 *                    ^                                  |    ^                                                      |
 *                    |                                  |    |                                                      |
 *                    +------------- firstSeg / prevSeg -+    +------------------------------------------------------+
 */
typedef struct NextSegHdr {
    eItem head;
    CommonSegHdr *prevSeg; /* pointer to previous segment */
    FirstSegHdr *firstSeg; /* pointer to first segment of the bucket */
} NextSegHdr;

typedef struct ebVector {
    uint64_t items;
    uint64_t vecSize; /* Power of 2 */
    eItem vec[];
} ebVector;

typedef struct ebStack {
    /* Hierarchical ebuckets stack */
    ebuckets l1;          /* LEVEL1: TTL < 2^(ebpStackL2.precision+1) msec */
    ebuckets l2;          /* LEVEL2: expiration-time < 2^48 msec */
    ebVector *l3;         /* LEVEL3: expiration-time >= 2^48 msec (infinity) */
    
    uint64_t items;       /* Total number of items in the stack */
} ebStack;

/* Unlike ebuckets level 1 in ebStack, the level 2 precision is not configurable
 * and is fixed to 21 bits (~34.9 minutes). This is because active expiration
 * cycle is fine-tuned based on it. For up to 30 days of expiration values, it
 * will be limited by no more than 1235 buckets and in the common case, it will be
 * considerably less. */
const EBucketPrecision ebpStackL2 = {
    .precision = 21, /* L2 Bucket bits precision */
    .keySize = EB_PRECISION2KEYSIZE(21),
};

/* ExpireMetaL3 is used for items stored in ebStack Level 3 (the "infinity" vector)
 * which holds very long expiration times (> EB_EXPIRE_TIME_MAX = 48 bits). Regular 
 * ExpireMeta can only store 48-bit timestamps (sufficient until year 10889), but 
 * L3 items need full 64-bit timestamps for practically infinite expiration times.
 *
 * The struct is aligned with ExpireMeta to allow in-place conversion.
 */
typedef struct ExpireMetaL3 {
    /* 48 bits of Index into infinity vector (Overlays ExpireMeta.expireTimeLo/Hi) */
    uint32_t expireIndexLo;
    uint16_t expireIndexHi;
    
    /* Same flags like ExpireMeta (but only `storedIn` is being used) */
    EXPIRE_META_FLAGS
    
    /* 8-byte of expiration time (Overlays the `ExpireMeta.next` pointer) */
    uint64_t expireTime;
} ExpireMetaL3;

/* Selective declarations from server.h instead of including it */
#ifndef static_assert
#define static_assert(expr, lit) extern char __static_assert_failure[(expr) ? 1:-1]
#endif

#ifndef REDIS_TEST
typedef long long mstime_t; /* millisecond time type. */
mstime_t commandTimeSnapshot(void);
#else /* Let tests control time */
uint64_t __NOW__ = 0;
#define commandTimeSnapshot() __NOW__
#endif

/* Verify that "head" field is aligned in FirstSegHdr, NextSegHdr and CommonSegHdr */
static_assert(offsetof(FirstSegHdr, head) == 0, "FirstSegHdr head is not aligned");
static_assert(offsetof(NextSegHdr, head) == 0, "FirstSegHdr head is not aligned");
static_assert(offsetof(CommonSegHdr, head) == 0, "FirstSegHdr head is not aligned");
/* Verify attached metadata to rax is aligned */
static_assert(offsetof(rax, metadata) % sizeof(void*) == 0, "metadata field is not aligned in rax");
/* Verify that EBucketPrecision is sizeof uint64_t */
static_assert(sizeof(EBucketPrecision) == sizeof(uint64_t), "EBucketPrecision is not 64bit");
/* Verify that ExpireMetaL3 aligned with ExpireMeta */
static_assert(sizeof(ExpireMetaL3) == sizeof(ExpireMeta), "ExpireMetaL3 not aligned");
static_assert(offsetof(ExpireMetaL3, expireIndexLo) == offsetof(ExpireMeta, expireTimeLo), "ExpireMetaL3 not aligned");
static_assert(offsetof(ExpireMetaL3, expireIndexHi) == offsetof(ExpireMeta, expireTimeHi), "ExpireMetaL3 not aligned");
static_assert(offsetof(ExpireMetaL3, expireTime) == offsetof(ExpireMeta, next), "ExpireMetaL3 not aligned");

/* EBucketNew - Indicates the caller to create a new bucket following the addition
 * of another item to a bucket (either single-segment or extended-segment). */
typedef struct EBucketNew {
    FirstSegHdr segment;
    ExpireMeta *mLast;  /* last item in the chain */
    uint64_t ebKey;
} EBucketNew;

static void ebNewBucket(EbucketsType *type, EBucketNew *newBucket, eItem item, uint64_t key);
static int ebBucketPrint(uint64_t bucketKey, EbucketsType *type, FirstSegHdr *firstSeg);
static uint64_t *ebRaxNumItems(rax *rax);

/*** Static functions ***/

/* Extract pointer to list from ebuckets handler */
static inline rax *ebGetRaxPtr(ebuckets eb) { return (rax *)eb; }

/* The lsb in ebuckets pointer determines whether the pointer points to rax or list. */
static inline int ebIsList(ebuckets eb) {
    return (((uintptr_t)(void *)eb & 0x1) == 1);
}
/* set lsb in ebuckets pointer to 1 to mark it as list. Unless empty (NULL) */
static inline ebuckets ebMarkAsList(eItem item) {
    if (item == NULL) return item;

    /* either 'itemsAddrAreOdd' or not, we end up with lsb is set to 1 */
    return (void *) ((uintptr_t) item | 1);
}

/* Extract pointer to the list from ebuckets handler */
static inline eItem ebGetListPtr(EbucketsType *type, ebuckets eb) {
    /* if 'itemsAddrAreOdd' then no need to reset lsb bit */
    if (type->itemsAddrAreOdd)
        return eb;
    else
        return (void*)((uintptr_t)(eb) & ~1);
}

/* Set the index of the item in the infinity vector (L3) */
static inline void ebSetMetaL3Index(ExpireMetaL3 *em3, uint64_t idx) { 
    ebSetMetaExpTime( (ExpireMeta *)em3, idx); 
}

static inline void ebSetMetaL3ExpireTime(ExpireMetaL3 *em3, uint64_t t) { 
    em3->expireTime = t; 
}

/* Converts the logical starting time value of a given bucket-key to its equivalent
 * "physical" value in the context of an rax tree (rax-key). Although their values
 * are the same, their memory layouts differ. The raxKey layout orders bytes in
 * memory is from the MSB to the LSB, and the length of the key is "t->keySize" */
static inline void bucketKey2RaxKey(uint64_t bucketKey, unsigned char *raxKey, const EbucketsType *t) {
    for (int i = t->ebp.keySize-1; i >= 0; --i) {
        raxKey[i] = (unsigned char) (bucketKey & 0xFF);
        bucketKey >>= 8;
    }
}

/* Converts the "physical" value of rax-key to its logical counterpart, representing
 * the starting time value of a bucket. The values are equivalent, but their memory
 * layouts differ. The raxKey is assumed to be ordered from the MSB to the LSB with
 * a length of "t->keySize". The resulting bucket-key is the logical representation
 * with respect to ebuckets. */
static inline uint64_t raxKey2BucketKey(unsigned char *raxKey, const EbucketsType *t) {
    uint64_t bucketKey = 0;
    for (unsigned int i = 0; i < t->ebp.keySize ; ++i)
        bucketKey = (bucketKey<<8) + raxKey[i];
    return bucketKey;
}

/* Add another item to a bucket that consists of extended-segments. In this
 * scenario, all items in the bucket share the same bucket-key value and the first
 * segment is already full (if not, the function ebSegAddAvail() would have being
 * called). This requires the creation of another segment. The layout of the
 * segments before and after the addition of the new item is as follows:
 *
 *  Before:                               [segHdr] -> {item1,..,item16} -> [..]
 *  After:   [segHdr] -> {newItem} -> [nextSegHdr] -> {item1,..,item16} -> [..]
 *
 *  Taken care to persist `segHdr` to be the same instance after the change.
 *  This is important because the rax tree is pointing to it. */
static int ebSegAddExtended(EbucketsType *type, FirstSegHdr *firstSegHdr, eItem newItem) {
    /* Allocate nextSegHdr and let it take the items of first segment header */
    NextSegHdr *nextSegHdr = zmalloc(sizeof(NextSegHdr));
    nextSegHdr->head = firstSegHdr->head;
    /* firstSegHdr will stay the first and new nextSegHdr will follow it */
    nextSegHdr->prevSeg = (CommonSegHdr *) firstSegHdr;
    nextSegHdr->firstSeg = firstSegHdr;

    ExpireMeta *mIter = type->getExpireMeta(nextSegHdr->head);
    mIter->firstItemBucket = 0;
    for (int i = 0 ; i < EB_SEG_MAX_ITEMS-1 ; i++)
        mIter = type->getExpireMeta(mIter->next);

    if (mIter->lastItemBucket) {
        mIter->next = nextSegHdr;
    } else {
        /* Update next-next-segment to point back to next-segment */
        NextSegHdr *nextNextSegHdr = mIter->next;
        nextNextSegHdr->prevSeg = (CommonSegHdr *) nextSegHdr;
    }

    firstSegHdr->numSegs += 1;
    firstSegHdr->totalItems += 1;
    firstSegHdr->head = newItem;

    ExpireMeta *mNewItem = type->getExpireMeta(newItem);
    mNewItem->numItems = 1;
    mNewItem->next = nextSegHdr;
    mNewItem->firstItemBucket = 1;
    mNewItem->lastInSegment = 1;

    return 0;
}

/* Add another eItem to a segment with available space. Keep items sorted in ascending order */
static int ebSegAddAvail(EbucketsType *type, FirstSegHdr *seg, eItem item) {
    eItem head = seg->head;
    ExpireMeta *nextMeta;
    ExpireMeta *mHead = type->getExpireMeta(head);
    ExpireMeta *mItem = type->getExpireMeta(item);
    uint64_t itemExpireTime = ebGetMetaExpTime(mItem);

    seg->totalItems++;

    assert(mHead->numItems < EB_SEG_MAX_ITEMS);

    /* if new item expiry time is smaller than the head then add it before the head */
    if (ebGetMetaExpTime(mHead) > itemExpireTime) {
        /* Insert item as the new head */
        mItem->next = head;
        mItem->firstItemBucket = mHead->firstItemBucket;
        mItem->numItems = mHead->numItems + 1;
        mHead->firstItemBucket = 0;
        mHead->numItems = 0;
        seg->head = item;
        return 0;
    }

    /* Insert item in the middle of segment */
    ExpireMeta *mIter = mHead;
    for (int i = 1 ; i < mHead->numItems ; i++) {
        nextMeta = type->getExpireMeta(mIter->next);
        /* Insert item in the middle */
        if (ebGetMetaExpTime(nextMeta) > itemExpireTime) {
            mHead->numItems = mHead->numItems + 1;
            mItem->next = mIter->next;
            mIter->next = item;
            return 0;
        }
        mIter = nextMeta;
    }

    /* Insert item as the last item of the segment. Inherit flags from previous last item */
    mHead->numItems = mHead->numItems + 1;
    mItem->next = mIter->next;
    mItem->lastInSegment = mIter->lastInSegment;
    mItem->lastItemBucket = mIter->lastItemBucket;
    mIter->lastInSegment = 0;
    mIter->lastItemBucket = 0;
    mIter->next = item;
    return 0;
}

/* Return 1 if split segment to two succeeded. Else, return 0. The only reason
 * the split can fail is that All the items in the segment have the same bucket-key */
static int ebTrySegSplit(EbucketsType *type, FirstSegHdr *seg, EBucketNew *newBucket) {
    int minMidDist=(EB_SEG_MAX_ITEMS / 2), bestMiddleIndex = -1;
    uint64_t splitKey = -1;
    eItem firstItemSecondPart;
    ExpireMeta *mLastItemFirstPart, *mFirstItemSecondPart;

    eItem head = seg->head;
    ExpireMeta *mHead = type->getExpireMeta(head);
    ExpireMeta *mNext, *mIter = mHead;

    /* Search for best middle index to split the segment into two segments. As the
     * items are arranged in ascending order, it cannot split between two items that
     * have the same expiration time and therefore the split won't necessarily be
     * balanced (Or won't be possible to split at all if all have the same exp-time!)
     */
    for (int i = 0 ; i < EB_SEG_MAX_ITEMS-1 ; i++) {
        //printf ("i=%d\n", i);
        mNext = type->getExpireMeta(mIter->next);
        if (EB_BUCKET_KEY(ebGetMetaExpTime(mNext), type) >
            EB_BUCKET_KEY(ebGetMetaExpTime(mIter), type)) {
            /* If found better middle index before reaching halfway, save it */
            if (i < (EB_SEG_MAX_ITEMS/2)) {
                splitKey = EB_BUCKET_KEY(ebGetMetaExpTime(mNext), type);
                bestMiddleIndex = i;
                mLastItemFirstPart = mIter;
                mFirstItemSecondPart = mNext;
                firstItemSecondPart = mIter->next;
                minMidDist = (EB_SEG_MAX_ITEMS / 2) - bestMiddleIndex;
            } else {
                /* after crossing the middle need only to look for the first diff */
                if (minMidDist > (i + 1 - EB_SEG_MAX_ITEMS / 2)) {
                    splitKey = EB_BUCKET_KEY(ebGetMetaExpTime(mNext), type);
                    bestMiddleIndex = i;
                    mLastItemFirstPart = mIter;
                    mFirstItemSecondPart = mNext;
                    firstItemSecondPart = mIter->next;
                    minMidDist = i + 1 - EB_SEG_MAX_ITEMS / 2;
                }
            }
        }
        mIter = mNext;
    }

    /* If cannot find index to split because all with same EB_BUCKET_KEY(), then
     * segment should be treated as extended segment */
    if (bestMiddleIndex == -1)
        return 0;

    /* New bucket */
    newBucket->segment.head = firstItemSecondPart;
    newBucket->segment.numSegs = 1;
    newBucket->segment.totalItems = EB_SEG_MAX_ITEMS - bestMiddleIndex - 1;
    mFirstItemSecondPart->numItems = EB_SEG_MAX_ITEMS - bestMiddleIndex - 1;
    newBucket->mLast = mIter;
    newBucket->ebKey = splitKey;
    mIter->lastInSegment = 1;
    mIter->lastItemBucket = 1;
    mIter->next = &newBucket->segment; /* to be updated by caller */
    mFirstItemSecondPart->firstItemBucket = 1;

    /* update existing bucket */
    seg->totalItems = bestMiddleIndex + 1;
    mHead->numItems = bestMiddleIndex + 1;
    mLastItemFirstPart->lastInSegment = 1;
    mLastItemFirstPart->lastItemBucket = 1;
    mLastItemFirstPart->next = seg;
    return 1;
}

/* Return 1 if managed to expire the entire segment. Returns 0 otherwise. */
int ebSingleSegExpire(FirstSegHdr *firstSegHdr,
                      EbucketsType *type,
                      ExpireInfo *info,
                      eItem *updateList)
{
    uint64_t itemExpTime;
    eItem iter = firstSegHdr->head;
    ExpireMeta *mIter = type->getExpireMeta(iter);
    uint32_t i=0, numItemsInSeg = mIter->numItems;

    while (info->itemsExpired < info->maxToExpire) {
        itemExpTime = ebGetMetaExpTime(mIter);

        /* Items are arranged in ascending expire-time order in a segment. Stops
         * active expiration when an item's expire time is greater than `now`. */
        if (itemExpTime > info->now)
            break;

        /* keep aside next before deletion of iter */
        eItem next = mIter->next;
        int storedInTmp = mIter->storedIn;
        mIter->storedIn = EB_STORED_IN_TRASH;
        ExpireAction act = info->onExpireItem(iter, info->ctx);

        /* if (act == ACT_REMOVE_EXP_ITEM)
         *  then don't touch the item. Assume it got deleted */

        /* If indicated to stop then break (cb didn't delete the item) */
        if (act == ACT_STOP_ACTIVE_EXP) {
            mIter->storedIn = storedInTmp;
            break;
        }

        /* If indicated to re-insert the item, then chain it to updateList.
         * it will be ebAdd() back to ebuckets at the end of ebExpire() */
        if (act == ACT_UPDATE_EXP_ITEM) {
            mIter->next = *updateList;
            *updateList = iter;
        }

        ++info->itemsExpired;

        /* if deleted all items in segment, delete header and return */
        if (++i == numItemsInSeg) {
            zfree(firstSegHdr);
            return 1;
        }

        /* More items in the segment. Set iter to next item and update mIter */
        iter = next;
        mIter = type->getExpireMeta(iter);
    }

    /* Update the single-segment with remaining items */
    mIter->numItems = numItemsInSeg - i;
    mIter->firstItemBucket = 1;
    firstSegHdr->head = iter;
    firstSegHdr->totalItems -= i;

    /* Update nextExpireTime */
    info->nextExpireTime = ebGetMetaExpTime(mIter);

    return 0;
}

/* return 1 if managed to expire the entire segment. Returns 0 otherwise. */
static int ebSegExpire(FirstSegHdr *firstSegHdr,
                       EbucketsType *type,
                       ExpireInfo *info,
                       eItem *updateList)
{
    eItem iter = firstSegHdr->head;
    uint32_t numSegs = firstSegHdr->numSegs;
    void *nextSegHdr = firstSegHdr;

    if (numSegs == 1)
        return ebSingleSegExpire(firstSegHdr, type, info, updateList);

    /*
     * In an extended-segment, there's no need to verify the expiration time of
     * each item. This is because all items in an extended-segment share the same
     * bucket-key. Therefore, we can remove all items without checking their
     * individual expiration times. This is different from a single-segment
     * scenario, where items can have different bucket-keys.
     */
    for (uint32_t seg=0 ; seg < numSegs ; seg++) {
        uint32_t i;
        ExpireMeta *mIter = type->getExpireMeta(iter);
        uint32_t numItemsInSeg = mIter->numItems;

        for (i = 0; (i < numItemsInSeg) && (info->itemsExpired < info->maxToExpire) ; ++i) {
            mIter = type->getExpireMeta(iter);

            /* keep aside `next` before removing `iter` by onExpireItem */
            eItem next = mIter->next;
            int storedInTmp = mIter->storedIn;
            mIter->storedIn = EB_STORED_IN_TRASH;
            ExpireAction act = info->onExpireItem(iter, info->ctx);

            /* if (act == ACT_REMOVE_EXP_ITEM)
             *  then don't touch the item. Assume it got deleted */

            /* If indicated to stop then break (callback didn't delete the item) */
            if (act == ACT_STOP_ACTIVE_EXP) {
                mIter->storedIn = storedInTmp;
                break;
            }

            /* If indicated to re-insert the item, then chain it to updateList.
             * it will be ebAdd() back to ebuckets at the end of ebExpire() */
            if (act == ACT_UPDATE_EXP_ITEM) {
                mIter->next = *updateList;
                *updateList = iter;
            }

            /* Item was REMOVED/UPDATED. Advance to `next` item */
            iter = next;
            ++info->itemsExpired;
            firstSegHdr->totalItems -= 1;
        }

        /* if deleted all items in segment */
        if (i == numItemsInSeg) {
            /* If not last segment in bucket, then delete segment header */
            if (seg + 1 < numSegs) {
                nextSegHdr = iter;
                iter = ((NextSegHdr *) nextSegHdr)->head;
                zfree(nextSegHdr);
                firstSegHdr->numSegs -= 1;
                firstSegHdr->head = iter;
                mIter = type->getExpireMeta(iter);
                mIter->firstItemBucket = 1;
            }
        } else {
            /* We reached here because for-loop above break due to
             * ACT_STOP_ACTIVE_EXP or reached maxToExpire */
            firstSegHdr->head = iter;
            mIter = type->getExpireMeta(iter);
            mIter->numItems = numItemsInSeg - i;
            mIter->firstItemBucket = 1;
            info->nextExpireTime = ebGetMetaExpTime(mIter);

            /* If deleted one or more segments, update prevSeg of next seg to point firstSegHdr.
             * If it is the last segment, then last item need to point firstSegHdr */
            if (seg>0) {
                int numItems = mIter->numItems;
                for (int i = 0; i < numItems - 1; i++)
                    mIter = type->getExpireMeta(mIter->next);

                if (mIter->lastItemBucket) {
                    mIter->next = firstSegHdr;
                } else {
                    /* Update next-segment to point back to firstSegHdr */
                    NextSegHdr *nsh = mIter->next;
                    nsh->prevSeg = (CommonSegHdr *) firstSegHdr;
                }
            }

            return 0;
        }
    }

    /* deleted last segment in bucket */
    zfree(firstSegHdr);
    return 1;
}

/*** Static functions of list ***/

/* Convert a list to rax.
 *
 * To create a new rax, the function first converts the list to a segment by
 * allocating a segment header and attaching to it the already existing list.
 * Then, it adds the new segment to the rax as the first bucket. */
static rax *ebConvertListToRax(eItem listHead, EbucketsType *type) {
    FirstSegHdr *firstSegHdr = zmalloc(sizeof(FirstSegHdr));
    firstSegHdr->head = listHead;
    firstSegHdr->totalItems = EB_LIST_MAX_ITEMS ;
    firstSegHdr->numSegs = 1;

    /* update last item to point on the segment header */
    ExpireMeta *metaItem = type->getExpireMeta(listHead);
    uint64_t bucketKey = EB_BUCKET_KEY(ebGetMetaExpTime(metaItem), type);
    while (metaItem->lastItemBucket == 0)
        metaItem = type->getExpireMeta(metaItem->next);
    metaItem->next = firstSegHdr;

    /* Use min expire-time for the first segment in rax */
    unsigned char raxKey[6 /* max keySize */];
    bucketKey2RaxKey(bucketKey, raxKey, type);
    rax *rax = raxNewWithMetadata(sizeof(uint64_t));
    *ebRaxNumItems(rax) = EB_LIST_MAX_ITEMS;
    raxInsert(rax, raxKey, type->ebp.keySize, firstSegHdr, NULL);
    return rax;
}

/**
 * Adds another 'item' to the ebucket of type list, keeping the list sorted by
 * ascending expiration time.
 *
 * @param eb - Pointer to the ebuckets handler of type list. Gets updated if the item is
 * added as the new head.
 * @param type - Pointer to the EbucketsType structure defining the type of ebucket.
 * @param item - The eItem to be added to the list.
 *
 * @return 1 if the maximum list length is reached; otherwise, return 0.
 */
static int ebAddToList(ebuckets *eb, EbucketsType *type, eItem item) {
    ExpireMeta *metaItem = type->getExpireMeta(item);

    /* if ebucket-list is empty (NULL), then create a new list by marking 'item'
     * as the head and tail of the list */
    if (unlikely(ebIsEmpty(*eb))) {
        metaItem->next = NULL;
        metaItem->numItems = 1;
        metaItem->lastInSegment = 1;
        metaItem->firstItemBucket = 1;
        metaItem->lastItemBucket = 1;
        *eb = ebMarkAsList(item);
        return 0;
    }

    eItem head = ebGetListPtr(type, *eb);
    ExpireMeta *metaHead = type->getExpireMeta(head);

    /* If reached max items in list, then return 1 */
    if (metaHead->numItems == EB_LIST_MAX_ITEMS)
        return 1;

    /* if expiry time of 'item' is smaller than the head then add it as the new head */
    if (ebGetMetaExpTime(metaHead) > ebGetMetaExpTime(metaItem)) {
        /* Insert item as the new head */
        metaItem->next = head;
        metaItem->firstItemBucket = 1;
        metaItem->numItems = metaHead->numItems + 1;
        metaHead->firstItemBucket = 0;
        metaHead->numItems = 0;
        *eb = ebMarkAsList(item);
        return 0;
    }


    /* Try insert item in the middle of list */
    ExpireMeta *mIter = metaHead;
    for (int i = 1 ; i < metaHead->numItems ; i++) {
        ExpireMeta *nextMeta = type->getExpireMeta(mIter->next);
        /* Insert item in the middle */
        if (ebGetMetaExpTime(nextMeta) > ebGetMetaExpTime(metaItem)) {
            metaHead->numItems += 1;
            metaItem->next = mIter->next;
            mIter->next = item;
            return 0;
        }
        mIter = nextMeta;
    }

    /* Insert item as the last item of the list. */
    metaHead->numItems += 1;
    metaItem->next = NULL;
    metaItem->lastInSegment = 1;
    metaItem->lastItemBucket = 1;
    /* Update obsolete last item */
    mIter->lastInSegment = 0;
    mIter->lastItemBucket = 0;
    mIter->next = item;
    return 0;
}

/* return 1 if removed from list. Otherwise, return 0 */
static int ebRemoveFromList(ebuckets *eb, EbucketsType *type, eItem item) {
    if (ebIsEmpty(*eb))
        return 0; /* not removed */

    ExpireMeta *metaItem = type->getExpireMeta(item);
    eItem head = ebGetListPtr(type, *eb);

    /* if item is the head of the list */
    if (head == item) {
        eItem newHead = metaItem->next;
        if (newHead != NULL) {
            ExpireMeta *mNewHead = type->getExpireMeta(newHead);
            mNewHead->numItems = metaItem->numItems - 1;
            mNewHead->firstItemBucket = 1;
            *eb = ebMarkAsList(newHead);
            return 1; /* removed */
        }
        *eb = NULL;
        return 1; /* removed */
    }

    /* item is not the head of the list */
    ExpireMeta *metaHead = type->getExpireMeta(head);

    eItem iter = head;
    while (iter != NULL) {
        ExpireMeta *metaIter = type->getExpireMeta(iter);
        if (metaIter->next == item) {
            metaIter->next = metaItem->next;
            /* If deleted item is the last in the list, then update new last item */
            if (metaItem->next == NULL) {
                metaIter->lastInSegment = 1;
                metaIter->lastItemBucket = 1;
            }
            metaHead->numItems -= 1;
            return 1; /* removed */
        }
        iter = metaIter->next;
    }
    return 0; /* not removed */
}

static void ebRaxExpire(ebuckets *eb, EbucketsType *type, ExpireInfo *info, eItem *updateList) {
    rax *rax = ebGetRaxPtr(*eb);
    raxIterator iter;

    raxStart(&iter, rax);

    uint64_t nowKey = EB_BUCKET_KEY(info->now, type);
    uint64_t itemsExpiredBefore = info->itemsExpired;

    while (1) {
        raxSeek(&iter,"^",NULL,0);
        if (!raxNext(&iter)) break;

        uint64_t bucketKey = raxKey2BucketKey(iter.key, type);

        FirstSegHdr *firstSegHdr = iter.data;

        /* We need to take into consideration bucketPrecision. The value of
         * "info->now" will be adjusted to lookup only for all buckets with assigned
         * keys that are older than 1<<bucketPrecision msec ago. That is, it
         * is needed to visit only the buckets with keys that are "<" than:
         * EB_BUCKET_KEY(info->now, type). */
        if (bucketKey >= nowKey) {
            /* Take care to update next expire time based on next segment to expire */
            info->nextExpireTime = ebGetMetaExpTime(
                    type->getExpireMeta(firstSegHdr->head));
            break;
        }

        /* If not managed to remove entire bucket then return */
        if (ebSegExpire(firstSegHdr, type, info, updateList) == 0)
            break;

        raxRemove(iter.rt, iter.key, type->ebp.keySize, NULL);
    }

    raxStop(&iter);
    *ebRaxNumItems(rax) -= info->itemsExpired - itemsExpiredBefore;

    if(raxEOF(&iter) && (*updateList == 0)) {
        raxFree(rax);
        *eb = NULL;
    }
}

static void ebListExpire(ebuckets *eb,
                         EbucketsType *type,
                         ExpireInfo *info,
                         eItem *updateList)
{
    uint32_t expired = 0;
    eItem item = ebGetListPtr(type, *eb);
    ExpireMeta *metaItem = type->getExpireMeta(item);
    uint32_t numItems = metaItem->numItems; /* first item must exists */

    while (item != NULL) {
        metaItem = type->getExpireMeta(item);
        uint64_t itemExpTime = ebGetMetaExpTime(metaItem);

        /* Items are arranged in ascending expire-time order in a list. Stops list
         * active expiration when an item's expiration time is greater than `now`. */
////////        if (itemExpTime > info->now)
        if (itemExpTime >= info->now)
            break;

        if (info->itemsExpired == info->maxToExpire)
            break;

        /* keep aside `next` before removing `iter` by onExpireItem */
        eItem *next = metaItem->next;
        int storedInTmp = metaItem->storedIn;
        metaItem->storedIn = EB_STORED_IN_TRASH;
        ExpireAction act = info->onExpireItem(item, info->ctx);

        /* if (act == ACT_REMOVE_EXP_ITEM)
         *  then don't touch the item. Assume it got deleted */

        /* If indicated to stop then break (cb didn't delete the item) */
        if (act == ACT_STOP_ACTIVE_EXP) {
            metaItem->storedIn = storedInTmp;
            break;
        }

        /* If indicated to re-insert the item, then chain it to updateList.
         * it will be ebAdd() back to ebuckets at the end of ebExpire() */
        if (act == ACT_UPDATE_EXP_ITEM) {
            metaItem->next = *updateList;
            *updateList = item;
        }

        ++expired;
        ++(info->itemsExpired);
        item = next;
    }

    if (expired == numItems) {
        *eb = NULL;
        info->nextExpireTime = EB_EXPIRE_TIME_INVALID;
        return;
    }

    metaItem->numItems = numItems - expired;
    metaItem->firstItemBucket = 1;
    info->nextExpireTime = ebGetMetaExpTime(metaItem);
    *eb = ebMarkAsList(item);
}

/* Validate the general structure of the list */
static void ebValidateList(eItem head, EbucketsType *type) {
    if (head == NULL)
        return;

    ExpireMeta *mHead = type->getExpireMeta(head);
    eItem iter = head;
    ExpireMeta *mIter = type->getExpireMeta(iter), *mIterPrev = NULL;

    for (int i = 0; i < mHead->numItems ; ++i) {
        mIter = type->getExpireMeta(iter);
        if (i == 0) {
            /* first item */
            assert(mIter->numItems > 0 && mIter->numItems <= EB_LIST_MAX_ITEMS);
            assert(mIter->firstItemBucket == 1);
        } else  {
            /* Verify that expire time of previous item is smaller or equal */
            assert(ebGetMetaExpTime(mIterPrev) <= ebGetMetaExpTime(mIter));
            assert(mIter->numItems == 0);
            assert(mIter->firstItemBucket == 0);
        }

        if (i == (mHead->numItems - 1)) {
            /* last item */
            assert(mIter->lastInSegment == 1);
            assert(mIter->lastItemBucket == 1);
            assert(mIter->next == NULL);
        } else {
            assert(mIter->lastInSegment == 0);
            assert(mIter->lastItemBucket == 0);
            assert(mIter->next != NULL);
            mIterPrev = mIter;
            iter = mIter->next;
        }
    }
}

/*** Static functions of ebuckets / rax ***/

static uint64_t *ebRaxNumItems(rax *rax) {
    return (uint64_t*) rax->metadata;
}

/* Allocate a single segment with a single item */
static void ebNewBucket(EbucketsType *type, EBucketNew *newBucket, eItem item, uint64_t key) {
    ExpireMeta *mItem = type->getExpireMeta(item);

    newBucket->segment.head = item;
    newBucket->segment.totalItems = 1;
    newBucket->segment.numSegs = 1;
    newBucket->mLast = type->getExpireMeta(item);
    newBucket->ebKey = key;
    mItem->numItems = 1;
    mItem->firstItemBucket = 1;
    mItem->lastInSegment = 1;
    mItem->lastItemBucket = 1;
    mItem->next = &newBucket->segment;
}

/*
 * ebBucketPrint - Prints all the segments in the bucket and time expiration
 * of each item in the following fashion:
 *
 *      Bucket(tot=0008,sgs=0001) :    [11, 21, 26, 27, 29, 49, 59, 62]
 *      Bucket(tot=0007,sgs=0001) :    [67, 86, 90, 92, 115, 123, 126]
 *      Bucket(tot=0005,sgs=0001) :    [130, 135, 135, 136, 140]
 *      Bucket(tot=0009,sgs=0002) :    [182]
 *                                     [162, 163, 167, 168, 172, 177, 183, 186]
 *      Bucket(tot=0001,sgs=0001) :    [193]
 */
static int ebBucketPrint(uint64_t bucketKey, EbucketsType *type, FirstSegHdr *firstSeg) {
    eItem iter;
    ExpireMeta *mIter, *mHead;
    static int PRINT_EXPIRE_META_FLAGS=0;

    iter = firstSeg->head;
    mHead = type->getExpireMeta(iter);

    printf("Bucket(key=0x%06" PRIx64 ",tot=%04d,sgs=%04d) :", bucketKey, firstSeg->totalItems, firstSeg->numSegs);
    while (1) {
        mIter = type->getExpireMeta(iter);  /* not really needed. Just to hash the compiler */
        printf("    [");
        for (int i = 0; i < mHead->numItems ; ++i) {
            mIter = type->getExpireMeta(iter);
            uint64_t expireTime = ebGetMetaExpTime(mIter);

            if (i == 0 && PRINT_EXPIRE_META_FLAGS)
                printf("0x%" PRIx64 "<n=%d,f=%d,ls=%d,lb=%d>, ",
                       expireTime, mIter->numItems, mIter->firstItemBucket,
                       mIter->lastInSegment, mIter->lastItemBucket);
            else if (i == (mHead->numItems - 1) && PRINT_EXPIRE_META_FLAGS) {
                printf("0x%" PRIx64 "<n=%d,f=%d,ls=%d,lb=%d>",
                       expireTime, mIter->numItems, mIter->firstItemBucket,
                       mIter->lastInSegment, mIter->lastItemBucket);
            } else
                printf("0x%" PRIx64 "%s", expireTime, (i == mHead->numItems - 1) ? "" : ", ");

            iter = mIter->next;
        }

        if (mIter->lastItemBucket) {
            printf("]\n");
            break;
        }
        printf("]\n                           ");
        iter = ((NextSegHdr *) mIter->next)->head;
        mHead = type->getExpireMeta(iter);

    }
    return 0;
}

/* Add another eItem to bucket. If needed return 'newBucket' for insertion in rax tree.
 *
 * 1) If the bucket is based on a single, not full segment, then add the item to the segment.
 * 2) If a single, full segment, then try to split it and then add the item.
 * 3) If failed to split, then all items in the bucket have the same bucket-key.
 *    - If the new item has the same bucket-key, then extend the segment to
 *      be an extended-segment, if not already, and add the item to it.
 *    - If the new item has a different bucket-key, then allocate a new bucket
 *      for it.
 */
static int ebAddToBucket(EbucketsType *type,
                         FirstSegHdr *firstSegBkt,
                         eItem item,
                         EBucketNew *newBucket,
                         uint64_t *updateBucketKey)
{
    newBucket->segment.head = NULL; /* no new bucket as default */

    if (firstSegBkt->numSegs == 1) {
        /* If bucket is a single, not full segment, then add the item to the segment */
        if (firstSegBkt->totalItems < EB_SEG_MAX_ITEMS)
            return ebSegAddAvail(type, firstSegBkt, item);

        /* If bucket is a single, full segment, and segment split succeeded */
        if (ebTrySegSplit(type, firstSegBkt, newBucket) == 1) {
            /* The split got failed only because all items in the segment have the
             * same bucket-key */
            ExpireMeta *mItem = type->getExpireMeta(item);

            /* Check which of the two segments the new item should be added to. Note that
             * after the split, bucket-key of `newBucket` is bigger than bucket-key of
             * `firstSegBkt`. That is `firstSegBkt` preserves its bucket-key value
             * (and its location in rax tree) before the split */
            if (EB_BUCKET_KEY(ebGetMetaExpTime(type->getExpireMeta(item)), type) < newBucket->ebKey) {
                return ebSegAddAvail(type, firstSegBkt, item);
            } else {
                /* Add the `item` to the new bucket */
                ebSegAddAvail(type, &(newBucket->segment), item);

                /* if new item is now last item in the segment, then update lastItemBucket */
                if (mItem->lastItemBucket)
                    newBucket->mLast = mItem;
                return 0;
            }
        }
    }

    /* If reached here, then either:
     * (1) a bucket with multiple segments
     * (2) Or, a single, full segment which failed to split.
     *
     * Either way, all items in the bucket have the same bucket-key value. Thus:
     * (A) If 'item' has the same bucket-key as the ones in this bucket, then add it as well
     * (B) Else, allocate a new bucket for it.
     */

    ExpireMeta *mHead = type->getExpireMeta(firstSegBkt->head);
    ExpireMeta *mItem = type->getExpireMeta(item);

    uint64_t bucketKey = EB_BUCKET_KEY(ebGetMetaExpTime(mHead), type); /* same for all items in the segment */
    uint64_t itemKey = EB_BUCKET_KEY(ebGetMetaExpTime(mItem), type);

    if (bucketKey == itemKey) {
        /* New item has the same bucket-key as the ones in this bucket, Add it as well */
        if (mHead->numItems < EB_SEG_MAX_ITEMS) {
            /* Add item to the head of the segment (Don't sort. All with same expiry) */
            mItem->next = firstSegBkt->head;
            mItem->firstItemBucket = 1;
            mItem->numItems = mHead->numItems + 1;
            /* Modify previous head to be the second item */
            mHead->firstItemBucket = 0;
            mHead->numItems = 0;
            /* Update firstSegHdr */
            firstSegBkt->totalItems++;
            firstSegBkt->head = item;
            return 0;
        }
        else  {
            /* If a regular segment becomes extended-segment, then update the
             * bucket-key to be aligned with the expiration-time of the items
             * it contains */
            if (firstSegBkt->numSegs == 1)
                *updateBucketKey = bucketKey;

            return ebSegAddExtended(type, firstSegBkt, item); /* Add item in a new segment */
        }
    } else {
        /* If the item cannot be added to the visited (extended-segment) bucket
         * because it has a key not equal to bucket-key, then need to allocate a new
         * bucket for the item. If the key of the item is below the bucket-key of
         * the visited bucket, then the new item will be added to a new segment
         * before it and the visited bucket key will be updated to accurately
         * reflect the bucket-key of the (extended-segment) bucket */
        if (bucketKey > itemKey)
            *updateBucketKey = bucketKey;

        ebNewBucket(type, newBucket, item, EB_BUCKET_KEY(ebGetMetaExpTime(mItem), type));
        return 0;
    }
}

/*
 * Remove item from rax
 *
 * Return 1 if removed. Otherwise, return 0
 *
 * Note: The function is optimized to remove items locally from segments without
 *       traversing rax tree or stepping long extended-segments. Therefore, it is
 *       assumed that the item is present in the bucket without verification.
 *
 * TODO: Written straightforward. Should be optimized to merge small segments.
 */
static int ebRemoveFromRax(ebuckets *eb, EbucketsType *type, eItem item) {
    ExpireMeta *mItem = type->getExpireMeta(item);
    rax *rax = ebGetRaxPtr(*eb);

    /* if item is the only one left in a single-segment bucket, then delete bucket */
    if (unlikely(mItem->firstItemBucket && mItem->lastItemBucket)) {
        raxIterator ri;
        raxStart(&ri, rax);
        unsigned char raxKey[6 /* max keySize */];
        bucketKey2RaxKey(EB_BUCKET_KEY(ebGetMetaExpTime(mItem), type), raxKey, type);
        raxSeek(&ri, "<=", raxKey, type->ebp.keySize);

        if (raxNext(&ri) == 0)
            return 0; /* not removed */

        FirstSegHdr *segHdr = ri.data;

        if (segHdr->head != item)
            return 0; /* not removed */

        zfree(segHdr);
        raxRemove(ri.rt, ri.key, type->ebp.keySize, NULL);
        raxStop(&ri);

        /* If last bucket in rax, then delete the rax */
        if (rax->numele == 0) {
            raxFree(rax);
            *eb = NULL;
            return 1; /* removed */
        }
    } else if (mItem->numItems == 1) {
        /* If the `item` is the only one in its segment, there must be additional
         * items and segments in this bucket. If there weren't, the item would
         * have been removed by the previous condition. */

        if (mItem->firstItemBucket) {
            /* If the first item/segment in extended-segments, then
             * - Remove current segment (with single item) and promote next-segment to be first.
             * - Update first item of next-segment to be firstItemBucket
             * - Update `prevSeg` next-of-next segment to point new header of next-segment
             * - Update FirstSegHdr to totalItems-1, numSegs-1 */
            NextSegHdr *nextHdr = mItem->next;
            FirstSegHdr *firstHdr = (FirstSegHdr *) nextHdr->prevSeg;
            firstHdr->head = nextHdr->head;
            firstHdr->totalItems--;
            firstHdr->numSegs--;
            zfree(nextHdr);
            eItem *iter = firstHdr->head;
            ExpireMeta *mIter = type->getExpireMeta(iter);
            mIter->firstItemBucket = 1;
            while (mIter->lastInSegment == 0) {
                iter = mIter->next;
                mIter = type->getExpireMeta(iter);
            }
            if (mIter->lastItemBucket)
                mIter->next = firstHdr;
            else
                ((NextSegHdr *) mIter->next)->prevSeg = (CommonSegHdr *) firstHdr;

        } else if (mItem->lastItemBucket) {
            /* If last item/segment in bucket, then
             * - promote previous segment to be last segment
             * - Update FirstSegHdr to totalItems-1, numSegs-1 */
            NextSegHdr *currHdr = mItem->next;
            CommonSegHdr *prevHdr = currHdr->prevSeg;
            eItem iter = prevHdr->head;
            ExpireMeta *mIter = type->getExpireMeta(iter);
            while (mIter->lastInSegment == 0) {
                iter = mIter->next;
                mIter = type->getExpireMeta(iter);
            }
            currHdr->firstSeg->totalItems--;
            currHdr->firstSeg->numSegs--;
            mIter->next = prevHdr;
            mIter->lastItemBucket = 1;
            zfree(currHdr);

        } else {
            /* item/segment is not the first or last item/segment.
             * - Update previous segment to point next segment.
             * - Update `prevSeg` of next segment
             * - Update FirstSegHdr to totalItems-1, numSegs-1 */
            NextSegHdr *nextHdr = mItem->next;
            NextSegHdr *currHdr = (NextSegHdr *) nextHdr->prevSeg;
            CommonSegHdr *prevHdr = currHdr->prevSeg;

            ExpireMeta *mIter = type->getExpireMeta(prevHdr->head);
            while (mIter->lastInSegment == 0)
                mIter = type->getExpireMeta(mIter->next);

            mIter->next = nextHdr;
            nextHdr->prevSeg = prevHdr;
            nextHdr->firstSeg->totalItems--;
            nextHdr->firstSeg->numSegs--;
            zfree(currHdr);

        }
    } else {
        /* At least 2 items in current segment */
        if (mItem->numItems) {
            /* If item is first item in segment (Must be numItems>1), then
             * - Find segment header and update to point next item.
             * - Let next inherit 'item' flags {firstItemBucket, numItems-1}
             * - Update FirstSegHdr to totalItems-1 */
            ExpireMeta *mIter = mItem;
            CommonSegHdr *currHdr;
            while (mIter->lastInSegment == 0)
                mIter = type->getExpireMeta(mIter->next);
            if (mIter->lastItemBucket)
                currHdr = (CommonSegHdr *) mIter->next;
            else
                currHdr = (CommonSegHdr *) ((NextSegHdr *) mIter->next)->prevSeg;

            if (mItem->firstItemBucket)
                ((FirstSegHdr *) currHdr)->totalItems--;
            else
                ((NextSegHdr *) currHdr)->firstSeg->totalItems--;

            eItem *newHead = mItem->next;
            ExpireMeta *mNewHead = type->getExpireMeta(newHead);
            mNewHead->firstItemBucket = mItem->firstItemBucket;
            mNewHead->numItems = mItem->numItems - 1;
            currHdr->head = newHead;

        } else if (mItem->lastInSegment) {
            /* If item is last in segment, then
             * - find previous item and let it inherit (next, lastInSegment, lastItemBucket)
             * - Find and update segment header to numItems-1
             * - Update FirstSegHdr to totalItems-1 */
            CommonSegHdr *currHdr;
            if (mItem->lastItemBucket)
                currHdr = (CommonSegHdr *) mItem->next;
            else
                currHdr = (CommonSegHdr *) ((NextSegHdr *) mItem->next)->prevSeg;

            ExpireMeta *mHead = type->getExpireMeta(currHdr->head);
            mHead->numItems--;
            ExpireMeta *mIter = mHead;
            while (mIter->next != item)
                mIter = type->getExpireMeta(mIter->next);

            mIter->next = mItem->next;
            mIter->lastInSegment = mItem->lastInSegment;
            mIter->lastItemBucket = mItem->lastItemBucket;

            if (mHead->firstItemBucket)
                ((FirstSegHdr *) currHdr)->totalItems--;
            else
                ((NextSegHdr *) currHdr)->firstSeg->totalItems--;

        } else {
            /* - Item is in the middle of segment. Find previous item and update to point next.
             * - Find and Update segment header to numItems-1
             * - Update FirstSegHdr to totalItems-1 */
            ExpireMeta *mIter = mItem;
            CommonSegHdr *currHdr;
            while (mIter->lastInSegment == 0)
                mIter = type->getExpireMeta(mIter->next);
            if (mIter->lastItemBucket)
                currHdr = (CommonSegHdr *) mIter->next;
            else
                currHdr = (CommonSegHdr *) ((NextSegHdr *) mIter->next)->prevSeg;

            ExpireMeta *mHead = type->getExpireMeta(currHdr->head);
            mHead->numItems--;
            mIter = mHead;
            while (mIter->next != item)
                mIter = type->getExpireMeta(mIter->next);

            mIter->next = mItem->next;
            mIter->lastInSegment = mItem->lastInSegment;
            mIter->lastItemBucket = mItem->lastItemBucket;

            if (mHead->firstItemBucket)
                ((FirstSegHdr *) currHdr)->totalItems--;
            else
                ((NextSegHdr *) currHdr)->firstSeg->totalItems--;
        }
    }
    *ebRaxNumItems(rax) -= 1;
    return 1; /* removed */
}

static int ebRemoveFromStack(ebuckets *eb, EbucketsType *type, eItem item) {
    int res;
    ebStack *stack = (ebStack *)*eb;
    ExpireMeta *metaItem = type->getExpireMeta(item);
    if (metaItem->storedIn == EB_STORED_IN_EB) {
        EB_STACK_EXEC_L1(type, res = ebRemove(&stack->l1, type, item));
    } if (metaItem->storedIn == EB_STORED_IN_EB_L2) {
        EB_STACK_EXEC_L2(type, res = ebRemove(&stack->l2, type, item));
    } else if (metaItem->storedIn == EB_STORED_IN_EB_L3) {
        ExpireMetaL3 *m = (ExpireMetaL3 *) metaItem;
        uint64_t idx = m->expireIndexLo | ((uint64_t) m->expireIndexHi << 32);
        debugAssert(stack->l3 && idx < stack->l3->items && stack->l3->vec[idx] == item);
        if (idx < stack->l3->items - 1) {
            /* Move last item in the vector to the place of the removed item */
            eItem another = stack->l3->vec[stack->l3->items - 1];
            stack->l3->vec[idx] = another;
            /* Update expireMeta of the moved item */
            ebSetMetaL3Index((ExpireMetaL3 *) type->getExpireMeta(another), idx);
        }

        /* Free or shrink L3 vector if needed */
        if (--stack->l3->items == 0) {
            zfree(stack->l3);
            stack->l3 = NULL;
        } else if (stack->l3->items < stack->l3->vecSize / 4) {
            uint64_t newSize = stack->l3->vecSize / 2;
            stack->l3 = zrealloc(stack->l3, EB_STACK_L3_SIZEOF(newSize));
            stack->l3->vecSize = newSize;
        }
        res = 1;
    }
    
    /* Free stack if L1, L2 and L3 are empty (then ebuckets must be NULL) */
    if (--stack->items == 0) {
        zfree(stack);
        *eb = NULL;
    }

    return res;
}




/* Add another eItem to ebStack and based on commandTimeSnapshot() determines
 * if it should be added to L1, L2. */
int ebAddToStack(ebuckets *eb, EbucketsType *type, eItem item, uint64_t expireTime) {
    ebStack *stack;
    int result = 0;
    /* Initialize ebStack if it doesn't exist */
    if (unlikely(ebIsEmpty(*eb))) {
        stack = zmalloc(sizeof(ebStack));
        stack->items = 0;
        stack->l1 = ebCreate();
        stack->l2 = ebCreate();
        stack->l3 = NULL;
        *eb = stack;
    }
    stack = (ebStack *)*eb;

    ExpireMeta *itemMeta = type->getExpireMeta(item);
    
    debugAssert(itemMeta->storedIn == EB_STORED_IN_TRASH);

    /* If ebStack level 3 (infinity) */
    if (unlikely(expireTime > EB_EXPIRE_TIME_MAX)) {
        /* init or expand the l3 if needed */
        if (stack->l3 == NULL) {
            stack->l3 = zmalloc(EB_STACK_L3_SIZEOF(2));
            stack->l3->items = 0;
            stack->l3->vecSize = 2;
        } else if (stack->l3->items == stack->l3->vecSize) {
            /* Expand vector (power of 2) */
            uint64_t newSize = stack->l3->vecSize * 2;
            stack->l3 = zrealloc(stack->l3, EB_STACK_L3_SIZEOF(newSize));
            stack->l3->vecSize = newSize;
        }

        /* Store expireTime & L3 index of new item in its expireMeta */
        ebSetMetaL3Index(((ExpireMetaL3 *) itemMeta) , stack->l3->items);
        ebSetMetaL3ExpireTime(((ExpireMetaL3 *) itemMeta), expireTime);

        itemMeta->storedIn = EB_STORED_IN_EB_L3;

        /* Add item to infinity vector */
        stack->l3->vec[stack->l3->items++] = item;
        stack->items++;
        return 0;
    }

    /* Determine which level to add the item to based on bucket key precision */
    uint64_t now = commandTimeSnapshot();
    uint64_t nowL2BucketKey = now >> ebpStackL2.precision;
    uint64_t itemL2BucketKey = expireTime >> ebpStackL2.precision;

    /* If ebStack level 1 */
    if (itemL2BucketKey <= (nowL2BucketKey + 1)) {
        EB_STACK_EXEC_L1(type, result = ebAdd(&stack->l1, type, item, expireTime));
        if (result == 0)
            stack->items++;
        return result;
    } else {
        /* If ebStack level 2 */
        EB_STACK_EXEC_L2(type, result = ebAdd(&stack->l2, type, item, expireTime));
        if (result == 0) {
            itemMeta->storedIn = EB_STORED_IN_EB_L2; /* override */
            stack->items++;
        }
        return result;
    }
}

typedef struct ebCascadeCtx {
    ebuckets *l1;
    EbucketsType l1Type;
} ebCascadeCtx;

/* Callback for ebExpire() to cascade items from L2 to L1 */
static ExpireAction onCascadeItem(eItem item, void *ctx) {
    ebCascadeCtx *cc = (ebCascadeCtx *) ctx;

    ExpireMeta *itemMeta = cc->l1Type.getExpireMeta(item);
    uint64_t expireTime = ebGetMetaExpTime(itemMeta);

    /* If adding to L1 failed, then stop */
    if (ebAdd(cc->l1, &cc->l1Type, item, expireTime) != 0)
        return ACT_STOP_ACTIVE_EXP;

    /* Update storage location from L2 to L1 */
    itemMeta->storedIn = EB_STORED_IN_EB;

    return ACT_REMOVE_EXP_ITEM;
}

/**
 * Cascades items from Level 2 (L2) to Level 1 (L1)
 *
 * This function should be called periodically to promote items that are approaching
 * expiration in L2 and require higher precision tracking in L1.
 *
 * The cascade threshold is calculated as:
 *     now + (2 << ebStackL2Precision)
 *
 * This defines a time boundary starting from the current time (`now`), extended by
 * the duration of two L2 buckets. Any items in L2 with expiration times less than or
 * equal to this threshold are considered eligible for cascading into L1.
 *
 * With hardcoded ebStackL2Precision = 21 (i.e., 2^21 ms = ~34.9 minutes):
 * - L2 buckets that are less or equal to `now + (1 << ebStackL2Precision)`(=~34.9min) 
 *   will be cascaded to L1 with corresponding TTLs ranges ~35min-70min
 *
 * This staging provides ample time to cascade all items in such segments before expiration.
 *
 * @param eb - Pointer to the ebuckets handler (must be an ebStack)
 * @param type - Pointer to the EbucketsType structure (must describe an ebStack)
 * @param now - Current logical time
 * @param maxCascade - Maximum number of items to cascade in this invocation
 *
 * @return Number of items cascaded from L2 to L1
 */
uint64_t ebStackCascade(ebuckets *eb, EbucketsType *type, uint64_t now, uint64_t maxCascade)
{
    /* If empty, not ebStack, or L2 is empty, then nothing to cascade */
    if (ebIsEmpty(*eb) || !type->isEbStack || ebIsEmpty(((ebStack *)*eb)->l2))
        return 0;

    ebStack *stack = (ebStack *)*eb;

    /* Use iterator to traverse L2 and find items to cascade */
    ebCascadeCtx ctx;
    ctx.l1 = &stack->l1;
    ctx.l1Type = *type;
    ctx.l1Type.isEbStack = 0; /* L1 is not hierarchical */

    /* Calculate cascade threshold */
    uint64_t cascadeThreshold = now + (2 << ebpStackL2.precision);

    /* Reuse ebExpire() to cascade items from L2 to L1 */
    ExpireInfo info = {
        .maxToExpire = maxCascade,
        .onExpireItem = onCascadeItem,
        .ctx = &ctx,
        .now = cascadeThreshold,
        .itemsExpired = 0
    };

    EB_STACK_EXEC_L2(type, ebExpire(&stack->l2, type, &info));
    return info.itemsExpired; /* Number of items cascaded */
}

int ebAddToRax(ebuckets *eb, EbucketsType *type, eItem item, uint64_t bucketKeyItem) {
    EBucketNew newBucket; /* ebAddToBucket takes care to update newBucket.segment.head */
    raxIterator iter;
    unsigned char raxKey[6 /* max keySize */];
    bucketKey2RaxKey(bucketKeyItem, raxKey, type);
    rax *rax = ebGetRaxPtr(*eb);
    raxStart(&iter,rax);
    raxSeek(&iter, "<=", raxKey, type->ebp.keySize);
    *ebRaxNumItems(rax) += 1;
    /* If expireTime of the item is below the bucket-key of first bucket in rax,
     * then need to add it as a new bucket at the beginning of the rax. */
    if(unlikely(raxNext(&iter) == 0)) {
        FirstSegHdr *firstSegHdr = zmalloc(sizeof(FirstSegHdr));
        firstSegHdr->head = item;
        firstSegHdr->totalItems = 1;
        firstSegHdr->numSegs = 1;

        /* update last item to point on the segment header */
        ExpireMeta *metaItem = type->getExpireMeta(item);
        metaItem->lastItemBucket = 1;
        metaItem->lastInSegment = 1;
        metaItem->firstItemBucket = 1;
        metaItem->numItems = 1;
        metaItem->next = firstSegHdr;
        raxInsert(rax, raxKey, type->ebp.keySize, firstSegHdr, NULL);
        raxStop(&iter);
        return 0;
    }

    /* Add the new item into the first segment of the bucket that we found */
    uint64_t updateBucketKey = 0;
    ebAddToBucket(type, iter.data, item, &newBucket, &updateBucketKey);

    /* If following the addition need to `updateBucketKey` of `foundBucket` in rax */
    if(unlikely(updateBucketKey && updateBucketKey != raxKey2BucketKey(iter.key, type))) {
        raxRemove(iter.rt, iter.key, type->ebp.keySize, NULL);
        bucketKey2RaxKey(updateBucketKey, raxKey, type);
        raxInsert(iter.rt, raxKey, type->ebp.keySize, iter.data, NULL);
    }

    /* If ebAddToBucket() returned a new bucket, then add the bucket to rax.
     *
     * This might happen when trying to add another item to a bucket that is:
     * 1. A single, full segment. Will result in a bucket (segment) split.
     * 2. Extended segment with a different bucket-key than the new item.
     *    Will result in a new bucket (of size 1) for the new item.
     */
    if (newBucket.segment.head != NULL) {
        /* Allocate segment header for the new bucket */
        FirstSegHdr *newSeg = zmalloc(sizeof(FirstSegHdr));
        /* Move the segment from 'newBucket' to allocated segment header */
        *newSeg = newBucket.segment;
        /* Update 'next' of last item in segment to point to 'FirstSegHdr` */
        newBucket.mLast->next = newSeg;
        /* Insert the new bucket to rax */
        bucketKey2RaxKey(newBucket.ebKey, raxKey, type);
        raxInsert(iter.rt, raxKey, type->ebp.keySize, newSeg, NULL);
    }

    raxStop(&iter);
    return 0;
}

/* Validate the general structure of the buckets in rax */
static void ebValidateRax(rax *rax, EbucketsType *type) {
    uint64_t numItemsTotal = 0;
    raxIterator raxIter;
    raxStart(&raxIter, rax);
    raxSeek(&raxIter, "^", NULL, 0);
    while (raxNext(&raxIter)) {
        int expectFirstItemBucket = 1;
        FirstSegHdr *firstSegHdr = raxIter.data;
        eItem iter;
        ExpireMeta *mIter, *mHead;
        iter = firstSegHdr->head;
        mHead = type->getExpireMeta(iter);
        uint64_t numItemsBucket = 0, countSegments = 0;

        int extendedSeg = (firstSegHdr->numSegs > 1) ? 1 : 0;
        void *segHdr = firstSegHdr;

        mIter = type->getExpireMeta(iter);
        while (1) {
            uint64_t curBktKey, prevBktKey;
            for (int i = 0; i < mHead->numItems ; ++i) {
                assert(iter != NULL);
                mIter = type->getExpireMeta(iter);
                curBktKey = EB_BUCKET_KEY(ebGetMetaExpTime(mIter), type);

                if (i == 0) {
                    assert(mIter->numItems > 0 && mIter->numItems <= EB_SEG_MAX_ITEMS);
                    assert(mIter->firstItemBucket == expectFirstItemBucket);
                    expectFirstItemBucket = 0;
                    prevBktKey = curBktKey;
                } else  {
                    assert( (extendedSeg && prevBktKey == curBktKey) ||
                            (!extendedSeg && prevBktKey <= curBktKey) );
                    assert(mIter->numItems == 0);
                    assert(mIter->firstItemBucket == 0);
                    prevBktKey = curBktKey;
                }

                if (i == mHead->numItems - 1)
                    assert(mIter->lastInSegment == 1);
                else
                    assert(mIter->lastInSegment == 0);

                iter = mIter->next;
            }

            numItemsBucket += mHead->numItems;
            countSegments += 1;

            if (mIter->lastItemBucket)
                break;

            NextSegHdr *nextSegHdr = mIter->next;
            assert(nextSegHdr->firstSeg == firstSegHdr);
            assert(nextSegHdr->prevSeg == segHdr);
            iter = nextSegHdr->head;
            mHead = type->getExpireMeta(iter);
            segHdr = nextSegHdr;
        }
        /* Verify next of last item, `totalItems` and `numSegs` in iterated bucket */
        assert(mIter->next == segHdr);
        assert(numItemsBucket == firstSegHdr->totalItems);
        assert(countSegments == firstSegHdr->numSegs);
        numItemsTotal += numItemsBucket;
    }
    raxStop(&raxIter);
    assert(numItemsTotal == *ebRaxNumItems(rax));
}

struct deleteCbCtx { EbucketsType *type; void *userCtx; };
void ebRaxDeleteCb(void *item, void *context) {
    struct deleteCbCtx *ctx = context;
    FirstSegHdr *firstSegHdr = item;
    eItem itemIter = firstSegHdr->head;
    uint32_t numSegs = firstSegHdr->numSegs;
    void *nextSegHdr = firstSegHdr;

    for (uint32_t seg=0 ; seg < numSegs ; seg++) {
        zfree(nextSegHdr);

        ExpireMeta *mIter = ctx->type->getExpireMeta(itemIter);
        uint32_t numItemsInSeg = mIter->numItems;

        for (uint32_t i = 0; i < numItemsInSeg ; ++i) {
            mIter = ctx->type->getExpireMeta(itemIter);
            eItem toDelete = itemIter;
            mIter->storedIn = EB_STORED_IN_TRASH;
            itemIter = mIter->next;
            if (ctx->type->onDeleteItem) ctx->type->onDeleteItem(toDelete, &ctx->userCtx);
        }
        nextSegHdr = itemIter;

        if (seg + 1 < numSegs)
            itemIter = ((NextSegHdr *) nextSegHdr)->head;
    }

}

/*** API functions ***/

/**
 * Deletes all items from given ebucket, invoking optional item deletion callbacks.
 *
 * @param eb - The ebucket to be deleted.
 * @param type - Pointer to the EbucketsType structure defining the type of ebucket.
 * @param ctx - A context pointer that can be used in optional item deletion callbacks.
 */
void ebDestroy(ebuckets *eb, EbucketsType *type, void *ctx) {
    if (ebIsEmpty(*eb))
        return;

    if (type->isEbStack) {
        ebStack *stack = (ebStack *) *eb;
        EB_STACK_EXEC_L1(type, ebDestroy(&stack->l1, type, ctx));
        EB_STACK_EXEC_L2(type, ebDestroy(&stack->l2, type, ctx));
        if (stack->l3) {
            for (uint64_t i = 0; i < stack->l3->items; i++) {
                eItem toDelete = stack->l3->vec[i];
                ExpireMeta *metaToDelete = type->getExpireMeta(toDelete);
                metaToDelete->storedIn = EB_STORED_IN_TRASH;
                if (type->onDeleteItem) type->onDeleteItem(toDelete, ctx);
            }
            zfree(stack->l3);
        }
        zfree(stack);
    } else if (ebIsList(*eb)) {
        eItem head = ebGetListPtr(type, *eb);
        eItem *pItemNext = &head;
        while ( (*pItemNext) != NULL) {
            eItem toDelete = *pItemNext;
            ExpireMeta *metaToDelete = type->getExpireMeta(toDelete);
            *pItemNext = metaToDelete->next;
            metaToDelete->storedIn = EB_STORED_IN_TRASH;
            if (type->onDeleteItem) type->onDeleteItem(toDelete, ctx);
        }
    } else {
        struct deleteCbCtx deleteCtx = {type, ctx};
        raxFreeWithCbAndContext(ebGetRaxPtr(*eb), ebRaxDeleteCb, &deleteCtx);
    }

    *eb = NULL;
}

/**
 * Removes the specified item from the given ebucket, updating the ebuckets handler
 * accordingly. The function is optimized to remove items locally from segments
 * without traversing rax tree or stepping long extended-segments. Therefore,
 * it is assumed that the item is present in the bucket without verification.
 *
 * @param eb   - Pointer to the ebuckets handler, which may get updated if the removal
 *               affects the structure.
 * @param type - Pointer to the EbucketsType structure defining the type of ebucket.
 * @param item - The eItem to be removed from the ebucket.
 *
 * @return 1 if the item was successfully removed; otherwise, return 0.
 */
int ebRemove(ebuckets *eb, EbucketsType *type, eItem item) {
    int res;
    if (ebIsEmpty(*eb))
        return 0; /* not removed */
        
    if (type->isEbStack)
        res = ebRemoveFromStack(eb, type, item);
    else if (ebIsList(*eb))
        res = ebRemoveFromList(eb, type, item);
    else  /* rax */
        res = ebRemoveFromRax(eb, type, item);

    /* if removed then mark as trash */
    if (res)
        type->getExpireMeta(item)->storedIn = EB_STORED_IN_TRASH;

    EB_VALIDATE_STRUCTURE(*eb, type);

    return res;
}

/**
 * Adds the specified item to the ebucket structure based on expiration time.
 * If the ebucket is a list or empty, it attempts to add the item to the list.
 * Otherwise, it adds the item to rax. If the list reaches its maximum size, it
 * is converted to rax. The ebuckets handler may be updated accordingly.
 *
 * @param eb - Pointer to the ebuckets handler, which may get updated
 * @param type - Pointer to the EbucketsType structure defining the type of ebucket.
 * @param item - The eItem to be added to the ebucket.
 * @param expireTime - The expiration time of the item.
 *
 * @return 0 (C_OK) if the item was successfully added;
 *         Otherwise, return -1 (C_ERR) on failure.
 */
int ebAdd(ebuckets *eb, EbucketsType *type, eItem item, uint64_t expireTime) {
    int res;
    if (type->isEbStack)
        return ebAddToStack(eb, type, item, expireTime);

    assert(expireTime <= EB_EXPIRE_TIME_MAX);

    /* Set expire-time and reset segment flags */
    ExpireMeta *itemMeta = type->getExpireMeta(item);
    ebSetMetaExpTime(itemMeta, expireTime);
    itemMeta->lastInSegment = 0;
    itemMeta->firstItemBucket = 0;
    itemMeta->lastItemBucket = 0;
    itemMeta->numItems = 0;
    itemMeta->storedIn = EB_STORED_IN_EB;

    if (ebIsList(*eb) || (ebIsEmpty(*eb))) {
        /* Try add item to list */
        if ( (res = ebAddToList(eb, type, item)) == 1) {
            /* Failed to add since list reached maximum size. Convert to rax */
            *eb = ebConvertListToRax(ebGetListPtr(type, *eb), type);
            res = ebAddToRax(eb, type, item, EB_BUCKET_KEY(expireTime, type));
        }
    } else {
        /* Add item to rax */
        res = ebAddToRax(eb, type, item, EB_BUCKET_KEY(expireTime, type));
    }

    EB_VALIDATE_STRUCTURE(*eb, type);

    return res;
}

/**
 * Performs expiration on the given ebucket, removing items that have expired.
 * Only buckets with keys < EB_BUCKET_KEY(info->now, type) are considered expired.
 * If all items in the data structure are expired, 'eb' will be set to NULL.
 *
 * @param eb - Pointer to the ebuckets handler, which may get updated
 * @param type - Pointer to the EbucketsType structure defining the type of ebucket.
 * @param info - Providing information about the expiration action.
 */
void ebExpire(ebuckets *eb, EbucketsType *type, ExpireInfo *info) {

    if (type->isEbStack) {
        /* Only items from L1 get expired */
        EB_STACK_EXEC_L1(type, ebExpire(&((ebStack *)*eb)->l1, type, info));
        return;
    }

    /* updateList - maintain a list of expired items that the callback `onExpireItem`
     * indicated to update their expiration time rather than removing them.
     * At the end of this function, the items will be `ebAdd()` back.
     *
     * Note, this list of items does not allocate any memory, but temporary reuses
     * the `next` pointer of the `ExpireMeta` structure of the expired items. */
    eItem updateList = NULL;

    /* reset info outputs */
    info->nextExpireTime = EB_EXPIRE_TIME_INVALID;
    info->itemsExpired = 0;

    /* if empty ebuckets */
    if (ebIsEmpty(*eb)) return;

    if (ebIsList(*eb)) {
        ebListExpire(eb, type, info, &updateList);
    } else {
        ebRaxExpire(eb, type, info, &updateList);
    }

    /* Add back items with updated expiration time */
    while (updateList) {
        ExpireMeta *mItem = type->getExpireMeta(updateList);
        eItem next = mItem->next;
        uint64_t expireAt = ebGetMetaExpTime(mItem);

        /* Update next minimum expire time if needed.
         * Condition is valid also if nextExpireTime is EB_EXPIRE_TIME_INVALID */
        if (expireAt < info->nextExpireTime)
            info->nextExpireTime = expireAt;

        ebAdd(eb, type, updateList, expireAt);
        updateList = next;
    }

    EB_VALIDATE_STRUCTURE(*eb, type);
}

/* Performs active expiration dry-run to evaluate number of expired items
 *
 * It is faster than actual active-expire because it iterates only over the
 * headers of the buckets until the first non-expired bucket, and no more than
 * EB_SEG_MAX_ITEMS items in the last bucket
 *
 * @param eb - The ebucket to be checked.
 * @param type - Pointer to the EbucketsType structure defining the type of ebucket.
 * @param now - The current time in milliseconds.
 */
uint64_t ebExpireDryRun(ebuckets eb, EbucketsType *type, uint64_t now) {
    if (ebIsEmpty(eb)) return 0;
    
    if (unlikely(type->isEbStack)) {
        /* Only items from L1 get expired */
        ebStack *stack = (ebStack *) eb;
        uint64_t res = 0;
        EB_STACK_EXEC_L1(type, res = ebExpireDryRun(stack->l1, type, now));
        return res;
    }

    uint64_t numExpired = 0;

    /* If list, then iterate and count expired ones */
    if (ebIsList(eb)) {
        ExpireMeta *mIter = type->getExpireMeta(ebGetListPtr(type, eb));
        while (1) {
            if (ebGetMetaExpTime(mIter) >= now)
                return numExpired;

            numExpired++;

            if (mIter->lastInSegment)
                return numExpired;

            mIter = type->getExpireMeta(mIter->next);
        }
    }

    /* Handle rax active-expire */
    rax *rax = ebGetRaxPtr(eb);
    raxIterator iter;
    raxStart(&iter, rax);
    uint64_t nowKey = EB_BUCKET_KEY(now, type);
    raxSeek(&iter,"^",NULL,0);
    assert(raxNext(&iter)); /* must be at least one bucket */
    FirstSegHdr *currBucket = iter.data;

    while (1) {
        /* if 'currBucket' is last bucket, then break */
        if(!raxNext(&iter)) break;
        FirstSegHdr *nextBucket = iter.data;

        /* if 'nextBucket' is not less than now then break */
        if (raxKey2BucketKey(iter.key, type) >= nowKey) break;

        /* nextBucket less than now. For sure all items in currBucket are expired */
        numExpired += currBucket->totalItems;
        currBucket = nextBucket;
    }
    raxStop(&iter);

    /* If single segment bucket, iterate over items and count expired ones */
    if (currBucket->numSegs == 1) {
        ExpireMeta *mIter = type->getExpireMeta(currBucket->head);
        while (1) {
            if (ebGetMetaExpTime(mIter) >= now)
                return numExpired;

            numExpired++;

            if (mIter->lastInSegment)
                return numExpired;

            mIter = type->getExpireMeta(mIter->next);
        }
    }

    /* Bucket key exactly reflect expiration time of all items (currBucket->numSegs > 1) */
    if (type->ebp.precision == 0) {
        if (ebGetMetaExpTime(type->getExpireMeta(currBucket->head)) >= now)
            return numExpired;
        else
            return numExpired + currBucket->totalItems;
    }

    /* Iterate extended-segment and count expired ones */

    /* Unreachable code, provided for completeness. Following operation is not
     * bound in time and this is the main reason why we set above
     * bucketPrecision to 0 and have early return on previous condition */

    ExpireMeta *mIter = type->getExpireMeta(currBucket->head);
    while(1) {
        if (ebGetMetaExpTime(mIter) < now)
            numExpired++;

        if (mIter->lastItemBucket)
            return numExpired;

        if (mIter->lastInSegment)
            mIter = type->getExpireMeta(((NextSegHdr *) mIter->next)->head);
        else
            mIter = type->getExpireMeta(mIter->next);
    }
}

/**
 * Retrieves the expiration time of the item with the nearest expiration
 *
 * @param eb - The ebucket to be checked (not ebStack)
 * @param type - Pointer to the EbucketsType structure defining the type of ebucket.
 *
 * @return The expiration time of the item with the nearest expiration time in
 *         the ebucket. If empty, return EB_EXPIRE_TIME_INVALID. If ebuckets is
 *         of type rax and minimal bucket is extended-segment, then it might not
 *         return accurate result up-to 1<<bucketPrecision-1 msec (we
 *         don't want to traverse the entire extended-segment since it might not
 *         bounded).
 */
uint64_t ebGetNextTimeToExpire(ebuckets eb, EbucketsType *type) {
    debugAssert(eb.isEbStack == 0); /* Not required for now */
    if (ebIsEmpty(eb))
        return EB_EXPIRE_TIME_INVALID;

    debugAssert(type->isEbStack == 0); /* Not required for now */

    if (ebIsList(eb))
        return ebGetMetaExpTime(type->getExpireMeta(ebGetListPtr(type, eb)));

    /* rax */
    uint64_t minExpire;
    rax *rax = ebGetRaxPtr(eb);
    raxIterator iter;
    raxStart(&iter, rax);
    raxSeek(&iter, "^", NULL, 0);
    raxNext(&iter); /* seek to the last bucket */
    FirstSegHdr *firstSegHdr = iter.data;
    if ((firstSegHdr->numSegs == 1) || (type->ebp.precision == 0)) {
        /* Single segment, or extended-segments that all have same expiration time.
         * return the first item with the nearest expiration time */
        minExpire = ebGetMetaExpTime(type->getExpireMeta(firstSegHdr->head));
    } else {

        /* If reached here, then it is because it is extended segment and buckets
         * are with lower precision than 1msec. In that case it is better not to
         * iterate extended-segments, which might be unbounded, and just return
         * worst possible expiration time in this bucket.
         *
         * The reason we return blindly worst case expiration time value in this
         * bucket is because the only usage of this function is to figure out
         * when is the next time active expiration should be performed, and it
         * is better to do it only after 1 or more items were expired and not the
         * other way around.
         */
        uint64_t expTime = ebGetMetaExpTime(type->getExpireMeta(firstSegHdr->head));
        unsigned int mask = (1<<type->ebp.precision) - 1;
        minExpire = (expTime+mask) & (~mask);
    }
    raxStop(&iter);
    return minExpire;
}

/* Returns number of items in ebStack level (Used only for testing) */
uint64_t ebStackItems(ebuckets eb, EbucketsType *type, int level) {
    debugAssert(type->isEbStack);
    uint64_t res = 0;
    if (ebIsEmpty(eb))
        return 0;
    ebStack *stack = (ebStack *) eb;
    if (level == 1)
        EB_STACK_EXEC_L1(type, res = ebGetTotalItems(stack->l1, type));
    else if (level == 2)
        EB_STACK_EXEC_L2(type, res = ebGetTotalItems(stack->l2, type));
    else if (level == 3)
        res = stack->l3 ? stack->l3->items : 0;
    else
        debugAssert(0);
    return res;
}

/**
 * Retrieves the total number of items in the ebucket.
 */
uint64_t ebGetTotalItems(ebuckets eb, EbucketsType *type) {
    if (ebIsEmpty(eb))
        return 0;
    if (type->isEbStack)
        return ((ebStack *) eb)->items;

    if (ebIsList(eb))
        return type->getExpireMeta(ebGetListPtr(type, eb))->numItems;

    /* else rax */ 
    return *ebRaxNumItems(ebGetRaxPtr(eb));
}

/* print expiration-time of items, ebuckets layout and some statistics */
void ebPrintItems(ebuckets eb, EbucketsType *type) {
    if (ebIsEmpty(eb))
        return;

    if (type->isEbStack) {
        ebStack *stack = (ebStack *) eb;
        printf("[EBSTACK-LEVEL1]:\n");
        EB_STACK_EXEC_L1(type, ebPrintItems(stack->l1, type));
        printf("[EBSTACK-LEVEL2]:\n");
        EB_STACK_EXEC_L2(type, ebPrintItems(stack->l2, type));
        printf("[EBSTACK-LEVEL3]:\n");
        if (stack->l3)
            printf("Infinity bucket: %" PRIu64 " items\n", stack->l3->items);
        return;
    }

    if (ebIsList(eb)) {
        /* mock rax segment */
        eItem head = ebGetListPtr(type, eb);
        ExpireMeta *metaHead = type->getExpireMeta(head);
        FirstSegHdr mockSeg = { head, metaHead->numItems, 1};
        ebBucketPrint(0, type, &mockSeg);
        return;
    }
    rax *rax = ebGetRaxPtr(eb);
    raxIterator iter;
    raxStart(&iter, rax);
    raxSeek(&iter, "^", NULL, 0);
    while (raxNext(&iter)) {
        FirstSegHdr *seg = iter.data;
        ebBucketPrint(raxKey2BucketKey(iter.key, type), type, seg);
    }
}

/* Validate the general structure of ebuckets. Calls assert(0) on error. */
void ebValidate(ebuckets eb, EbucketsType *type) {
    if (ebIsEmpty(eb))
        return;

    if (type->isEbStack) {
        ebStack *stack = (ebStack *) eb;
        EB_STACK_EXEC_L1(type, ebValidate(stack->l1, type));
        EB_STACK_EXEC_L2(type, ebValidate(stack->l2, type));
    } else if (ebIsList(eb)) {
        ebValidateList(ebGetListPtr(type, eb), type);
    } else {
        ebValidateRax(ebGetRaxPtr(eb), type);
    }
}

/* Defrag callback for radix tree iterator, called for each node,
 * used in order to defrag the nodes allocations. */
int ebDefragRaxNode(raxNode **noderef, void *privdata) {
    ebDefragFunctions *defragfns = privdata;
    raxNode *newnode = defragfns->defragAlloc(*noderef);
    if (newnode) {
        *noderef = newnode;
        return 1;
    }
    return 0;
}

/* Defragments items in list-based bucket. */
void ebDefragList(ebuckets *eb, EbucketsType *type, ebDefragFunctions *defragfns, void *privdata) {
    ExpireMeta *previtem = NULL;
    eItem newitem, curitem = ebGetListPtr(type, *eb);
    while (curitem != NULL) {
        if ((newitem = defragfns->defragItem(curitem, privdata))) {
            curitem = newitem;
            if (previtem) {
                previtem->next = curitem;
            } else {
                *eb = ebMarkAsList(curitem);
            }
        }
        /* Move to the next item in the list. */
        previtem = type->getExpireMeta(curitem);
        curitem = previtem->next;
    }
}

/* Defragments a single bucket in rax, including its segments and items. */
void ebDefragRaxBucket(EbucketsType *type, raxIterator *ri,
                       ebDefragFunctions *defragfns, void *privdata)
{
    CommonSegHdr *currentSegHdr = ri->data;
    eItem iter = ((FirstSegHdr*)currentSegHdr)->head;
    ExpireMeta *mHead = type->getExpireMeta(iter);
    ExpireMeta *prevSegLastItem = NULL; /* The last item of the previous segment */

    while (1) {
        unsigned int numItems = mHead->numItems;
        assert(numItems);  /* Avoid compiler warning with old build chain. */
        ExpireMeta *prevIter = NULL;
        ExpireMeta *mIter = NULL;

        for (unsigned int i = 0; i < numItems; ++i) {
            eItem newiter = defragfns->defragItem(iter, privdata);
            if (newiter) {
                iter = newiter;

                if (prevIter == NULL) {
                    /* If this is the first item in the segment, update the segment
                     * header to point to the new item location. */
                    currentSegHdr->head = iter;
                } else {
                    /* Update the previous item's next pointer to point to the newly defragmented item */
                    prevIter->next = iter;
                }
            }
            mIter = type->getExpireMeta(iter);
            prevIter = mIter;
            iter = mIter->next;
        }

        /* Try to defragment the current segment. */
        CommonSegHdr *newSegHdr = defragfns->defragAlloc(currentSegHdr);
        if (newSegHdr) {
            if (currentSegHdr == ri->data) {
                /* If it's the first segment, update the rax data pointer. */
                raxSetData(ri->node, ri->data=newSegHdr);
            } else {
                /* For non-first segments, update the previous segment's next
                 * item to new pointer. */
                prevSegLastItem->next = newSegHdr;
            }
            currentSegHdr = newSegHdr;
        }

        /* Remember last item in this segment for next iteration */
        prevSegLastItem = mIter;

        if (mIter->lastItemBucket) {
            /* The last eitem needs to point back to the segment. */
            if (newSegHdr) mIter->next = currentSegHdr;
            break;
        }

        NextSegHdr *nextSegHdr = mIter->next;
        if (newSegHdr) {
            /* Update next segment's prev to point to the defragmented segment. */
            nextSegHdr->prevSeg = newSegHdr;
        }

        /* Update pointers for next segment iteration */
        iter = nextSegHdr->head;
        mHead = type->getExpireMeta(iter);
        currentSegHdr = (CommonSegHdr *)nextSegHdr;
    }
}

/* Defragments items in rax-based bucket.
 * returns 0 if no more work needs to be been done, and 1 if more work is needed. */
int ebDefragRax(ebuckets *eb, EbucketsType *type, unsigned long *cursor,
                ebDefragFunctions *defragfns, void *privdata)
{
    rax *newrax, *rax = ebGetRaxPtr(*eb);
    raxIterator ri;
    static unsigned char next[6];

    /* defrag the rax struct */
    if (!*cursor) {
        if ((newrax = defragfns->defragAlloc(rax))) {
            *eb = newrax;
            rax = newrax;
        }
    }

    raxStart(&ri,rax);
    if (!*cursor) {
        ebDefragRaxNode(&rax->head, defragfns);
        /* assign the iterator node callback before the seek, so that the
         * initial nodes that are processed till the first item are covered */
        ri.node_cb = ebDefragRaxNode;
        ri.privdata = defragfns;
        raxSeek(&ri, "^", NULL, 0);
    } else {
        /* if cursor is non-zero, we seek to the static 'next'.
         * Since node_cb is set after seek operation, any node traversed during seek wouldn't
         * be defragmented. To prevent this, we advance to next node before exiting previous
         * run, ensuring it gets defragmented instead of being skipped during current seek. */
        if (!raxSeek(&ri, ">=", next, type->ebp.keySize)) {
            *cursor = 0;
            raxStop(&ri);
            return 0;
        }
        /* assign the iterator node callback after the seek, so that the
         * initial nodes that are processed till now aren't covered */
        ri.node_cb = ebDefragRaxNode;
        ri.privdata = defragfns;
    }

    /* Defrag the bucket in the rax node. */
    assert(raxNext(&ri));
    ebDefragRaxBucket(type, &ri, defragfns, privdata);

    /* Move to next node. */
    if (!raxNext(&ri)) {
        /* If we reached the end, we can stop. */
        *cursor = 0;
        raxStop(&ri);
        return 0;
    }

    (*cursor)++;
    assert(ri.key_len == sizeof(next));
    memcpy(next, ri.key, ri.key_len);
    raxStop(&ri);
    return 1;
}

/* Reallocates the memory used by ebucket components (segments and items)
 * using the provided allocation functions. This feature was added for
 * the active defrag feature.
 *
 * The 'defragfns' callbacks are called with a pointer to memory that callback
 * can reallocate. The callbacks should return a new memory address or NULL,
 * where NULL means that no reallocation happened and the old memory is still
 * valid. */
int ebScanDefrag(ebuckets *eb, EbucketsType *type, unsigned long *cursor,
                 ebDefragFunctions *defragfns, void *privdata)
{
    if (ebIsEmpty(*eb)) {
        *cursor = 0;
        return 0;
    }
    
    if (type->isEbStack) {
        assert(0); // TODO_MOTI: Implement ebScanDefragStack() for ebStack
    }

    if (ebIsList(*eb)) {
        ebDefragList(eb, type, defragfns, privdata);
        *cursor = 0;
        return 0;
    } else {
        return ebDefragRax(eb, type, cursor, defragfns, privdata);
    }
}
/* Gets the item's expiration timestamp. Returns EB_EXPIRE_TIME_INVALID 
 * if the item's ExpireMeta is marked as trash  */
uint64_t ebGetExpireTime(EbucketsType *type, eItem item) {
    debugAssert(type->isEbStack == 0); /* See ebStackGetExpireTime() */
    ExpireMeta *meta = type->getExpireMeta(item);
    
    if (unlikely(meta->storedIn == EB_STORED_IN_TRASH))
        return EB_EXPIRE_TIME_INVALID;
    
    if (unlikely(meta->storedIn == EB_STORED_IN_EB_L3))
        return ((ExpireMetaL3 *) meta)->expireTime;
    
    return ebGetMetaExpTime(meta);
}

/* Gets the expiration timestamp for an item stored in ebStack.
 * 
 * ebStack also extends ebuckets to handle timestamps beyond the standard limit
 * (timestamps ≥ 2^48 msec are considered "infinite") with L3 vector. 
 * 
 * Returns -1 if the item is marked as trash, otherwise returns the actual
 * expiration time. (can't use here the value EB_EXPIRE_TIME_INVALID!) */
long long ebStackGetExpireTime(EbucketsType *type, eItem item) {
    debugAssert(type->isEbStack);
    
    ExpireMeta *meta = type->getExpireMeta(item);

    if (unlikely(meta->storedIn == EB_STORED_IN_TRASH))
        return -1;

    /* L3 */
    if (unlikely(meta->storedIn == EB_STORED_IN_EB_L3))
        return ((ExpireMetaL3 *) meta)->expireTime;

    /* L1 or L2 */
    return ebGetMetaExpTime(meta);
}

/* Init ebuckets iterator
 *
 * This is a non-safe iterator. Any modification to ebuckets will invalidate the
 * iterator. Calling this function takes care to reference the first item
 * in ebuckets with minimal expiration time. If no items to iterate, then
 * iter->currItem will be NULL and iter->itemsCurrBucket will be set to 0.
 */
void ebStart(EbucketsIterator *iter, ebuckets eb, EbucketsType *type) {
    debugAssert(type->isEbStack == 0); /* Not required for now */
    iter->eb = eb;
    iter->type = type;
    iter->isRax = 0;

    if (ebIsEmpty(eb)) {
        iter->currItem = NULL;
        iter->itemsCurrBucket = 0;
    } else if (ebIsList(eb)) {
        iter->currItem = ebGetListPtr(type, eb);
        iter->itemsCurrBucket = type->getExpireMeta(iter->currItem)->numItems;
    } else {
        rax *rax = ebGetRaxPtr(eb);
        raxStart(&iter->raxIter, rax);
        raxSeek(&iter->raxIter, "^", NULL, 0);
        raxNext(&iter->raxIter);
        FirstSegHdr *firstSegHdr = iter->raxIter.data;
        iter->itemsCurrBucket = firstSegHdr->totalItems;
        iter->currItem = firstSegHdr->head;
        iter->isRax = 1;
    }
}

/* Advance iterator to the next item
 *
 * Returns:
 *   - 0 if the end of ebuckets has been reached, setting `iter->currItem`
 *       to NULL.
 *   - 1 otherwise, updating `iter->currItem` to the next item.
 */
int ebNext(EbucketsIterator *iter) {
    debugAssert(iter->type->isEbStack == 0); /* Not required for now */
    if (iter->currItem == NULL)
        return 0;

    eItem item = iter->currItem;
    ExpireMeta *meta = iter->type->getExpireMeta(item);
    if (iter->isRax) {
        if (meta->lastItemBucket) {
            if (raxNext(&iter->raxIter)) {
                FirstSegHdr *firstSegHdr = iter->raxIter.data;
                iter->currItem = firstSegHdr->head;
                iter->itemsCurrBucket = firstSegHdr->totalItems;
            } else {
                iter->currItem = NULL;
            }
        } else if (meta->lastInSegment) {
            NextSegHdr *nextSegHdr = meta->next;
            iter->currItem = nextSegHdr->head;
        } else {
            iter->currItem = meta->next;
        }
    } else {
        iter->currItem = meta->next;
    }

    if (iter->currItem == NULL) {
        iter->itemsCurrBucket = 0;
        return 0;
    }

    return 1;
}

/* Advance the iterator to the next bucket
 *
 * Returns:
 *   - 0 if no more ebuckets are available, setting `iter->currItem` to NULL
 *       and `iter->itemsCurrBucket` to 0.
 *   - 1 otherwise, updating `iter->currItem` and `iter->itemsCurrBucket` for the
 *       next ebucket.
 */
int ebNextBucket(EbucketsIterator *iter) {
    debugAssert(iter->type->isEbStack == 0); /* Not required for now */
    if (iter->currItem == NULL)
        return 0;

    if ((iter->isRax) && (raxNext(&iter->raxIter))) {
        FirstSegHdr *currSegHdr = iter->raxIter.data;
        iter->currItem = currSegHdr->head;
        iter->itemsCurrBucket = currSegHdr->totalItems;
    } else {
        iter->currItem = NULL;
        iter->itemsCurrBucket = 0;
    }
    return 1;
}

/* Stop and cleanup the ebuckets iterator */
void ebStop(EbucketsIterator *iter) {
    debugAssert(iter->type->isEbStack == 0); /* Not required for now */
    if (iter->isRax)
        raxStop(&iter->raxIter);
}

/* Collect ebuckets statistics.
   Caller needs to provide a pre-allocated, empty ebucketsStats structure. */ 
void ebGetStats(ebuckets eb, EbucketsType *type, ebucketsStats *stats) {
    if (ebIsEmpty(eb)) {
        return;
    }
    if (type->isEbStack) {
        ebStack *stack = (ebStack *) eb;
        EB_STACK_EXEC_L1(type, ebGetStats(stack->l1, type, stats));        
        EB_STACK_EXEC_L2(type, ebGetStats(stack->l2, type, stats));
    } else if (ebIsList(eb)) {
        eItem head = ebGetListPtr(type, eb);
        ExpireMeta *meta = type->getExpireMeta(head);
        uint64_t numItems = meta->numItems;
        stats->totalItems += numItems;
        stats->totalBuckets += 1;  /* List is treated as single bucket */
        stats->totalSegments += 1; /* List is treated as single segment */
    } else {
        rax *rax = ebGetRaxPtr(eb);
        raxIterator iter;
        raxStart(&iter, rax);
        raxSeek(&iter, "^", NULL, 0);
        while (raxNext(&iter)) {
            FirstSegHdr *seg = iter.data;
            stats->totalItems += seg->totalItems;
            stats->totalSegments += seg->numSegs;
            stats->totalBuckets++;
        }
        raxStop(&iter);
    }
    if (stats->totalBuckets == 0) return;
    
    stats->avgItemsPerBucket = stats->totalItems / stats->totalBuckets;
    stats->avgItemsPerSegment = stats->totalItems / stats->totalSegments;
    stats->avgSegPerBucket = stats->totalSegments / stats->totalBuckets;
}

size_t ebGetStatsMsg(char *buf, size_t bufsize, ebucketsStats *stats, int full) {
    if (stats->totalItems == 0) {
        return snprintf(buf, bufsize,"Ebuckets stats:\nNo stats available for empty ebuckets\n");
    }

    size_t l = 0;

    l += snprintf(buf + l, bufsize - l,
                  "Ebuckets stats:\n"
                  " total items: %lu\n"
                  " total buckets: %lu\n"
                  " total segments: %lu\n",
                  stats->totalItems, stats->totalBuckets, stats->totalSegments);

    if (full && stats->totalBuckets > 0 && stats->totalSegments > 0) {
        l += snprintf(buf + l, bufsize - l,
                      " avg items per bucket: %lu\n"
                      " avg items per segment: %lu\n"
                      " avg segments per bucket: %lu\n",
                      stats->avgItemsPerBucket, stats->avgItemsPerSegment, stats->avgSegPerBucket);
    }

    /* Make sure there is a NULL term at the end. */
    buf[bufsize-1] = '\0';
    /* Unlike snprintf(), return the number of characters actually written. */
    return strlen(buf);
}

/*** Unit tests ***/

#ifdef REDIS_TEST
#include <stddef.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <string.h>
#include "testhelp.h"

#define TEST(name) printf("[TEST] >>> %s\n", name);
#define TEST_COND(name, cond) printf("[%s] >>> %s\n", (cond) ? "TEST" : "BYPS", name);  if (cond)

typedef struct MyItem {
    int index;
    ExpireMeta mexpire;
} MyItem;

typedef struct TimeRange {
    uint64_t start;
    uint64_t end;
} TimeRange;

ExpireMeta *getMyItemExpireMeta(const void *item) {
    return &((MyItem *)item)->mexpire;
}

ExpireAction expireItemCb(void *item, eItem ctx);
void deleteItemCb(eItem item, void *ctx);
EbucketsType myEbType = {
    .getExpireMeta = getMyItemExpireMeta,
    .onDeleteItem = deleteItemCb,
    .itemsAddrAreOdd = 0,
    .ebp.precision = 0,
    .ebp.keySize = EB_PRECISION2KEYSIZE(0 /*.precision*/),
};

EbucketsType myEbType2 = {
    .getExpireMeta = getMyItemExpireMeta,
    .onDeleteItem = NULL,
    .itemsAddrAreOdd = 0,
    .ebp.precision = 0,
    .ebp.keySize = EB_PRECISION2KEYSIZE(0 /*.precision*/),
};

/* XOR over all items time-expiration. Must be 0 after all addition/removal */
uint64_t expItemsHashValue = 0;

ExpireAction expireItemCb(eItem item, void *ctx) {
    ExpireMeta *meta = myEbType.getExpireMeta(item);
    uint64_t expTime = ebGetMetaExpTime(meta);
    expItemsHashValue = expItemsHashValue ^ expTime;

    TimeRange *range = (TimeRange *) ctx;
    /* Verify expiration time is within the range */
    if (range != NULL) assert(expTime >= range->start && expTime <= range->end);

/* If benchmarking then avoid from heavyweight free operation. It is user side logic */
#ifndef EB_TEST_BENCHMARK
    zfree(item);
#endif
    return ACT_REMOVE_EXP_ITEM;
}

ExpireAction expireUpdateThirdItemCb(eItem item, void *ctx) {
    uint64_t expTime = (uint64_t) (uintptr_t) ctx;
    static int calls = 0;
    if ((calls++) == 3) {
        ebSetMetaExpTime(&(((MyItem *)item)->mexpire), expTime );
        return ACT_UPDATE_EXP_ITEM;
    }

    return ACT_REMOVE_EXP_ITEM;
}

void deleteItemCb(eItem item, void *ctx) {
    UNUSED(ctx);
    zfree(item);
}

void addItems(ebuckets *eb, uint64_t startExpire, int step, uint64_t numItems, MyItem **ar) {
    for (uint64_t i = 0 ; i < numItems ; i++) {
        uint64_t expireTime = startExpire + (i * step);
        expItemsHashValue = expItemsHashValue ^ expireTime;
        MyItem *item = zmalloc(sizeof(MyItem));
        if (ar) ar[i] = item;
        ebAdd(eb, &myEbType, item, expireTime);
    }
}

/* expireRanges - is given as bucket-key to be agnostic to the different configuration
 *                of EB_BUCKET_KEY_PRECISION */
void distributeTest(int lowestTime,
                    uint64_t *expireRanges,
                    const int *ItemsPerRange,
                    int numRanges,
                    int isExpire,
                    int printStat) {
    struct timeval timeBefore, timeAfter, timeDryRun, timeCreation, timeDestroy;
    ebuckets eb = ebCreate();

    /* create items with random expiry */
    uint64_t startRange = lowestTime;

    expItemsHashValue = 0;
    void *listOfItems = NULL;
    for (int i = 0; i < numRanges; i++) {
        uint64_t endRange = EB_BUCKET_EXP_TIME(expireRanges[i], &myEbType);
        for (int j = 0; j < ItemsPerRange[i]; j++) {
            uint64_t randomExpirey = (rand() % (endRange - startRange)) + startRange;
            expItemsHashValue = expItemsHashValue ^ (uint32_t) randomExpirey;
            MyItem *item = zmalloc(sizeof(MyItem));
            getMyItemExpireMeta(item)->next = listOfItems;
            listOfItems = item;
            ebSetMetaExpTime(getMyItemExpireMeta(item), randomExpirey);
        }
        startRange = EB_BUCKET_EXP_TIME(expireRanges[i], &myEbType); /* next start range */
    }

    /* Take to sample memory after all items allocated and before insertion to ebuckets */
    size_t  usedMemBefore =  zmalloc_used_memory();

    gettimeofday(&timeBefore, NULL);
    while (listOfItems) {
        MyItem *item = (MyItem *)listOfItems;
        listOfItems = getMyItemExpireMeta(item)->next;
        uint64_t expireTime = ebGetMetaExpTime(&item->mexpire);
        ebAdd(&eb, &myEbType, item, expireTime);
    }
    gettimeofday(&timeAfter, NULL);
    timersub(&timeAfter, &timeBefore, &timeCreation);

    gettimeofday(&timeBefore, NULL);
    ebExpireDryRun(eb, &myEbType, 0xFFFFFFFFFFFF);  /* expire dry-run all */
    gettimeofday(&timeAfter, NULL);
    timersub(&timeAfter, &timeBefore, &timeDryRun);

    if (printStat) {
        char buf[4096];
        ebucketsStats stats = {0}; /* must be zeroed */
        ebGetStats(eb, &myEbType, &stats);
        ebGetStatsMsg(buf, sizeof(buf), &stats, 1);
        printf("%s", buf);
        
        /* print used memory */
        printf("Total Ebuckets memory usage (including FirstSegHdr/NexSegHdr): %.2f KBytes\n",
               (double) (zmalloc_used_memory() - usedMemBefore) / 1024);
    }

    gettimeofday(&timeBefore, NULL);
    if (isExpire) {
        startRange = lowestTime;
        /* Active expire according to the ranges */
        for (int i = 0 ; i < numRanges ; i++) {

            /* When checking how many items are expired, we need to take into
             * consideration EbucketsType.ebp.precision. The value of "info->now"
             * will be adjusted by ebActiveExpire() to lookup only for all buckets
             * with assigned keys that are older than 1<<EbucketsType.ebp.precision
             * msec ago. That is, it is needed to visit only the buckets with keys
             * that are "<" EB_BUCKET_KEY(info->now) and not "<=".
             * But if there is a list behind ebuckets, then this limitation is not
             * applied and the operator "<=" will be used instead.
             *
             * The '-1' in case of list brings makes both cases aligned to have
             * same result */
            uint64_t now = EB_BUCKET_EXP_TIME(expireRanges[i], &myEbType);

            TimeRange range = {EB_BUCKET_EXP_TIME(startRange, &myEbType), EB_BUCKET_EXP_TIME(expireRanges[i], &myEbType) };
            ExpireInfo info = {
                    .maxToExpire = 0xFFFFFFFF,
                    .onExpireItem = expireItemCb,
                    .ctx = &range,
                    .now = now,
                    .itemsExpired = 0};

            ebExpire(&eb, &myEbType, &info);

            assert( (eb==NULL && (i + 1 == numRanges)) || (eb!=NULL && (i + 1 < numRanges)) );
            assert( info.itemsExpired == (uint64_t) ItemsPerRange[i]);
            startRange = expireRanges[i];
        }
        assert(eb == NULL);
        assert( (expItemsHashValue & 0xFFFFFFFF) == 0);
    }
    ebDestroy(&eb, &myEbType, NULL);
    gettimeofday(&timeAfter, NULL);
    timersub(&timeAfter, &timeBefore, &timeDestroy);

    if (printStat) {
        printf("Time elapsed ebuckets creation     : %ld.%06ld\n", (long int)timeCreation.tv_sec, (long int)timeCreation.tv_usec);
        printf("Time elapsed active-expire dry-run : %ld.%06ld\n", (long int)timeDryRun.tv_sec, (long int)timeDryRun.tv_usec);
        if (isExpire)
            printf("Time elapsed active-expire         : %ld.%06ld\n", (long int)timeDestroy.tv_sec, (long int)timeDestroy.tv_usec);
        else
            printf("Time elapsed destroy               : %ld.%06ld\n", (long int)timeDestroy.tv_sec, (long int)timeDestroy.tv_usec);
    }

}

#define UNUSED(x) (void)(x)
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

void *defragCallback(void *ptr) {
    size_t size = zmalloc_usable_size(ptr);
    void *newitem = zmalloc(size);
    memcpy(newitem, ptr, size);
    zfree(ptr);
    return newitem;
}

void *defragItemCallback(void *ptr, void *privdata) {
    MyItem *item = ptr;
    MyItem **items = privdata;
    int index = item->index;
    void *newitem = defragCallback(ptr);
    if (newitem)
        items[index] = newitem;
    return newitem;
}

int ebucketsTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    srand(0);

    int verbose = (flags & REDIS_TEST_VERBOSE) ? 2 : 1;
    UNUSED(verbose);

#ifdef EB_TEST_BENCHMARK
    TEST("ebuckets - benchmark 10 million items: alloc + add + activeExpire") {

        struct TestParams {
            uint64_t minExpire;
            uint64_t maxExpire;
            int items;
            const char *description;
        } testCases[] = {
            { 1805092100000, 1805092100000 + (uint64_t) 1,                10000000, "1 msec distribution"  },
            { 1805092100000, 1805092100000 + (uint64_t) 1000,             10000000, "1 sec distribution"   },
            { 1805092100000, 1805092100000 + (uint64_t) 1000*60,          10000000, "1 min distribution"   },
            { 1805092100000, 1805092100000 + (uint64_t) 1000*60*60,       10000000, "1 hour distribution"  },
            { 1805092100000, 1805092100000 + (uint64_t) 1000*60*60*24,    10000000, "1 day distribution"   },
            { 1805092100000, 1805092100000 + (uint64_t) 1000*60*60*24*7,  10000000, "1 week distribution"  },
            { 1805092100000, 1805092100000 + (uint64_t) 1000*60*60*24*30, 10000000, "1 month distribution" }
        };

        /* selected test */
        uint32_t tid = EB_TEST_BENCHMARK;

        printf("\n------ TEST EBUCKETS: %s ------\n", testCases[tid].description);
        uint64_t expireRanges[] = { testCases[tid].minExpire, testCases[tid].maxExpire };
        int itemsPerRange[] = { 0, testCases[tid].items };

        /* expireRanges[] is provided to distributeTest() as bucket-key values */
        for (uint32_t j = 0; j < ARRAY_SIZE(expireRanges); ++j) {
            expireRanges[j] = expireRanges[j] >> myEbType.precision;
        }

        distributeTest(0, expireRanges, itemsPerRange, ARRAY_SIZE(expireRanges), 1, 1);
        return 0;
    }
#endif

    TEST("basic iterator test") {
        MyItem *items[100];
        for (uint32_t numItems = 0 ; numItems < ARRAY_SIZE(items) ; ++numItems) {
            ebuckets eb = NULL;
            EbucketsIterator iter;

            /* Create and add items to ebuckets */
            for (uint32_t i = 0; i < numItems; i++) {
                items[i] = zmalloc(sizeof(MyItem));
                ebAdd(&eb, &myEbType, items[i], i);
            }

            /* iterate items */
            ebStart(&iter, eb, &myEbType);
            for (uint32_t i = 0; i < numItems; i++) {
                assert(iter.currItem == items[i]);
                int res = ebNext(&iter);
                if (i+1<numItems) {
                    assert(res == 1);
                    assert(iter.currItem != NULL);
                } else {
                    assert(res == 0);
                    assert(iter.currItem == NULL);
                }
            }
            ebStop(&iter);

            /* iterate buckets */
            ebStart(&iter, eb, &myEbType);
            uint32_t countItems = 0;

            uint32_t countBuckets = 0;
            while (1) {
                countItems += iter.itemsCurrBucket;
                if (!ebNextBucket(&iter)) break;
                countBuckets++;
            }
            ebStop(&iter);
            assert(countItems == numItems);
            if (numItems>=8) assert(numItems/8 >= countBuckets);
            ebDestroy(&eb, &myEbType, NULL);
        }
    }

    TEST("list - Create a single item, get TTL, and remove") {
        MyItem *singleItem = zmalloc(sizeof(MyItem));
        ebuckets eb = NULL;
        ebAdd(&eb, &myEbType, singleItem, 1000);
        assert(ebGetExpireTime(&myEbType, singleItem) == 1000 );

        /* remove the item */
        assert(ebRemove(&eb, &myEbType, singleItem));
        /* now the ebuckets is empty */
        assert(ebRemove(&eb, &myEbType, singleItem) == 0);

        zfree(singleItem);

        ebDestroy(&eb, &myEbType, NULL);
    }

    TEST("list - Create few items on different times, get TTL, and then remove") {
        MyItem *items[EB_LIST_MAX_ITEMS];
        ebuckets eb = NULL;
        for (int i = 0 ; i < EB_LIST_MAX_ITEMS  ; i++) {
            items[i] = zmalloc(sizeof(MyItem));
            ebAdd(&eb, &myEbType, items[i], i);
        }

        for (uint64_t i = 0 ; i < EB_LIST_MAX_ITEMS ; i++) {
            assert(ebGetExpireTime(&myEbType, items[i]) == i );
            assert(ebRemove(&eb, &myEbType, items[i]));
        }

        for (int i = 0 ; i < EB_LIST_MAX_ITEMS  ; i++)
            zfree(items[i]);

        ebDestroy(&eb, &myEbType, NULL);
    }

    TEST("list - Create few items on different times, get TTL, and then delete") {
        MyItem *items[EB_LIST_MAX_ITEMS];
        ebuckets eb = NULL;
        for (int i = 0 ; i < EB_LIST_MAX_ITEMS  ; i++) {
            items[i] = zmalloc(sizeof(MyItem));
            ebAdd(&eb, &myEbType, items[i], i);
        }

        for (uint64_t i = 0 ; i < EB_LIST_MAX_ITEMS ; i++) {
            assert(ebGetExpireTime(&myEbType, items[i]) == i );
        }

        ebDestroy(&eb, &myEbType, NULL);
    }

    TEST_COND("ebuckets - Add items with increased/decreased expiration time and then expire",
              myEbType.ebp.precision > 0)
    {
        ebuckets eb = NULL;

        for (int isDecr = 0; isDecr < 2; ++isDecr) {
            for (uint32_t numItems = 1; numItems < 64; ++numItems) {
                uint64_t step = 1 << myEbType.ebp.precision;

                if (isDecr == 0)
                    addItems(&eb, 0, step, numItems, NULL);
                else
                    addItems(&eb, (numItems - 1) * step, -step, numItems, NULL);

                for (uint32_t i = 1; i <= numItems; i++) {
                    TimeRange range = {EB_BUCKET_EXP_TIME(i - 1, &myEbType), EB_BUCKET_EXP_TIME(i, &myEbType)};
                    ExpireInfo info = {
                            .maxToExpire = 1,
                            .onExpireItem = expireItemCb,
                            .ctx = &range,
                            .now = EB_BUCKET_EXP_TIME(i, &myEbType),
                            .itemsExpired = 0};

                    ebExpire(&eb, &myEbType, &info);
                    assert(info.itemsExpired == 1);
                    if (i == numItems) { /* if last item */
                        assert(eb == NULL);
                        assert(info.nextExpireTime == EB_EXPIRE_TIME_INVALID);
                    } else {
                        assert(info.nextExpireTime == EB_BUCKET_EXP_TIME(i, &myEbType));
                    }
                }
            }
        }
    }

    TEST("ebuckets - Create items with same expiration time and then expire")
    {
        /*temporary modify myEbType precision for the test*/
        EBucketPrecision tmp = myEbType.ebp;
        myEbType.ebp.precision = 8;
        myEbType.ebp.keySize = EB_PRECISION2KEYSIZE(myEbType.ebp.precision);

        ebuckets eb = NULL;
        uint64_t expirePerIter = 2;
        for (uint32_t numIterations = 1; numIterations < 100; ++numIterations) {
            uint32_t numItems = numIterations * expirePerIter;
            uint64_t expireTime = (1 << myEbType.ebp.precision) + 1;
            addItems(&eb, expireTime, 0, numItems, NULL);

            for (uint32_t i = 1; i <= numIterations; i++) {
                ExpireInfo info = {
                        .maxToExpire = expirePerIter,
                        .onExpireItem = expireItemCb,
                        .ctx = NULL,
                        .now = (2 << myEbType.ebp.precision),
                        .itemsExpired = 0};
                ebExpire(&eb, &myEbType, &info);
                assert(info.itemsExpired == expirePerIter);
                if (i == numIterations) { /* if last item */
                    assert(eb == NULL);
                    assert(info.nextExpireTime == EB_EXPIRE_TIME_INVALID);
                } else {
                    assert(info.nextExpireTime == expireTime);
                }
            }
        }

        /*restore*/
        myEbType.ebp = tmp;
    }

    TEST("ebStack - L1 & L2 basic functionality") {
        EbucketsType type = myEbType2;
        type.isEbStack = 1; /* Set to hierarchical ebuckets stack for the test */

        for (int nowAlignment = 0; nowAlignment < 2; nowAlignment++) {
            const int L2_BUCKET_INTERVAL = 1 << ebpStackL2.precision;
            __NOW__ = L2_BUCKET_INTERVAL + nowAlignment;
            uint64_t l1MaxExpireTime = (__NOW__ + (1 << ebpStackL2.precision)) | ((1 << ebpStackL2.precision) - 1);
            uint64_t l2MinExpireTime = l1MaxExpireTime + 1;
            uint64_t l2MaxDeltaExpireTime = (5 * L2_BUCKET_INTERVAL);        
            MyItem items[80];        
            uint64_t l1, l2, l3;
            //srand(0);
    
            for (int cascadeIter = 1; cascadeIter <= 2; cascadeIter++) {
                uint64_t cascadeTime = __NOW__ + (cascadeIter << ebpStackL2.precision);            
                for (uint64_t totalItems = 0 ; totalItems < 80; totalItems++) {
                    for (l1 = 0; l1 <= totalItems; l1++) {
                        for (l2 = 0; l2 <= totalItems - l1; l2++) {
                            l3 = totalItems - l1 - l2;                            
                            uint64_t cascadedItemsLo = 0, cascadedItemsHi = 0;    
                            /* 1. Create ebStack */
                            ebuckets eb = ebCreate();
                            
                            /* 2. Randomize l1/l2/l3 items */
                            int counter = 0;
                            for (uint64_t i = 0; i < l1; i++) {
                                uint64_t expireTime = __NOW__ + (rand() % (l1MaxExpireTime - __NOW__));
                                ebAdd(&eb, &type, items + counter, expireTime);
                                counter++;
                            }                       
                            
                            for (uint64_t i = 0; i < l2; i++) {
                                uint64_t expireTime = l2MinExpireTime + (rand() % l2MaxDeltaExpireTime);
                                ebAdd(&eb, &type, items + counter, expireTime);
                                counter++;
                                
                                /* Can expired either based on accurate comparison of the item
                                 * expire time, or based on bucket key. */
                                if (expireTime < cascadeTime + (2 << ebpStackL2.precision))
                                    cascadedItemsLo++;
                                if ( (expireTime >> ebpStackL2.precision) < (2 + (cascadeTime >> ebpStackL2.precision)))
                                    cascadedItemsHi++;
                            }
                            for (uint64_t i = 0; i < l3; i++) {
                                uint64_t expireTime = ((uint64_t)rand() << 32) | (uint64_t)rand();
                                if (expireTime < (1ULL << 48)) 
                                    expireTime += (1ULL << 48); 
        
                                ebAdd(&eb, &type, items + counter, expireTime);
                                counter++;
                            }
                            assert(ebGetTotalItems(eb, &type) == totalItems);
                            assert(ebStackItems(eb, &type, 1) == l1);
                            assert(ebStackItems(eb, &type, 2) == l2);
                            assert(ebStackItems(eb, &type, 3) == l3);
                            
                            /* Now let's cascade */
                            ebStack *stack = (ebStack *)eb;
                            
                            if (ebIsEmpty(eb) || ebIsEmpty(stack->l2))
                                continue;

                            /* Verify no cascade before cascadeTime */
                            assert(0 == ebCascade(&eb, &type, __NOW__, 1000000 /*no limit*/ ));
                            /* Verify cascade on cascadeTime */
                            uint64_t cascaded = ebCascade(&eb, &type, cascadeTime, 1000000 /*no limit*/ );
                            assert( (cascadedItemsHi <= cascaded) && (cascaded <= cascadedItemsLo));
            
                            ebDestroy(&eb, &type, NULL);
                        }
                    }
                }
            }
        }
    }    

    TEST("list - Create few items on random times and then expire/delete ") {
        for (int isExpire = 0 ; isExpire <= 1 ; ++isExpire ) {
            uint64_t expireRanges[] = {1000};   /* bucket-keys */
            int itemsPerRange[] = {EB_LIST_MAX_ITEMS};
            distributeTest(0, expireRanges, itemsPerRange,
                           ARRAY_SIZE(expireRanges), isExpire, 0);
        }
    }

    TEST("list - Create few items (list) on same time and then active expire/delete ") {
        for (int isExpire = 0 ; isExpire <= 1 ; ++isExpire ) {
            uint64_t expireRanges[] = {1, 2};  /* bucket-keys */
            int itemsPerRange[] = {0, EB_LIST_MAX_ITEMS};

            distributeTest(0, expireRanges, itemsPerRange,
                           ARRAY_SIZE(expireRanges), isExpire, 0);
        }
    }

    TEST("ebuckets - Create many items on same time and then active expire/delete ") {
        for (int isExpire = 1 ; isExpire <= 1 ; ++isExpire ) {
            uint64_t expireRanges[] = {1, 2}; /* bucket-keys */
            int itemsPerRange[] = {0, 20};

            distributeTest(0, expireRanges, itemsPerRange,
                           ARRAY_SIZE(expireRanges), isExpire, 0);
        }
    }

    TEST("ebuckets - Create items on different times and then expire/delete ") {
        for (int isExpire = 0 ; isExpire <= 0 ; ++isExpire ) {
            for (int numItems = 1 ; numItems < 100 ; ++numItems ) {
                uint64_t expireRanges[] = {1000000}; /* bucket-keys */
                int itemsPerRange[] = {numItems};
                distributeTest(0, expireRanges, itemsPerRange,
                               ARRAY_SIZE(expireRanges), 1, 0);
            }
        }
    }

    TEST("ebuckets - Create items on different times and then ebRemove() ") {
        ebuckets eb = NULL;

        for (int step = -1 ; step <= 1 ; ++step) {
            for (int numItems = 1; numItems <= EB_SEG_MAX_ITEMS*3; ++numItems) {
                for (int offset = 0; offset < numItems; offset++) {
                    MyItem *items[numItems];
                    uint64_t startValue = 1000 << myEbType.ebp.precision;
                    int stepValue = step * (1 << myEbType.ebp.precision);
                    addItems(&eb, startValue, stepValue, numItems, items);
                    for (int i = 0; i < numItems; i++) {
                        int at = (i + offset) % numItems;
                        assert(ebRemove(&eb, &myEbType, items[at]));
                        zfree(items[at]);
                    }
                    assert(eb == NULL);
                }
            }
        }
    }

    TEST("ebuckets - test min/max expire time") {
        ebuckets eb = NULL;
        MyItem items[3*EB_SEG_MAX_ITEMS];
        for (int numItems = 1 ; numItems < (int)ARRAY_SIZE(items) ; numItems++) {
            uint64_t minExpTime = RAND_MAX, maxExpTime = 0;
            for (int i = 0; i < numItems; i++) {
                 /* generate random expiration time */
                uint64_t expireTime = rand();
                if (expireTime < minExpTime) minExpTime = expireTime;
                if (expireTime > maxExpTime) maxExpTime = expireTime;
                ebAdd(&eb, &myEbType2, items + i, expireTime);
                assert(ebGetNextTimeToExpire(eb, &myEbType2) == minExpTime);
            }
            ebDestroy(&eb, &myEbType2, NULL);
        }
    }

    TEST("ebuckets - test min/max expire time, with extended-segment") {
        /*temporary modify myEbType2 precision for the test*/
        EBucketPrecision tmp = myEbType2.ebp;
        myEbType2.ebp.precision = 10;  // Take care: (1<<precision) > 2*EB_SEG_MAX_ITEMS
        myEbType2.ebp.keySize = EB_PRECISION2KEYSIZE(myEbType2.ebp.precision);

        ebuckets eb = NULL;
        MyItem items[(2*EB_SEG_MAX_ITEMS)-1];
        for (int numItems = EB_SEG_MAX_ITEMS+1 ; numItems < (int)ARRAY_SIZE(items) ; numItems++) {
            /* First reach extended-segment (two chained segments in a bucket) */
            for (int i = 0; i <= EB_SEG_MAX_ITEMS; i++) {
                uint64_t itemExpireTime = (1<<myEbType2.ebp.precision) + i;
                ebAdd(&eb, &myEbType2, items + i, itemExpireTime);
            }

            /* Now start adding more items to extended-segment and verify min/max */
            for (int i = EB_SEG_MAX_ITEMS+1; i < numItems; i++) {
                uint64_t itemExpireTime = (1<<myEbType2.ebp.precision) + i;
                ebAdd(&eb, &myEbType2, items + i, itemExpireTime);
                uint64_t nextTimeToExp = ebGetNextTimeToExpire(eb, &myEbType2);
                assert(nextTimeToExp == (uint64_t)(2<<myEbType2.ebp.precision));
            }
            ebDestroy(&eb, &myEbType2, NULL);
        }

        myEbType2.ebp.v = tmp.v; /*restore*/
    }

    TEST("ebuckets - active-expire dry-run") {
        ebuckets eb = NULL;
        MyItem items[2*EB_SEG_MAX_ITEMS];

        for (int numItems = 1 ; numItems < (int)ARRAY_SIZE(items) ; numItems++) {
            int maxExpireKey = (numItems % 2) ? 40 : 2;
            /* Allocate numItems and add to ebuckets */
            for (int i = 0; i < numItems; i++) {
                /* generate random expiration time */
                uint64_t expireTime = (rand() % maxExpireKey) << myEbType2.ebp.precision;
                ebAdd(&eb, &myEbType2, items + i, expireTime);
            }

            for (int i = 0 ; i <= maxExpireKey ; ++i) {
                uint64_t now = i << myEbType2.ebp.precision;

                /* Count how much items are expired */
                uint64_t expectedNumExpired = 0;
                for (int j = 0; j < numItems; j++) {
                    if (ebGetExpireTime(&myEbType2, items + j) < now)
                        expectedNumExpired++;
                }
                /* Perform dry-run and verify number of expired items */
                assert(ebExpireDryRun(eb, &myEbType2, now) == expectedNumExpired);
            }
            ebDestroy(&eb, &myEbType2, NULL);
        }
    }

    TEST("ebuckets - active expire callback returns ACT_UPDATE_EXP_ITEM") {
        ebuckets eb = NULL;
        MyItem items[2*EB_SEG_MAX_ITEMS];
        int numItems = 2*EB_SEG_MAX_ITEMS;

        /* timeline */
        int expiredAt           = 2,
            applyActiveExpireAt = 3,
            updateItemTo        = 5,
            expectedExpiredAt   = 6;

        /* Allocate numItems and add to ebuckets */
        for (int i = 0; i < numItems; i++)
            ebAdd(&eb, &myEbType2, items + i, expiredAt << myEbType2.ebp.precision);

        /* active-expire. Expected that all but one will be expired */
        ExpireInfo info = {
                .maxToExpire = 0xFFFFFFFF,
                .onExpireItem = expireUpdateThirdItemCb,
                .ctx = (void *) (uintptr_t) (updateItemTo << myEbType2.ebp.precision),
                .now = applyActiveExpireAt << myEbType2.ebp.precision,
                .itemsExpired = 0};
        ebExpire(&eb, &myEbType2, &info);
        assert(info.itemsExpired == (uint64_t) numItems);
        assert(info.nextExpireTime == (uint64_t)updateItemTo << myEbType2.ebp.precision);
        assert(ebGetTotalItems(eb, &myEbType2) == 1);

        /* active-expire. Expected that all will be expired */
        ExpireInfo info2 = {
                .maxToExpire = 0xFFFFFFFF,
                .onExpireItem = expireUpdateThirdItemCb,
                .ctx = (void *) (uintptr_t) (updateItemTo << myEbType2.ebp.precision),
                .now = expectedExpiredAt << myEbType2.ebp.precision,
                .itemsExpired = 0};
        ebExpire(&eb, &myEbType2, &info2);
        assert(info2.itemsExpired == (uint64_t) 1);
        assert(info2.nextExpireTime == EB_EXPIRE_TIME_INVALID);
        assert(ebGetTotalItems(eb, &myEbType2) == 0);

        ebDestroy(&eb, &myEbType2, NULL);

    }

    TEST("item defragmentation") {
        for (int s = 1; s <= EB_LIST_MAX_ITEMS * 3; s++) {
            ebuckets eb = NULL;
            MyItem *items[s];
            for (int i = 0; i < s; i++) {
                items[i] = zmalloc(sizeof(MyItem));
                items[i]->index = i;
                ebAdd(&eb, &myEbType, items[i], i);
            }
            assert((s <= EB_LIST_MAX_ITEMS) ? ebIsList(eb) : !ebIsList(eb));
            /* Defrag all the items. */
            unsigned long cursor = 0;
            ebDefragFunctions defragfns = {
                .defragAlloc = defragCallback,
                .defragItem = defragItemCallback,
            };
            while (ebScanDefrag(&eb, &myEbType, &cursor, &defragfns, items)) {}
            /* Verify that the data is not corrupted. */
            ebValidate(eb, &myEbType);
            for (int i = 0; i < s; i++)
                assert(items[i]->index == i);
            ebDestroy(&eb, &myEbType, NULL);
        }
    }

//    TEST("segment - Add smaller item to full segment that all share same ebucket-key")
//    TEST("segment - Add item to full segment and make it extended-segment (all share same ebucket-key)")
//    TEST("ebuckets - Create rax tree with extended-segment and add item before")

    return 0;
}

#endif
