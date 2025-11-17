/* AVL -- Array-based AVL tree implementation.
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
 * This is an array-based implementation of an AVL tree, a self-balancing
 * binary search tree. Unlike traditional pointer-based implementations,
 * this version stores all nodes in contiguous arrays for better cache
 * locality and memory efficiency.
 *
 * Key features:
 *
 * - All nodes stored in arrays (not individual heap allocations)
 * - Uses indices instead of pointers for child/parent relationships
 * - Maintains AVL balance property (height difference <= 1)
 * - Free list for efficient node reuse after deletion
 * - Dynamic resizing: grows when full, shrinks when sparse
 * - Generic values via compare function callback
 *
 * Memory layout:
 *
 * The tree uses parallel arrays for storing node data:
 *   - nodes[]:   Contains value pointer and child indices
 *   - heights[]: Node heights for AVL balancing
 *   - parents[]: Parent indices for efficient updates
 *
 * When a node is deleted, it's added to a free list (using the left
 * field as a "next" pointer). New insertions first check the free list
 * before allocating from the end of the array.
 *
 * The tree automatically doubles capacity when full and shrinks to half
 * when usage falls to 1/4 of capacity (minimum 8 nodes).
 */

#ifndef AVL_H
#define AVL_H

#include <stdbool.h>
#include <stdint.h>

#define INITIAL_CAPACITY 8
#define AVL_NULL UINT32_MAX  /* Special value for null/none indices */

/* Compare function type: returns negative if a < b, 0 if a == b, positive if a > b */
typedef int (*avlCompareFunc)(const void *a, const void *b);

/* Node structure stored in array */
typedef struct {
    void *value;
    uint32_t left;   /* Index of left child (AVL_NULL if none) */
    uint32_t right;  /* Index of right child (AVL_NULL if none) */
} AVLNode;

/* AVL Tree structure */
typedef struct {
    AVLNode *nodes;         /* Array of nodes */
    uint8_t *heights;       /* Array of node heights */
    uint32_t *parents;      /* Array of parent indices */
    uint32_t capacity;      /* Current array capacity */
    uint32_t size;          /* Number of nodes in use */
    uint32_t root;          /* Index of root node (AVL_NULL if empty) */
    uint32_t firstFree;     /* Index of first free node (AVL_NULL if none) */
    avlCompareFunc compare; /* Compare function for values */
} AVLTree;

/* Exported API. */
AVLTree *avlCreate(avlCompareFunc compare);
void avlDestroy(AVLTree *tree);
bool avlInsert(AVLTree *tree, void *value);
bool avlRemove(AVLTree *tree, void *value);
void *avlFind(AVLTree *tree, void *value);
int avlGetSize(AVLTree *tree);
bool avlIsEmpty(AVLTree *tree);

#ifdef REDIS_TEST
int avlTest(int argc, char *argv[], int flags);
#endif

#endif /* AVL_H */
