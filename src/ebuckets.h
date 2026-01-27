/*
 * Copyright Redis Ltd. 2024 - present
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 *
 * WHAT IS EBUCKETS?
 * -----------------
 * ebuckets is being used to store items that are set with expiration-time. It
 * supports the basic API of add, remove and active expiration. The implementation
 * of it is based on rax-tree, or plain linked-list when small. The expiration time
 * of the items are used as the key to traverse rax-tree.
 *
 * Instead of holding a distinct item in each leaf of the rax-tree we can aggregate
 * items into small segments and hold it in each leaf. This way we can  avoid
 * frequent modification of the rax-tree, since many of the modifications
 * will be done only at the segment level. It will also save memory because
 * rax-tree can be costly, around 40 bytes per leaf (with rax-key limited to 6
 * bytes). Whereas each additional item in the segment will cost the size of the
 * 'next' pointer in a list (8 bytes) and few more bytes for maintenance of the
 * segment.
 *
 * EBUCKETS STRUCTURE
 * ------------------
 * The ebuckets data structure is organized in a hierarchical manner as follows:
 *
 * 1. ebuckets: This is the top-level data structure. It can be either a rax tree
 *    or a plain linked list. It contains one or more buckets, each representing
 *    an interval in time.
 *
 * 2. bucket: Each bucket represents an interval in time and contains one or more
 *    segments. The key in the rax-tree for each bucket represents low
 *    bound expiration-time for the items within this bucket. The key of the
 *    following bucket represents the upper bound expiration-time.
 *
 * 3. segment: Each segment within a bucket can hold up to `EB_SEG_MAX_ITEMS`
 *    items as a linked list. If there are more, the segment will try to
 *    split the bucket. To avoid wasting memory, it is a singly linked list (only
 *    next-item pointer). It is a cyclic linked-list to allow efficient removal of
 *    items from the middle of the segment without traversing the rax tree.
 *
 * 4. item: Each item that is stored in ebuckets should embed the ExpireMeta
 *    struct and supply getter function (see EbucketsType.getExpireMeta). This
 *    struct holds the expire-time of the item and few more fields that are used
 *    to maintain the segments data-structure.
 *
 * SPLITTING BUCKET
 * ----------------
 * Each segment can hold up-to `EB_SEG_MAX_ITEMS` items. On insertion of new
 * item, it will try to split the segment. Here is an example For adding item
 * with expiration of 42 to a segment that already reached its maximum capacity
 * which will cause to split of the segment and in turn split of the bucket as
 * well to a finer grained ranges:
 *
 *       BUCKETS                             BUCKETS
 *      [ 00-10 ] -> size(Seg0) = 11   ==>  [ 00-10 ] -> size(Seg0) = 11
 *      [ 11-76 ] -> size(Seg1) = 16        [ 11-36 ] -> size(Seg1) = 9
 *                                          [ 37-76 ] -> size(Seg2) = 7
 *
 * EXTENDING BUCKET
 * ----------------
 * In the example above, the reason it wasn't split evenly is that Seg1 must have
 * been holding items with same TTL and they must reside together in the same
 * bucket after the split. Which brings us to another important point. If there
 * is a segment that reached its maximum capacity and all the items have same
 * expiration-time key, then we cannot split the bucket but aggregate all the
 * items, with same expiration time key, by allocating an extended-segment and
 * chain it to the first segment in visited bucket. In that sense, extended
 * segments will only hold items with same expiration-time key.
 *
 *       BUCKETS                            BUCKETS
 *      [ 00-10 ] -> size(Seg0)=11   ==>   [ 00-10 ] -> size(Seg0)=11
 *      [ 11-12 ] -> size(Seg1)=16         [ 11-12 ] -> size(Seg1)=1 -> size(Seg2)=16
 *
 * LIMITING RAX TREE DEPTH
 * -----------------------
 * The rax tree is basically a B-tree and its depth is bounded by the sizeof of
 * the key. Holding 6 bytes for expiration-time key is more than enough to represent
 * unix-time in msec, and in turn the depth of the tree is limited to 6 levels.
 * At a first glance it might look sufficient but we need take into consideration
 * the heavyweight maintenance and traversal of each node in the B-tree.
 *
 * And so, we can further prune the tree such that holding keys with msec precision
 * in the tree doesn't bring with it much value. The active-expiration operation can
 * live with deletion of expired items, say, older than 1 sec, which means the size
 * of time-expiration keys to the rax tree become no more than ~4.5 bytes and we
 * also get rid of the "noisy" bits which most probably will cause to yet another
 * branching and modification of the rax tree in case of items with time-expiration
 * difference of less than 1 second. The lazy expiration will still be precise and
 * without compromise on accuracy because the exact expiration-time is kept
 * attached as well to each item, in `ExpireMeta`, and each traversal of item with
 * expiration will behave as expected down to the msec. Take care to configure
 * `EB_BUCKET_KEY_PRECISION` according to your needs.
 *
 * EBUCKET KEY
 * -----------
 * Taking into account configured value of `EB_BUCKET_KEY_PRECISION`, two items
 * with expiration-time t1 and t2 will be considered to have the same key in the
 * rax-tree/buckets if and only if:
 *
 *              EB_BUCKET_KEY(t1) == EB_BUCKET_KEY(t2)
 *
 * EBUCKETS CREATION
 * -----------------
 * To avoid the cost of allocating rax data-structure for only few elements,
 * ebuckets will start as a simple linked-list and only when it reaches some
 * threshold, it will be converted to rax.
 *
 * TODO
 * ----
 * - ebRemove() optimize to merge small segments into one segment.
 * - ebAdd() Fix pathological case of cascade addition of items into rax such
 *   that their values are smaller/bigger than visited extended-segment which ends
 *   up with multiple segments with a single item in each segment.
 */

#ifndef __EBUCKETS_H
#define __EBUCKETS_H

#include <stdlib.h>
#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>
#include "rax.h"

/*
 * EB_BUCKET_KEY_PRECISION - Defines the number of bits to ignore from the
 * expiration-time when mapping to buckets. The higher the value, the more items
 * with similar expiration-time will be aggregated into the same bucket. The lower
 * the value, the more "accurate" the active expiration of buckets will be.
 *
 * Note that the accurate time expiration of each item is preserved anyway and
 * enforced by lazy expiration. It only impacts the active expiration that will
 * be able to work on buckets older than (1<<EB_BUCKET_KEY_PRECISION) msec ago.
 * For example if EB_BUCKET_KEY_PRECISION is 10, then active expiration
 * will work only on buckets that already got expired at least 1sec ago.
 *
 * The idea of it is to trim the rax tree depth, avoid having too many branches,
 * and reduce frequent modifications of the tree to the minimum.
 */
#define EB_BUCKET_KEY_PRECISION 0   /* TBD: modify to 10 */

/* From expiration time to bucket-key */
#define EB_BUCKET_KEY(exptime) ((exptime) >> EB_BUCKET_KEY_PRECISION)


#define EB_EXPIRE_TIME_MAX     ((uint64_t)0x0000FFFFFFFFFFFF) /* Maximum expire-time. */
#define EB_EXPIRE_TIME_INVALID (EB_EXPIRE_TIME_MAX+1) /* assumed bigger than max */

/* Handler to ebuckets DS. Pointer to a list, rax or NULL (empty DS). See also ebIsList(). */
typedef void *ebuckets;

/* Users of ebuckets will store `eItem` which is just a void pointer to their
 * element. In addition, eItem should embed the ExpireMeta struct and supply
 * getter function (see EbucketsType.getExpireMeta).
 */
typedef void *eItem;

/* This struct Should be embedded inside `eItem` and must be aligned in memory. */
typedef struct ExpireMeta {
    /* 48bits of unix-time in msec.  This value is sufficient to represent, in
     * unix-time, until the date of 02 August, 10889
     */
    uint32_t expireTimeLo;              /* Low bits of expireTime. */
    uint16_t expireTimeHi;              /* High bits of expireTime. */

    unsigned int lastInSegment    : 1;  /* Last item in segment. If set, then 'next' will
                                           point to the NextSegHdr, unless lastItemBucket=1
                                           then it will point to segment header of the
                                           current segment. */
    unsigned int firstItemBucket  : 1;  /* First item in bucket. This flag assist
                                           to manipulate segments directly without
                                           the need to traverse from start the
                                           rax tree  */
    unsigned int lastItemBucket   : 1;  /* Last item in bucket. This flag assist
                                           to manipulate segments directly without
                                           the need to traverse from start the
                                           rax tree  */
    unsigned int numItems         : 5;  /* Only first item in segment will maintain
                                           this value. */

    unsigned int trash            : 1;  /* This flag indicates whether the ExpireMeta
                                           associated with the item is leftover.
                                           There is always a potential to reuse the
                                           item after removal/deletion. Note that,
                                           the user can still safely O(1) TTL lookup
                                           a given item and verify whether attached
                                           TTL is valid or leftover. See function
                                           ebGetExpireTime(). */

    unsigned int userData         : 3;  /* ebuckets can be used to store in same
                                           instance few different types of items,
                                           such as, listpack and hash. This field
                                           is reserved to store such identification
                                           associated with the item and can help
                                           to distinct on delete or expire callback.
                                           It is not used by ebuckets internally and
                                           should be maintained by the user */

    unsigned int reserved         : 4;

    void *next;                       /* - If not last item in segment then next
                                           points to next eItem (lastInSegment=0).
                                         - If last in segment but not last in
                                           bucket (lastItemBucket=0) then it
                                           points to next segment header.
                                         - If last in bucket then it points to
                                           current segment header (Can be either
                                           of type FirstSegHdr or NextSegHdr). */
} ExpireMeta;

/* Each instance of ebuckets need to have corresponding EbucketsType that holds
 * the necessary callbacks and configuration to operate correctly on the type
 * of items that are stored in it. Conceptually it should have hold reference
 * from ebuckets instance to this type, but to save memory we will pass it as
 * an argument to each API call. */
typedef struct EbucketsType {
    /* getter to extract the ExpireMeta from the item */
    ExpireMeta* (*getExpireMeta)(const eItem item);

    /* Called during ebDestroy(). Set to NULL if not needed. */
    void (*onDeleteItem)(eItem item, void *ctx);

    /* Is addresses of items are odd in memory. It is taken into consideration
     * and used by ebuckets to know how to distinct between ebuckets pointer to
     * rax versus a pointer to item which is head of list. */
    unsigned int itemsAddrAreOdd;
} EbucketsType;

/* Returned value by `onExpireItem` callback to indicate the action to be taken by
 * ebExpire(). */
typedef enum ExpireAction {
    ACT_REMOVE_EXP_ITEM=0,      /* Remove the item from ebuckets. */
    ACT_UPDATE_EXP_ITEM,        /* Re-insert the item with updated expiration-time.
                                   Before returning this value, the cb need to
                                   update expiration time of the item by assisting
                                   function ebSetMetaExpTime(). The item will be
                                   kept aside and will be added again to ebuckets
                                   at the end of ebExpire() */
    ACT_STOP_ACTIVE_EXP         /* Stop active-expiration. It will assume that
                                   provided 'item' wasn't deleted by the callback. */
} ExpireAction;

/* ExpireInfo is used to pass input and output parameters to ebExpire(). */
typedef struct ExpireInfo {
    /* onExpireItem - Called during active-expiration by ebExpire() */
    ExpireAction (*onExpireItem)(eItem item, void *ctx);

    uint64_t maxToExpire;         /* [INPUT ] Limit of number expired items to scan */
    void *ctx;                    /* [INPUT ] context to pass to onExpireItem */
    uint64_t now;                 /* [INPUT ] Current time in msec. */
    uint64_t itemsExpired;        /* [OUTPUT] Returns the number of expired or updated items. */
    uint64_t nextExpireTime;      /* [OUTPUT] Next expiration time. Returns
                                     EB_EXPIRE_TIME_INVALID if none left. */
} ExpireInfo;

/* Iterator to traverse ebuckets items */
typedef struct EbucketsIterator {
    /* private data of iterator */
    ebuckets eb;
    EbucketsType *type;
    raxIterator raxIter;
    int isRax;

    /* public read only */
    eItem currItem;               /* Current item ref. Use ebGetMetaExpTime()
                                     on `currItem` to get expiration time.*/
    uint64_t itemsCurrBucket;     /* Number of items in current bucket. */
} EbucketsIterator;

typedef void *(ebDefragAllocFunction)(void *ptr);
typedef void *(ebDefragAllocItemFunction)(void *ptr, void *privdata);
typedef struct {
    ebDefragAllocFunction *defragAlloc; /* Used for rax nodes, segment etc. */
    ebDefragAllocItemFunction *defragItem;  /* Defrag-realloc eitem */
} ebDefragFunctions;

/* ebuckets API */

static inline ebuckets ebCreate(void) { return NULL; } /* Empty ebuckets */

void ebDestroy(ebuckets *eb, EbucketsType *type, void *deletedItemsCbCtx);

void ebExpire(ebuckets *eb, EbucketsType *type, ExpireInfo *info);

uint64_t ebExpireDryRun(ebuckets eb, EbucketsType *type, uint64_t now);

static inline int ebIsEmpty(ebuckets eb) { return eb == NULL; }

uint64_t ebGetNextTimeToExpire(ebuckets eb, EbucketsType *type);

uint64_t ebGetMaxExpireTime(ebuckets eb, EbucketsType *type, int accurate);

uint64_t ebGetTotalItems(ebuckets eb, EbucketsType *type);

/* Item related API */

int ebRemove(ebuckets *eb, EbucketsType *type, eItem item);

int ebAdd(ebuckets *eb, EbucketsType *type, eItem item, uint64_t expireTime);

uint64_t ebGetExpireTime(EbucketsType *type, eItem item);

void ebStart(EbucketsIterator *iter, ebuckets eb, EbucketsType *type);

void ebStop(EbucketsIterator *iter);

int ebNext(EbucketsIterator *iter);

int ebNextBucket(EbucketsIterator *iter);

int ebScanDefrag(ebuckets *eb, EbucketsType *type, unsigned long *cursor,
                 ebDefragFunctions *defragfns, void *privdata);

static inline uint64_t ebGetMetaExpTime(ExpireMeta *expMeta) {
    return (((uint64_t)(expMeta)->expireTimeHi << 32) | (expMeta)->expireTimeLo);
}

static inline void ebSetMetaExpTime(ExpireMeta *expMeta, uint64_t t) {
    expMeta->expireTimeLo = (uint32_t)(t&0xFFFFFFFF);
    expMeta->expireTimeHi = (uint16_t)((t) >> 32);
}

/* Debug API */

void ebValidate(ebuckets eb, EbucketsType *type);

void ebPrint(ebuckets eb, EbucketsType *type);

#ifdef REDIS_TEST
int ebucketsTest(int argc, char *argv[], int flags);
#endif

#endif /* __EBUCKETS_H */
