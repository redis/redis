/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

#ifndef __HASHTABLE_H
#define __HASHTABLE_H

/* Hash table implementation.
 *
 * This is a cache-friendly hash table implementation. For details about the
 * implementation and documentation of functions, see comments in hashtable.c.
 *
 * The entries in a hashtable are of a user-defined type, but an entry needs to
 * contain a key. It can represent a key-value entry, or it can be just a key,
 * if set semantics are desired.
 *
 * Terminology:
 *
 * hashtable
 *         An instance of the data structure.
 *
 * entry
 *         An entry in the hashtable. This may be of the same type as the key,
 *         or a struct containing a key and other fields.
 * key
 *         The part of the entry used for looking the entry up in the hashtable.
 *         May be the entire entry or a struct field within the entry.
 *
 * type
 *         A struct containing callbacks, such as hash function, key comparison
 *         function and how to get the key in an entry.
 */

#include "fmacros.h"
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>

/* --- Opaque types --- */

typedef struct hashtable hashtable;
typedef struct hashtableStats hashtableStats;

/* Can types that can be stack allocated. */
typedef uint64_t hashtableIterator[6];
typedef uint64_t hashtablePosition[2];
typedef uint64_t hashtableIncrementalFindState[5];

/* --- Non-opaque types --- */

/* The hashtableType is a set of callbacks for a hashtable. All callbacks are
 * optional. With all callbacks omitted, the hashtable is effectively a set of
 * pointer-sized integers. */
typedef struct {
    /* If the type of an entry is not the same as the type of a key used for
     * lookup, this callback needs to return the key within an entry. */
    const void *(*entryGetKey)(const void *entry);
    /* Hash function. Defaults to hashing the bits in the pointer, effectively
     * treating the pointer as an integer. */
    uint64_t (*hashFunction)(const void *key);
    /* Compare function, returns 0 if the keys are equal. Defaults to just
     * comparing the pointers for equality. */
    int (*keyCompare)(const void *key1, const void *key2);
    /* Check for entry access should be masked or not. Masked access will just treat the entry as not-exist. */
    bool (*validateEntry)(hashtable *ht, void *entry);
    /* Callback to free an entry when it's overwritten or deleted.
     * Optional. */
    void (*entryDestructor)(hashtable *ht, void *entry);
    /* Callback to prefetch the value associated with a hashtable entry. */
    void (*entryPrefetchValue)(const void *entry);
    /* Callback to control when resizing should be allowed. */
    int (*resizeAllowed)(size_t moreMem, double usedRatio);
    /* Invoked at the start of rehashing. */
    void (*rehashingStarted)(hashtable *ht);
    /* Invoked at the end of rehashing. */
    void (*rehashingCompleted)(hashtable *ht);
    /* Track memory usage using this callback. It is called with a positive
     * number when the hashtable allocates some memory and with a negative number
     * when freeing. */
    void (*trackMemUsage)(hashtable *ht, ssize_t delta);
    /* Allow a hashtable to carry extra caller-defined metadata. The extra memory
     * is initialized to 0. */
    size_t (*getMetadataSize)(void);
    /* Pointer to user data, useful in callbacks. */
    void *userdata;
    /* Flag to disable incremental rehashing */
    unsigned instant_rehashing : 1;

} hashtableType;

typedef enum {
    HASHTABLE_RESIZE_ALLOW = 0,
    HASHTABLE_RESIZE_AVOID,
    HASHTABLE_RESIZE_FORBID,
} hashtableResizePolicy;

typedef void (*hashtableScanFunction)(void *privdata, void *entry);

/* Constants */
#define HASHTABLE_BUCKET_SIZE 64 /* bytes, the most common cache line size */

/* Scan flags */
#define HASHTABLE_SCAN_EMIT_REF (1 << 0)

/* Iterator flags */
#define HASHTABLE_ITER_SAFE (1 << 0)
#define HASHTABLE_ITER_PREFETCH_VALUES (1 << 1)
#define HASHTABLE_ITER_SKIP_VALIDATION (1 << 2)
#define HASHTABLE_ITER_INCLUDE_IMPORTING (1 << 3)

/* --- Prototypes --- */

/* Hash function (global seed) */
void hashtableSetHashFunctionSeed(const uint8_t *seed);
uint8_t *hashtableGetHashFunctionSeed(void);
uint64_t hashtableGenHashFunction(const char *buf, size_t len);
uint64_t hashtableGenCaseHashFunction(const char *buf, size_t len);

/* Global resize policy */
void hashtableSetResizePolicy(hashtableResizePolicy policy);

/* Hashtable instance */
hashtable *hashtableCreate(hashtableType *type);
void hashtableRelease(hashtable *ht);
void hashtableEmpty(hashtable *ht, void(callback)(hashtable *));
hashtableType *hashtableGetType(hashtable *ht);
hashtableType *hashtableSetType(hashtable *ht, hashtableType *type);
void *hashtableMetadata(hashtable *ht);
size_t hashtableHeaderSize(void);
size_t hashtableSize(const hashtable *ht);
size_t hashtableBuckets(hashtable *ht);
size_t hashtableChainedBuckets(hashtable *ht, int table);
unsigned hashtableEntriesPerBucket(void);
size_t hashtableMemUsage(hashtable *ht);
void hashtablePauseAutoShrink(hashtable *ht);
void hashtableResumeAutoShrink(hashtable *ht);
bool hashtableIsRehashing(hashtable *ht);
bool hashtableIsRehashingPaused(hashtable *ht);
ssize_t hashtableGetRehashingIndex(hashtable *ht);
void hashtableRehashingInfo(hashtable *ht, size_t *from_size, size_t *to_size);
int hashtableRehashMicroseconds(hashtable *ht, uint64_t us);
bool hashtableExpand(hashtable *ht, size_t size);
bool hashtableTryExpand(hashtable *ht, size_t size);
bool hashtableExpandIfNeeded(hashtable *ht);
bool hashtableShrinkIfNeeded(hashtable *ht);
bool hashtableRightsizeIfNeeded(hashtable *ht);
hashtable *hashtableDefragTables(hashtable *ht, void *(*defragfn)(void *));
void dismissHashtable(hashtable *ht);

/* Entries */
bool hashtableFind(hashtable *ht, const void *key, void **found);
void **hashtableFindRef(hashtable *ht, const void *key);
bool hashtableAdd(hashtable *ht, void *entry);
bool hashtableAddOrFind(hashtable *ht, void *entry, void **existing);
bool hashtableFindPositionForInsert(hashtable *ht, void *key, hashtablePosition *position, void **existing);
void hashtableInsertAtPosition(hashtable *ht, void *entry, hashtablePosition *position);
void **hashtableGetRefAtPosition(hashtablePosition *position);
bool hashtablePop(hashtable *ht, const void *key, void **popped);
bool hashtableDelete(hashtable *ht, const void *key);
void **hashtableTwoPhasePopFindRef(hashtable *ht, const void *key, hashtablePosition *position);
void hashtableTwoPhasePopDelete(hashtable *ht, hashtablePosition *position);
bool hashtableReplaceReallocatedEntry(hashtable *ht, const void *old_entry, void *new_entry);
void hashtableIncrementalFindInit(hashtableIncrementalFindState *state, hashtable *ht, const void *key);
bool hashtableIncrementalFindStep(hashtableIncrementalFindState *state);
bool hashtableIncrementalFindGetResult(hashtableIncrementalFindState *state, void **found);

/* Iteration & scan */
size_t hashtableScan(hashtable *ht, size_t cursor, hashtableScanFunction fn, void *privdata);
size_t hashtableScanDefrag(hashtable *ht, size_t cursor, hashtableScanFunction fn, void *privdata, void *(*defragfn)(void *), int flags);
void hashtableInitIterator(hashtableIterator *iter, hashtable *ht, uint8_t flags);
void hashtableRetargetIterator(hashtableIterator *iterator, hashtable *ht);
void hashtableCleanupIterator(hashtableIterator *iter);
hashtableIterator *hashtableCreateIterator(hashtable *ht, uint8_t flags);
void hashtableReleaseIterator(hashtableIterator *iter);
bool hashtableNext(hashtableIterator *iter, void **elemptr);

/* Random entries */
bool hashtableRandomEntry(hashtable *ht, void **found);
bool hashtableFairRandomEntry(hashtable *ht, void **found);
unsigned hashtableSampleEntries(hashtable *ht, void **dst, unsigned count);

/* Debug & stats */

void hashtableFreeStats(hashtableStats *stats);
void hashtableCombineStats(hashtableStats *from, hashtableStats *into);
hashtableStats *hashtableGetStatsHt(hashtable *ht, int htidx, int full);
size_t hashtableGetStatsMsg(char *buf, size_t bufsize, hashtableStats *stats, int full);
void hashtableGetStats(char *buf, size_t bufsize, hashtable *ht, int full);

#endif /* __HASHTABLE_H */
