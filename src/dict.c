/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, entries are stored in 64-byte
 * buckets and collisions that don't fit in a bucket are handled by chaining
 * child buckets. See the source code for more information... :)
 *
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>
#include <stddef.h>

#include "dict.h"
#include "zmalloc.h"
#include "redisassert.h"
#include "monotonic.h"
#include "atomicvar.h"
#include "util.h"

/* Using dictSetResizeEnabled() we make possible to disable
 * resizing and rehashing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to DICT_RESIZE_AVOID, not all
 * resizes are prevented:
 *  - A hash table is still allowed to expand if the ratio between the number
 *    of elements and the slots >= dict_force_resize_ratio.
 *  - A hash table is still allowed to shrink if the ratio between the number
 *    of elements and the slots <= 1 / (HASHTABLE_MIN_FILL * dict_force_resize_ratio). */
static redisAtomic dictResizeEnable dict_can_resize = DICT_RESIZE_ENABLE;
static const unsigned int dict_force_resize_ratio = 4;

/* -------------------------- types ----------------------------------------- */
struct dictEntry {
    void *key;
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
};

/* Bucket layout (64 bytes):
 *
 *   chained(1 bit) presence(SLOTS bits) hashes[SLOTS] slots[SLOTS]
 *
 * presence tells which positions hold an entry. hashes[] holds the top byte of
 * the hash of each entry and is used to rule out most non-matching entries
 * without touching them. When chained is set, the last slot is not an entry
 * but a pointer to a child bucket and its presence bit is clear. */
#if DICT_BUCKET_SLOTS == 7
#define BUCKET_BITS_TYPE uint8_t
#define BUCKET_FACTOR 5
#else
#define BUCKET_BITS_TYPE uint16_t
#define BUCKET_FACTOR 3
#endif
#define BUCKET_DIVISOR 32
#define BUCKET_BITS_TYPE_EX __extension__ BUCKET_BITS_TYPE

struct dictBucket {
    BUCKET_BITS_TYPE_EX chained : 1;
    BUCKET_BITS_TYPE_EX presence : DICT_BUCKET_SLOTS;
    uint8_t hashes[DICT_BUCKET_SLOTS];
    dictEntry *slots[DICT_BUCKET_SLOTS];
};

static_assert(sizeof(dictBucket) == DICT_BUCKET_BYTES, "dictBucket must be one cache line");

typedef BUCKET_BITS_TYPE bucketBits;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict *d);
static void _dictShrinkIfNeeded(dict *d);
static void _dictRehashStepIfNeeded(dict *d, uint64_t visitedIdx);
static signed char _dictNextExp(unsigned long size);
static int _dictInit(dict *d, dictType *type);
static int dictDefaultCompare(dictCmpCache *cache, const void *key1, const void *key2);
static dictEntryLink dictFindLinkInternal(dict *d, const void *key, uint64_t hash, dictEntryLink *bucket);
static dictEntry *dictInsertKey(dict *d, uint64_t hash, void *key __stored_key, dictEntryLink *link);

/* -------------------------- unused  --------------------------- */
void dictSetSignedIntegerVal(dictEntry *de, int64_t val);
int64_t dictGetSignedIntegerVal(const dictEntry *de);
double dictIncrDoubleVal(dictEntry *de, double val);
void *dictEntryMetadata(dictEntry *de);
int64_t dictIncrSignedIntegerVal(dictEntry *de, int64_t val);

/* -------------------------- misc inline functions -------------------------------- */

typedef int (*keyCmpFunc)(dictCmpCache *cache, const void *key1, const void *key2);
static inline keyCmpFunc dictGetCmpFunc(dict *d) {
    if (d->type->keyCompare)
        return d->type->keyCompare;
    return dictDefaultCompare;
}

static const void *dictStoredKey2Key(dict *d, const void *key __stored_key) {
    return (d->type->keyFromStoredKey) ? d->type->keyFromStoredKey(key) : key;
}

/* -------------------------- hash functions -------------------------------- */

static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed,seed,sizeof(dict_hash_function_seed));
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, size_t len) {
    return siphash(key, len, dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len) {
    return siphash_nocase(buf,len,dict_hash_function_seed);
}

/* --------------------- dictEntry pointer bit tricks ----------------------  */

/* The 3 least significant bits in a pointer to a dictEntry determines what the
 * pointer actually points to. If the least bit is set, it's a key. Otherwise,
 * the bit pattern of the least 3 significant bits mark the kind of entry. */

#define ENTRY_PTR_MASK        7 /* 111 */
#define ENTRY_PTR_NORMAL      0 /* 000 : If a pointer to an entry with value. */
#define ENTRY_PTR_IS_ODD_KEY  1 /* XX1 : If a pointer to odd key address (must be 1). */
#define ENTRY_PTR_IS_EVEN_KEY 2 /* 010 : If a pointer to even key address. (must be 2 or 4). */
#define ENTRY_PTR_UNUSED      4 /* 100 : Unused. */

/* Returns 1 if the entry pointer is a pointer to a key, rather than to an
 * allocated entry. Returns 0 otherwise. */
static inline int entryIsKey(const dictEntry *de) {
    return ((uintptr_t)de & (ENTRY_PTR_IS_ODD_KEY | ENTRY_PTR_IS_EVEN_KEY));
}

/* Returns 1 if the pointer is actually a pointer to a dictEntry struct. Returns
 * 0 otherwise. */
static inline int entryIsNormal(const dictEntry *de) {
    return ((uintptr_t)(void *)de & ENTRY_PTR_MASK) == ENTRY_PTR_NORMAL;
}

static inline dictEntry *encodeMaskedPtr(const void *ptr, unsigned int bits) {
    assert(((uintptr_t)ptr & ENTRY_PTR_MASK) == 0);
    return (dictEntry *)(void *)((uintptr_t)ptr | bits);
}

static inline void *decodeMaskedPtr(const dictEntry *de) {
    return (void *)((uintptr_t)(void *)de & ~ENTRY_PTR_MASK);
}

/* Encode a key pointer for storage in a no_value dict bucket.
 * For odd keys (like SDS strings), the key can be stored directly.
 * For even keys, we need to tag it with ENTRY_PTR_IS_EVEN_KEY. */
static inline dictEntry *encodeEntryKey(dict *d, void *key) {
    if (d->type->keys_are_odd) {
        debugAssert(((uintptr_t)key & ENTRY_PTR_IS_ODD_KEY) == ENTRY_PTR_IS_ODD_KEY);
        return key;
    } else {
        return encodeMaskedPtr(key, ENTRY_PTR_IS_EVEN_KEY);
    }
}

/* Returns 1 if the entry has a value field and 0 otherwise. */
static inline int entryHasValue(const dictEntry *de) {
    return entryIsNormal(de);
}

/* ----------------------------- bucket helpers ----------------------------- */

/* The tag stored per slot is the top byte of the hash; the bucket index uses
 * the low bits, so the two are independent for tables up to 2^56 buckets. */
static inline uint8_t hashTag(uint64_t hash) {
    return hash >> (CHAR_BIT * 7);
}

/* Returns the number of positions of a bucket that can hold an entry. */
static inline int numBucketPositions(const dictBucket *b) {
    return DICT_BUCKET_SLOTS - (b->chained ? 1 : 0);
}

static inline int bucketIsFull(const dictBucket *b) {
    return b->presence == (bucketBits)((1u << numBucketPositions(b)) - 1);
}

static inline int isPositionFilled(const dictBucket *b, int pos) {
    return b->presence & (1u << pos);
}

static inline dictBucket *getChildBucket(const dictBucket *b) {
    return b->chained ? (dictBucket *)(void *)b->slots[DICT_BUCKET_SLOTS - 1] : NULL;
}

static inline void setChildBucket(dictBucket *b, dictBucket *child) {
    b->slots[DICT_BUCKET_SLOTS - 1] = (dictEntry *)(void *)child;
}

/* A bucket with nothing in it and no child. */
static inline int bucketIsEmpty(const dictBucket *b) {
    return b->presence == 0 && !b->chained;
}

/* Returns the number of entries in a bucket chain. */
static unsigned long chainEntries(const dictBucket *b) {
    unsigned long n = 0;
    for (; b; b = getChildBucket(b)) n += __builtin_popcount(b->presence);
    return n;
}

/* Returns 1 if the table holds no entries and no child buckets, 0 otherwise.
 * Deletes while compaction is paused may leave child buckets without
 * entries. */
static inline int tableIsEmpty(dict *d, int htidx) {
    return d->ht_used[htidx] == 0 && d->child_buckets[htidx] == 0;
}

/* Returns the mask of the positions whose hash tag is `tag`. */
static inline bucketBits findHashMatches(const uint8_t *hashes, uint8_t tag) {
    bucketBits m = 0;
    for (int i = 0; i < DICT_BUCKET_SLOTS; i++)
        m |= (bucketBits)((hashes[i] == tag) << i);
    return m;
}

/* Child buckets are allocated and freed with these functions, which keep
 * child_buckets[] and the bucketChanged callback (which counts top-level and
 * child buckets) in sync. _dictClear() is the exception, as its callers
 * subtract all the buckets at once. */
static dictBucket *allocChildBucket(dict *d, int table) {
    dictBucket *b = zcalloc(sizeof(*b));
    d->child_buckets[table]++;
    if (d->type->bucketChanged) d->type->bucketChanged(d, 1);
    return b;
}

static void freeChildBucket(dict *d, dictBucket *b, int table) {
    zfree(b);
    debugAssert(d->child_buckets[table] > 0);
    d->child_buckets[table]--;
    if (d->type->bucketChanged) d->type->bucketChanged(d, -1);
}

/* Move an entry from one bucket position to another. */
static void moveEntry(dictBucket *to, int pos_to, dictBucket *from, int pos_from) {
    assert(!isPositionFilled(to, pos_to));
    assert(isPositionFilled(from, pos_from));
    to->slots[pos_to] = from->slots[pos_from];
    to->hashes[pos_to] = from->hashes[pos_from];
    to->presence |= (1u << pos_to);
    from->presence &= ~(1u << pos_from);
}

/* Exchange the entries of two filled bucket positions. */
static void swapEntries(dictBucket *a, int pos_a, dictBucket *b, int pos_b) {
    assert(isPositionFilled(a, pos_a) && isPositionFilled(b, pos_b));
    dictEntry *de = a->slots[pos_a];
    uint8_t tag = a->hashes[pos_a];
    a->slots[pos_a] = b->slots[pos_b];
    a->hashes[pos_a] = b->hashes[pos_b];
    b->slots[pos_b] = de;
    b->hashes[pos_b] = tag;
}

/* Converts a full bucket to a chained one, moving its last entry to a new
 * child bucket. */
static void bucketConvertToChained(dict *d, dictBucket *b, int table) {
    assert(!b->chained);
    int pos = DICT_BUCKET_SLOTS - 1;
    assert(isPositionFilled(b, pos));
    dictBucket *child = allocChildBucket(d, table);
    moveEntry(child, 0, b, pos);
    b->chained = 1;
    setChildBucket(b, child);
}

/* Frees the last bucket of a chain, which must be empty or have a single
 * entry. The before-last bucket becomes the new last bucket; a lone entry
 * takes the place of its child pointer. */
static void pruneLastBucket(dict *d, dictBucket *before_last, dictBucket *last, int table) {
    assert(before_last->chained && getChildBucket(before_last) == last);
    assert(!last->chained);
    assert(last->presence == 0 || __builtin_popcount(last->presence) == 1);
    before_last->chained = 0;
    assert(!isPositionFilled(before_last, DICT_BUCKET_SLOTS - 1));
    if (last->presence != 0) {
        int pos_in_last = __builtin_ctz(last->presence);
        moveEntry(before_last, DICT_BUCKET_SLOTS - 1, last, pos_in_last);
    }
    freeChildBucket(d, last, table);
}

/* After removing an entry from a chained bucket, fill the hole with an entry
 * from the end of the chain and free the last bucket if it becomes (nearly)
 * empty. A last bucket that is already empty (deletes while compaction was
 * paused) is freed first, and the entry is taken from the new last bucket. If
 * `b` itself becomes the last bucket, the hole is left as it is. */
static void fillBucketHole(dict *d, dictBucket *b, int pos, int table) {
    assert(b->chained && !isPositionFilled(b, pos));
    while (1) {
        dictBucket *before_last = b;
        dictBucket *last = getChildBucket(b);
        while (last->chained) {
            before_last = last;
            last = getChildBucket(last);
        }
        if (last->presence == 0) {
            pruneLastBucket(d, before_last, last, table);
            if (!b->chained) return;
            continue;
        }
        int pos_in_last = __builtin_ctz(last->presence);
        moveEntry(b, pos, last, pos_in_last);
        if (last->presence == 0 || __builtin_popcount(last->presence) == 1) {
            pruneLastBucket(d, before_last, last, table);
        }
        return;
    }
}

/* Compact the chain after removing the entry at `pos` of bucket `b`, unless
 * that is paused. `parent` is the bucket whose child is `b`, or NULL. Without
 * a parent, a last bucket that became (nearly) empty is left as it is; a
 * top-level bucket has no parent and is never freed here. */
static void compactAfterDelete(dict *d, dictBucket *b, dictBucket *parent, int pos, int table) {
    if (d->pauserehash != 0) return;
    if (b->chained)
        fillBucketHole(d, b, pos, table);
    else if (parent && (b->presence == 0 || __builtin_popcount(b->presence) == 1))
        pruneLastBucket(d, parent, b, table);
}

/* Deleting entries while compaction is paused leaves holes. This fills them by
 * moving entries from the end of the chain, and frees the buckets that are no
 * longer needed. */
static void compactBucketChain(dict *d, size_t bucket_index, int table) {
    dictBucket *b = &d->ht_table[table][bucket_index];
    while (b->chained) {
        dictBucket *next = getChildBucket(b);
        if (next->chained && next->presence == 0) {
            /* Empty bucket in the middle of the chain: unlink it. */
            setChildBucket(b, getChildBucket(next));
            freeChildBucket(d, next, table);
            continue;
        }
        if (!next->chained && (next->presence == 0 || __builtin_popcount(next->presence) == 1)) {
            pruneLastBucket(d, b, next, table);
            return;
        }
        /* Filling a hole touches no other position of b. */
        bucketBits holes = ~b->presence & (bucketBits)((1u << (DICT_BUCKET_SLOTS - 1)) - 1);
        while (holes) {
            int pos = __builtin_ctz(holes);
            holes &= holes - 1;
            fillBucketHole(d, b, pos, table);
            if (!b->chained) return;
        }
        b = next;
    }
}

/* Find a free position for an entry with the given hash, in the table that
 * accepts inserts. Chains the bucket if it is full. */
static dictBucket *findBucketForInsert(dict *d, uint64_t hash, int *pos_out, int *table_out) {
    int table = dictIsRehashing(d) ? 1 : 0;
    assert(d->ht_table[table]);
    dictBucket *b = &d->ht_table[table][hash & DICTHT_SIZE_MASK(d->ht_size_exp[table])];
    while (bucketIsFull(b)) {
        if (!b->chained) bucketConvertToChained(d, b, table);
        b = getChildBucket(b);
    }
    /* The bucket is not full, so its first clear presence bit is a usable
     * position (a chained bucket also has positions before its child slot). */
    int pos = __builtin_ctz(~(unsigned)b->presence);
    debugAssert(pos < numBucketPositions(b));
    *pos_out = pos;
    if (table_out) *table_out = table;
    return b;
}

/* Search a single bucket (not its child buckets) for the key. Returns the
 * position of the key, or -1 if it is not in this bucket. */
static inline int bucketFindKey(dict *d, dictBucket *b, uint8_t tag, const void *key,
                                keyCmpFunc cmp, dictCmpCache *cache)
{
    bucketBits candidates = b->presence & findHashMatches(b->hashes, tag);
    while (candidates) {
        int pos = __builtin_ctz(candidates);
        const void *visitedKey = dictStoredKey2Key(d, dictGetKey(b->slots[pos]));
        if (key == visitedKey || cmp(cache, key, visitedKey)) return pos;
        candidates &= candidates - 1;
    }
    return -1;
}

/* Find the bucket and position of a key. Does not rehash or write anything.
 * If `parent_out` is not NULL, it is set to the bucket whose child is the
 * returned bucket, or NULL for a top-level bucket. */
static dictBucket *findBucketForKey(dict *d, uint64_t hash, const void *key, int *pos_out,
                                    int *table_out, dictBucket **parent_out)
{
    dictCmpCache cache = {0};
    keyCmpFunc cmp = dictGetCmpFunc(d);
    uint8_t tag = hashTag(hash);
    for (int table = 0; table <= 1; table++) {
        if (d->ht_used[table] == 0) continue;
        size_t idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        /* Buckets before rehashidx have already been moved to the new table. */
        if (table == 0 && (long)idx < d->rehashidx) continue;
        dictBucket *b = &d->ht_table[table][idx];
        dictBucket *parent = NULL;
        do {
            int pos = bucketFindKey(d, b, tag, key, cmp, &cache);
            if (pos >= 0) {
                *pos_out = pos;
                if (table_out) *table_out = table;
                if (parent_out) *parent_out = parent;
                return b;
            }
            parent = b;
            b = getChildBucket(b);
        } while (b);
    }
    return NULL;
}

/* ----------------------------- API implementation ------------------------- */

/* Reset hash table parameters already initialized with _dictInit()*/
static void _dictReset(dict *d, int htidx)
{
    d->ht_table[htidx] = NULL;
    d->ht_size_exp[htidx] = -1;
    d->ht_used[htidx] = 0;
    d->child_buckets[htidx] = 0;
}

/* Create a new hash table */
dict *dictCreate(dictType *type)
{
    size_t metasize = type->dictMetadataBytes ? type->dictMetadataBytes(NULL) : 0;
    dict *d = zmalloc(sizeof(*d)+metasize);
    if (metasize > 0) {
        memset(dictMetadata(d), 0, metasize);
    }
    _dictInit(d,type);
    return d;
}

/* Change dictType of dict to another one with metadata support
 * Rest of dictType's values must stay the same */
void dictTypeAddMeta(dict **d, dictType *typeWithMeta) {
    /* Verify new dictType is compatible with the old one */
    dictType toCmp = *typeWithMeta;
    /* Ignore 'dictMetadataBytes' and 'onDictRelease' in comparison */
    toCmp.dictMetadataBytes = (*d)->type->dictMetadataBytes;
    toCmp.onDictRelease = (*d)->type->onDictRelease;
    assert(memcmp((*d)->type, &toCmp, sizeof(dictType)) == 0); /* The rest of the dictType fields must be the same */

    *d = zrealloc(*d, sizeof(dict) + typeWithMeta->dictMetadataBytes(*d));
    (*d)->type = typeWithMeta;
}

/* Initialize the hash table */
int _dictInit(dict *d, dictType *type)
{
    _dictReset(d, 0);
    _dictReset(d, 1);
    d->type = type;
    d->rehashidx = -1;
    d->pauserehash = 0;
    d->pauseAutoResize = 0;
    return DICT_OK;
}

/* Resize or create the hash table,
 * when malloc_failed is non-NULL, it'll avoid panic if malloc fails (in which case it'll be set to 1).
 * Returns DICT_OK if resize was performed, and DICT_ERR if skipped. */
int _dictResize(dict *d, unsigned long size, int* malloc_failed)
{
    if (malloc_failed) *malloc_failed = 0;

    /* We can't rehash twice if rehashing is ongoing. */
    assert(!dictIsRehashing(d));

    /* the new hash table */
    dictBucket *new_ht_table;
    unsigned long new_ht_used;
    signed char new_ht_size_exp = _dictNextExp(size);

    /* Detect overflows */
    size_t newsize = DICTHT_SIZE(new_ht_size_exp);
    if (newsize * DICT_BUCKET_SLOTS < size || newsize * DICT_BUCKET_BYTES < newsize)
        return DICT_ERR;

    /* Rehashing to the same table size is not useful. */
    if (new_ht_size_exp == d->ht_size_exp[0]) return DICT_ERR;

    /* Allocate the new hash table and initialize all buckets to empty */
    if (malloc_failed) {
        new_ht_table = ztrycalloc(newsize*DICT_BUCKET_BYTES);
        *malloc_failed = new_ht_table == NULL;
        if (*malloc_failed)
            return DICT_ERR;
    } else
        new_ht_table = zcalloc(newsize*DICT_BUCKET_BYTES);

    new_ht_used = 0;

    /* Prepare a second hash table for incremental rehashing.
     * We do this even for the first initialization, so that we can trigger the
     * rehashingStarted more conveniently, we will clean it up right after. */
    d->ht_size_exp[1] = new_ht_size_exp;
    d->ht_used[1] = new_ht_used;
    d->ht_table[1] = new_ht_table;
    d->rehashidx = 0;
    if (d->type->rehashingStarted) d->type->rehashingStarted(d);
    if (d->type->bucketChanged)
        d->type->bucketChanged(d, DICTHT_SIZE(d->ht_size_exp[1]));

    /* Is this the first initialization or is the first hash table empty? If so
     * it's not really a rehashing, we can just set the first hash table so that
     * it can accept keys. An empty table is kept while rehashing is paused, as
     * a safe iterator or a scan may still be walking its buckets. */
    if (d->ht_table[0] == NULL || (tableIsEmpty(d, 0) && d->pauserehash == 0)) {
        if (d->type->rehashingCompleted) d->type->rehashingCompleted(d);
        if (d->type->bucketChanged)
            d->type->bucketChanged(d, -(long long)DICTHT_SIZE(d->ht_size_exp[0]));
        if (d->ht_table[0]) zfree(d->ht_table[0]);
        d->ht_size_exp[0] = new_ht_size_exp;
        d->ht_used[0] = new_ht_used;
        d->ht_table[0] = new_ht_table;
        _dictReset(d, 1);
        d->rehashidx = -1;
        return DICT_OK;
    }

    /* Force a full rehashing of the dictionary */
    if (d->type->force_full_rehash && d->pauserehash == 0) {
        while (dictRehash(d, 1000)) {
            /* Continue rehashing */
        }
    }
    return DICT_OK;
}

int _dictExpand(dict *d, unsigned long size, int* malloc_failed) {
    /* the size is invalid if it is smaller than the slots of the hash table 
     * or smaller than the number of elements already inside the hash table */
    if (dictIsRehashing(d) || d->ht_used[0] > size ||
        DICTHT_SIZE(d->ht_size_exp[0]) * DICT_BUCKET_SLOTS >= size)
        return DICT_ERR;
    return _dictResize(d, size, malloc_failed);
}

/* return DICT_ERR if expand was not performed */
int dictExpand(dict *d, unsigned long size) {
    return _dictExpand(d, size, NULL);
}

/* return DICT_ERR if expand failed due to memory allocation failure */
int dictTryExpand(dict *d, unsigned long size) {
    int malloc_failed = 0;
    _dictExpand(d, size, &malloc_failed);
    return malloc_failed? DICT_ERR : DICT_OK;
}

/* return DICT_ERR if shrink was not performed */
int dictShrink(dict *d, unsigned long size) {
    /* the size is invalid if it is bigger than the slots of the hash table
     * or smaller than the number of elements already inside the hash table */
    if (dictIsRehashing(d) || d->ht_used[0] > size ||
        DICTHT_SIZE(d->ht_size_exp[0]) * DICT_BUCKET_SLOTS <= size)
        return DICT_ERR;
    return _dictResize(d, size, NULL);
}

/* Helper function for `dictRehash` and `_dictBucketRehash` which rehashes all the keys
 * in the bucket chain at index `idx` from the old to the new hash HT. */
static void rehashEntriesInBucketAtIndex(dict *d, uint64_t idx) {
    dictBucket *root = &d->ht_table[0][idx];
    for (dictBucket *b = root; b; b = getChildBucket(b)) {
        bucketBits present = b->presence;
        while (present) {
            int pos = __builtin_ctz(present);
            present &= present - 1;
            dictEntry *de = b->slots[pos];
            uint64_t h;
            uint8_t tag;
            /* Get the hash bits that locate the bucket in the new hash table */
            if (d->ht_size_exp[1] > d->ht_size_exp[0]) {
                const void *key = dictStoredKey2Key(d, dictGetKey(de));
                h = dictGetHash(d, key);
                tag = hashTag(h);
            } else {
                /* We're shrinking the table. The tables sizes are powers of
                 * two, so we simply mask the bucket index in the larger table
                 * to get the bucket index in the smaller table. The hash tag
                 * of the entry stays the same. */
                h = idx;
                tag = b->hashes[pos];
            }
            int dpos;
            dictBucket *dst = findBucketForInsert(d, h, &dpos, NULL);
            dst->slots[dpos] = de;
            dst->hashes[dpos] = tag;
            dst->presence |= (1u << dpos);
            d->ht_used[0]--;
            d->ht_used[1]++;
        }
    }
    /* All the entries of the chain moved, so free its child buckets. */
    dictBucket *child = getChildBucket(root);
    while (child) {
        dictBucket *next = getChildBucket(child);
        freeChildBucket(d, child, 0);
        child = next;
    }
    root->chained = 0;
    root->presence = 0;
}

/* This checks if we already rehashed the whole table and if more rehashing is required */
static int dictCheckRehashingCompleted(dict *d) {
    if (!tableIsEmpty(d, 0)) return 0;
    
    if (d->type->rehashingCompleted) d->type->rehashingCompleted(d);
    if (d->type->bucketChanged)
        d->type->bucketChanged(d, -(long long)DICTHT_SIZE(d->ht_size_exp[0]));
    zfree(d->ht_table[0]);
    /* Copy the new ht onto the old one */
    d->ht_table[0] = d->ht_table[1];
    d->ht_used[0] = d->ht_used[1];
    d->child_buckets[0] = d->child_buckets[1];
    d->ht_size_exp[0] = d->ht_size_exp[1];
    _dictReset(d, 1);
    d->rehashidx = -1;
    return 1;
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table (or child buckets left
 * without keys to free), otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (with all the keys in
 * it and in its child buckets) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
int dictRehash(dict *d, int n) {
    int empty_visits = n*10; /* Max number of empty buckets to visit. */
    unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
    unsigned long s1 = DICTHT_SIZE(d->ht_size_exp[1]);
    dictResizeEnable can_resize;
    atomicGet(dict_can_resize, can_resize);
    if (can_resize == DICT_RESIZE_FORBID || !dictIsRehashing(d)) return 0;
    /* Entries must not move while rehashing is paused. */
    if (d->pauserehash != 0) return 0;
    /* If dict_can_resize is DICT_RESIZE_AVOID, we want to avoid rehashing.
     * - If expanding, the threshold is dict_force_resize_ratio which is 4.
     * - If shrinking, the threshold is 1 / (HASHTABLE_MIN_FILL * dict_force_resize_ratio) which is 1/32.
     *   New tables are sized with spare slots, so a shrink forced at 1/32 fill
     *   may target a table only 16 times smaller. Such a shrink still makes
     *   progress while the fill of the old table stays at or below 1/32. */
    if (can_resize == DICT_RESIZE_AVOID &&
        ((s1 > s0 && s1 < dict_force_resize_ratio * s0) ||
         (s1 < s0 && s0 < HASHTABLE_MIN_FILL * dict_force_resize_ratio * s1 &&
          dictSize(d) > s0 * DICT_BUCKET_SLOTS / (HASHTABLE_MIN_FILL * dict_force_resize_ratio))))
    {
        return 0;
    }

    while(n-- && !tableIsEmpty(d, 0)) {
        /* Note that rehashidx can't overflow as we are sure there are more
         * entries or child buckets in ht[0] */
        assert(DICTHT_SIZE(d->ht_size_exp[0]) > (unsigned long)d->rehashidx);
        while(bucketIsEmpty(&d->ht_table[0][d->rehashidx])) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        /* Move all the keys in this bucket from the old to the new hash HT */
        rehashEntriesInBucketAtIndex(d, d->rehashidx);
        d->rehashidx++;
    }

    return !dictCheckRehashingCompleted(d);
}

long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

/* Rehash in us+"delta" microseconds. The value of "delta" is larger
 * than 0, and is smaller than 1000 in most cases. The exact upper bound
 * depends on the running time of dictRehash(d,100).*/
int dictRehashMicroseconds(dict *d, uint64_t us) {
    if (d->pauserehash > 0) return 0;

    monotime timer;
    elapsedStart(&timer);
    int rehashes = 0;

    while(dictRehash(d,100)) {
        rehashes += 100;
        if (elapsedUs(timer) >= us) break;
    }
    return rehashes;
}

/* This function performs just a step of rehashing, and only if hashing has
 * not been paused for our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some elements can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
static void _dictRehashStep(dict *d) {
    if (d->pauserehash == 0) dictRehash(d,1);
}

/* Performs rehashing on a single bucket. */
int _dictBucketRehash(dict *d, uint64_t idx) {
    if (d->pauserehash != 0) return 0;
    unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
    unsigned long s1 = DICTHT_SIZE(d->ht_size_exp[1]);
    dictResizeEnable can_resize;
    atomicGet(dict_can_resize, can_resize);
    if (can_resize == DICT_RESIZE_FORBID || !dictIsRehashing(d)) return 0;
    /* If dict_can_resize is DICT_RESIZE_AVOID, we want to avoid rehashing.
     * - If expanding, the threshold is dict_force_resize_ratio which is 4.
     * - If shrinking, the threshold is 1 / (HASHTABLE_MIN_FILL * dict_force_resize_ratio) which is 1/32.
     *   New tables are sized with spare slots, so a shrink forced at 1/32 fill
     *   may target a table only 16 times smaller. Such a shrink still makes
     *   progress while the fill of the old table stays at or below 1/32. */
    if (can_resize == DICT_RESIZE_AVOID &&
        ((s1 > s0 && s1 < dict_force_resize_ratio * s0) ||
         (s1 < s0 && s0 < HASHTABLE_MIN_FILL * dict_force_resize_ratio * s1 &&
          dictSize(d) > s0 * DICT_BUCKET_SLOTS / (HASHTABLE_MIN_FILL * dict_force_resize_ratio))))
    {
        return 0;
    }
    rehashEntriesInBucketAtIndex(d, idx);
    dictCheckRehashingCompleted(d);
    return 1;
}

/* Add an element to the target hash table */
int dictAdd(dict *d, void *key __stored_key, void *val)
{
    dictEntry *entry = dictAddRaw(d,key,NULL);

    if (!entry) return DICT_ERR;
    if (!d->type->no_value) dictSetVal(d, entry, val);
    return DICT_OK;
}

int dictCompareKeys(dict *d, const void *key1, const void *key2) {
    dictCmpCache cache = {0};
    keyCmpFunc cmpFunc = dictGetCmpFunc(d);
    return cmpFunc(&cache, key1, key2);
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as they wish.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */
dictEntry *dictAddRaw(dict *d, void *key __stored_key, dictEntry **existing)
{
    const void *lookupKey = dictStoredKey2Key(d, key);
    uint64_t hash = dictGetHash(d, lookupKey);
    int pos;
    if (existing) *existing = NULL;

    /* Rehash the hash table if needed */
    _dictRehashStepIfNeeded(d, hash & DICTHT_SIZE_MASK(d->ht_size_exp[0]));

    /* Expand the hash table if needed */
    _dictExpandIfNeeded(d);

    /* Return NULL if the key already exists. */
    dictBucket *b = findBucketForKey(d, hash, lookupKey, &pos, NULL, NULL);
    if (b) {
        if (existing) *existing = b->slots[pos];
        return NULL;
    }

    /* Dup the key if necessary. */
    if (d->type->keyDup) key = d->type->keyDup(d, key);

    return dictInsertKey(d, hash, key, NULL);
}

/* Adds a key that is not in the dict yet, at a free position of the bucket
 * chain that its hash maps to, and returns the new entry. For no_value dicts
 * the entry is the encoded key. If `link` is not NULL, it is set to the link
 * of the new entry. This is a low level function which assumes that
 * dictExpandIfNeeded() was called before. */
static dictEntry *dictInsertKey(dict *d, uint64_t hash, void *key __stored_key, dictEntryLink *link) {
    dictEntry *entry;
    int pos, htidx;
    if (d->type->no_value) {
        /* We can store the key directly in the bucket slot without
         * allocating dictEntry. */
        entry = encodeEntryKey(d, key);
        assert(entryIsKey(entry));
    } else {
        /* Allocate the memory and store the new entry. */
        entry = zmalloc(sizeof(*entry));
        assert(entryIsNormal(entry)); /* Check alignment of allocation */
        entry->key = key;
    }
    /* If rehashing is ongoing, we insert in table 1, otherwise in table 0. */
    dictBucket *b = findBucketForInsert(d, hash, &pos, &htidx);
    b->slots[pos] = entry;
    b->hashes[pos] = hashTag(hash);
    b->presence |= (1u << pos);
    d->ht_used[htidx]++;
    if (link) *link = &b->slots[pos];

    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
int dictReplace(dict *d, void *key __stored_key, void *val)
{
    dictEntry *entry, *existing;

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
    entry = dictAddRaw(d,key,&existing);
    if (entry) {
        dictSetVal(d, entry, val);
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    void *oldval = dictGetVal(existing);
    dictSetVal(d, existing, val);
    if (d->type->valDestructor)
        d->type->valDestructor(d, oldval);
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
dictEntry *dictAddOrFind(dict *d, void *key __stored_key) {
    dictEntry *entry, *existing;
    entry = dictAddRaw(d,key,&existing);
    return entry ? entry : existing;
}

/* Search and remove an element. This is a helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions. */
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    dictBucket *b, *parent;
    uint64_t h, idx;
    int pos, table;

    /* dict is empty */
    if (dictSize(d) == 0) return NULL;

    h = dictGetHash(d, key);
    idx = h & DICTHT_SIZE_MASK(d->ht_size_exp[0]);

    /* Rehash the hash table if needed */
    _dictRehashStepIfNeeded(d,idx);

    b = findBucketForKey(d, h, key, &pos, &table, &parent);
    if (!b) return NULL; /* not found */

    dictEntry *he = b->slots[pos];
    /* Unlink the element from the bucket and compact the chain, unless that
     * is paused */
    b->presence &= ~(1u << pos);
    compactAfterDelete(d, b, parent, pos, table);
    if (!nofree) {
        dictFreeUnlinkedEntry(d, he);
    }
    d->ht_used[table]--;
    _dictShrinkIfNeeded(d);
    return he;
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. */
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 */
dictEntry *dictUnlink(dict *d, const void *key) {
    return dictGenericDelete(d,key,1);
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    if (!entryIsKey(he)) zfree(decodeMaskedPtr(he));
}

/* Destroy an entire dictionary */
int _dictClear(dict *d, int htidx, void(callback)(dict*)) {
    unsigned long i;

    /* Free all the elements */
    for (i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]) && !tableIsEmpty(d, htidx); i++) {
        dictBucket *root = &d->ht_table[htidx][i], *b, *next;
        /* Callback will be called once for every 65536 buckets. Beware,
         * if the table has 65536 buckets or less, it will not be called at all.*/
        if (callback && i != 0 && (i & 65535) == 0) callback(d);

        if (bucketIsEmpty(root)) continue;
        for (b = root; b; b = next) {
            bucketBits present = b->presence;
            while (present) {
                dictEntry *he = b->slots[__builtin_ctz(present)];
                present &= present - 1;
                dictFreeKey(d, he);
                dictFreeVal(d, he);
                if (!entryIsKey(he)) zfree(decodeMaskedPtr(he));
                d->ht_used[htidx]--;
            }
            next = getChildBucket(b);
            /* The callers already subtracted the child buckets with
             * bucketChanged. */
            if (b != root) {
                zfree(b);
                d->child_buckets[htidx]--;
            }
        }
    }
    /* Free the table and the allocated cache structure */
    zfree(d->ht_table[htidx]);
    /* Re-initialize the table */
    _dictReset(d, htidx);
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
void dictRelease(dict *d)
{
    /* Someone may be monitoring a dict that started rehashing, before
     * destroying the dict fake completion. */
    if (dictIsRehashing(d) && d->type->rehashingCompleted)
        d->type->rehashingCompleted(d);

    /* Subtract the size of all buckets. */
    if (d->type->bucketChanged)
        d->type->bucketChanged(d, -(long long)(dictBuckets(d) + dictChildBuckets(d)));

    if (d->type->onDictRelease)
        d->type->onDictRelease(d);

    _dictClear(d,0,NULL);
    _dictClear(d,1,NULL);
    zfree(d);
}

/* Finds a given key. Like dictFindLink(), yet search bucket even if dict is empty. 
 * 
 * Returns dictEntryLink reference if found. Otherwise, return NULL.
 * 
 * bucket - return a link to the first slot of the bucket that the key was
 *          mapped to, in the table that accepts inserts, unless the table
 *          is not allocated.
 */
static dictEntryLink dictFindLinkInternal(dict *d, const void *key, uint64_t hash, dictEntryLink *bucket) {
    uint64_t idx;
    int pos;

    if (bucket) {
        *bucket = NULL;
    } else {
        /* If dict is empty and no need to find bucket, return NULL */
        if (dictSize(d) == 0) return NULL;
    }

    idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[0]);

    /* Rehash the hash table if needed */
    _dictRehashStepIfNeeded(d,idx);

    if (bucket) {
        int table = dictIsRehashing(d) ? 1 : 0;
        if (d->ht_table[table]) {
            idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
            *bucket = &d->ht_table[table][idx].slots[0];
        }
    }

    dictBucket *b = findBucketForKey(d, hash, key, &pos, NULL, NULL);
    return b ? &b->slots[pos] : NULL;
}

dictEntry *dictFind(dict *d, const void *key)
{
    dictEntryLink link = dictFindLink(d, key, NULL);
    return (link) ? *link : NULL;
}

/* Finds the dictEntry using pointer and pre-calculated hash.
 * oldptr is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is a pointer to the dictEntry if found, or NULL if not found. */
dictEntry *dictFindByHashAndPtr(dict *d, const void *oldptr, const uint64_t hash) {
    unsigned long idx, table;
    uint8_t tag = hashTag(hash);

    if (dictSize(d) == 0) return NULL; /* dict is empty */
    for (table = 0; table <= 1; table++) {
        idx = hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
        if (table == 0 && (long)idx < d->rehashidx) continue;
        for (dictBucket *b = &d->ht_table[table][idx]; b; b = getChildBucket(b)) {
            bucketBits candidates = b->presence & findHashMatches(b->hashes, tag);
            while (candidates) {
                dictEntry *he = b->slots[__builtin_ctz(candidates)];
                if (oldptr == dictGetKey(he))
                    return he;
                candidates &= candidates - 1;
            }
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

/* Find a key and return its dictEntryLink reference. Otherwise, return NULL
 * 
 * A dictEntryLink is the address of the bucket slot that holds the entry of the
 * searched item. It is Useful for deletion, addition, unlinking and updating,
 * especially for dict configured with 'no_value'. In such cases returning only
 * `dictEntry` from a lookup may be insufficient since it might be opt-out to be
 * the object itself. By locating the slot (dictEntryLink) these ops can be
 * properly handled.
 *
 * A link is valid only until the dict is modified: an insert into the same
 * bucket chain (a full bucket moves its last entry to a child bucket), a delete
 * that compacts the chain, a rehash step (that lookups do too) and a resize
 * can invalidate it. dictPauseRehashing() keeps it valid across lookups and
 * deletes of other keys in the same dict, but not across inserts.
 * 
 * After calling link = dictFindLink(...), any necessary updates based on returned 
 * link or bucket must be performed immediately after by calling dictSetKeyAtLink() 
 * without any intervening operations on given dict. Otherwise, `dictEntryLink` may 
 * become invalid. Example with kvobj of replacing key with new key:
 * 
 *      link = dictFindLink(d, key, &bucket);
 *      ... Do something, but don't modify the dict ...
 *      // assert(link != NULL);
 *      dictSetKeyAtLink(d, kv, &link, 0);
 *      
 * To add new value (dictSetKeyAtLink() expands the dict if there is no space
 * for the new key, and looks up the insert position again):
 *   
 *      link = dictFindLink(d, key, &bucket);
 *      ... Do something, but don't modify the dict ...
 *      // assert(link == NULL);
 *      dictSetKeyAtLink(d, kv, &bucket, 1);
 *  
 *  bucket - return link to bucket that the key was mapped. unless dict is empty.
 */
dictEntryLink dictFindLink(dict *d, const void *key, dictEntryLink *bucket) {
    if (bucket) *bucket = NULL;
    if (unlikely(dictSize(d) == 0))
        return NULL;
    
    return dictFindLinkInternal(d, key, dictGetHash(d, key), bucket);
}

/* Same as dictFindLink(), but for a caller that already computed this exact
 * key's hash (via dictGetHash(d, key)) for another purpose and wants to avoid
 * paying for the dict type's hashFunction() a second time. The hash is not
 * validated against key -- passing a mismatched hash silently searches the
 * wrong bucket. */
dictEntryLink dictFindLinkWithHash(dict *d, const void *key, uint64_t hash, dictEntryLink *bucket) {
    if (bucket) *bucket = NULL;
    if (unlikely(dictSize(d) == 0))
        return NULL;

    return dictFindLinkInternal(d, key, hash, bucket);
}

/* Set the key with link
 *
 * link:    - When `newItem` is set, `link` points to the bucket of the key, or
 *            to NULL. The insert position is looked up again, as it depends
 *            on the hash of the key.
 *          - When `newItem` is not set, `link` points to the link of the key.
 *          - When `newItem` is not set and *link is NULL, dictFindLink() will
 *            be called to locate the key.
 *          - On return, get updated, by need, to the inserted key. 
 *
 * newItem: 1 = Add a key with a new dictEntry.
 *          0 = Set a key to an existing dictEntry. 
 */
void dictSetKeyAtLink(dict *d, void *key __stored_key, dictEntryLink *link, int newItem) {
    dictEntryLink dummy = NULL;
    if (link == NULL) link = &dummy;
    void *addedKey = (d->type->keyDup) ? d->type->keyDup(d, key) : key;
    
    if (newItem) {
        /* Make room if needed for the new key */
        dictExpandIfNeeded(d);

        uint64_t hash = dictGetHash(d, dictStoredKey2Key(d, key));
        dictInsertKey(d, hash, addedKey, link);
        return;
    } 
    
    /* Setting key of existing dictEntry (newItem == 0)*/
    
    if (*link == NULL) {
        *link = dictFindLink(d, key, NULL);
        assert(*link != NULL);
    }
    
    dictEntry **de = *link;
    if (entryIsKey(*de)) {
        /* `de` opt-out to be actually a key. Replace key but keep the lsb flags */
        *de = encodeEntryKey(d, addedKey);
    } else {
        (*de)->key = addedKey;
    }
}

void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;

    he = dictFind(d,key);
    return he ? dictGetVal(he) : NULL;
}

/* Find an element from the table. A link is returned if the element is found, and
 * the user should later call `dictTwoPhaseUnlinkFree` with it in order to unlink
 * and release it. Otherwise if the key is not found, NULL is returned. These two
 * functions should be used in pair.
 * `dictTwoPhaseUnlinkFind` pauses rehash and `dictTwoPhaseUnlinkFree` resumes rehash.
 *
 * We can use like this:
 *
 * dictEntryLink link = dictTwoPhaseUnlinkFind(db->dict,key->ptr, &table);
 * // Do something, but we can't modify the dict
 * dictTwoPhaseUnlinkFree(db->dict, link, table); // We don't need to lookup again
 *
 * If we want to find an entry before delete this entry, this an optimization to avoid
 * dictFind followed by dictDelete. i.e. the first API is a find, and it gives some info
 * to the second one to avoid repeating the lookup
 *
 * `table_index` holds the table and the position of the entry in its bucket.
 */
dictEntryLink dictTwoPhaseUnlinkFind(dict *d, const void *key, int *table_index) {
    dictBucket *b, *parent;
    uint64_t h;
    int pos, table;

    if (dictSize(d) == 0) return NULL; /* dict is empty */
    if (dictIsRehashing(d)) _dictRehashStep(d);

    h = dictGetHash(d, key);    
    b = findBucketForKey(d, h, key, &pos, &table, &parent);
    if (!b) return NULL;

    /* Freeing an entry of the last bucket of a chain may free that bucket,
     * which needs its parent bucket. Move the entry into the parent (swapping
     * it with an entry of the parent, if there is one), so that
     * dictTwoPhaseUnlinkFree() can compact the chain without it. Entries must
     * not move while rehashing is paused, in which case it stays where it is. */
    if (parent && !b->chained && d->pauserehash == 0) {
        bucketBits filled = parent->presence;
        int parent_pos = filled ? __builtin_ctz(filled) : 0;
        if (filled)
            swapEntries(parent, parent_pos, b, pos);
        else
            moveEntry(parent, parent_pos, b, pos);
        b = parent;
        pos = parent_pos;
    }
    *table_index = table | (pos << 1);
    dictPauseRehashing(d);
    return &b->slots[pos];
}

void dictTwoPhaseUnlinkFree(dict *d, dictEntryLink plink, int table_index) {
    if (plink == NULL || *plink == NULL) return;
    int table = table_index & 1;
    int pos = table_index >> 1;
    dictBucket *b = (dictBucket *)(void *)((char *)(plink - pos) - offsetof(dictBucket, slots));
    debugAssert(isPositionFilled(b, pos) && !(b->chained && pos == DICT_BUCKET_SLOTS - 1));
    dictEntry *de = *plink;
    d->ht_used[table]--;

    b->presence &= ~(1u << pos);
    dictFreeKey(d, de);
    dictFreeVal(d, de);
    if (!entryIsKey(de)) zfree(decodeMaskedPtr(de));
    dictResumeRehashing(d);
    /* The parent of the bucket isn't known: dictTwoPhaseUnlinkFind() moved the
     * entry out of the last bucket of the chain, unless rehashing was paused. */
    compactAfterDelete(d, b, NULL, pos, table);
    _dictShrinkIfNeeded(d);
}

void dictSetKey(dict *d, dictEntry* de, void *key __stored_key) {
    assert(!d->type->no_value);
    if (d->type->keyDup)
        de->key = d->type->keyDup(d, key);
    else
        de->key = key;
}

void dictSetVal(dict *d, dictEntry *de, void *val) {
    assert(entryHasValue(de));
    de->v.val = d->type->valDup ? d->type->valDup(d, val) : val;
}

void dictSetSignedIntegerVal(dictEntry *de, int64_t val) {
    assert(entryHasValue(de));
    de->v.s64 = val;
}

void dictSetUnsignedIntegerVal(dictEntry *de, uint64_t val) {
    assert(entryHasValue(de));
    de->v.u64 = val;
}

void dictSetDoubleVal(dictEntry *de, double val) {
    assert(entryHasValue(de));
    de->v.d = val;
}

int64_t dictIncrSignedIntegerVal(dictEntry *de, int64_t val) {
    assert(entryHasValue(de));
    return de->v.s64 += val;
}

uint64_t dictIncrUnsignedIntegerVal(dictEntry *de, uint64_t val) {
    assert(entryHasValue(de));
    return de->v.u64 += val;
}

double dictIncrDoubleVal(dictEntry *de, double val) {
    assert(entryHasValue(de));
    return de->v.d += val;
}

int dictEntryIsKey(const dictEntry *de) {
    return entryIsKey(de);
}

void *dictGetKey(const dictEntry *de) {
    /* if entryIsKey() */
    if ((uintptr_t)de & ENTRY_PTR_IS_ODD_KEY) return (void *) de;
    if ((uintptr_t)de & ENTRY_PTR_IS_EVEN_KEY) return decodeMaskedPtr(de);    
    /* Regular entry */
    return de->key;
}

void *dictGetVal(const dictEntry *de) {
    assert(entryHasValue(de));
    return de->v.val;
}

int64_t dictGetSignedIntegerVal(const dictEntry *de) {
    assert(entryHasValue(de));
    return de->v.s64;
}

uint64_t dictGetUnsignedIntegerVal(const dictEntry *de) {
    assert(entryHasValue(de));
    return de->v.u64;
}

double dictGetDoubleVal(const dictEntry *de) {
    assert(entryHasValue(de));
    return de->v.d;
}

/* Returns a mutable reference to the value as a double within the entry. */
double *dictGetDoubleValPtr(dictEntry *de) {
    assert(entryHasValue(de));
    return &de->v.d;
}

/* Returns the memory usage in bytes of the dict, excluding the size of the keys
 * and values. */
size_t dictMemUsage(const dict *d) {
    /* The buckets (top-level and child) plus the per-entry structures. no_value=1
     * dicts (sets, the sorted-set element index, hashes) store the key in the
     * bucket slot and allocate nothing per entry, see dictEntryMemUsage(). */
    return dictSize(d) * dictEntryMemUsage(d->type->no_value) +
        (dictBuckets(d) + dictChildBuckets(d)) * DICT_BUCKET_BYTES;
}

size_t dictEntryMemUsage(int noValueDict) {
    return (noValueDict) ? 0 : sizeof(dictEntry);
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
unsigned long long dictFingerprint(dict *d) {
    unsigned long long integers[6], hash = 0;
    int j;

    integers[0] = (long) d->ht_table[0];
    integers[1] = d->ht_size_exp[0];
    integers[2] = d->ht_used[0];
    integers[3] = (long) d->ht_table[1];
    integers[4] = d->ht_size_exp[1];
    integers[5] = d->ht_used[1];

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

void dictInitIterator(dictIterator *iter, dict *d)
{
    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
    iter->bucket = NULL;
    iter->pos = 0;
    iter->last_seen_size = 0;
}

void dictInitSafeIterator(dictIterator *iter, dict *d)
{
    dictInitIterator(iter, d);
    iter->safe = 1;
}

void dictResetIterator(dictIterator *iter)
{
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe)
            dictResumeRehashing(iter->d);
        else
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
}

dictIterator *dictGetIterator(dict *d)
{
    dictIterator *iter = zmalloc(sizeof(*iter));
    dictInitIterator(iter, d);
    return iter;
}

dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;
    return i;
}

dictEntry *dictNext(dictIterator *iter)
{
    dict *d = iter->d;
    dictBucket *b;
    int pos;

    if (iter->index == -1 && iter->table == 0) {
        if (iter->safe) {
            dictPauseRehashing(d);
            iter->last_seen_size = d->ht_used[0];
        } else {
            iter->fingerprint = dictFingerprint(d);
        }

        /* skip the rehashed buckets in table[0] */
        iter->index = dictIsRehashing(d) ? d->rehashidx : 0;
        if (d->ht_table[0] == NULL ||
            iter->index >= (long) DICTHT_SIZE(d->ht_size_exp[0]))
        {
            iter->bucket = NULL;
            return NULL;
        }
        b = &d->ht_table[0][iter->index];
        pos = 0;
    } else {
        b = iter->bucket;
        if (b == NULL) return NULL;
        if (b->chained && iter->pos == DICT_BUCKET_SLOTS - 1) {
            /* The last returned entry was in the last slot of a bucket that
             * was not chained then. An insert chained the bucket since, moving
             * that entry to slot 0 of the child bucket, so skip it. */
            b = getChildBucket(b);
            pos = 1;
        } else {
            pos = iter->pos + 1;
        }
    }

    while (1) {
        /* Return the next filled position of the bucket. The user of the
         * iterator may delete the entry we are returning: a safe iterator
         * pauses the compaction of bucket chains, so no entry moves. */
        bucketBits present = b->presence >> pos;
        if (present) {
            iter->bucket = b;
            iter->pos = pos + __builtin_ctz(present);
            return b->slots[iter->pos];
        }
        if (b->chained) {
            b = getChildBucket(b);
            pos = 0;
            continue;
        }

        /* The chain is done. Compact it if the table lost entries while the
         * chain was walked, and nothing else pauses rehashing. */
        if (iter->safe && d->pauserehash == 1 && d->ht_used[iter->table] < iter->last_seen_size)
            compactBucketChain(d, iter->index, iter->table);
        iter->index++;
        if (iter->index >= (long) DICTHT_SIZE(d->ht_size_exp[iter->table])) {
            if (dictIsRehashing(d) && iter->table == 0) {
                iter->table++;
                iter->index = 0;
            } else {
                iter->bucket = NULL;
                return NULL;
            }
        }
        b = &d->ht_table[iter->table][iter->index];
        pos = 0;
        if (iter->safe) iter->last_seen_size = d->ht_used[iter->table];
    }
}

void dictReleaseIterator(dictIterator *iter)
{
    dictResetIterator(iter);
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
dictEntry *dictGetRandomKey(dict *d)
{
    dictBucket *b;
    unsigned long h;
    unsigned long listlen, listele;

    if (dictSize(d) == 0) return NULL;
    if (dictIsRehashing(d)) _dictRehashStep(d);
    if (dictIsRehashing(d)) {
        unsigned long s0 = DICTHT_SIZE(d->ht_size_exp[0]);
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            h = d->rehashidx + (randomULong() % (dictBuckets(d) - d->rehashidx));
            b = (h >= s0) ? &d->ht_table[1][h - s0] : &d->ht_table[0][h];
        } while((listlen = chainEntries(b)) == 0);
    } else {
        unsigned long m = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
        do {
            h = randomULong() & m;
            b = &d->ht_table[0][h];
        } while((listlen = chainEntries(b)) == 0);
    }

    /* Now we found a non empty bucket, but it may be a chain of buckets
     * and we need to get a random element from the chain. The only sane
     * way to do so is counting the elements and select a random index. */
    listele = random() % listlen;
    for (; b; b = getChildBucket(b)) {
        unsigned long n = __builtin_popcount(b->presence);
        if (listele < n) {
            bucketBits present = b->presence;
            while (listele--) present &= present - 1;
            return b->slots[__builtin_ctz(present)];
        }
        listele -= n;
    }
    assert(0);
    return NULL;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long j; /* internal hash table id, 0 or 1. */
    unsigned long tables; /* 1 or 2 tables? */
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps;

    if (dictSize(d) < count) count = dictSize(d);
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d))
            _dictRehashStep(d);
        else
            break;
    }

    tables = dictIsRehashing(d) ? 2 : 1;
    maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[0]);
    if (tables > 1 && maxsizemask < DICTHT_SIZE_MASK(d->ht_size_exp[1]))
        maxsizemask = DICTHT_SIZE_MASK(d->ht_size_exp[1]);

    /* Pick a random point inside the larger table. */
    unsigned long i = randomULong() & maxsizemask;
    unsigned long emptylen = 0; /* Continuous empty entries so far. */
    while(stored < count && maxsteps--) {
        for (j = 0; j < tables; j++) {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            if (tables == 2 && j == 0 && i < (unsigned long) d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= DICTHT_SIZE(d->ht_size_exp[1]))
                    i = d->rehashidx;
                else
                    continue;
            }
            if (i >= DICTHT_SIZE(d->ht_size_exp[j])) continue; /* Out of range for this table. */
            dictBucket *b = &d->ht_table[j][i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (bucketIsEmpty(b)) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    i = randomULong() & maxsizemask;
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
                for (; b; b = getChildBucket(b)) {
                    bucketBits present = b->presence;
                    while (present) {
                        dictEntry *he = b->slots[__builtin_ctz(present)];
                        present &= present - 1;
                        /* Collect all the elements of the buckets found non empty while iterating.
                         * To avoid the issue of being unable to sample the end of a long chain,
                         * we utilize the Reservoir Sampling algorithm to optimize the sampling process.
                         * This means that even when the maximum number of samples has been reached,
                         * we continue sampling until we reach the end of the chain.
                         * See https://en.wikipedia.org/wiki/Reservoir_sampling. */
                        if (stored < count) {
                            des[stored] = he;
                        } else {
                            unsigned long r = randomULong() % (stored + 1);
                            if (r < count) des[r] = he;
                        }
                        stored++;
                    }
                }
                if (stored >= count) goto end;
            }
        }
        i = (i+1) & maxsizemask;
    }

end:
    return stored > count ? count : stored;
}


/* Reallocate the dictEntry, key and value allocations in a bucket using the
 * provided allocation functions in order to defrag them. The child buckets of
 * the chain are reallocated too. */
static void dictDefragBucket(dict *d, dictBucket *b, dictDefragFunctions *defragfns) {
    dictDefragAllocFunction *defragalloc = defragfns->defragAlloc;
    dictDefragAllocFunction *defragkey = defragfns->defragKey;
    dictDefragAllocFunction *defragval = defragfns->defragVal;
    while (b) {
        bucketBits present = b->presence;
        while (present) {
            int pos = __builtin_ctz(present);
            present &= present - 1;
            dictEntry *de = b->slots[pos], *newde = NULL;
            void *newkey = defragkey ? defragkey(dictGetKey(de)) : NULL;

            if (d->type->no_value) {
                if (newkey) b->slots[pos] = encodeEntryKey(d, newkey);
            } else {
                void *newval = defragval ? defragval(dictGetVal(de)) : NULL;
                assert(entryIsNormal(de));
                newde = defragalloc(de);
                if (newde) de = newde;
                if (newkey) de->key = newkey;
                if (newval) de->v.val = newval;
            }
            if (newde) {
                b->slots[pos] = newde;
            }
        }
        dictBucket *child = getChildBucket(b), *newchild;
        if (child && (newchild = defragalloc(child))) {
            setChildBucket(b, newchild);
            child = newchild;
        }
        b = child;
    }
}

/* This is like dictGetRandomKey() from the POV of the API, but will do more
 * work to ensure a better distribution of the returned element.
 *
 * This function improves the distribution because the dictGetRandomKey()
 * problem is that it selects a random bucket, then it selects a random
 * element from the chain in the bucket. However elements being in different
 * chain lengths will have different probabilities of being reported. With
 * this function instead what we do is to consider a "linear" range of the table
 * that may be constituted of N buckets with chains of different lengths
 * appearing one after the other. Then we report a random element in the range.
 * In this way we smooth away the problem of different chain lengths. */
#define GETFAIR_NUM_ENTRIES 15
dictEntry *dictGetFairRandomKey(dict *d) {
    dictEntry *entries[GETFAIR_NUM_ENTRIES];
    unsigned int count = dictGetSomeKeys(d,entries,GETFAIR_NUM_ENTRIES);
    /* Note that dictGetSomeKeys() may return zero elements in an unlucky
     * run() even if there are actually elements inside the hash table. So
     * when we get zero, we call the true dictGetRandomKey() that will always
     * yield the element if the hash table has at least one. */
    if (count == 0) return dictGetRandomKey(d);
    unsigned int idx = rand() % count;
    return entries[idx];
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v) {
    unsigned long s = CHAR_BIT * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0UL;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument, the dictionary entry
 * 'de' as second argument and the link of the entry as third argument.
 *
 * The callback may delete, insert or replace entries. Bucket chains are not
 * compacted while a callback runs, so deletes leave holes and never move an
 * entry that is yet to be returned. The chain being scanned is compacted once
 * its callbacks are done, unless something else also pauses rehashing; holes
 * left in other chains are filled by later inserts, scans, safe iterators or
 * rehashing.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       void *privdata)
{
    return dictScanDefrag(d, v, fn, NULL, privdata);
}

void dictScanDefragBucket(dict *d,dictScanFunction *fn,
                          dictDefragFunctions *defragfns,
                          void *privdata,
                          int htidx, unsigned long idx) {
    dictBucket *root = &d->ht_table[htidx][idx];

    /* Defrag the bucket chain if requested, then emit its entries */
    if (defragfns) dictDefragBucket(d, root, defragfns);

    /* Rehashing is paused, so the deletes of the callbacks only clear presence
     * bits: no entry moves and no bucket is freed while the chain is walked.
     * presence is read again after each callback, since inserts may fill
     * holes or chain a full bucket. The child slot of a chained bucket never
     * has its presence bit set. */
    for (dictBucket *b = root; b; b = getChildBucket(b)) {
        bucketBits rest;
        for (int pos = 0; (rest = b->presence >> pos) != 0; pos++) {
            pos += __builtin_ctz(rest);
            fn(privdata, b->slots[pos], &b->slots[pos]);
        }
    }

    /* Fill the holes that deletes left in this chain, unless something else
     * (an outer scan or iterator of this dict, a held link) also pauses
     * rehashing: entries must not move under it. */
    if (d->pauserehash == 1) compactBucketChain(d, idx, htidx);
}

/* Like dictScan, but additionally reallocates the memory used by the dict
 * entries and child buckets using the provided allocation function. This
 * feature was added for the active defrag feature.
 *
 * The 'defragfns' callbacks are called with a pointer to memory that callback
 * can reallocate. The callbacks should return a new memory address or NULL,
 * where NULL means that no reallocation happened and the old memory is still
 * valid. */
unsigned long dictScanDefrag(dict *d,
                             unsigned long v,
                             dictScanFunction *fn,
                             dictDefragFunctions *defragfns,
                             void *privdata)
{
    int htidx0, htidx1;
    unsigned long m0, m1;

    if (dictSize(d) == 0) return 0;

    /* This is needed in case the scan callback tries to do dictFind or alike. */
    dictPauseRehashing(d);

    if (!dictIsRehashing(d)) {
        htidx0 = 0;
        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);
        dictScanDefragBucket(d, fn, defragfns, privdata, htidx0, v & m0);

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        v |= ~m0;

        /* Increment the reverse cursor */
        v = rev(v);
        v++;
        v = rev(v);

    } else {
        htidx0 = 0;
        htidx1 = 1;

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (DICTHT_SIZE(d->ht_size_exp[htidx0]) > DICTHT_SIZE(d->ht_size_exp[htidx1])) {
            htidx0 = 1;
            htidx1 = 0;
        }

        m0 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx0]);
        m1 = DICTHT_SIZE_MASK(d->ht_size_exp[htidx1]);

        dictScanDefragBucket(d, fn, defragfns, privdata, htidx0, v & m0);

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            dictScanDefragBucket(d, fn, defragfns, privdata, htidx1, v & m1);

            /* Increment the reverse cursor not covered by the smaller mask.*/
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    dictResumeRehashing(d);

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* Because we may need to allocate huge memory chunk at once when dict
 * resizes, we will check this allocation is allowed or not if the dict
 * type has resizeAllowed member function. */
static int dictTypeResizeAllowed(dict *d, size_t size) {
    if (d->type->resizeAllowed == NULL) return 1;
    return d->type->resizeAllowed(
                    DICTHT_SIZE(_dictNextExp(size)) * DICT_BUCKET_BYTES,
                    (double)d->ht_used[0] / (DICTHT_SIZE(d->ht_size_exp[0]) * DICT_BUCKET_SLOTS));
}

/* Returning DICT_OK indicates a successful expand or the dictionary is undergoing rehashing, 
 * and there is nothing else we need to do about this dictionary currently. While DICT_ERR indicates
 * that expand has not been triggered (may be try shrinking?)*/
int dictExpandIfNeeded(dict *d) {
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    if (DICTHT_SIZE(d->ht_size_exp[0]) == 0) {
        dictExpand(d, DICT_HT_INITIAL_SIZE);
        return DICT_OK;
    }

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/slots is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    size_t slots = DICTHT_SIZE(d->ht_size_exp[0]) * DICT_BUCKET_SLOTS;
    dictResizeEnable can_resize;
    atomicGet(dict_can_resize, can_resize);
    if ((can_resize == DICT_RESIZE_ENABLE &&
         d->ht_used[0] >= slots) ||
        (can_resize != DICT_RESIZE_FORBID &&
         d->ht_used[0] >= dict_force_resize_ratio * slots))
    {
        if (dictTypeResizeAllowed(d, d->ht_used[0] + 1))
            dictExpand(d, d->ht_used[0] + 1);
        return DICT_OK;
    }
    return DICT_ERR;
}

/* Expand the hash table if needed (OK=Expanded, ERR=Not expanded) */
static int _dictExpandIfNeeded(dict *d) {
    /* Automatic resizing is disallowed. Return */
    if (d->pauseAutoResize > 0) return DICT_ERR;
    
    return dictExpandIfNeeded(d);
}

/* Returning DICT_OK indicates a successful shrinking or the dictionary is undergoing rehashing, 
 * and there is nothing else we need to do about this dictionary currently. While DICT_ERR indicates
 * that shrinking has not been triggered (may be try expanding?)*/
int dictShrinkIfNeeded(dict *d) {
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;
    
    /* If the size of hash table is DICT_HT_INITIAL_SIZE, don't shrink it. */
    size_t slots = DICTHT_SIZE(d->ht_size_exp[0]) * DICT_BUCKET_SLOTS;
    if (slots <= DICT_HT_INITIAL_SIZE) return DICT_OK;

    /* If we reached below 1:8 elements/slots ratio, and we are allowed to resize
     * the hash table (global setting) or we should avoid it but the ratio is below 1:32,
     * we'll trigger a resize of the hash table. */
    dictResizeEnable can_resize;
    atomicGet(dict_can_resize, can_resize);
    if ((can_resize == DICT_RESIZE_ENABLE &&
         d->ht_used[0] * HASHTABLE_MIN_FILL <= slots) ||
        (can_resize != DICT_RESIZE_FORBID &&
         d->ht_used[0] * HASHTABLE_MIN_FILL * dict_force_resize_ratio <= slots))
    {
        if (dictTypeResizeAllowed(d, d->ht_used[0]))
            dictShrink(d, d->ht_used[0]);
        return DICT_OK;
    }
    return DICT_ERR;
}

static void _dictShrinkIfNeeded(dict *d) 
{
    /* Automatic resizing is disallowed. Return */
    if (d->pauseAutoResize > 0) return;

    dictShrinkIfNeeded(d);
}

static void _dictRehashStepIfNeeded(dict *d, uint64_t visitedIdx) {
    if ((!dictIsRehashing(d)) || (d->pauserehash != 0))
        return;
    /* rehashing not in progress if rehashidx == -1 */
    if ((long)visitedIdx >= d->rehashidx && !bucketIsEmpty(&d->ht_table[0][visitedIdx])) {
        /* If we have a valid hash entry at `idx` in ht0, we perform
         * rehash on the bucket at `idx` (being more CPU cache friendly) */
        _dictBucketRehash(d, visitedIdx);
    } else {
        /* If the hash entry is not in ht0, we rehash the buckets based
         * on the rehashidx (not CPU cache friendly). */
        dictRehash(d,1);
    }
}

/* Our hash table capability is a power of two number of buckets. We aim for a
 * fill of at most BUCKET_DIVISOR / (BUCKET_FACTOR * DICT_BUCKET_SLOTS) when the
 * table is sized for `size` elements, i.e. 91% with 7 slots per bucket
 * (32 / 5 / 7) or 89% with 12 (32 / 3 / 12), which lets us avoid a division:
 * buckets = ceil(size * BUCKET_FACTOR / BUCKET_DIVISOR). */
static signed char _dictNextExp(unsigned long size)
{
    if (size <= DICT_HT_INITIAL_SIZE) return DICT_HT_INITIAL_EXP;
    unsigned long buckets = (size * BUCKET_FACTOR - 1) / BUCKET_DIVISOR + 1;
    if (buckets == 1) return 0;
    if (buckets >= LONG_MAX) return (8*sizeof(long)-1);

    return 8*sizeof(long) - __builtin_clzl(buckets-1);
}

void dictEmpty(dict *d, void(callback)(dict*)) {
    /* Someone may be monitoring a dict that started rehashing, before
     * destroying the dict fake completion. */
    if (dictIsRehashing(d) && d->type->rehashingCompleted)
        d->type->rehashingCompleted(d);

    /* Subtract the size of all buckets. */
    if (d->type->bucketChanged)
        d->type->bucketChanged(d, -(long long)(dictBuckets(d) + dictChildBuckets(d)));

    _dictClear(d,0,callback);
    _dictClear(d,1,callback);
    d->rehashidx = -1;
    d->pauserehash = 0;
    d->pauseAutoResize = 0;
}

void dictSetResizeEnabled(dictResizeEnable enable) {
    atomicSet(dict_can_resize, enable);
}

/* Compiler inlines this for internal calls within dict.c (verified with -O3). */
uint64_t dictGetHash(dict *d, const void *key) {
    return d->type->hashFunction(key);
}

/* Provides the old and new ht size for a given dictionary during rehashing. This method
 * should only be invoked during initialization/rehashing. */
void dictRehashingInfo(dict *d, unsigned long long *from_size, unsigned long long *to_size) {
    /* Invalid method usage if rehashing isn't ongoing. */
    assert(dictIsRehashing(d));
    *from_size = DICTHT_SIZE(d->ht_size_exp[0]);
    *to_size = DICTHT_SIZE(d->ht_size_exp[1]);
}

/* ------------------------------- Debugging ---------------------------------*/
#define DICT_STATS_VECTLEN 50
void dictFreeStats(dictStats *stats) {
    zfree(stats->clvector);
    zfree(stats);
}

void dictCombineStats(dictStats *from, dictStats *into) {
    into->buckets += from->buckets;
    into->childBuckets += from->childBuckets;
    into->maxChainLen = (from->maxChainLen > into->maxChainLen) ? from->maxChainLen : into->maxChainLen;
    into->totalChainLen += from->totalChainLen;
    into->htSize += from->htSize;
    into->htUsed += from->htUsed;
    for (int i = 0; i < DICT_STATS_VECTLEN; i++) {
        into->clvector[i] += from->clvector[i];
    }
}

dictStats *dictGetStatsHt(dict *d, int htidx, int full) {
    unsigned long *clvector = zcalloc(sizeof(unsigned long) * DICT_STATS_VECTLEN);
    dictStats *stats = zcalloc(sizeof(dictStats));
    stats->htidx = htidx;
    stats->clvector = clvector;
    stats->htSize = DICTHT_SIZE(d->ht_size_exp[htidx]);
    stats->htUsed = d->ht_used[htidx];
    stats->childBuckets = d->child_buckets[htidx];
    if (!full) return stats;
    /* Compute stats. */
    for (unsigned long i = 0; i < DICTHT_SIZE(d->ht_size_exp[htidx]); i++) {
        /* Count the entries of this bucket and of its child buckets. */
        unsigned long chainlen = chainEntries(&d->ht_table[htidx][i]);

        if (chainlen == 0) {
            clvector[0]++;
            continue;
        }
        stats->buckets++;
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        if (chainlen > stats->maxChainLen) stats->maxChainLen = chainlen;
        stats->totalChainLen += chainlen;
    }

    return stats;
}

/* Generates human readable stats. */
size_t dictGetStatsMsg(char *buf, size_t bufsize, dictStats *stats, int full) {
    if (bufsize == 0) return 0;
    if (stats->htUsed == 0) {
        snprintf(buf,bufsize,
            "Hash table %d stats (%s):\n"
            "No stats available for empty dictionaries\n",
            stats->htidx, (stats->htidx == 0) ? "main hash table" : "rehashing target");
        return strlen(buf);
    }
    size_t l = 0;
    l += snprintf(buf + l, bufsize - l,
                  "Hash table %d stats (%s):\n"
                  " table size: %lu\n"
                  " number of elements: %lu\n",
                  stats->htidx, (stats->htidx == 0) ? "main hash table" : "rehashing target",
                  stats->htSize, stats->htUsed);
    /* snprintf() returns the length of the whole message, so once it is
     * truncated l >= bufsize, and bufsize - l would wrap around. */
    if (full && l < bufsize) {
        l += snprintf(buf + l, bufsize - l,
                      " different slots: %lu\n"
                      " child buckets: %lu\n"
                      " max chain length: %lu\n"
                      " avg chain length (counted): %.02f\n"
                      " avg chain length (computed): %.02f\n"
                      " Chain length distribution:\n",
                      stats->buckets, stats->childBuckets, stats->maxChainLen,
                      (float) stats->totalChainLen / stats->buckets, (float) stats->htUsed / stats->buckets);

        for (unsigned long i = 0; i < DICT_STATS_VECTLEN - 1; i++) {
            if (stats->clvector[i] == 0) continue;
            if (l >= bufsize) break;
            l += snprintf(buf + l, bufsize - l,
                          "   %ld: %ld (%.02f%%)\n",
                          i, stats->clvector[i], ((float) stats->clvector[i] / stats->htSize) * 100);
        }
    }

    /* Make sure there is a NULL term at the end. */
    buf[bufsize-1] = '\0';
    /* Unlike snprintf(), return the number of characters actually written. */
    return strlen(buf);
}

void dictGetStats(char *buf, size_t bufsize, dict *d, int full) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    if (bufsize == 0) return;
    dictStats *mainHtStats = dictGetStatsHt(d, 0, full);
    l = dictGetStatsMsg(buf, bufsize, mainHtStats, full);
    dictFreeStats(mainHtStats);
    buf += l;
    bufsize -= l;
    if (dictIsRehashing(d) && bufsize > 0) {
        dictStats *rehashHtStats = dictGetStatsHt(d, 1, full);
        dictGetStatsMsg(buf, bufsize, rehashHtStats, full);
        dictFreeStats(rehashHtStats);
    }
    /* Make sure there is a NULL term at the end. */
    orig_buf[orig_bufsize-1] = '\0';
}

static int dictDefaultCompare(dictCmpCache *cache, const void *key1, const void *key2) {
    (void)(cache); /*unused*/
    return key1 == key2;
}

/* ------------------------- Prefetch state machine ------------------------- */

/* Steps of a lookup driven by dictPrefetchNext(). */
enum {
    PREFETCH_BUCKET,        /* Pick the next table and prefetch the key's bucket */
    PREFETCH_MATCH,         /* Match the hash tags of the bucket (now in cache) */
    PREFETCH_CANDIDATE,     /* Prefetch the next tag-matching entry, or the child bucket */
    PREFETCH_ENTRY_KEY,     /* dictType-driven prefetch of the entry's key payload (for keyCompare) */
    PREFETCH_ENTRY_VALUE,   /* Compare keys; on match, dictType-driven prefetch of the value payload */
    PREFETCH_DONE           /* Indicates that prefetching for this key is complete */
};

/* Initialize the state of a prefetch lookup of `key` in `d`. The lookup is
 * done right away if `d` is NULL or empty. */
void dictPrefetchInit(dictPrefetchState *st, dict *d, const void *key) {
    st->d = d;
    st->key = key;
    st->hash = 0;
    st->bucket = NULL;
    st->current_entry = NULL;
    st->candidates = 0;
    st->ht_idx = -1;
    st->lone = 0;
    if (!d || dictSize(d) == 0) {
        st->stage = PREFETCH_DONE;
        st->done = 1;
        return;
    }
    /* We skip prefetch during loading, so ht_table[0] should never be NULL
     * when dictSize() > 0 (which only happens mid-dictEmpty via _dictReset). */
    assert(d->ht_table[0]);
    st->hash = dictGetHash(d, key);
    st->stage = PREFETCH_BUCKET;
    st->done = 0;
}

/* Advance the lookup until it gets to an address that is worth prefetching,
 * and return it. The steps follow a dictFind(): the bucket, the hash tag
 * match in the bucket (which is in cache by then), the tag-matching entry,
 * the key payload of the entry, and the key compare followed by the value
 * payload of the entry. If the key is not found in a bucket, its child bucket
 * is next, and then the bucket in the other table if the dict is rehashing.
 *
 * When the lookup completes, st->done is set and the address of the value
 * payload or NULL is returned. */
void *dictPrefetchNext(dictPrefetchState *st) {
    dict *d = st->d;
    while (1) {
        switch (st->stage) {
            case PREFETCH_BUCKET: {
                /* Pick the next table that may hold the key; none left means done. */
                int table = st->ht_idx + 1;
                size_t idx = 0;
                for (; table <= 1; table++) {
                    if (table == 1 && !dictIsRehashing(d)) {
                        table = 2;
                        break;
                    }
                    if (d->ht_used[table] == 0) continue;
                    idx = st->hash & DICTHT_SIZE_MASK(d->ht_size_exp[table]);
                    if (table == 0 && (long)idx < d->rehashidx) continue;
                    break;
                }
                if (table > 1) {
                    st->stage = PREFETCH_DONE;
                    st->done = 1;
                    return NULL;
                }
                st->ht_idx = table;
                st->bucket = &d->ht_table[table][idx];
                st->stage = PREFETCH_MATCH;
                return st->bucket;
            }
            case PREFETCH_MATCH: {
                dictBucket *b = st->bucket;
                st->candidates = b->presence & findHashMatches(b->hashes, hashTag(st->hash));
                st->lone = (st->candidates && (st->candidates & (st->candidates - 1)) == 0 &&
                            !b->chained && !dictIsRehashing(d));
                st->stage = PREFETCH_CANDIDATE;
                break;
            }
            case PREFETCH_CANDIDATE: {
                dictBucket *b = st->bucket;
                if (st->candidates) {
                    int pos = __builtin_ctz(st->candidates);
                    st->candidates &= st->candidates - 1;
                    st->current_entry = b->slots[pos];
                    st->stage = PREFETCH_ENTRY_KEY;
                    return st->current_entry;
                }
                /* No (more) candidates here: the child bucket, else the next table. */
                dictBucket *child = getChildBucket(b);
                if (child) {
                    st->bucket = child;
                    st->stage = PREFETCH_MATCH;
                    return child;
                }
                st->stage = PREFETCH_BUCKET;
                break;
            }
            case PREFETCH_ENTRY_KEY:
                st->stage = PREFETCH_ENTRY_VALUE;
                if (d->type->prefetchEntryKey) {
                    void *addr = d->type->prefetchEntryKey(st->current_entry);
                    if (addr) return addr;
                }
                break;
            case PREFETCH_ENTRY_VALUE: {
                const void *cmp_key = dictStoredKey2Key(d, dictGetKey(st->current_entry));
                /* The only tag match in an unchained bucket, while not rehashing,
                 * is assumed to be a hit without comparing the keys. */
                if (st->lone || dictCompareKeys(d, st->key, cmp_key)) {
                    st->stage = PREFETCH_DONE;
                    st->done = 1;
                    if (d->type->prefetchEntryValue)
                        return d->type->prefetchEntryValue(st->current_entry);
                    return NULL;
                }
                st->stage = PREFETCH_CANDIDATE;
                break;
            }
            default:
                st->done = 1;
                return NULL;
        }
    }
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef REDIS_TEST
#include "testhelp.h"

#define UNUSED(V) ((void) V)
#define TEST(name) printf("test — %s\n", name);

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

int compareCallback(dictCmpCache *cache, const void *key1, const void *key2) {
    int l1,l2;
    UNUSED(cache);

    l1 = strlen((char*)key1);
    l2 = strlen((char*)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(dict *d, void *val) {
    UNUSED(d);

    zfree(val);
}

char *stringFromLongLong(long long value) {
    char buf[32];
    int len;
    char *s;

    len = snprintf(buf,sizeof(buf),"%lld",value);
    s = zmalloc(len+1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

char *stringFromSubstring(void) {
    #define LARGE_STRING_SIZE 10000
    #define MIN_STRING_SIZE 100
    #define MAX_STRING_SIZE 500
    static char largeString[LARGE_STRING_SIZE + 1];
    static int init = 0;
    if (init == 0) {
        /* Generate a large string */
        for (size_t i = 0; i < LARGE_STRING_SIZE; i++) {
            /* Random printable ASCII character (33 to 126) */
            largeString[i] = 33 + (rand() % 94);
        }
        /* Null-terminate the large string */
        largeString[LARGE_STRING_SIZE] = '\0';
        init = 1;
    }
    /* Randomly choose a size between minSize and maxSize */
    size_t substringSize = MIN_STRING_SIZE + (rand() % (MAX_STRING_SIZE - MIN_STRING_SIZE + 1));
    size_t startIndex = rand() % (LARGE_STRING_SIZE - substringSize + 1);
    /* Allocate memory for the substring (+1 for null terminator) */
    char *s = zmalloc(substringSize + 1);
    memcpy(s, largeString + startIndex, substringSize); // Copy the substring
    s[substringSize] = '\0'; // Null-terminate the string
    return s;
}

dictType BenchmarkDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL,
    NULL
};

/* Same as BenchmarkDictType, but a no_value=1 (set-style) dict -- used to verify
 * that dictMemUsage() charges nothing per entry for dicts without values. */
static dictType BenchmarkDictTypeNoValue = {
    .hashFunction = hashCallback,
    .keyCompare = compareCallback,
    .keyDestructor = freeCallback,
    .no_value = 1,
};

/* ------------------------- verification helpers --------------------------- */

/* Sum of the bucketChanged() deltas of the dict under test. */
static long long test_buckets = 0;

static void testBucketChanged(dict *d, long long delta) {
    UNUSED(d);
    test_buckets += delta;
}

/* A hash function that maps every key to the same bucket chain (but still
 * produces a valid tag), to build long bucket chains. */
static uint64_t constHashCallback(const void *key) {
    UNUSED(key);
    return 0x5a5a5a5a5a5a5a5aULL;
}

/* Maps every key to one of 4 bucket indexes (but keeps the real tag), so
 * there are several long chains instead of a single one. */
static uint64_t fewBucketsHashCallback(const void *key) {
    return (hashCallback(key) & 0x3) | (hashCallback(key) & 0xff00000000000000ULL);
}

static dictType verifyDictType = {
    .hashFunction = hashCallback,
    .keyCompare = compareCallback,
    .keyDestructor = freeCallback,
    .no_value = 1,
    .keys_are_odd = 0,
    .bucketChanged = testBucketChanged,
};

static dictType verifyDictTypeVal = {
    .hashFunction = hashCallback,
    .keyCompare = compareCallback,
    .keyDestructor = freeCallback,
    .bucketChanged = testBucketChanged,
};

static dictType chainDictType = {
    .hashFunction = constHashCallback,
    .keyCompare = compareCallback,
    .keyDestructor = freeCallback,
    .no_value = 1,
    .bucketChanged = testBucketChanged,
};

static dictType fewBucketsDictType = {
    .hashFunction = fewBucketsHashCallback,
    .keyCompare = compareCallback,
    .keyDestructor = freeCallback,
    .bucketChanged = testBucketChanged,
};

/* Checks the structural invariants of the dict: presence bits, tags, bucket
 * indexes, chains, counters and memory accounting. */
static void dictVerify(dict *d) {
    size_t counted[2] = {0, 0}, children[2] = {0, 0};
    for (int t = 0; t <= 1; t++) {
        if (!d->ht_table[t]) {
            assert(d->ht_size_exp[t] == -1);
            assert(d->ht_used[t] == 0 && d->child_buckets[t] == 0);
            continue;
        }
        size_t nb = DICTHT_SIZE(d->ht_size_exp[t]);
        for (size_t idx = 0; idx < nb; idx++) {
            dictBucket *root = &d->ht_table[t][idx];
            if (t == 0 && dictIsRehashing(d) && (long)idx < d->rehashidx)
                assert(bucketIsEmpty(root)); /* already moved */
            for (dictBucket *b = root; b; b = getChildBucket(b)) {
                if (b != root) children[t]++;
                if (b->chained) {
                    assert(!isPositionFilled(b, DICT_BUCKET_SLOTS - 1));
                    assert(getChildBucket(b) != NULL);
                }
                for (int pos = 0; pos < DICT_BUCKET_SLOTS; pos++) {
                    if (!isPositionFilled(b, pos)) continue;
                    dictEntry *e = b->slots[pos];
                    if (d->type->no_value) assert(entryIsKey(e));
                    else assert(entryIsNormal(e));
                    uint64_t hash = dictGetHash(d, dictStoredKey2Key(d, dictGetKey(e)));
                    assert(hashTag(hash) == b->hashes[pos]);
                    assert((hash & DICTHT_SIZE_MASK(d->ht_size_exp[t])) == idx);
                    counted[t]++;
                }
            }
        }
    }
    for (int t = 0; t <= 1; t++) {
        assert(counted[t] == d->ht_used[t]);
        assert(children[t] == d->child_buckets[t]);
    }
    if (dictIsRehashing(d)) {
        assert(d->ht_table[0] && d->ht_table[1]);
        assert(d->rehashidx >= 0 && (size_t)d->rehashidx <= DICTHT_SIZE(d->ht_size_exp[0]));
    } else {
        assert(d->ht_table[1] == NULL && d->ht_used[1] == 0);
    }
    if (d->type->bucketChanged == testBucketChanged)
        assert(test_buckets == (long long)(dictBuckets(d) + dictChildBuckets(d)));
}

/* Finish an ongoing rehashing. Rehashing is not forced under
 * DICT_RESIZE_AVOID, so make sure it's enabled. */
static void drainRehash(dict *d) {
    dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    while (dictIsRehashing(d)) dictRehashMicroseconds(d, 1000);
}

/* Reference model for randomized tests: keys are "k<id>" strings. */
#define MODEL_KEYS 4000

typedef struct modelCtx {
    uint8_t present[MODEL_KEYS];
    uint32_t seen[MODEL_KEYS];
    unsigned long count;
    int delete_percent; /* in scan/iterator callbacks */
    dict *d;
} modelCtx;

static int keyId(const void *key) {
    return atoi((const char *)key + 1);
}

static char *keyForId(int id) {
    char buf[32];
    snprintf(buf, sizeof(buf), "k%d", id);
    size_t len = strlen(buf) + 1;
    char *s = zmalloc(len);
    memcpy(s, buf, len);
    return s;
}

static int modelAdd(modelCtx *m, dict *d, int id) {
    char *k = keyForId(id);
    int ret = dictAdd(d, k, (void *)(long)id);
    if (m->present[id]) {
        assert(ret == DICT_ERR);
        zfree(k);
        return 0;
    }
    assert(ret == DICT_OK);
    m->present[id] = 1;
    m->count++;
    return 1;
}

static int modelDel(modelCtx *m, dict *d, int id) {
    char *k = keyForId(id);
    int ret = dictDelete(d, k);
    zfree(k);
    if (!m->present[id]) {
        assert(ret == DICT_ERR);
        return 0;
    }
    assert(ret == DICT_OK);
    m->present[id] = 0;
    m->count--;
    return 1;
}

/* Verify content equals the model by iterating (safe) and by lookups. */
static void modelCheck(modelCtx *m, dict *d) {
    assert(dictSize(d) == m->count);
    memset(m->seen, 0, sizeof(m->seen));
    dictIterator it;
    dictEntry *de;
    dictInitSafeIterator(&it, d);
    while ((de = dictNext(&it))) {
        int id = keyId(dictGetKey(de));
        assert(m->present[id]);
        assert(++m->seen[id] == 1);
    }
    dictResetIterator(&it);
    for (int i = 0; i < MODEL_KEYS; i++) assert(m->seen[i] == m->present[i]);
    for (int i = 0; i < MODEL_KEYS; i += 7) {
        char *k = keyForId(i);
        assert((dictFind(d, k) != NULL) == m->present[i]);
        zfree(k);
    }
    dictVerify(d);
}

static void modelScanCb(void *privdata, const dictEntry *de, dictEntryLink plink) {
    modelCtx *m = privdata;
    UNUSED(plink);
    int id = keyId(dictGetKey(de));
    m->seen[id]++;
    if ((int)(rand() % 100) < m->delete_percent) {
        /* Deleting the entry passed to us is allowed. */
        char *k = keyForId(id);
        assert(dictDelete(m->d, k) == DICT_OK);
        zfree(k);
        m->present[id] = 0;
        m->count--;
    }
}

typedef struct scanMutationCtx {
    dict *d;
    int seen[DICT_BUCKET_SLOTS + 3];
    int deletion; /* dictDelete, dictUnlink, or two-phase unlink */
    int additions;
    int nested;
    int delete_id;
} scanMutationCtx;

static void scanMutationCb(void *privdata, const dictEntry *de, dictEntryLink plink) {
    scanMutationCtx *ctx = privdata;
    int id = keyId(dictGetKey(de));
    UNUSED(plink);
    assert(id >= 0 && id < DICT_BUCKET_SLOTS + 3);
    ctx->seen[id]++;
    if (ctx->deletion == 3) {
        /* Refilling an unchained hole is an insertion, not compaction. */
        assert(ctx->seen[id] == 1);
        assert(dictDelete(ctx->d, dictGetKey(de)) == DICT_OK);
        assert(dictAdd(ctx->d, keyForId(id), NULL) == DICT_OK);
        return;
    }
    if (id != ctx->delete_id) return;

    if (ctx->nested == 1) {
        ctx->nested = 2;
        assert(dictScan(ctx->d, 0, scanMutationCb, ctx) == 0);
        return;
    }
    char *key = keyForId(id);
    if (ctx->deletion == 0) {
        assert(dictDelete(ctx->d, key) == DICT_OK);
    } else if (ctx->deletion == 1) {
        dictEntry *unlinked = dictUnlink(ctx->d, key);
        assert(unlinked != NULL);
        dictFreeUnlinkedEntry(ctx->d, unlinked);
    } else {
        int table;
        dictEntryLink link = dictTwoPhaseUnlinkFind(ctx->d, key, &table);
        assert(link != NULL);
        dictTwoPhaseUnlinkFree(ctx->d, link, table);
    }
    zfree(key);
    for (int i = 0; i < ctx->additions; i++)
        assert(dictAdd(ctx->d, keyForId(DICT_BUCKET_SLOTS + 1 + i), NULL) == DICT_OK);
}

#define SCAN_DELETE_PREV_KEYS (3 * DICT_BUCKET_SLOTS)

typedef struct scanDeletePrevCtx {
    dict *d;
    int seen[SCAN_DELETE_PREV_KEYS];
    int prev_id;
} scanDeletePrevCtx;

/* Deletes the entry emitted before this one, which is not the entry passed
 * to the callback. Compacting right away would move an entry from the end of
 * the chain into that already scanned slot. */
static void scanDeletePrevCb(void *privdata, const dictEntry *de, dictEntryLink plink) {
    scanDeletePrevCtx *ctx = privdata;
    int id = keyId(dictGetKey(de));
    UNUSED(plink);
    assert(id >= 0 && id < SCAN_DELETE_PREV_KEYS);
    ctx->seen[id]++;
    if (ctx->prev_id >= 0) {
        char *key = keyForId(ctx->prev_id);
        assert(dictDelete(ctx->d, key) == DICT_OK);
        zfree(key);
    }
    ctx->prev_id = id;
}

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg) do { \
    elapsed = timeInMilliseconds()-start; \
    printf(msg ": %ld items in %lld ms\n", count, elapsed); \
} while(0)

/* Randomized test against a reference model. */
static void dictRandomizedTest(dictType *type, int rounds, int ops_per_round) {
    modelCtx *m = zcalloc(sizeof(*m));
    test_buckets = 0;
    dict *d = dictCreate(type);
    m->d = d;
    dictSetResizeEnabled(DICT_RESIZE_ENABLE);

    for (int round = 0; round < rounds; round++) {
        /* Phase profile: mostly growth, then mostly shrink, etc. */
        int add_bias = (round % 4 < 2) ? 70 : 30;
        for (int op = 0; op < ops_per_round; op++) {
            int id = rand() % MODEL_KEYS;
            int r = rand() % 100;
            if (r < add_bias) modelAdd(m, d, id);
            else if (r < 92) modelDel(m, d, id);
            else if (r < 94) {
                char *k = keyForId(id);
                dictEntry *de = dictFind(d, k);
                assert((de != NULL) == m->present[id]);
                zfree(k);
            } else if (r < 96) {
                /* Two-phase delete with nested lookups in between. */
                char *k = keyForId(id);
                int table;
                dictEntryLink link = dictTwoPhaseUnlinkFind(d, k, &table);
                assert((link != NULL) == m->present[id]);
                if (link) {
                    char *k2 = keyForId(rand() % MODEL_KEYS);
                    dictFind(d, k2); /* may look up, must not move entries */
                    zfree(k2);
                    dictTwoPhaseUnlinkFree(d, link, table);
                    m->present[id] = 0;
                    m->count--;
                }
                zfree(k);
            } else if (r < 97) {
                dictRehash(d, 1 + rand() % 3);
            } else if (r < 98) {
                dictSetResizeEnabled(rand() % 2 ? DICT_RESIZE_AVOID : DICT_RESIZE_ENABLE);
            } else if (r < 99) {
                if (rand() % 2) dictExpand(d, dictSize(d) * 2 + 1);
                else dictShrink(d, dictSize(d) + 1);
            } else {
                drainRehash(d);
            }
            assert(dictSize(d) == m->count);
        }
        dictVerify(d);

        /* Safe iteration while deleting what was returned and inserting
         * keys that were absent at the start and not deleted (inserts may
         * or may not be returned, but at most once). */
        {
            memset(m->seen, 0, sizeof(m->seen));
            uint8_t initial[MODEL_KEYS];
            memcpy(initial, m->present, sizeof(initial));
            uint8_t deleted[MODEL_KEYS] = {0};
            dictIterator it;
            dictEntry *de;
            dictInitSafeIterator(&it, d);
            while ((de = dictNext(&it))) {
                int id = keyId(dictGetKey(de));
                assert(m->present[id]);
                m->seen[id]++;
                int r = rand() % 100;
                if (r < 30) {
                    modelDel(m, d, id); /* the returned entry */
                    deleted[id] = 1;
                } else if (r < 50) {
                    /* Only insert keys that were absent at the start and were
                     * not returned and deleted: the iterator may return
                     * those, but only once. */
                    int nid = rand() % MODEL_KEYS;
                    if (!initial[nid] && !deleted[nid]) modelAdd(m, d, nid);
                }
            }
            dictResetIterator(&it);
            for (int i = 0; i < MODEL_KEYS; i++) {
                assert(m->seen[i] <= 1);
                if (initial[i] && !deleted[i]) assert(m->seen[i] == 1);
            }
            assert(dictSize(d) == m->count);
            dictVerify(d);
        }

        /* Full scan with deletes of the current entry in the callback. */
        {
            memset(m->seen, 0, sizeof(m->seen));
            uint8_t initial[MODEL_KEYS];
            memcpy(initial, m->present, sizeof(initial));
            m->delete_percent = rand() % 40;
            unsigned long cursor = 0;
            do {
                cursor = dictScan(d, cursor, modelScanCb, m);
                /* Interleave a bit of rehashing between scan calls. */
                if (rand() % 4 == 0) dictRehash(d, 1);
            } while (cursor);
            m->delete_percent = 0;
            for (int i = 0; i < MODEL_KEYS; i++)
                if (initial[i]) assert(m->seen[i] >= 1);
            assert(dictSize(d) == m->count);
            dictVerify(d);
        }

        if (round % 3 == 0) modelCheck(m, d);
    }
    dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    drainRehash(d);
    modelCheck(m, d);
    dictRelease(d);
    assert(test_buckets == 0);
    zfree(m);
}

/* Exercise every deletion API, including the key-clearing step used by
 * asynchronous database deletion. */
static void testDeleteKey(dict *d, const char *key, int deletion) {
    if (deletion == 0) {
        assert(dictDelete(d, key) == DICT_OK);
    } else if (deletion == 1) {
        dictEntry *de = dictUnlink(d, key);
        assert(de != NULL);
        dictFreeUnlinkedEntry(d, de);
    } else {
        int table;
        dictEntryLink link = dictTwoPhaseUnlinkFind(d, key, &table);
        assert(link != NULL);
        if (deletion == 3) {
            void *oldkey = dictGetKey(*link);
            dictSetKeyAtLink(d, NULL, &link, 0);
            zfree(oldkey);
        }
        dictTwoPhaseUnlinkFree(d, link, table);
    }
}

/* ./redis-server test dict [<count> | --accurate] */
int dictTest(int argc, char **argv, int flags) {
    long j;
    long long start, elapsed;
    int retval;
    dict *d = dictCreate(&BenchmarkDictType);
    dictEntry* de = NULL;
    dictEntry* existing = NULL;
    long count = 0;
    unsigned long current_dict_used, remain_keys, capacity, new_buckets;
    int accurate = (flags & REDIS_TEST_ACCURATE);

    if (argc == 4) {
        if (accurate) {
            count = 5000000;
        } else {
            count = strtol(argv[3],NULL,10);
        }
    } else {
        count = 5000;
    }

    TEST("Initial table is a single bucket") {
        assert(dictBuckets(d) == 0);
        retval = dictAdd(d, stringFromLongLong(1000000), (void*)1);
        assert(retval == DICT_OK);
        assert(dictBuckets(d) == 1);
        assert(dictChildBuckets(d) == 0);
        assert(dictDelete(d, "1000000") == DICT_OK);
        assert(dictBuckets(d) == 1); /* never shrinks below one bucket */
        dictVerify(d);
    }

    TEST("Add 16 keys and verify dict resize is ok") {
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
        for (j = 0; j < 16; j++) {
            retval = dictAdd(d,stringFromLongLong(j),(void*)j);
            assert(retval == DICT_OK);
        }
        drainRehash(d);
        assert(dictSize(d) == 16);
        assert(dictBuckets(d) * DICT_BUCKET_SLOTS >= 16);
        /* A power of two number of buckets. */
        assert((dictBuckets(d) & (dictBuckets(d) - 1)) == 0);
        dictVerify(d);
    }

    TEST("Use DICT_RESIZE_AVOID to disable the dict resize and pad to (dict_force_resize_ratio * capacity)") {
        dictSetResizeEnabled(DICT_RESIZE_AVOID);
        capacity = dictBuckets(d) * DICT_BUCKET_SLOTS;
        for (j = 16; j < (long)(dict_force_resize_ratio * capacity); j++) {
            retval = dictAdd(d,stringFromLongLong(j),(void*)j);
            assert(retval == DICT_OK);
        }
        current_dict_used = dict_force_resize_ratio * capacity;
        assert(dictSize(d) == current_dict_used);
        assert(dictBuckets(d) * DICT_BUCKET_SLOTS == capacity);
        assert(!dictIsRehashing(d));
        dictVerify(d);
    }

    TEST("Add one more key, trigger the dict resize") {
        retval = dictAdd(d,stringFromLongLong(current_dict_used),(void*)(current_dict_used));
        assert(retval == DICT_OK);
        current_dict_used++;
        new_buckets = DICTHT_SIZE(_dictNextExp(current_dict_used));
        assert(dictSize(d) == current_dict_used);
        assert(dictIsRehashing(d));
        assert(DICTHT_SIZE(d->ht_size_exp[0]) * DICT_BUCKET_SLOTS == capacity);
        assert(DICTHT_SIZE(d->ht_size_exp[1]) == new_buckets);
        dictVerify(d);

        /* Wait for rehashing. */
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
        drainRehash(d);
        assert(dictSize(d) == current_dict_used);
        assert(DICTHT_SIZE(d->ht_size_exp[0]) == new_buckets);
        assert(d->ht_size_exp[1] == -1);
        dictVerify(d);
    }

    TEST("Delete keys until we can trigger shrink in next test") {
        /* Delete keys until we can satisfy (1 / HASHTABLE_MIN_FILL) in the next test. */
        capacity = dictBuckets(d) * DICT_BUCKET_SLOTS;
        for (j = capacity / HASHTABLE_MIN_FILL + 1; j < (long)current_dict_used; j++) {
            char *key = stringFromLongLong(j);
            retval = dictDelete(d, key);
            zfree(key);
            assert(retval == DICT_OK);
        }
        current_dict_used = capacity / HASHTABLE_MIN_FILL + 1;
        assert(dictSize(d) == current_dict_used);
        assert(dictBuckets(d) * DICT_BUCKET_SLOTS == capacity);
        assert(!dictIsRehashing(d));
    }

    TEST("Delete one more key, trigger the dict resize") {
        current_dict_used--;
        char *key = stringFromLongLong(current_dict_used);
        retval = dictDelete(d, key);
        zfree(key);
        new_buckets = DICTHT_SIZE(_dictNextExp(current_dict_used));
        assert(retval == DICT_OK);
        assert(dictSize(d) == current_dict_used);
        assert(dictIsRehashing(d));
        assert(DICTHT_SIZE(d->ht_size_exp[0]) * DICT_BUCKET_SLOTS == capacity);
        assert(DICTHT_SIZE(d->ht_size_exp[1]) == new_buckets);

        /* Wait for rehashing. */
        drainRehash(d);
        assert(dictSize(d) == current_dict_used);
        assert(DICTHT_SIZE(d->ht_size_exp[0]) == new_buckets);
        assert(d->ht_size_exp[1] == -1);
        dictVerify(d);
    }

    TEST("Empty the dictionary and add 128 keys") {
        dictEmpty(d, NULL);
        for (j = 0; j < 128; j++) {
            retval = dictAdd(d,stringFromLongLong(j),(void*)j);
            assert(retval == DICT_OK);
        }
        drainRehash(d);
        assert(dictSize(d) == 128);
        dictVerify(d);
    }

    TEST("Use DICT_RESIZE_AVOID to disable the dict resize and reduce to a few keys") {
        /* Use DICT_RESIZE_AVOID to disable the dict reset, and reduce
         * the number of keys until we can trigger shrinking in next test. */
        dictSetResizeEnabled(DICT_RESIZE_AVOID);
        capacity = dictBuckets(d) * DICT_BUCKET_SLOTS;
        remain_keys = capacity / (HASHTABLE_MIN_FILL * dict_force_resize_ratio) + 1;
        for (j = remain_keys; j < 128; j++) {
            char *key = stringFromLongLong(j);
            retval = dictDelete(d, key);
            zfree(key);
            assert(retval == DICT_OK);
        }
        current_dict_used = remain_keys;
        assert(dictSize(d) == remain_keys);
        assert(dictBuckets(d) * DICT_BUCKET_SLOTS == capacity);
        assert(!dictIsRehashing(d));
    }

    TEST("Delete one more key, trigger the dict resize (AVOID)") {
        current_dict_used--;
        char *key = stringFromLongLong(current_dict_used);
        retval = dictDelete(d, key);
        zfree(key);
        assert(retval == DICT_OK);
        assert(dictSize(d) == current_dict_used);
        assert(dictIsRehashing(d));
        assert(DICTHT_SIZE(d->ht_size_exp[0]) * DICT_BUCKET_SLOTS == capacity);
        new_buckets = DICTHT_SIZE(d->ht_size_exp[1]);

        /* Wait for rehashing. */
        for (int steps = 0; dictIsRehashing(d) && steps < 1000; steps++)
            dictRehash(d, 100);
        assert(!dictIsRehashing(d));
        assert(dictSize(d) == current_dict_used);
        assert(DICTHT_SIZE(d->ht_size_exp[0]) == new_buckets);
        assert(d->ht_size_exp[1] == -1);
        dictVerify(d);
    }

    TEST("Forced shrink at 1/32 occupancy finishes under DICT_RESIZE_AVOID") {
        dictType type = BenchmarkDictType;
        type.hashFunction = constHashCallback;
        dict *ds = dictCreate(&type);
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
        assert(dictExpand(ds, 800) == DICT_OK);
        for (int i = 0; i < 100; i++)
            assert(dictAdd(ds, stringFromLongLong(i), NULL) == DICT_OK);
        size_t threshold = dictBuckets(ds) * DICT_BUCKET_SLOTS / (HASHTABLE_MIN_FILL * dict_force_resize_ratio);
        dictSetResizeEnabled(DICT_RESIZE_AVOID);
        for (int i = 99; i >= (int)threshold; i--) {
            char *key = stringFromLongLong(i);
            assert(dictDelete(ds, key) == DICT_OK);
            zfree(key);
            if (dictSize(ds) > threshold) assert(!dictIsRehashing(ds));
        }
        assert(dictIsRehashing(ds));
        assert(DICTHT_SIZE(ds->ht_size_exp[0]) == 16 * DICTHT_SIZE(ds->ht_size_exp[1]));
        for (int steps = 0; dictIsRehashing(ds) && steps < 1000; steps++)
            dictRehash(ds, 100);
        assert(!dictIsRehashing(ds));
        assert(dictSize(ds) == threshold);
        dictVerify(ds);
        dictRelease(ds);
    }

    TEST("Restore to original state") {
        dictEmpty(d, NULL);
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    }

    TEST("dictMemUsage charges one dictEntry per element only for dicts with values") {
        long n = 1000;
        dict *dn = dictCreate(&BenchmarkDictType);          /* no_value = 0 */
        dict *dv = dictCreate(&BenchmarkDictTypeNoValue);   /* no_value = 1 */
        for (long i = 0; i < n; i++) {
            assert(dictAdd(dn, stringFromLongLong(i), (void *)i) == DICT_OK);
            assert(dictAdd(dv, stringFromLongLong(i), NULL) == DICT_OK);
        }

        /* Identical keys and resize policy => identical bucket geometry, so the
         * two dicts' reported memory differs only by the dictEntry of each of
         * the n elements that the no_value dict does not allocate. */
        assert(dictSize(dn) == (unsigned long)n && dictSize(dv) == (unsigned long)n);
        assert(dictBuckets(dn) == dictBuckets(dv));
        assert(dictEntryMemUsage(1) == 0 && dictEntryMemUsage(0) == sizeof(dictEntry));
        assert(dictMemUsage(dn) - dictMemUsage(dv) == (size_t)n * dictEntryMemUsage(0));
        assert(dictMemUsage(dv) == (dictBuckets(dv) + dictChildBuckets(dv)) * DICT_BUCKET_BYTES);

        dictRelease(dn);
        dictRelease(dv);
    }

    TEST("Long bucket chains: insert, delete, compaction and accounting") {
        test_buckets = 0;
        dict *dc = dictCreate(&chainDictType);
        const int n = 300; /* all keys share one chain */
        for (int i = 0; i < n; i++) {
            assert(dictAdd(dc, stringFromLongLong(i), NULL) == DICT_OK);
            dictVerify(dc);
        }
        assert(dc->child_buckets[0] + dc->child_buckets[1] > 0);
        /* Delete in a scattered order, every deletion must keep the chain
         * valid and compact it. */
        for (int i = 0; i < n; i += 2) {
            char *k = stringFromLongLong(i);
            assert(dictDelete(dc, k) == DICT_OK);
            zfree(k);
            dictVerify(dc);
        }
        drainRehash(dc);
        for (int i = 1; i < n; i += 2) {
            char *k = stringFromLongLong(i);
            assert(dictFind(dc, k) != NULL);
            zfree(k);
        }
        for (int i = 0; i < n; i += 2) {
            char *k = stringFromLongLong(i);
            assert(dictFind(dc, k) == NULL);
            zfree(k);
        }
        /* Chain is fully compacted: only as many buckets as needed. */
        unsigned long used = dictSize(dc);
        unsigned long needed = (used + DICT_BUCKET_SLOTS - 2) / (DICT_BUCKET_SLOTS - 1);
        unsigned long children = dc->child_buckets[0];
        assert(children <= needed + 1);
        dictRelease(dc);
        assert(test_buckets == 0);
    }

    TEST("Deleting tail entries prunes overflow buckets through every deletion API") {
        for (int no_value = 0; no_value <= 1; no_value++) {
            dictType type = chainDictType;
            type.no_value = no_value;
            for (int deletion = 0; deletion < 4; deletion++) {
                test_buckets = 0;
                dict *dc = dictCreate(&type);
                const int n = 2 * DICT_BUCKET_SLOTS;
                assert(dictExpand(dc, n) == DICT_OK);
                for (int i = 0; i < n; i++)
                    assert(dictAdd(dc, keyForId(i), NULL) == DICT_OK);
                unsigned long roots = dictBuckets(dc);
                assert(dictChildBuckets(dc) == 2);
                for (int i = n - 1; i >= DICT_BUCKET_SLOTS - 1; i--) {
                    char *key = keyForId(i);
                    testDeleteKey(dc, key, deletion);
                    zfree(key);
                    unsigned long children = (dictSize(dc) - 2) / (DICT_BUCKET_SLOTS - 1);
                    assert(dictBuckets(dc) == roots && !dictIsRehashing(dc));
                    assert(dictChildBuckets(dc) == children);
                    assert(dictMemUsage(dc) == (roots + children) * DICT_BUCKET_BYTES +
                                              dictSize(dc) * dictEntryMemUsage(no_value));
                    dictVerify(dc);
                }
                for (int i = 0; i < DICT_BUCKET_SLOTS - 1; i++) {
                    char *key = keyForId(i);
                    assert(dictFind(dc, key) != NULL);
                    zfree(key);
                }
                assert(dictChildBuckets(dc) == 0);
                dictRelease(dc);
                assert(test_buckets == 0);
            }
        }
    }

    TEST("Tail pruning stays paused during safe iteration") {
        for (int deletion = 0; deletion < 4; deletion++) {
            test_buckets = 0;
            dict *dc = dictCreate(&chainDictType);
            const int n = 2 * DICT_BUCKET_SLOTS;
            assert(dictExpand(dc, n) == DICT_OK);
            for (int i = 0; i < n; i++)
                assert(dictAdd(dc, keyForId(i), NULL) == DICT_OK);
            dictIterator it;
            dictEntry *de;
            int visited = 0;
            dictInitSafeIterator(&it, dc);
            while ((de = dictNext(&it))) {
                int id = keyId(dictGetKey(de));
                visited++;
                if (id >= DICT_BUCKET_SLOTS - 1) {
                    char *key = keyForId(id);
                    testDeleteKey(dc, key, deletion);
                    zfree(key);
                    assert(dictChildBuckets(dc) == 2);
                }
            }
            dictResetIterator(&it);
            assert(visited == n);
            assert(dictSize(dc) == DICT_BUCKET_SLOTS - 1 && dictChildBuckets(dc) == 0);
            dictVerify(dc);
            dictRelease(dc);
            assert(test_buckets == 0);
        }
    }

    TEST("Safe iterator deleting the returned entry in a long chain returns each entry once") {
        test_buckets = 0;
        dict *dc = dictCreate(&chainDictType);
        const int n = 200;
        for (int i = 0; i < n; i++) assert(dictAdd(dc, stringFromLongLong(i), NULL) == DICT_OK);
        uint8_t seen[200] = {0};
        dictIterator it;
        dictInitSafeIterator(&it, dc);
        dictEntry *e;
        int visited = 0;
        while ((e = dictNext(&it))) {
            int id = atoi(dictGetKey(e));
            assert(!seen[id]);
            seen[id] = 1;
            visited++;
            if (id % 3 != 0) {
                char *k = stringFromLongLong(id);
                assert(dictDelete(dc, k) == DICT_OK);
                zfree(k);
            }
        }
        dictResetIterator(&it);
        assert(visited == n);
        dictVerify(dc);
        assert(dictSize(dc) == (unsigned long)((n + 2) / 3));
        dictRelease(dc);
        assert(test_buckets == 0);
    }

    TEST("Safe iterator inserting while iterating never returns an entry twice") {
        test_buckets = 0;
        dict *dc = dictCreate(&chainDictType);
        const int n = 40;
        for (int i = 0; i < n; i++) assert(dictAdd(dc, stringFromLongLong(i), NULL) == DICT_OK);
        uint8_t seen[400] = {0};
        dictIterator it;
        dictInitSafeIterator(&it, dc);
        dictEntry *e;
        int next_new = n;
        while ((e = dictNext(&it))) {
            int id = atoi(dictGetKey(e));
            assert(id < 400 && !seen[id]);
            seen[id] = 1;
            if (next_new < 399) {
                assert(dictAdd(dc, stringFromLongLong(next_new++), NULL) == DICT_OK);
            }
        }
        dictResetIterator(&it);
        for (int i = 0; i < n; i++) assert(seen[i]);
        dictVerify(dc);
        dictRelease(dc);
        assert(test_buckets == 0);
    }

    TEST("Scan: callback deletes the entry it is given, chains stay valid") {
        test_buckets = 0;
        dict *dc = dictCreate(&fewBucketsDictType);
        modelCtx *m = zcalloc(sizeof(*m));
        m->d = dc;
        for (int i = 0; i < MODEL_KEYS; i++) modelAdd(m, dc, i);
        drainRehash(dc);
        dictVerify(dc);
        m->delete_percent = 60;
        unsigned long cursor = 0;
        do {
            cursor = dictScan(dc, cursor, modelScanCb, m);
        } while (cursor);
        for (int i = 0; i < MODEL_KEYS; i++) assert(m->seen[i] >= 1);
        assert(dictSize(dc) == m->count);
        dictVerify(dc);
        dictRelease(dc);
        assert(test_buckets == 0);
        zfree(m);
    }

    TEST("Scan preserves surviving keys when callbacks delete and insert, including nested scans") {
        for (int no_value = 0; no_value <= 1; no_value++) {
            dictType type = chainDictType;
            type.no_value = no_value;
            test_buckets = 0;
            dict *unchained = dictCreate(&type);
            dictSetResizeEnabled(DICT_RESIZE_FORBID);
            for (int i = 0; i < DICT_BUCKET_SLOTS; i++)
                assert(dictAdd(unchained, keyForId(i), NULL) == DICT_OK);
            scanMutationCtx replacement = {.d = unchained, .deletion = 3};
            assert(dictScan(unchained, 0, scanMutationCb, &replacement) == 0);
            for (int i = 0; i < DICT_BUCKET_SLOTS; i++) assert(replacement.seen[i] == 1);
            dictVerify(unchained);
            dictRelease(unchained);
            assert(test_buckets == 0);
            for (int deletion = 0; deletion < 3; deletion++) {
                for (int additions = 1; additions <= 2; additions++) {
                    for (int nested = 0; nested <= 1; nested++) {
                        test_buckets = 0;
                        dict *dc = dictCreate(&type);
                        dictSetResizeEnabled(DICT_RESIZE_FORBID);
                        for (int i = 0; i <= DICT_BUCKET_SLOTS; i++)
                            assert(dictAdd(dc, keyForId(i), NULL) == DICT_OK);
                        scanMutationCtx ctx = {.d = dc, .deletion = deletion,
                                               .additions = additions, .nested = nested};
                        assert(dictScan(dc, 0, scanMutationCb, &ctx) == 0);
                        for (int i = 1; i <= DICT_BUCKET_SLOTS; i++) assert(ctx.seen[i] >= 1);
                        assert(dictFind(dc, "k0") == NULL);
                        assert(dictSize(dc) == DICT_BUCKET_SLOTS + (unsigned long)additions);
                        dictVerify(dc);
                        dictRelease(dc);
                        assert(test_buckets == 0);
                    }
                }
            }
        }
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    }

    TEST("Scan emits every entry when callbacks delete already emitted entries") {
        test_buckets = 0;
        dict *dc = dictCreate(&chainDictType);
        dictSetResizeEnabled(DICT_RESIZE_FORBID);
        for (int i = 0; i < SCAN_DELETE_PREV_KEYS; i++)
            assert(dictAdd(dc, keyForId(i), NULL) == DICT_OK);
        assert(dictChildBuckets(dc) > 1);
        scanDeletePrevCtx ctx = {.d = dc, .prev_id = -1};
        assert(dictScan(dc, 0, scanDeletePrevCb, &ctx) == 0);
        for (int i = 0; i < SCAN_DELETE_PREV_KEYS; i++) assert(ctx.seen[i] == 1);
        assert(dictSize(dc) == 1 && dictChildBuckets(dc) == 0);
        dictVerify(dc);
        dictRelease(dc);
        assert(test_buckets == 0);
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    }

    TEST("Scan defers pruning its tail until surviving entries have been emitted") {
        for (int no_value = 0; no_value <= 1; no_value++) {
            dictType type = chainDictType;
            type.no_value = no_value;
            for (int deletion = 0; deletion < 3; deletion++) {
                for (int last = 0; last <= 1; last++) {
                    for (int additions = 0; additions <= 1; additions++) {
                        for (int nested = 0; nested <= 1; nested++) {
                            test_buckets = 0;
                            dict *dc = dictCreate(&type);
                            dictSetResizeEnabled(DICT_RESIZE_FORBID);
                            for (int i = 0; i <= DICT_BUCKET_SLOTS; i++)
                                assert(dictAdd(dc, keyForId(i), NULL) == DICT_OK);
                            if (additions) {
                                /* Leave an earlier hole so the callback can insert
                                 * without refilling the tail or changing dictSize. */
                                dictPauseRehashing(dc);
                                assert(dictDelete(dc, "k0") == DICT_OK);
                                dictResumeRehashing(dc);
                            }
                            scanMutationCtx ctx = {.d = dc, .deletion = deletion,
                                                   .delete_id = DICT_BUCKET_SLOTS - 1 + last,
                                                   .additions = additions, .nested = nested};
                            assert(dictScan(dc, 0, scanMutationCb, &ctx) == 0);
                            for (int i = additions; i <= DICT_BUCKET_SLOTS; i++)
                                assert(ctx.seen[i] >= 1);
                            assert(dictSize(dc) == DICT_BUCKET_SLOTS && dictChildBuckets(dc) == 0);
                            dictVerify(dc);
                            dictRelease(dc);
                            assert(test_buckets == 0);
                        }
                    }
                }
            }
        }
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    }

    TEST("Randomized operations against a reference model (no_value)") {
        dictRandomizedTest(&verifyDictType, accurate ? 600 : 60, 1500);
    }

    TEST("Randomized operations against a reference model (with values)") {
        dictRandomizedTest(&verifyDictTypeVal, accurate ? 300 : 30, 1500);
    }

    TEST("Randomized operations with very long chains") {
        dictRandomizedTest(&fewBucketsDictType, accurate ? 200 : 20, 1000);
    }

    TEST("Rehash during safe iteration is paused and resumes afterwards") {
        test_buckets = 0;
        dict *dr = dictCreate(&verifyDictType);
        for (int i = 0; i < 500; i++) assert(dictAdd(dr, stringFromLongLong(i), NULL) == DICT_OK);
        drainRehash(dr);
        dictIterator it;
        dictInitSafeIterator(&it, dr);
        dictEntry *e = dictNext(&it);
        assert(e != NULL);
        /* Force a resize while iterating: rehashing must not progress. */
        dictExpand(dr, 100000);
        assert(dictIsRehashing(dr));
        assert(dictRehash(dr, 100) == 0);
        assert(dictIsRehashing(dr));
        int visited = 1;
        while (dictNext(&it)) visited++;
        dictResetIterator(&it);
        assert(visited == 500);
        drainRehash(dr);
        assert(dictSize(dr) == 500);
        dictVerify(dr);
        dictRelease(dr);
        assert(test_buckets == 0);
    }

    TEST("Safe iterator compacts the chains it leaves in both tables while rehashing") {
        test_buckets = 0;
        dict *dr = dictCreate(&fewBucketsDictType);
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
        const int n = 200;
        for (int i = 0; i < n; i++) assert(dictAdd(dr, keyForId(i), NULL) == DICT_OK);
        drainRehash(dr);
        /* Start a rehash, and move part of the entries to the new table. */
        assert(dictExpand(dr, 4 * n) == DICT_OK);
        assert(dictIsRehashing(dr));
        dictRehash(dr, 2);
        assert(dictIsRehashing(dr) && dr->ht_used[0] > 0 && dr->ht_used[1] > 0);
        uint8_t seen[200] = {0};
        dictIterator it;
        dictEntry *de;
        int visited = 0;
        dictInitSafeIterator(&it, dr);
        while ((de = dictNext(&it))) {
            int id = keyId(dictGetKey(de));
            assert(!seen[id]);
            seen[id] = 1;
            visited++;
            if (id % 2) assert(dictDelete(dr, dictGetKey(de)) == DICT_OK);
        }
        dictResetIterator(&it);
        assert(visited == n);
        assert(dictSize(dr) == (unsigned long)n / 2);
        /* Every chain was left with deletes in it, so none has holes. */
        for (int t = 0; t <= 1; t++) {
            for (size_t idx = 0; idx < DICTHT_SIZE(dr->ht_size_exp[t]); idx++) {
                for (dictBucket *b = &dr->ht_table[t][idx]; b->chained; b = getChildBucket(b))
                    assert(bucketIsFull(b));
            }
        }
        dictVerify(dr);
        dictRelease(dr);
        assert(test_buckets == 0);
    }

    srand(12345);
    start_benchmark();
    for (j = 0; j < count; j++) {
        /* Create a dynamically allocated substring */
        char *key = stringFromSubstring();

        /* Insert the range directly from the large string */
        de = dictAddRaw(d, key, &existing);
        assert(de != NULL || existing != NULL);
        /* If key already exists NULL is returned so we need to free the temp key string */
        if (de == NULL) zfree(key);
    }
    end_benchmark("Inserting random substrings (100-500B) from large string with symbols");
    assert((long)dictSize(d) <= count);
    dictEmpty(d, NULL);

    start_benchmark();
    for (j = 0; j < count; j++) {
        retval = dictAdd(d,stringFromLongLong(j),(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting via dictAdd() non existing");
    assert((long)dictSize(d) == count);

    dictEmpty(d, NULL);

    start_benchmark();
    for (j = 0; j < count; j++) {
        de = dictAddRaw(d,stringFromLongLong(j),NULL);
        assert(de != NULL);
    }
    end_benchmark("Inserting via dictAddRaw() non existing");
    assert((long)dictSize(d) == count);

    start_benchmark();
    for (j = 0; j < count; j++) {
        void *key = stringFromLongLong(j);
        de = dictAddRaw(d,key,&existing);
        assert(existing != NULL);
        zfree(key);
    }
    end_benchmark("Inserting via dictAddRaw() existing (no insertion)");
    assert((long)dictSize(d) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(d)) {
        dictRehashMicroseconds(d,100*1000);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(d,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(d,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        dictEntry *de = dictFind(d,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        dictEntry *de = dictGetRandomKey(d);
        assert(de != NULL);
    }
    end_benchmark("Accessing random keys");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(d,key);
        assert(de == NULL);
        zfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        retval = dictDelete(d,key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(d,key,(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
    dictRelease(d);

    TEST("Use dict without values (no_value=1)") {
        dictType dt = BenchmarkDictType;
        dt.no_value = 1;

        /* Allocate array of size count and fill it with keys (stringFromLongLong(j) */
        char **lookupKeys = zmalloc(sizeof(char*) * count);
        for (long j = 0; j < count; j++)
            lookupKeys[j] = stringFromLongLong(j);


        /* Add keys without values. */
        dict *d = dictCreate(&dt);
        for (j = 0; j < count; j++) {
            retval = dictAdd(d,lookupKeys[j],NULL);
            assert(retval == DICT_OK);
        }

        /* Now, we should be able to find the keys. */
        for (j = 0; j < count; j++) {
            dictEntry *de = dictFind(d,lookupKeys[j]);
            assert(de != NULL);
        }

        /* Find non exists keys. */
        for (j = 0; j < count; j++) {
            /* Temporarily override first char of key */
            char tmp = lookupKeys[j][0];
            lookupKeys[j][0] = 'X';
            dictEntry *de = dictFind(d,lookupKeys[j]);
            lookupKeys[j][0] = tmp;
            assert(de == NULL);
        }

        dictRelease(d);
        zfree(lookupKeys);
    }

    TEST("Test dictFindLink() functionality") {
        dictType dt = BenchmarkDictType;
        dict *d = dictCreate(&dt);
        
        /* find in empty dict */
        dictEntryLink link = dictFindLink(d, "key", NULL);
        assert(link == NULL);

        /* Add keys to dict and test */
        for (j = 0; j < 10; j++) {
            /* Add another key to dict */
            char *key = stringFromLongLong(j);
            retval = dictAdd(d, key, (void*)j);
            assert(retval == DICT_OK);
            /* find existing keys with dictFindLink() */
            dictEntryLink link = dictFindLink(d, key, NULL);
            assert(link != NULL);
            assert(*link != NULL);
            assert(dictGetKey(*link) != NULL);
            
            /* Test that the key found is the correct one */
            void *foundKey = dictGetKey(*link);
            assert(compareCallback( NULL, foundKey, key));

            /* Test finding a non-existing key */
            char *nonExistingKey = stringFromLongLong(j + 10);
            link = dictFindLink(d, nonExistingKey, NULL);
            assert(link == NULL);

            /* Test with bucket parameter */
            dictEntryLink bucket = NULL;
            link = dictFindLink(d, key, &bucket);
            assert(link != NULL);
            assert(bucket != NULL);

            /* Test bucket parameter with non-existing key */
            link = dictFindLink(d, nonExistingKey, &bucket);
            assert(link == NULL);
            assert(bucket != NULL); /* Bucket should still be set even for non-existing keys */

            /* Clean up */
            zfree(nonExistingKey);
        }

        dictRelease(d);
    }

    TEST("dictFindLink() + dictSetKeyAtLink() insert and replace, with rehashing") {
        test_buckets = 0;
        dict *dl = dictCreate(&verifyDictType);
        for (int i = 0; i < 2000; i++) {
            char *key = stringFromLongLong(i);
            dictEntryLink bucket = NULL;
            dictEntryLink link = dictFindLink(dl, key, &bucket);
            assert(link == NULL);
            dictSetKeyAtLink(dl, key, &bucket, 1);
            /* bucket now points at the inserted entry. */
            assert(bucket != NULL && dictGetKey(*bucket) == key);
            link = dictFindLink(dl, key, NULL);
            assert(link == bucket || dictGetKey(*link) == key);
            if (i % 5 == 0) {
                /* Replace the key object with an equal one. */
                char *key2 = stringFromLongLong(i);
                void *old = dictGetKey(*link);
                dictSetKeyAtLink(dl, key2, &link, 0);
                assert(dictGetKey(*link) == key2);
                zfree(old);
            }
        }
        assert(dictSize(dl) == 2000);
        drainRehash(dl);
        dictVerify(dl);
        dictRelease(dl);
        assert(test_buckets == 0);
    }

    return 0;
}
#endif
