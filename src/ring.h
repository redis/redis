/* Ring -- A circular ring buffer implementation.
 *
 * Copyright (c) 2017-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef RING_H
#define RING_H

#include <stdint.h>
#include <stddef.h>

/* Ring buffer structure.
 *
 * This implements a dynamically-sized circular buffer that stores pointers
 * to data structures. The buffer automatically grows when full and can
 * optionally shrink when mostly empty to conserve memory.
 *
 * The ring buffer uses a head and tail pointer to track the position of
 * elements. When head == tail, the buffer is empty. The buffer grows by
 * doubling its capacity when full and shrinks by half when less than 25%
 * utilized (with a minimum capacity).
 *
 * Basic operations:
 * - ringPush: Add element at the end (tail)
 * - ringPop: Remove and return element from the front (head)
 * - ringFront: Peek at the first element without removing it
 * - ringSize: Get the current number of elements
 *
 * Example usage:
 *     ring *r = ringNew();
 *     void *data = myDataStructure();
 *     ringPush(r, data);
 *     void *item = ringPop(r);
 *     ringFree(r);
 */

#define RING_MIN_CAPACITY 8  /* Minimum capacity for the ring buffer */

typedef struct ring {
    void **items;      /* Array of pointers to data */
    size_t capacity;   /* Total allocated capacity */
    size_t head;       /* Index of the first element */
    size_t tail;       /* Index where next element will be inserted */
    size_t count;      /* Number of elements currently in the buffer */
    void (*free_callback)(void*);  /* Optional callback called when item is removed */
} ring;

/* Exported API */

/* Create a new ring buffer with default initial capacity.
 * Returns NULL on out of memory. */
ring *ringNew(void);

/* Create a new ring buffer with specified initial capacity.
 * Returns NULL on out of memory. */
ring *ringNewWithCapacity(size_t initial_capacity);

/* Push an item onto the end of the ring buffer.
 * Returns 1 on success, 0 on out of memory. */
int ringPush(ring *r, void *item);

/* Remove and return the first item from the ring buffer.
 * Returns NULL if the buffer is empty. */
void *ringPop(ring *r);

/* Return the first item without removing it.
 * Returns NULL if the buffer is empty. */
void *ringFront(ring *r);

/* Return the number of items currently in the ring buffer. */
size_t ringSize(ring *r);

/* Return the current capacity of the ring buffer. */
size_t ringCapacity(ring *r);

/* Check if the ring buffer is empty.
 * Returns 1 if empty, 0 otherwise. */
int ringIsEmpty(ring *r);

/* Free the ring buffer structure.
 * Note: This does not free the items stored in the buffer.
 * The caller is responsible for freeing the items if needed. */
void ringFree(ring *r);

/* Free the ring buffer and call the provided callback for each item.
 * This is useful when you want to free both the ring and its contents. */
void ringFreeWithCallback(ring *r, void (*free_callback)(void*));

/* Clear all items from the ring buffer without freeing them.
 * Resets the buffer to its initial state. */
void ringClear(ring *r);

/* Set a callback function to be called when an item is removed from the ring.
 * The callback is invoked in ringPop before the item is returned.
 * Pass NULL to disable the callback. */
void ringSetFreeCallback(ring *r, void (*callback)(void*));

#ifdef REDIS_TEST
int ringTest(int argc, char *argv[], int flags);
#endif

#endif

