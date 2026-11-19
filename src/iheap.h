/*
 * Copyright Redis Ltd. 2024 - present
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 *
 * WHAT IS IHEAP?
 * --------------
 * iheap is an indexed binary min-heap: an array heap of (key, item) entries
 * whose smallest key is always at slot 0, with the twist that the embedder
 * can be told where each item currently sits. That slot is the handle for
 * updating an item's key or removing the item in O(log n), which a plain
 * priority queue cannot do.
 *
 * - The key is a uint64_t stored inline in the entry. A sift compares the
 *   keys in the array and never dereferences an item, so there is no
 *   comparator callback.
 * - The item is an opaque pointer. The heap never reads it; it only hands it
 *   back from iheapMin()/iheapPop() and passes it to the type callbacks.
 * - The optional setSlot callback is invoked for every entry the heap moves,
 *   with the entry's new slot. Where the slot is kept is the embedder's
 *   business. An embedder that only inserts and takes the minimum provides
 *   no callbacks and gets a plain priority queue (insert, min, pop).
 * - Keys may move in both directions: iheapUpdate() sifts up or down as the
 *   new key requires.
 * - There is one allocation per heap. Operations that may grow the array
 *   take an iheap** and update the caller's pointer.
 * - The heap has no global state, so it may be freed from any thread.
 *
 * SLOT CONTRACT
 * -------------
 * The heap calls setSlot(item, slot) exactly once for every entry it moves,
 * and once for an inserted or appended entry. An update that leaves the entry
 * in place makes no call. An insert, update, remove or pop on a heap of n
 * entries makes at most floor(log2(n)) + 1 calls; iheapBuild() makes at most
 * 2n. Items that leave the heap (remove, pop) get no call: the caller knows.
 *
 * iheapUpdate() and iheapRemove() take a slot, so they are only meaningful
 * when the type provides setSlot. The slot handed to them must be the one
 * most recently reported for that item.
 */

#ifndef __IHEAP_H
#define __IHEAP_H

#include <stdint.h>
#include <stddef.h>

typedef struct iheapType {
    /* Called with the item's new slot whenever the heap places or moves the
     * item. May be NULL, in which case update and remove are unavailable. */
    void (*setSlot)(void *item, uint32_t slot);
    /* Returns the slot the embedder stored for the item. Used only by
     * iheapValidate(); may be NULL. */
    uint32_t (*getSlot)(void *item);
} iheapType;

typedef struct iheapEntry {
    uint64_t key;
    void *item;
} iheapEntry;

typedef struct iheap {
    const iheapType *type;
    uint32_t count;
    uint32_t capacity;
    iheapEntry e[];
} iheap;

/* Create an empty heap with room for 'capacity' entries (at least 1). 'type'
 * may be NULL for a heap with no callbacks. The capacity must leave the
 * allocation size within size_t, which only a 32-bit target can violate;
 * that is asserted. */
iheap *iheapNew(const iheapType *type, uint32_t capacity);
void iheapFree(iheap *h);

/* Insert an entry; grows the array if needed and updates *hp. */
void iheapInsert(iheap **hp, uint64_t key, void *item);
/* Change the key of the entry at 'slot' and restore the heap property. */
void iheapUpdate(iheap *h, uint32_t slot, uint64_t key);
/* Remove the entry at 'slot'. */
void iheapRemove(iheap *h, uint32_t slot);
/* Read the entry with the smallest key. Returns 0 when the heap is empty.
 * Either output pointer may be NULL. */
int iheapMin(const iheap *h, uint64_t *key, void **item);
/* Remove the entry with the smallest key and return it like iheapMin(). */
int iheapPop(iheap *h, uint64_t *key, void **item);

/* Bulk load: append entries in any order, then call iheapBuild() once. The
 * heap is not valid between the first append and the build. */
void iheapAppend(iheap **hp, uint64_t key, void *item);
void iheapBuild(iheap *h);

/* Check every invariant. Returns 1 when the heap is consistent. O(n). */
int iheapValidate(const iheap *h);

static inline uint32_t iheapCount(const iheap *h) {
    return h->count;
}

/* Bytes occupied by the heap: header plus the capacity it can hold. */
static inline size_t iheapAllocSize(const iheap *h) {
    return sizeof(iheap) + (size_t)h->capacity * sizeof(iheapEntry);
}

#ifdef REDIS_TEST
int iheapTest(int argc, char *argv[], int flags);
#endif

#endif /* __IHEAP_H */
