/* vector.h - Simple append-only vector interface
 *
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef REDIS_VECTOR_H
#define REDIS_VECTOR_H

#include <stddef.h>

/*
 * Simple append-only vector (dynamic array) of void * elements.
 *
 * Design:
 * --------
 * - Stores elements in a contiguous array (void **).
 * - Supports append (vecPush) and read access.
 * - Optionally uses caller-provided stack buffer to avoid heap allocations.
 * - See also comment in vector.c of vecInit() for more details.
 *
 * Memory:
 * -------
 * - Allocation goes through the Redis allocator (zmalloc / zrealloc / zfree).
 * - vecRelease() frees heap memory if used.
 * - Stack buffer is never freed.
 * - Stored elements are not freed unless a free method is set via
 *   vecSetFreeMethod().
 *
 * Modes:
 * -------
 * 1. Start On Stack (grow to heap): vec v;
 *                                   void *vstack[8];
 *                                   ...
 *                                   vecInit(&v, vstack, 8);
 *
 *   Start Embedded (grow to heap):  typedef struct {
 *                                     vec v;
 *                                     void *vembedded[8];
 *                                   } obj;
 *                                   ...
 *                                   vecInit(&obj->v, obj->vembedded, 8);
 *
 * 2. Heap only, init capacity 8:    vec v;
 *                                   ...
 *                                   vecInit(&v, NULL, 8);
 *
 *    Heap only, init capacity 0:    vec v;
 *                                   ...
 *                                   vecInit(&v, NULL, 0);
 *
 * 3. Depends on var size:           vec v;
 *                                   void *vstack[8];
 *                                   vecInit(&v, vstack, 8);
 *                                   vecReserve(&v, varsize); // varsize <= 8 ? stack : heap
 *
 * Notes:
 * ------
 * - Not thread-safe.
 * - If stack == NULL and initcap > 0, initcap is treated as an initial
 *   heap-capacity hint.
 */

typedef struct vec {
    size_t size;       /* Number of elements in the vector. */
    size_t cap;        /* Capacity of the vector. */
    void **data;       /* Heap-allocated storage or refers to stack. */
    void **stack;      /* Optional stack buffer. */
    void (*free)(void *ptr); /* Optional free method, applied to each
                              * element on vecRelease. NULL = no-op. */
} vec;

/* Return the contiguous backing array. */
static inline void **vecData(const vec *v) { return v->data; }

/* Return the number of elements in the vector. */
static inline size_t vecSize(const vec *v) { return v->size; }

/* Get element at index. index must be < vecSize(v). */
static inline void *vecGet(const vec *v, size_t index) { return v->data[index]; }

/* Initialize a vector */
void vecInit(vec *v, void **stack, size_t initcap);

/* Set a free method applied to every element on vecRelease.
 * Symmetric to listSetFreeMethod for adlist. */
static inline void vecSetFreeMethod(vec *v, void (*freefn)(void *ptr)) {
    v->free = freefn;
}

/* Release storage. If a free method is set, it is applied to every element
 * before the backing storage is released. Stack storage is never freed. */
void vecRelease(vec *v);

/* Reset the logical length to zero while preserving allocated storage.
 * If a free method is set, it is applied to every element before reset. */
void vecClear(vec *v);

/* Ensure capacity is at least mincap. */
void vecReserve(vec *v, size_t mincap);

/* Append one element, growing storage as needed. */
void vecPush(vec *v, void *value);

#ifdef REDIS_TEST
int vectorTest(int argc, char **argv, int flags);
#endif

#endif /* REDIS_VECTOR_H */
