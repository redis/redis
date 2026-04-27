/* vector.c - Simple append-only vector implementation
 *
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vector.h"
#include "redisassert.h"
#include "zmalloc.h"

#define VEC_DEFAULT_INITCAP 8

/*
 * Vector initialization.
 *
 * Modes:
 * - stack != NULL: use caller-provided storage for the first initcap items.
 * - stack == NULL && initcap > 0: start heap-backed with an initial 'initcap' capacity.
 * - stack == NULL && initcap == 0: start heap-backed with no initial storage.
 */
void vecInit(vec *v, void **stack, size_t initcap) {
    /* If stack is provided, initcap must be > 0 and at the size of the stack */
    assert(initcap > 0 || stack == NULL);
    
    v->size = 0;
    v->cap = initcap;
    v->stack = stack; /* stack is NULL if not used */
    v->free = NULL;

    /* now init data either stack, heap or NULL */
    v->data = (stack) ? stack : ((initcap > 0) ? zmalloc(initcap * sizeof(void *)) : NULL);
}

/* Release storage. If a free method is set, it is applied to every element
 * before the backing storage is released. Stack storage is never freed. */
void vecRelease(vec *v) {
    if (v->free) {
        for (size_t i = 0; i < v->size; i++)
            v->free(v->data[i]);
    }
    /* if data is not stack-allocated and is not NULL, free it */
    if (v->data && v->data != v->stack)
        zfree(v->data);
    v->size = 0;
    v->cap = 0;
    v->data = NULL;
    v->stack = NULL;
    v->free = NULL;
}

/* Reset the logical length to zero while preserving allocated storage. */
void vecClear(vec *v) {
    v->size = 0;
}

/* Get element at index. index must be < vecSize(v). */
void *vecGet(const vec *v, size_t index) {
    assert(index < v->size);
    return v->data[index];
}

/* Ensure capacity is at least mincap. */
void vecReserve(vec *v, size_t mincap) {
    void **newdata;

    if (mincap <= v->cap) return;

    /* If no heap storage is used yet, allocate and copy from stack if needed. */
    if (v->data == v->stack) {
        newdata = zmalloc(mincap * sizeof(void *));
        if (v->size) memcpy(newdata, v->data, v->size * sizeof(void *));
    } else {
        newdata = zrealloc(v->data, mincap * sizeof(void *));
    }

    v->data = newdata;
    v->cap = mincap;
}

/* Append one element, growing storage as needed. */
void vecPush(vec *v, void *value) {
    if (unlikely(v->size == v->cap)) {
        size_t newcap = (v->cap > 0) ? v->cap * 2 : VEC_DEFAULT_INITCAP;
        vecReserve(v, newcap);
    }

    v->data[v->size++] = value;
}

#ifdef REDIS_TEST

#include <stdio.h>
#include <stdlib.h>

#include "testhelp.h"

#define UNUSED(x) (void)(x)

static int vecTestFreeCalls = 0;
static void vecTestFree(void *ptr) {
    UNUSED(ptr);
    vecTestFreeCalls++;
}

int vectorTest(int argc, char **argv, int flags)
{
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    vec v;
    void *vstack[2];
    int one = 1, two = 2, three = 3, four = 4, five = 5, six = 6;

    vecInit(&v, vstack, 2);
    test_cond("vecInit() stack-backed size is 0", vecSize(&v) == 0);
    test_cond("vecInit() uses stack buffer", vecData(&v) == vstack);
    vecReserve(&v, 1);
    test_cond("vecReserve() no-ops when capacity is already sufficient",
              v.cap == 2 && vecData(&v) == vstack);
    vecPush(&v, &one);
    vecPush(&v, &two);
    test_cond("vecPush() appends into stack storage",
              vecSize(&v) == 2 && vecData(&v) == vstack &&
              vecGet(&v, 0) == &one && vecGet(&v, 1) == &two);
    vecReserve(&v, 4);
    test_cond("vecReserve() spills from stack to heap preserving values",
              v.cap == 4 && vecData(&v) != vstack &&
              vecGet(&v, 0) == &one && vecGet(&v, 1) == &two);
    vecPush(&v, &three);
    test_cond("vecPush() spills from stack to heap preserving values",
              vecSize(&v) == 3 &&
              vecData(&v) != vstack && vecGet(&v, 0) == &one &&
              vecGet(&v, 1) == &two && vecGet(&v, 2) == &three);

    void **heap_data = vecData(&v);
    vecClear(&v);
    test_cond("vecClear() resets size but preserves storage",
              vecSize(&v) == 0 && vecData(&v) == heap_data);
    vecRelease(&v);
    test_cond("vecRelease() resets vector state",
              vecSize(&v) == 0 && vecData(&v) == NULL && v.cap == 0);

    vecInit(&v, NULL, 4);
    test_cond("vecInit() heap-backed hint allocates storage",
              vecSize(&v) == 0 && vecData(&v) != NULL && v.cap == 4);
    vecPush(&v, &four);
    test_cond("vecPush() works in heap-backed mode",
              vecGet(&v, 0) == &four);
    vecReserve(&v, 8);
    test_cond("vecReserve() grows heap-backed storage preserving values",
              v.cap == 8 && vecGet(&v, 0) == &four);
    vecRelease(&v);

    vecInit(&v, NULL, 0);
    vecReserve(&v, 6);
    test_cond("vecReserve() allocates heap storage from empty vector",
              v.cap == 6 && vecData(&v) != NULL);
    vecPush(&v, &five);
    vecPush(&v, &six);
    test_cond("vecPush() works after vecReserve() on empty vector",
              vecSize(&v) == 2 &&
              vecGet(&v, 0) == &five && vecGet(&v, 1) == &six);
    vecRelease(&v);

    /* vecSetFreeMethod: element free callback is invoked on release. */
    void *vstack2[2];
    vecInit(&v, vstack2, 2);
    vecSetFreeMethod(&v, vecTestFree);
    vecPush(&v, &one);
    vecPush(&v, &two);
    vecPush(&v, &three); /* triggers spill to heap */
    vecTestFreeCalls = 0;
    vecRelease(&v);
    test_cond("vecRelease() invokes free method on each element",
              vecTestFreeCalls == 3);

    vecInit(&v, NULL, 4);
    vecSetFreeMethod(&v, vecTestFree);
    vecTestFreeCalls = 0;
    vecRelease(&v);
    test_cond("vecRelease() free method is a no-op on empty vector",
              vecTestFreeCalls == 0);

    return 0;
}
#endif
