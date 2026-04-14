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
 * - vecRelease() frees heap memory if used.
 * - Stack buffer is never freed.
 * - Stored elements are never freed.
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
 * - When used in Redis core, the implementation should use the Redis allocator
 *   wrappers (zmalloc / zrealloc / zfree) rather than libc allocation APIs.
 */

typedef struct vec {
    size_t size;       /* Number of elements in the vector. */
    size_t cap;        /* Capacity of the vector. */
    void **data;       /* Heap-allocated storage or refers to stack. */
    void **stack;      /* Optional stack buffer. */
} vec;

/* Initialize a vector */
void vecInit(vec *v, void **stack, size_t initcap);

/* Free only heap storage if any */
void vecRelease(vec *v);

/* Reset the logical length to zero while preserving allocated storage. */
void vecClear(vec *v);

size_t vecSize(const vec *v);

/* Requires index < vecSize(v). */
void *vecGet(const vec *v, size_t index);

/* Return the contiguous backing array. */
void **vecData(vec *v);

/* Ensure capacity is at least mincap. */
void vecReserve(vec *v, size_t mincap);

/* Append one element, growing storage as needed. */
void vecPush(vec *v, void *value);

#ifdef REDIS_TEST
int vectorTest(int argc, char **argv, int flags);
#endif

#endif /* REDIS_VECTOR_H */
