/*
 * Copyright Redis Ltd. 2024 - present
 *
 * Licensed under your choice of the Redis Source Available License 2.0 (RSALv2)
 * or the Server Side Public License v1 (SSPLv1).
 */

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <inttypes.h>
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
 *  Keep just enough bytes of bucket-key, taking into consideration configured
 *  EB_BUCKET_KEY_PRECISION, and ignoring LSB bits that has no impact.
 *
 * The main motivation is that since the bucket-key size determines the maximum
 * depth of the rax tree, then we can prune the tree to be more shallow and thus
 * reduce the maintenance and traversal of each node in the B-tree.
 */
#if EB_BUCKET_KEY_PRECISION < 8
#define EB_KEY_SIZE 6
#elif EB_BUCKET_KEY_PRECISION >= 8 && EB_BUCKET_KEY_PRECISION < 16
#define EB_KEY_SIZE 5
#else
#define EB_KEY_SIZE 4
#endif

/*
 * EB_SEG_MAX_ITEMS - Maximum number of items in rax-segment before trying to
 * split. To simplify, it has the same value as EB_LIST_MAX_ITEMS.
 */
#define EB_SEG_MAX_ITEMS 16
#define EB_LIST_MAX_ITEMS EB_SEG_MAX_ITEMS

/* From expiration time to bucket-key */
#define EB_BUCKET_KEY(exptime) ((exptime) >> EB_BUCKET_KEY_PRECISION)

 /* From bucket-key to expiration time */
#define EB_BUCKET_EXP_TIME(bucketKey) ((uint64_t)(bucketKey) << EB_BUCKET_KEY_PRECISION)

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

/* Selective copy of ifndef from server.h instead of including it */
#ifndef static_assert
#define static_assert(expr, lit) extern char __static_assert_failure[(expr) ? 1:-1]
#endif
/* Verify that "head" field is aligned in FirstSegHdr, NextSegHdr and CommonSegHdr */
static_assert(offsetof(FirstSegHdr, head) == 0, "FirstSegHdr head is not aligned");
static_assert(offsetof(NextSegHdr, head) == 0, "FirstSegHdr head is not aligned");
static_assert(offsetof(CommonSegHdr, head) == 0, "FirstSegHdr head is not aligned");
/* Verify attached metadata to rax is aligned */
static_assert(offsetof(rax, metadata) % sizeof(void*) == 0, "metadata field is not aligned in rax");

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

/* Converts the logical starting time value of a given bucket-key to its equivalent
 * "physical" value in the context of an rax tree (rax-key). Although their values
 * are the same, their memory layouts differ. The raxKey layout orders bytes in
 * memory is from the MSB to the LSB, and the length of the key is EB_KEY_SIZE. */
static inline void bucketKey2RaxKey(uint64_t bucketKey, unsigned char *raxKey) {
    for (int i = EB_KEY_SIZE-1; i >= 0; --i) {
        raxKey[i] = (unsigned char) (bucketKey & 0xFF);
        bucketKey >>= 8;
    }
}

/* Converts the "physical" value of rax-key to its logical counterpart, representing
 * the starting time value of a bucket. The values are equivalent, but their memory
 * layouts differ. The raxKey is assumed to be ordered from the MSB to the LSB with
 * a length of EB_KEY_SIZE. The resulting bucket-key is the logical representation
 * with respect to ebuckets. */
static inline uint64_t raxKey2BucketKey(unsigned char *raxKey) {
    uint64_t bucketKey = 0;
    for (int i = 0; i < EB_KEY_SIZE ; ++i)
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
        if (EB_BUCKET_KEY(ebGetMetaExpTime(mNext)) > EB_BUCKET_KEY(
                                                         ebGetMetaExpTime(mIter))) {
            /* If found better middle index before reaching halfway, save it */
            if (i < (EB_SEG_MAX_ITEMS/2)) {
                splitKey = EB_BUCKET_KEY(ebGetMetaExpTime(mNext));
                bestMiddleIndex = i;
                mLastItemFirstPart = mIter;
                mFirstItemSecondPart = mNext;
                firstItemSecondPart = mIter->next;
                minMidDist = (EB_SEG_MAX_ITEMS / 2) - bestMiddleIndex;
            } else {
                /* after crossing the middle need only to look for the first diff */
                if (minMidDist > (i + 1 - EB_SEG_MAX_ITEMS / 2)) {
                    splitKey = EB_BUCKET_KEY(ebGetMetaExpTime(mNext));
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
        mIter->trash = 1;
        ExpireAction act = info->onExpireItem(iter, info->ctx);

        /* if (act == ACT_REMOVE_EXP_ITEM)
         *  then don't touch the item. Assume it got deleted */

        /* If indicated to stop then break (cb didn't delete the item) */
        if (act == ACT_STOP_ACTIVE_EXP) {
            mIter->trash = 0;
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
            mIter->trash = 1;
            ExpireAction act = info->onExpireItem(iter, info->ctx);

            /* if (act == ACT_REMOVE_EXP_ITEM)
             *  then don't touch the item. Assume it got deleted */

            /* If indicated to stop then break (callback didn't delete the item) */
            if (act == ACT_STOP_ACTIVE_EXP) {
                mIter->trash = 0;
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
    uint64_t bucketKey = EB_BUCKET_KEY(ebGetMetaExpTime(metaItem));
    while (metaItem->lastItemBucket == 0)
        metaItem = type->getExpireMeta(metaItem->next);
    metaItem->next = firstSegHdr;

    /* Use min expire-time for the first segment in rax */
    unsigned char raxKey[EB_KEY_SIZE];
    bucketKey2RaxKey(bucketKey, raxKey);
    rax *rax = raxNewWithMetadata(sizeof(uint64_t));
    *ebRaxNumItems(rax) = EB_LIST_MAX_ITEMS;
    raxInsert(rax, raxKey, EB_KEY_SIZE, firstSegHdr, NULL);
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

/* return 1 if none left. Otherwise return 0 */
static int ebListExpire(ebuckets *eb,
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
        if (itemExpTime > info->now)
            break;

        if (info->itemsExpired == info->maxToExpire)
            break;

        /* keep aside `next` before removing `iter` by onExpireItem */
        eItem *next = metaItem->next;
        metaItem->trash = 1;
        ExpireAction act = info->onExpireItem(item, info->ctx);

        /* if (act == ACT_REMOVE_EXP_ITEM)
         *  then don't touch the item. Assume it got deleted */

        /* If indicated to stop then break (cb didn't delete the item) */
        if (act == ACT_STOP_ACTIVE_EXP) {
            metaItem->trash = 0;
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
        return 1;
    }

    metaItem->numItems = numItems - expired;
    metaItem->firstItemBucket = 1;
    info->nextExpireTime = ebGetMetaExpTime(metaItem);
    *eb = ebMarkAsList(item);
    return 0;
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

    printf("Bucket(key=%06" PRIu64 ",tot=%04d,sgs=%04d) :", bucketKey, firstSeg->totalItems, firstSeg->numSegs);
    while (1) {
        mIter = type->getExpireMeta(iter);  /* not really needed. Just to hash the compiler */
        printf("    [");
        for (int i = 0; i < mHead->numItems ; ++i) {
            mIter = type->getExpireMeta(iter);
            uint64_t expireTime = ebGetMetaExpTime(mIter);

            if (i == 0 && PRINT_EXPIRE_META_FLAGS)
                printf("%" PRIu64 "<n=%d,f=%d,ls=%d,lb=%d>, ",
                       expireTime, mIter->numItems, mIter->firstItemBucket,
                       mIter->lastInSegment, mIter->lastItemBucket);
            else if (i == (mHead->numItems - 1) && PRINT_EXPIRE_META_FLAGS) {
                printf("%" PRIu64 "<n=%d,f=%d,ls=%d,lb=%d>",
                       expireTime, mIter->numItems, mIter->firstItemBucket,
                       mIter->lastInSegment, mIter->lastItemBucket);
            } else
                printf("%" PRIu64 "%s", expireTime, (i == mHead->numItems - 1) ? "" : ", ");

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
            if (EB_BUCKET_KEY(ebGetMetaExpTime(type->getExpireMeta(item))) < newBucket->ebKey) {
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

    uint64_t bucketKey = EB_BUCKET_KEY(ebGetMetaExpTime(mHead)); /* same for all items in the segment */
    uint64_t itemKey = EB_BUCKET_KEY(ebGetMetaExpTime(mItem));

    if (bucketKey == itemKey) {
        /* New item has the same bucket-key as the ones in this bucket, Add it as well */
        if (mHead->numItems < EB_SEG_MAX_ITEMS)
            return ebSegAddAvail(type, firstSegBkt, item); /* Add item to first segment */
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

        ebNewBucket(type, newBucket, item, EB_BUCKET_KEY(ebGetMetaExpTime(mItem)));
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
        unsigned char raxKey[EB_KEY_SIZE];
        bucketKey2RaxKey(EB_BUCKET_KEY(ebGetMetaExpTime(mItem)), raxKey);
        raxSeek(&ri, "<=", raxKey, EB_KEY_SIZE);

        if (raxNext(&ri) == 0)
            return 0; /* not removed */

        FirstSegHdr *segHdr = ri.data;

        if (segHdr->head != item)
            return 0; /* not removed */

        zfree(segHdr);
        raxRemove(ri.rt, ri.key, EB_KEY_SIZE, NULL);
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

int ebAddToRax(ebuckets *eb, EbucketsType *type, eItem item, uint64_t bucketKeyItem) {
    EBucketNew newBucket; /* ebAddToBucket takes care to update newBucket.segment.head */
    raxIterator iter;
    unsigned char raxKey[EB_KEY_SIZE];
    bucketKey2RaxKey(bucketKeyItem, raxKey);
    rax *rax = ebGetRaxPtr(*eb);
    raxStart(&iter,rax);
    raxSeek(&iter, "<=", raxKey, EB_KEY_SIZE);
    *ebRaxNumItems(rax) += 1;
    /* If expireTime of the item is below the bucket-key of first bucket in rax,
     * then need to add it as a new bucket at the beginning of the rax. */
    if(raxNext(&iter) == 0) {
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
        bucketKey2RaxKey(bucketKeyItem, raxKey);
        raxInsert(rax, raxKey, EB_KEY_SIZE, firstSegHdr, NULL);
        raxStop(&iter);
        return 0;
    }

    /* Add the new item into the first segment of the bucket that we found */
    uint64_t updateBucketKey = 0;
    ebAddToBucket(type, iter.data, item, &newBucket, &updateBucketKey);

    /* If following the addition need to `updateBucketKey` of `foundBucket` in rax */
    if(unlikely(updateBucketKey && updateBucketKey != raxKey2BucketKey(iter.key))) {
        raxRemove(iter.rt, iter.key, EB_KEY_SIZE, NULL);
        bucketKey2RaxKey(updateBucketKey, raxKey);
        raxInsert(iter.rt, raxKey, EB_KEY_SIZE, iter.data, NULL);
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
        bucketKey2RaxKey(newBucket.ebKey, raxKey);
        raxInsert(iter.rt, raxKey, EB_KEY_SIZE, newSeg, NULL);
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
                curBktKey = EB_BUCKET_KEY(ebGetMetaExpTime(mIter));

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
            mIter->trash = 1;
            itemIter = mIter->next;
            if (ctx->type->onDeleteItem) ctx->type->onDeleteItem(toDelete, &ctx->userCtx);
        }
        nextSegHdr = itemIter;

        if (seg + 1 < numSegs)
            itemIter = ((NextSegHdr *) nextSegHdr)->head;
    }

}

static void _ebPrint(ebuckets eb, EbucketsType *type, int64_t usedMem, int printItems) {
    if (ebIsEmpty(eb)) {
        printf("Empty ebuckets\n");
        return;
    }

    if (ebIsList(eb)) {
        /* mock rax segment */
        eItem head = ebGetListPtr(type, eb);
        ExpireMeta *metaHead = type->getExpireMeta(head);
        FirstSegHdr mockSeg = { head, metaHead->numItems, 1};
        if (printItems)
            ebBucketPrint(0, type, &mockSeg);
        return;
    }

    uint64_t totalItems = 0;
    uint64_t numBuckets = 0;
    uint64_t numSegments = 0;

    rax *rax = ebGetRaxPtr(eb);
    raxIterator iter;
    raxStart(&iter, rax);
    raxSeek(&iter, "^", NULL, 0);
    while (raxNext(&iter)) {
        FirstSegHdr *seg = iter.data;
        if (printItems)
            ebBucketPrint(raxKey2BucketKey(iter.key), type, seg);
        totalItems += seg->totalItems;
        numBuckets++;
        numSegments += seg->numSegs;
    }

    printf("Total number of items              : %" PRIu64 "\n", totalItems);
    printf("Total number of buckets            : %" PRIu64 "\n", numBuckets);
    printf("Total number of segments           : %" PRIu64 "\n", numSegments);
    printf("Average items per bucket           : %.2f\n",
           (double) totalItems / numBuckets);
    printf("Average items per segment          : %.2f\n",
           (double) totalItems / numSegments);
    printf("Average segments per bucket        : %.2f\n",
           (double) numSegments / numBuckets);

    if (usedMem != -1)
    {
        printf("\nEbuckets memory usage (including FirstSegHdr/NexSegHdr):\n");
        printf("Total                              : %.2f KBytes\n",
               (double) usedMem / 1024);
        printf("Average per bucket                 : %" PRIu64 " Bytes\n",
               usedMem / numBuckets);
        printf("Average per item                   : %" PRIu64 " Bytes\n",
               usedMem / totalItems);
        printf("EB_BUCKET_KEY_PRECISION            : %d\n",
               EB_BUCKET_KEY_PRECISION);
        printf("EB_SEG_MAX_ITEMS                   : %d\n",
               EB_SEG_MAX_ITEMS);
    }
    raxStop(&iter);
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

    if (ebIsList(*eb)) {
        eItem head = ebGetListPtr(type, *eb);
        eItem *pItemNext = &head;
        while ( (*pItemNext) != NULL) {
            eItem toDelete = *pItemNext;
            ExpireMeta *metaToDelete = type->getExpireMeta(toDelete);
            *pItemNext = metaToDelete->next;
            metaToDelete->trash = 1;
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

    if (ebIsEmpty(*eb))
        return 0; /* not removed */

    int res;
    if (ebIsList(*eb))
        res = ebRemoveFromList(eb, type, item);
    else  /* rax */
        res = ebRemoveFromRax(eb, type, item);

    /* if removed then mark as trash */
    if (res)
        type->getExpireMeta(item)->trash = 1;

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

    assert(expireTime <= EB_EXPIRE_TIME_MAX);

    /* Set expire-time and reset segment flags */
    ExpireMeta *itemMeta = type->getExpireMeta(item);
    ebSetMetaExpTime(itemMeta, expireTime);
    itemMeta->lastInSegment = 0;
    itemMeta->firstItemBucket = 0;
    itemMeta->lastItemBucket = 0;
    itemMeta->numItems = 0;
    itemMeta->trash = 0;

    if (ebIsList(*eb) || (ebIsEmpty(*eb))) {
        /* Try add item to list */
        if ( (res = ebAddToList(eb, type, item)) == 1) {
            /* Failed to add since list reached maximum size. Convert to rax */
            *eb = ebConvertListToRax(ebGetListPtr(type, *eb), type);
            res = ebAddToRax(eb, type, item, EB_BUCKET_KEY(expireTime));
        }
    } else {
        /* Add item to rax */
        res = ebAddToRax(eb, type, item, EB_BUCKET_KEY(expireTime));
    }

    EB_VALIDATE_STRUCTURE(*eb, type);

    return res;
}

/**
 * Performs expiration on the given ebucket, removing items that have expired.
 *
 * If all items in the data structure are expired, 'eb' will be set to NULL.
 *
 * @param eb - Pointer to the ebuckets handler, which may get updated
 * @param type - Pointer to the EbucketsType structure defining the type of ebucket.
 * @param info - Providing information about the expiration action.
 */
void ebExpire(ebuckets *eb, EbucketsType *type, ExpireInfo *info) {
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
        goto END_ACTEXP;
    }

    /* handle rax expiry */

    rax *rax = ebGetRaxPtr(*eb);
    raxIterator iter;

    raxStart(&iter, rax);

    uint64_t nowKey = EB_BUCKET_KEY(info->now);
    uint64_t itemsExpiredBefore = info->itemsExpired;

    while (1) {
        raxSeek(&iter,"^",NULL,0);
        if (!raxNext(&iter)) break;

        uint64_t bucketKey = raxKey2BucketKey(iter.key);

        FirstSegHdr *firstSegHdr = iter.data;

        /* We need to take into consideration EB_BUCKET_KEY_PRECISION. The value of
         * "info->now" will be adjusted to lookup only for all buckets with assigned
         * keys that are older than 1<<EB_BUCKET_KEY_PRECISION msec ago. That is, it
         * is needed to visit only the buckets with keys that are "<" than:
         * EB_BUCKET_KEY(info->now). */
        if (bucketKey >= nowKey) {
            /* Take care to update next expire time based on next segment to expire */
            info->nextExpireTime = ebGetMetaExpTime(
                    type->getExpireMeta(firstSegHdr->head));
            break;
        }

        /* If not managed to remove entire bucket then return */
        if (ebSegExpire(firstSegHdr, type, info, &updateList) == 0)
            break;

        raxRemove(iter.rt, iter.key, EB_KEY_SIZE, NULL);
    }

    raxStop(&iter);
    *ebRaxNumItems(rax) -= info->itemsExpired - itemsExpiredBefore;

    if(raxEOF(&iter) && (updateList == 0)) {
        raxFree(rax);
        *eb = NULL;
    }

END_ACTEXP:
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

    return;
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
    uint64_t nowKey = EB_BUCKET_KEY(now);
    raxSeek(&iter,"^",NULL,0);
    assert(raxNext(&iter)); /* must be at least one bucket */
    FirstSegHdr *currBucket = iter.data;

    while (1) {
        /* if 'currBucket' is last bucket, then break */
        if(!raxNext(&iter)) break;
        FirstSegHdr *nextBucket = iter.data;

        /* if 'nextBucket' is not less than now then break */
        if (raxKey2BucketKey(iter.key) >= nowKey) break;

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
    if (EB_BUCKET_KEY_PRECISION == 0) {
        if (ebGetMetaExpTime(type->getExpireMeta(currBucket->head)) >= now)
            return numExpired;
        else
            return numExpired + currBucket->totalItems;
    }

    /* Iterate extended-segment and count expired ones */

    /* Unreachable code, provided for completeness. Following operation is not
     * bound in time and this is the main reason why we set above
     * EB_BUCKET_KEY_PRECISION to 0 and have early return on previous condition */

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
 * @param eb - The ebucket to be checked.
 * @param type - Pointer to the EbucketsType structure defining the type of ebucket.
 *
 * @return The expiration time of the item with the nearest expiration time in
 *         the ebucket. If empty, return EB_EXPIRE_TIME_INVALID. If ebuckets is
 *         of type rax and minimal bucket is extended-segment, then it might not
 *         return accurate result up-to 1<<EB_BUCKET_KEY_PRECISION-1 msec (we
 *         don't want to traverse the entire extended-segment since it might not
 *         bounded).
 */
uint64_t ebGetNextTimeToExpire(ebuckets eb, EbucketsType *type) {
    if (ebIsEmpty(eb))
        return EB_EXPIRE_TIME_INVALID;

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
    if ((firstSegHdr->numSegs == 1) || (EB_BUCKET_KEY_PRECISION == 0)) {
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
        minExpire = expTime | ( (1<<EB_BUCKET_KEY_PRECISION)-1) ;
    }
    raxStop(&iter);
    return minExpire;
}

/**
 * Retrieves the expiration time of the item with the latest expiration
 *
 * However, precision loss (EB_BUCKET_KEY_PRECISION) in rax tree buckets
 * may result in slight inaccuracies, up to a variation of
 * 1<<EB_BUCKET_KEY_PRECISION msec.
 *
 * @param eb - The ebucket to be checked.
 * @param type - Pointer to the EbucketsType structure defining the type of ebucket.
 * @param accurate - If 1, then the function will return accurate result. Otherwise,
 *                   it might return the upper limit with slight inaccuracy of
 *                   1<<EB_BUCKET_KEY_PRECISION msec.
 *
 *                   This special case is relevant only when the last bucket
 *                   is of type extended-segment. In this case, we might don't
 *                   want to traverse the entire bucket to find the accurate
 *                   expiration time  since there might be unbounded number of
 *                   items in the extended-segment. If EB_BUCKET_KEY_PRECISION
 *                   is 0, then the function will return accurate result anyway.
 *
 * @return The expiration time of the item with the latest expiration time in
 *         the ebucket. If empty, return EB_EXPIRE_TIME_INVALID.
 */
uint64_t ebGetMaxExpireTime(ebuckets eb, EbucketsType *type, int accurate) {
    if (ebIsEmpty(eb))
        return EB_EXPIRE_TIME_INVALID;

    if (ebIsList(eb)) {
        eItem item = ebGetListPtr(type, eb);
        ExpireMeta *em = type->getExpireMeta(item);
        while (em->lastInSegment == 0)
            em = type->getExpireMeta(em->next);
        return ebGetMetaExpTime(em);
    }

    /* rax */
    uint64_t maxExpire;
    rax *rax = ebGetRaxPtr(eb);
    raxIterator iter;
    raxStart(&iter, rax);
    raxSeek(&iter, "$", NULL, 0);
    raxNext(&iter); /* seek to the last bucket */
    FirstSegHdr *firstSegHdr = iter.data;
    if (firstSegHdr->numSegs == 1) {
        /* Single segment. return the last item with the highest expiration time */
        ExpireMeta *em = type->getExpireMeta(firstSegHdr->head);
        while (em->lastInSegment == 0)
            em = type->getExpireMeta(em->next);
        maxExpire = ebGetMetaExpTime(em);
    } else if (EB_BUCKET_KEY_PRECISION == 0) {
        /* Extended-segments that all have same expiration time */
        maxExpire = ebGetMetaExpTime(type->getExpireMeta(firstSegHdr->head));
    } else {
        if (accurate == 0) {
            /* return upper limit of the last bucket */
            int mask = (1<<EB_BUCKET_KEY_PRECISION)-1;
            uint64_t expTime = ebGetMetaExpTime(type->getExpireMeta(firstSegHdr->head));
            maxExpire = (expTime + (mask+1)) & (~mask);
        } else {
            maxExpire = 0;
            ExpireMeta *mIter = type->getExpireMeta(firstSegHdr->head);
            while(1) {
                while(1) {
                    if (maxExpire < ebGetMetaExpTime(mIter))
                        maxExpire = ebGetMetaExpTime(mIter);
                    if (mIter->lastInSegment == 1) break;
                    mIter = type->getExpireMeta(mIter->next);
                }

                if (mIter->lastItemBucket) break;
                mIter = type->getExpireMeta(((NextSegHdr *) mIter->next)->head);
            }
        }
    }
    raxStop(&iter);
    return maxExpire;
}

/**
 * Retrieves the total number of items in the ebucket.
 */
uint64_t ebGetTotalItems(ebuckets eb, EbucketsType *type) {
    if (ebIsEmpty(eb))
        return 0;

    if (ebIsList(eb))
        return type->getExpireMeta(ebGetListPtr(type, eb))->numItems;
    else
        return *ebRaxNumItems(ebGetRaxPtr(eb));
}

/* print expiration-time of items, ebuckets layout and some statistics */
void ebPrint(ebuckets eb, EbucketsType *type) {
    _ebPrint(eb, type, -1, 1);
}

/* Validate the general structure of ebuckets. Calls assert(0) on error. */
void ebValidate(ebuckets eb, EbucketsType *type) {
    if (ebIsEmpty(eb))
        return;

    if (ebIsList(eb))
        ebValidateList(ebGetListPtr(type, eb), type);
    else
        ebValidateRax(ebGetRaxPtr(eb), type);
}

/* Reallocates the memory used by the item using the provided allocation function.
 * This feature was added for the active defrag feature.
 *
 * The 'defragfn' callbacks are called with a pointer to memory that callback
 * can reallocate. The callbacks should return a new memory address or NULL,
 * where NULL means that no reallocation happened and the old memory is still valid.
 * 
 * Note: It is the caller's responsibility to ensure that the item has a valid expire time. */
eItem ebDefragItem(ebuckets *eb, EbucketsType *type, eItem item, ebDefragFunction *defragfn) {
    assert(!ebIsEmpty(*eb));
    if (ebIsList(*eb)) {
        ExpireMeta *prevem = NULL;
        eItem curitem = ebGetListPtr(type, *eb);
        while (curitem != NULL) {
            if (curitem == item) {
                if ((curitem = defragfn(curitem))) {
                    if (prevem)
                        prevem->next = curitem;
                    else
                        *eb = ebMarkAsList(curitem);
                }
                return curitem;
            }

            /* Move to the next item in the list. */
            prevem = type->getExpireMeta(curitem);
            curitem = prevem->next;
        }
    } else {
        CommonSegHdr *currHdr;
        ExpireMeta *mIter = type->getExpireMeta(item);
        assert(mIter->trash != 1);
        while (mIter->lastInSegment == 0)
            mIter = type->getExpireMeta(mIter->next);

        if (mIter->lastItemBucket)
            currHdr = (CommonSegHdr *) mIter->next;
        else  
            currHdr = (CommonSegHdr *) ((NextSegHdr *) mIter->next)->prevSeg;
        /* If the item is the first in the segment, then update the segment header */
        if (currHdr->head == item) {
            if ((item = defragfn(item))) {
                currHdr->head = item;
            }
            return item;
        }

        /* Iterate over all items in the segment until the next is 'item' */
        ExpireMeta *mHead = type->getExpireMeta(currHdr->head);
        mIter = mHead;
        while (mIter->next != item)
            mIter = type->getExpireMeta(mIter->next);
        assert(mIter->next == item);

        if ((item = defragfn(item))) {
            mIter->next = item;
        }
        return item;
    }
    redis_unreachable();
}

/* Retrieves the expiration time associated with the given item. If associated
 * ExpireMeta is marked as trash, then return EB_EXPIRE_TIME_INVALID */
uint64_t ebGetExpireTime(EbucketsType *type, eItem item) {
    ExpireMeta *meta = type->getExpireMeta(item);
    if (unlikely(meta->trash)) return EB_EXPIRE_TIME_INVALID;
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
    if (iter->isRax)
        raxStop(&iter->raxIter);
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

ExpireMeta *getMyItemExpireMeta(const eItem item) {
    return &((MyItem *)item)->mexpire;
}

ExpireAction expireItemCb(void *item, eItem ctx);
void deleteItemCb(eItem item, void *ctx);
EbucketsType myEbucketsType = {
    .getExpireMeta = getMyItemExpireMeta,
    .onDeleteItem = deleteItemCb,
    .itemsAddrAreOdd = 0,
};

EbucketsType myEbucketsType2 = {
    .getExpireMeta = getMyItemExpireMeta,
    .onDeleteItem = NULL,
    .itemsAddrAreOdd = 0,
};

/* XOR over all items time-expiration. Must be 0 after all addition/removal */
uint64_t expItemsHashValue = 0;

ExpireAction expireItemCb(eItem item, void *ctx) {
    ExpireMeta *meta = myEbucketsType.getExpireMeta(item);
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
        ebAdd(eb, &myEbucketsType, item, expireTime);
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
        uint64_t endRange = EB_BUCKET_EXP_TIME(expireRanges[i]);
        for (int j = 0; j < ItemsPerRange[i]; j++) {
            uint64_t randomExpirey = (rand() % (endRange - startRange)) + startRange;
            expItemsHashValue = expItemsHashValue ^ (uint32_t) randomExpirey;
            MyItem *item = zmalloc(sizeof(MyItem));
            getMyItemExpireMeta(item)->next = listOfItems;
            listOfItems = item;
            ebSetMetaExpTime(getMyItemExpireMeta(item), randomExpirey);
        }
        startRange = EB_BUCKET_EXP_TIME(expireRanges[i]); /* next start range */
    }

    /* Take to sample memory after all items allocated and before insertion to ebuckets */
    size_t  usedMemBefore =  zmalloc_used_memory();

    gettimeofday(&timeBefore, NULL);
    while (listOfItems) {
        MyItem *item = (MyItem *)listOfItems;
        listOfItems = getMyItemExpireMeta(item)->next;
        uint64_t expireTime = ebGetMetaExpTime(&item->mexpire);
        ebAdd(&eb, &myEbucketsType, item, expireTime);
    }
    gettimeofday(&timeAfter, NULL);
    timersub(&timeAfter, &timeBefore, &timeCreation);

    gettimeofday(&timeBefore, NULL);
    ebExpireDryRun(eb, &myEbucketsType, 0xFFFFFFFFFFFF);  /* expire dry-run all */
    gettimeofday(&timeAfter, NULL);
    timersub(&timeAfter, &timeBefore, &timeDryRun);

    if (printStat) {
        _ebPrint(eb, &myEbucketsType, zmalloc_used_memory() - usedMemBefore, 0);
    }

    gettimeofday(&timeBefore, NULL);
    if (isExpire) {
        startRange = lowestTime;
        /* Active expire according to the ranges */
        for (int i = 0 ; i < numRanges ; i++) {

            /* When checking how many items are expired, we need to take into
             * consideration EB_BUCKET_KEY_PRECISION. The value of "info->now"
             * will be adjusted by ebActiveExpire() to lookup only for all buckets
             * with assigned keys that are older than 1<<EB_BUCKET_KEY_PRECISION
             * msec ago. That is, it is needed to visit only the buckets with keys
             * that are "<" EB_BUCKET_KEY(info->now) and not "<=".
             * But if there is a list behind ebuckets, then this limitation is not
             * applied and the operator "<=" will be used instead.
             *
             * The '-1' in case of list brings makes both cases aligned to have
             * same result */
            uint64_t now = EB_BUCKET_EXP_TIME(expireRanges[i]) + (ebIsList(eb) ? -1 : 0);

            TimeRange range = {EB_BUCKET_EXP_TIME(startRange), EB_BUCKET_EXP_TIME(expireRanges[i]) };
            ExpireInfo info = {
                    .maxToExpire = 0xFFFFFFFF,
                    .onExpireItem = expireItemCb,
                    .ctx = &range,
                    .now = now,
                    .itemsExpired = 0};

            ebExpire(&eb, &myEbucketsType, &info);

            assert( (eb==NULL && (i + 1 == numRanges)) || (eb!=NULL && (i + 1 < numRanges)) );
            assert( info.itemsExpired == (uint64_t) ItemsPerRange[i]);
            startRange = expireRanges[i];
        }
        assert(eb == NULL);
        assert( (expItemsHashValue & 0xFFFFFFFF) == 0);
    }
    ebDestroy(&eb, &myEbucketsType, NULL);
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

eItem defragCallback(const eItem item) {
    size_t size = zmalloc_usable_size(item);
    eItem newitem = zmalloc(size);
    memcpy(newitem, item, size);
    zfree(item);
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
            expireRanges[j] = expireRanges[j] >> EB_BUCKET_KEY_PRECISION;
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
                ebAdd(&eb, &myEbucketsType, items[i], i);
            }

            /* iterate items */
            ebStart(&iter, eb, &myEbucketsType);
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
            ebStart(&iter, eb, &myEbucketsType);
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
            ebDestroy(&eb, &myEbucketsType, NULL);
        }
    }

    TEST("list - Create a single item, get TTL, and remove") {
        MyItem *singleItem = zmalloc(sizeof(MyItem));
        ebuckets eb = NULL;
        ebAdd(&eb, &myEbucketsType, singleItem, 1000);
        assert(ebGetExpireTime(&myEbucketsType, singleItem) == 1000 );

        /* remove the item */
        assert(ebRemove(&eb, &myEbucketsType, singleItem));
        /* now the ebuckets is empty */
        assert(ebRemove(&eb, &myEbucketsType, singleItem) == 0);

        zfree(singleItem);

        ebDestroy(&eb, &myEbucketsType, NULL);
    }

    TEST("list - Create few items on different times, get TTL, and then remove") {
        MyItem *items[EB_LIST_MAX_ITEMS];
        ebuckets eb = NULL;
        for (int i = 0 ; i < EB_LIST_MAX_ITEMS  ; i++) {
            items[i] = zmalloc(sizeof(MyItem));
            ebAdd(&eb, &myEbucketsType, items[i], i);
        }

        for (uint64_t i = 0 ; i < EB_LIST_MAX_ITEMS ; i++) {
            assert(ebGetExpireTime(&myEbucketsType, items[i]) == i );
            assert(ebRemove(&eb, &myEbucketsType, items[i]));
        }

        for (int i = 0 ; i < EB_LIST_MAX_ITEMS  ; i++)
            zfree(items[i]);

        ebDestroy(&eb, &myEbucketsType, NULL);
    }

    TEST("list - Create few items on different times, get TTL, and then delete") {
        MyItem *items[EB_LIST_MAX_ITEMS];
        ebuckets eb = NULL;
        for (int i = 0 ; i < EB_LIST_MAX_ITEMS  ; i++) {
            items[i] = zmalloc(sizeof(MyItem));
            ebAdd(&eb, &myEbucketsType, items[i], i);
        }

        for (uint64_t i = 0 ; i < EB_LIST_MAX_ITEMS ; i++) {
            assert(ebGetExpireTime(&myEbucketsType, items[i]) == i );
        }

        ebDestroy(&eb, &myEbucketsType, NULL);
    }

    TEST_COND("ebuckets - Add items with increased/decreased expiration time and then expire",
              EB_BUCKET_KEY_PRECISION > 0)
    {
        ebuckets eb = NULL;

        for (int isDecr = 0; isDecr < 2; ++isDecr) {
            for (uint32_t numItems = 1; numItems < 64; ++numItems) {
                uint64_t step = 1 << EB_BUCKET_KEY_PRECISION;

                if (isDecr == 0)
                    addItems(&eb, 0, step, numItems, NULL);
                else
                    addItems(&eb, (numItems - 1) * step, -step, numItems, NULL);

                for (uint32_t i = 1; i <= numItems; i++) {
                    TimeRange range = {EB_BUCKET_EXP_TIME(i - 1), EB_BUCKET_EXP_TIME(i)};
                    ExpireInfo info = {
                            .maxToExpire = 1,
                            .onExpireItem = expireItemCb,
                            .ctx = &range,
                            .now = EB_BUCKET_EXP_TIME(i),
                            .itemsExpired = 0};

                    ebExpire(&eb, &myEbucketsType, &info);
                    assert(info.itemsExpired == 1);
                    if (i == numItems) { /* if last item */
                        assert(eb == NULL);
                        assert(info.nextExpireTime == EB_EXPIRE_TIME_INVALID);
                    } else {
                        assert(info.nextExpireTime == EB_BUCKET_EXP_TIME(i));
                    }
                }
            }
        }
    }

    TEST_COND("ebuckets - Create items with same expiration time and then expire",
              EB_BUCKET_KEY_PRECISION > 0)
    {
        ebuckets eb = NULL;
        uint64_t expirePerIter = 2;
        for (uint32_t numIterations = 1; numIterations < 100; ++numIterations) {
            uint32_t numItems = numIterations * expirePerIter;
            uint64_t expireTime = (1 << EB_BUCKET_KEY_PRECISION) + 1;
            addItems(&eb, expireTime, 0, numItems, NULL);

            for (uint32_t i = 1; i <= numIterations; i++) {
                ExpireInfo info = {
                        .maxToExpire = expirePerIter,
                        .onExpireItem = expireItemCb,
                        .ctx = NULL,
                        .now = (2 << EB_BUCKET_KEY_PRECISION),
                        .itemsExpired = 0};
                ebExpire(&eb, &myEbucketsType, &info);
                assert(info.itemsExpired == expirePerIter);
                if (i == numIterations) { /* if last item */
                    assert(eb == NULL);
                    assert(info.nextExpireTime == EB_EXPIRE_TIME_INVALID);
                } else {
                    assert(info.nextExpireTime == expireTime);
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
                    uint64_t startValue = 1000 << EB_BUCKET_KEY_PRECISION;
                    int stepValue = step * (1 << EB_BUCKET_KEY_PRECISION);
                    addItems(&eb, startValue, stepValue, numItems, items);
                    for (int i = 0; i < numItems; i++) {
                        int at = (i + offset) % numItems;
                        assert(ebRemove(&eb, &myEbucketsType, items[at]));
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
                ebAdd(&eb, &myEbucketsType2, items + i, expireTime);
                assert(ebGetNextTimeToExpire(eb, &myEbucketsType2) == minExpTime);
                assert(ebGetMaxExpireTime(eb, &myEbucketsType2, 0) == maxExpTime);
            }
            ebDestroy(&eb, &myEbucketsType2, NULL);
        }
    }

    TEST_COND("ebuckets - test min/max expire time, with extended-segment",
              (1<<EB_BUCKET_KEY_PRECISION) > 2*EB_SEG_MAX_ITEMS) {
        ebuckets eb = NULL;
        MyItem items[(2*EB_SEG_MAX_ITEMS)-1];
        for (int numItems = EB_SEG_MAX_ITEMS+1 ; numItems < (int)ARRAY_SIZE(items) ; numItems++) {
            /* First reach extended-segment (two chained segments in a bucket) */
            for (int i = 0; i <= EB_SEG_MAX_ITEMS; i++) {
                uint64_t itemExpireTime = (1<<EB_BUCKET_KEY_PRECISION) + i;
                ebAdd(&eb, &myEbucketsType2, items + i, itemExpireTime);
            }

            /* Now start adding more items to extended-segment and verify min/max */
            for (int i = EB_SEG_MAX_ITEMS+1; i < numItems; i++) {
                uint64_t itemExpireTime = (1<<EB_BUCKET_KEY_PRECISION) + i;
                ebAdd(&eb, &myEbucketsType2, items + i, itemExpireTime);
                assert(ebGetNextTimeToExpire(eb, &myEbucketsType2) == (uint64_t)(2<<EB_BUCKET_KEY_PRECISION));
                assert(ebGetMaxExpireTime(eb, &myEbucketsType2, 0) == (uint64_t)(2<<EB_BUCKET_KEY_PRECISION));
                assert(ebGetMaxExpireTime(eb, &myEbucketsType2, 1) == (uint64_t)((1<<EB_BUCKET_KEY_PRECISION) + i));
            }
            ebDestroy(&eb, &myEbucketsType2, NULL);
        }
    }

    TEST("ebuckets - active-expire dry-run") {
        ebuckets eb = NULL;
        MyItem items[2*EB_SEG_MAX_ITEMS];

        for (int numItems = 1 ; numItems < (int)ARRAY_SIZE(items) ; numItems++) {
            int maxExpireKey = (numItems % 2) ? 40 : 2;
            /* Allocate numItems and add to ebuckets */
            for (int i = 0; i < numItems; i++) {
                /* generate random expiration time */
                uint64_t expireTime = (rand() % maxExpireKey) << EB_BUCKET_KEY_PRECISION;
                ebAdd(&eb, &myEbucketsType2, items + i, expireTime);
            }

            for (int i = 0 ; i <= maxExpireKey ; ++i) {
                uint64_t now = i << EB_BUCKET_KEY_PRECISION;

                /* Count how much items are expired */
                uint64_t expectedNumExpired = 0;
                for (int j = 0; j < numItems; j++) {
                    if (ebGetExpireTime(&myEbucketsType2, items + j) < now)
                        expectedNumExpired++;
                }
                /* Perform dry-run and verify number of expired items */
                assert(ebExpireDryRun(eb, &myEbucketsType2, now) == expectedNumExpired);
            }
            ebDestroy(&eb, &myEbucketsType2, NULL);
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
            ebAdd(&eb, &myEbucketsType2, items + i, expiredAt << EB_BUCKET_KEY_PRECISION);

        /* active-expire. Expected that all but one will be expired */
        ExpireInfo info = {
                .maxToExpire = 0xFFFFFFFF,
                .onExpireItem = expireUpdateThirdItemCb,
                .ctx = (void *) (uintptr_t) (updateItemTo << EB_BUCKET_KEY_PRECISION),
                .now = applyActiveExpireAt << EB_BUCKET_KEY_PRECISION,
                .itemsExpired = 0};
        ebExpire(&eb, &myEbucketsType2, &info);
        assert(info.itemsExpired == (uint64_t) numItems);
        assert(info.nextExpireTime == (uint64_t)updateItemTo << EB_BUCKET_KEY_PRECISION);
        assert(ebGetTotalItems(eb, &myEbucketsType2) == 1);

        /* active-expire. Expected that all will be expired */
        ExpireInfo info2 = {
                .maxToExpire = 0xFFFFFFFF,
                .onExpireItem = expireUpdateThirdItemCb,
                .ctx = (void *) (uintptr_t) (updateItemTo << EB_BUCKET_KEY_PRECISION),
                .now = expectedExpiredAt << EB_BUCKET_KEY_PRECISION,
                .itemsExpired = 0};
        ebExpire(&eb, &myEbucketsType2, &info2);
        assert(info2.itemsExpired == (uint64_t) 1);
        assert(info2.nextExpireTime == EB_EXPIRE_TIME_INVALID);
        assert(ebGetTotalItems(eb, &myEbucketsType2) == 0);

        ebDestroy(&eb, &myEbucketsType2, NULL);

    }

    TEST("item defragmentation") {
        for (int s = 1; s <= EB_LIST_MAX_ITEMS * 3; s++) {
            ebuckets eb = NULL;
            MyItem *items[s];
            for (int i = 0; i < s; i++) {
                items[i] = zmalloc(sizeof(MyItem));
                items[i]->index = i;
                ebAdd(&eb, &myEbucketsType, items[i], i);
            }
            assert((s <= EB_LIST_MAX_ITEMS) ? ebIsList(eb) : !ebIsList(eb));
            /* Defrag all the items. */
            for (int i = 0; i < s; i++) {
                MyItem *newitem = ebDefragItem(&eb, &myEbucketsType, items[i], defragCallback);
                if (newitem) items[i] = newitem;
            }
            /* Verify that the data is not corrupted. */
            ebValidate(eb, &myEbucketsType);
            for (int i = 0; i < s; i++)
                assert(items[i]->index == i);
            ebDestroy(&eb, &myEbucketsType, NULL);
        }
    }

//    TEST("segment - Add smaller item to full segment that all share same ebucket-key")
//    TEST("segment - Add item to full segment and make it extended-segment (all share same ebucket-key)")
//    TEST("ebuckets - Create rax tree with extended-segment and add item before")

    return 0;
}

#endif
