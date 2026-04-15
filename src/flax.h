/* Flax -- A flat sorted-array map for uint8_t keys.
 *
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef FLAX_H
#define FLAX_H

#include <stdint.h>
#include <stddef.h>

#define FLAX_INIT_CAPACITY 16

/* A flax is a sorted associative container that maps uint8_t keys to void*
 * values. Both arrays live in a single heap allocation ("data block") laid
 * out as follows:
 *
 *  flax struct            data block (single allocation)
 *  +------------+         +------------------------------------+
 *  | *data  ----------->  | keys[0..cap-1]   (uint8_t)         |
 *  | numele     |         +-- aligned to sizeof(void*) --------+
 *  | capacity   |         | values[0..cap-1]  (void*)          |
 *  | alloc_size |         +------------------------------------+
 *  +------------+
 *
 * Keys are maintained in ascending sorted order. Only the first 'numele'
 * slots in each array contain live data; the remainder up to 'capacity'
 * is reserved space for future inserts.
 *
 * Lookup, insert and delete use a linear scan over the keys array rather
 * than binary search. This is intentional: the expected element count is
 * small (e.g. per-consumer stream PEL), so the sequential, cache-friendly
 * access pattern outperforms binary search whose branch-misprediction cost
 * dominates at these sizes. The scan includes fast-path checks for the
 * head and tail positions to accelerate the common case of monotonically
 * increasing keys (e.g. stream entry IDs).
 *
 * Growth strategy: the data block doubles in capacity when full (on insert)
 * and can be shrunk to fit with flaxShrink().
 */
typedef struct flax {
    void *data;          /* Packed storage: keys array followed by values array. */
    uint16_t numele;     /* Number of elements currently stored (max 256). */
    uint16_t capacity;   /* Current allocated capacity. */
    uint32_t alloc_size; /* Total usable bytes: struct allocation + data block. */
} flax;

/* Flax iterator state. The typical lifecycle is:
 *
 *   flaxIterator it;
 *   flaxStart(&it, myflax);        -- initialize
 *   flaxSeek(&it, ">=", somekey);  -- position
 *   while (flaxNext(&it)) { ... }  -- iterate (or flaxPrev)
 *
 * After flaxStart() the iterator is in EOF state until a successful
 * flaxSeek(). The iterator does not allocate heap memory.
 *
 * WARNING: the iterator is invalidated by any mutation (insert / remove /
 * resize) on the underlying flax.  Do not modify the flax while iterating. */
typedef struct flaxIterator {
    flax *f;             /* Flax we are iterating. */
    uint8_t key;         /* The current key. */
    void *data;          /* Data associated to this key. */
    int16_t idx;         /* Current index into the flax arrays, -1 if EOF. */
} flaxIterator;

/* --- Creation and destruction --- */
flax *flaxNew(void);
void flaxFree(flax *f);
void flaxFreeWithCallback(flax *f,
                          void (*free_callback)(void *item, void *ctx),
                          void *ctx);

/* --- Lookup and mutation --- */
int flaxInsert(flax *f, uint8_t key, void *data, void **old);
int flaxTryInsert(flax *f, uint8_t key, void *data, void **old);
int flaxRemove(flax *f, uint8_t key, void **old);
int flaxFind(flax *f, uint8_t key, void **value);

/* --- Iterator --- */
void flaxStart(flaxIterator *it, flax *f);
int flaxSeek(flaxIterator *it, const char *op, uint8_t key);
int flaxNext(flaxIterator *it);
int flaxPrev(flaxIterator *it);
int flaxEOF(flaxIterator *it);
void flaxIterSetData(flaxIterator *it, void *data);

/* --- Introspection --- */
uint16_t flaxSize(flax *f);
size_t flaxAllocSize(flax *f);
void flaxShrink(flax *f);

#ifdef REDIS_TEST
int flaxTest(int argc, char *argv[], int flags);
#endif

#endif
