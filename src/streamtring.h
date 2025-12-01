/* TRING -- AVL tree with ring buffer storage implementation.
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * -----------------------------------------------------------------------------
 *
 * This is a ring buffer-based implementation of an AVL tree, a self-balancing
 * binary search tree. Unlike traditional array-based implementations with free
 * lists, this version uses a ring buffer to manage node storage.
 *
 * Key features:
 *
 * - Nodes stored in a ring buffer (circular buffer)
 * - Uses indices instead of pointers for child/parent relationships
 * - Maintains AVL balance property (height difference <= 1)
 * - Ring buffer with head, tail, capacity, and count tracking
 * - Dynamic resizing: grows when full
 * - Generic values via compare function callback
 * - No delete operation (nodes are not recycled)
 *
 * Memory layout:
 *
 * The tree uses an array of nodes, where each node contains:
 *   - value pointer
 *   - child indices (left, right)
 *   - parent index for efficient updates
 *   - height for AVL balancing
 *
 * Ring buffer management:
 *   - head:     Index of the first allocated node
 *   - tail:     Index where next node will be allocated
 *   - count:    Number of nodes currently in use
 *   - capacity: Total capacity of the ring buffer
 */

#ifndef STREAMTRING_H
#define STREAMTRING_H

#include <stdint.h>
#include <stddef.h>

#define TRING_INITIAL_CAPACITY 8
#define TRING_DEFAULT_MAX_CAPACITY 100000  /* Default max capacity: 100K */
#define TRING_NULL UINT32_MAX  /* Special value for null/none indices */

/* Compare function type: returns negative if a < b, 0 if a == b, positive if a > b */
typedef int (*tringCompareFunc)(const void *a, const void *b);

/* Free callback function type: called when a value is removed from the tree */
typedef void (*tringFreeCallback)(void *value, void *user_data);

/* Node structure stored in ring buffer */
typedef struct tringNode {
    void *value;
    uint32_t left;   /* Index of left child (TRING_NULL if none) */
    uint32_t right;  /* Index of right child (TRING_NULL if none) */
    uint32_t parent; /* Index of parent node (TRING_NULL if root) */
    uint32_t height; /* Height of the node for AVL balancing */
} tringNode;

/* TRING Tree structure - AVL tree with ring buffer storage */
typedef struct tringTree {
    tringNode *nodes;       /* Ring buffer array of nodes */
    uint32_t capacity;      /* Total ring buffer capacity */
    uint32_t max_capacity;  /* Maximum allowed capacity */
    uint32_t count;         /* Number of nodes in use */
    uint32_t head;          /* Index of first allocated node */
    uint32_t tail;          /* Index where next node will be allocated */
    uint32_t root;          /* Index of root node (TRING_NULL if empty) */
    tringCompareFunc compare; /* Compare function for values */
    tringFreeCallback free_callback;  /* Optional callback for cleanup */
    void *free_callback_user_data;  /* User data passed to free_callback */
    size_t *alloc_size;     /* Optional pointer to track total allocated memory */
} tringTree;

/* Exported API */

/* Create a new TRING tree with the given comparison function.
 * If passed `alloc_size` is non-NULL, tring will account for its used
 * memory at this location.
 * Returns NULL on out of memory. */
tringTree *tringNew(tringCompareFunc compare, size_t *alloc_size);

/* Free the TRING tree structure.
 * Note: This does not free the values stored in the tree.
 * The caller is responsible for freeing the values if needed. */
void tringFree(tringTree *tree);

/* Insert a value into the tree.
 * Returns 1 if a new value was inserted, 0 if the value already exists or on out of memory.
 * If existingOut is not NULL and a duplicate is found, stores the existing value in *existingOut.
 * Duplicate values are not allowed. */
int tringInsert(tringTree *tree, void *value, void **existingOut);

/* Search for a value in the tree.
 * Returns the value if found, NULL otherwise. */
void *tringFind(tringTree *tree, void *value);

/* Return the number of elements in the tree. */
size_t tringSize(tringTree *tree);

/* Check if the tree is empty.
 * Returns 1 if empty, 0 otherwise. */
int tringEmpty(tringTree *tree);

/* Return the first item (at head position) without removing it.
 * This is the oldest item still in the tree (FIFO order).
 * Returns NULL if the tree is empty. */
void *tringFront(tringTree *tree);

/* Return the last added item (at tail-1 position) without removing it.
 * This is the newest item in the tree (most recently inserted).
 * Returns NULL if the tree is empty. */
void *tringBack(tringTree *tree);

/* Remove the first (minimum) item from the tree.
 * Also increments the head pointer in the ring buffer.
 * Returns 1 on success, 0 if the tree is empty or on failure. */
int tringPopFront(tringTree *tree);

/* Remove the last (most recently added) item from the tree.
 * Also decrements the tail pointer in the ring buffer.
 * Returns 1 on success, 0 if the tree is empty or on failure. */
int tringPopBack(tringTree *tree);

/* Set the maximum capacity for the tree.
 * If the tree's current capacity exceeds the new max_capacity, the limit
 * will be enforced on the next resize operation.
 * Setting max_capacity to 0 means no limit. */
void tringSetMaxCapacity(tringTree *tree, uint32_t max_capacity);

/* Set a callback function to be called when a value is removed from the tree. */
void tringSetFreeCallback(tringTree *tree, tringFreeCallback callback, void *user_data);

/* Clear all entries from the tree and reset it to initial state.
 * For each entry, the free callback (if set) is called.
 * The tree's memory is freed and reallocated at initial capacity. */
void tringClear(tringTree *tree);

#ifdef REDIS_TEST
int tringTest(int argc, char *argv[], int flags);
#endif

#endif /* STREAMTRING_H */

