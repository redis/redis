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
void vec_init(vec *v, void **stack, size_t initcap) {
    /* If stack is provided, initcap must be > 0 and at the size of the stack */
    assert(initcap > 0 || stack == NULL);
    
    v->size = 0;
    v->cap = initcap;
    v->stack = stack; /* stack is NULL if not used */
    
    /* now init data either stack, heap or NULL */
    v->data = (stack) ? stack : ((initcap > 0) ? zmalloc(initcap * sizeof(void *)) : NULL);
}

/* Free only heap storage if any */
void vec_destroy(vec *v) {
    /* if data is not stack-allocated and is not NULL, free it */
    if (v->data && v->data != v->stack)
        zfree(v->data);
    v->size = 0;
    v->cap = 0;
    v->data = NULL;
    v->stack = NULL;
}

/* Reset the logical length to zero while preserving allocated storage. */
void vec_clear(vec *v) {
    v->size = 0;
}

/* Return the number of elements in the vector. */
size_t vec_size(const vec *v) {
    return v->size;
}

/* get element at index. index must be < vec_size(v). */
void *vec_get(const vec *v, size_t index) {
    assert(index < v->size);
    return v->data[index];
}

/* Return the contiguous backing array. */
void **vec_data(vec *v) {
    return v->data;
}

/* Append one element, growing storage as needed. Returns status. */
int vec_push(vec *v, void *value) {
    if (v->size == v->cap) {
        void **newdata;
        size_t newcap = (v->cap > 0) ? v->cap * 2 : VEC_DEFAULT_INITCAP;

        /* If so far didn't use heap, then malloc. Else realloc. */
        if (v->data == v->stack) {
            newdata = zmalloc(newcap * sizeof(void *));
            if (v->size) memcpy(newdata, v->data, v->size * sizeof(void *));
        } else {
            newdata = zrealloc(v->data, newcap * sizeof(void *));
        }

        v->data = newdata;
        v->cap = newcap;
    }

    v->data[v->size++] = value;
    return 1;
}

#ifdef REDIS_TEST

#include <stdio.h>
#include <stdlib.h>

#include "testhelp.h"

#define UNUSED(x) (void)(x)

int vectorTest(int argc, char **argv, int flags)
{
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    vec v;
    void *vstack[2];
    int one = 1, two = 2, three = 3, four = 4, five = 5;

    vec_init(&v, vstack, 2);
    test_cond("vec_init() stack-backed size is 0", vec_size(&v) == 0);
    test_cond("vec_init() uses stack buffer", vec_data(&v) == vstack);
    test_cond("vec_push() appends into stack storage",
              vec_push(&v, &one) && vec_push(&v, &two) &&
              vec_size(&v) == 2 && vec_data(&v) == vstack &&
              vec_get(&v, 0) == &one && vec_get(&v, 1) == &two);
    test_cond("vec_push() spills from stack to heap preserving values",
              vec_push(&v, &three) && vec_size(&v) == 3 &&
              vec_data(&v) != vstack && vec_get(&v, 0) == &one &&
              vec_get(&v, 1) == &two && vec_get(&v, 2) == &three);

    void **heap_data = vec_data(&v);
    vec_clear(&v);
    test_cond("vec_clear() resets size but preserves storage",
              vec_size(&v) == 0 && vec_data(&v) == heap_data);
    vec_destroy(&v);
    test_cond("vec_destroy() resets vector state",
              vec_size(&v) == 0 && vec_data(&v) == NULL && v.cap == 0);

    vec_init(&v, NULL, 4);
    test_cond("vec_init() heap-backed hint allocates storage",
              vec_size(&v) == 0 && vec_data(&v) != NULL && v.cap == 4);
    test_cond("vec_push() works in heap-backed mode",
              vec_push(&v, &four) && vec_get(&v, 0) == &four);
    vec_destroy(&v);

    vec_init(&v, NULL, 0);
    test_cond("vec_push() allocates on first push with default growth",
              vec_push(&v, &five) && vec_size(&v) == 1 && vec_data(&v) != NULL);
    vec_destroy(&v);

    return 0;
}
#endif
