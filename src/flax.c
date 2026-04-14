/* Flax -- A flat sorted-array map for uint8_t keys.
 *
 * Copyright (c) 2025-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "flax.h"
#include "redisassert.h"
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>

#ifndef flax_malloc
#ifndef FLAX_MALLOC_INCLUDE
#define FLAX_MALLOC_INCLUDE "flax_malloc.h"
#endif
#include FLAX_MALLOC_INCLUDE
#endif

/* ----------------------------------------------------------------------------
 * Flax internals
 *
 * A flax stores a sorted array of (uint8_t key, void *value) pairs inside a
 * single contiguous heap block.  The block is split into two sub-arrays:
 *
 *   [ keys: uint8_t * capacity ][ padding ][ values: void* * capacity ]
 *
 * The padding between keys and values ensures that the values array starts
 * at a pointer-aligned offset (see flax_values_offset()).
 *
 * Only the first `numele` slots in each sub-array hold live data; the
 * remainder up to `capacity` is unused reserved space.
 *
 * Lookup uses linear scan rather than binary search.  The expected element
 * count is small (e.g. per-consumer stream PEL), so sequential cache-friendly
 * access outperforms binary search whose branch-misprediction cost dominates
 * at these sizes.  Fast-path checks for the head and tail positions further
 * accelerate the common case of monotonically increasing keys.
 *
 * Growth: capacity doubles on insert when full.
 * Shrink: flaxShrink() reallocates to fit exactly.
 * -------------------------------------------------------------------------- */

/* ----------------------------- Internal helpers ---------------------------- */

/* Return the byte offset where the values array starts within the data
 * block for a given capacity. The offset is aligned to pointer size. */
static size_t flax_values_offset(uint16_t capacity) {
    size_t raw = (size_t)capacity * sizeof(uint8_t);
    size_t align = alignof(void *);
    return (raw + align - 1) & ~(align - 1);
}

/* Return a pointer to the keys array inside the flax data block. */
static uint8_t *flax_keys(flax *f) {
    return (uint8_t *)f->data;
}

/* Return a pointer to the values array inside the flax data block. */
static void **flax_values(flax *f) {
    return (void **)((char *)f->data + flax_values_offset(f->capacity));
}

/* Search for 'key' in the sorted 'keys' array of length 'numele'.
 *
 * Returns 1 if the key is found, storing its position in *out_idx.
 * Returns 0 if the key is absent, storing the insertion point in *out_idx
 * (i.e. the index where the key would be placed to keep the array sorted).
 *
 * The search is a linear scan rather than binary search.  This is deliberate:
 * flax instances are expected to be small (tens of elements -- e.g. a stream
 * consumer's PEL).  At these sizes, a sequential walk through a contiguous
 * uint8_t array is faster than binary search because:
 *   1. The entire keys array fits in one or two cache lines.
 *   2. Linear access has no branch-misprediction overhead -- the branch
 *      predictor can reliably learn the "not found yet, keep going" pattern.
 *   3. Binary search touches O(log N) *random* cache lines and suffers a
 *      misprediction at every comparison.
 *
 * Two fast paths are checked first:
 *   - Tail: key > keys[numele-1] is the append case, overwhelmingly common
 *     when keys are monotonically increasing sequence numbers.
 *   - Head: key <= keys[0] catches prepend and exact-match-at-zero. */
static int flax_search(const uint8_t *keys, uint32_t numele, uint8_t key, int16_t *out_idx) {
    if (numele == 0) {
        *out_idx = 0;
        return 0;
    }

    /* Fast path: append (most common — seq numbers grow monotonically). */
    if (key > keys[numele - 1]) {
        *out_idx = numele;
        return 0;
    }
    if (key == keys[numele - 1]) {
        *out_idx = numele - 1;
        return 1;
    }

    /* Fast path: match or prepend at head. */
    if (key <= keys[0]) {
        *out_idx = 0;
        return key == keys[0];
    }

    /* Linear scan through the middle. */
    for (uint32_t i = 1; i < numele - 1; i++) {
        if (keys[i] < key) continue;
        *out_idx = i;
        return keys[i] == key;
    }

    *out_idx = numele - 1;
    return 0;
}

/* Resize the internal storage to 'new_capacity'.
 *
 * A fresh data block is allocated and the live keys and values are copied
 * into it. Because the keys and values sub-arrays sit at different offsets
 * that depend on the capacity (the values offset is re-aligned for the new
 * capacity), we must perform two independent memcpy operations -- one for
 * the keys at the start of the block and one for the values at the new
 * aligned offset. The old data block is freed afterwards. */
static void flax_resize(flax *f, uint16_t new_capacity) {
    if (new_capacity > UINT8_MAX + 1) new_capacity = UINT8_MAX + 1;
    size_t new_voff = flax_values_offset(new_capacity);
    size_t new_alloc = new_voff + (size_t)new_capacity * sizeof(void *);
    size_t new_usable;
    void *new_data = flax_malloc_usable(new_alloc, &new_usable);

    if (f->data && f->numele > 0) {
        memcpy(new_data, f->data, (size_t)f->numele * sizeof(uint8_t));
        memcpy((char *)new_data + new_voff,
               (char *)f->data + flax_values_offset(f->capacity),
               (size_t)f->numele * sizeof(void *));
    }

    size_t old_usable;
    flax_free_usable(f->data, &old_usable);
    f->data = new_data;
    f->capacity = new_capacity;
    f->alloc_size += new_usable - old_usable;
}

/* Update the iterator key and data fields from the underlying flax
 * at the current index position. */
static void flaxIterRefresh(flaxIterator *it) {
    it->key = flax_keys(it->f)[it->idx];
    it->data = flax_values(it->f)[it->idx];
}

/* ----------------------------------------------------------------------------
 * Core API
 * -------------------------------------------------------------------------- */

/* Allocate a new flax and return its pointer. On out of memory the function
 * returns NULL. */
flax *flaxNew(void) {
    size_t usable;
    flax *f = flax_malloc_usable(sizeof(flax), &usable);
    f->alloc_size = usable;
    f->numele = 0;
    f->capacity = FLAX_INIT_CAPACITY;
    size_t voff = flax_values_offset(FLAX_INIT_CAPACITY);
    f->data = flax_malloc_usable(voff + (size_t)FLAX_INIT_CAPACITY * sizeof(void *), &usable);
    f->alloc_size += usable;
    return f;
}

/* Generic insert. If 'overwrite' is true and the key already exists, the
 * associated data is updated and 0 is returned. If 'overwrite' is false
 * and the key exists, the data is left unchanged and 0 is returned. In
 * both cases, if 'old' is not NULL the previous value is stored there.
 * When the key is new, a new element is created and 1 is returned (and
 * *old is set to NULL if provided). */
static int flaxGenericInsert(flax *f, uint8_t key, void *data, void **old, int overwrite) {
    int16_t idx;
    if (flax_search(flax_keys(f), f->numele, key, &idx)) {
        if (old) *old = flax_values(f)[idx];
        if (overwrite) flax_values(f)[idx] = data;
        return 0;
    }

    if (f->numele == f->capacity)
        flax_resize(f, f->capacity * 2);

    uint8_t *keys = flax_keys(f);
    void **vals = flax_values(f);
    int16_t tail = f->numele - idx;

    if (tail > 0) {
        memmove(&keys[idx + 1], &keys[idx], (size_t)tail * sizeof(uint8_t));
        memmove(&vals[idx + 1], &vals[idx], (size_t)tail * sizeof(void *));
    }

    keys[idx] = key;
    vals[idx] = data;
    f->numele++;
    if (old) *old = NULL;
    return 1;
}

/* Overwriting insert. This is just a wrapper for flaxGenericInsert(). */
int flaxInsert(flax *f, uint8_t key, void *data, void **old) {
    return flaxGenericInsert(f, key, data, old, 1);
}

/* Non overwriting insert function: this is just a wrapper for
 * flaxGenericInsert(). */
int flaxTryInsert(flax *f, uint8_t key, void *data, void **old) {
    return flaxGenericInsert(f, key, data, old, 0);
}

/* Remove the specified item. Returns 1 if the item was found and
 * deleted, 0 otherwise. If 'old' is not NULL the removed value is
 * stored at that address. */
int flaxRemove(flax *f, uint8_t key, void **old) {
    if (!f || f->numele == 0) {
        if (old) *old = NULL;
        return 0;
    }

    int16_t idx;
    if (!flax_search(flax_keys(f), f->numele, key, &idx)) {
        if (old) *old = NULL;
        return 0;
    }

    uint8_t *keys = flax_keys(f);
    void **vals = flax_values(f);
    if (old) *old = vals[idx];
    int16_t tail = f->numele - idx - 1;

    if (tail > 0) {
        memmove(&keys[idx], &keys[idx + 1], (size_t)tail * sizeof(uint8_t));
        memmove(&vals[idx], &vals[idx + 1], (size_t)tail * sizeof(void *));
    }

    f->numele--;

    if (f->numele > 0 &&
        f->capacity > FLAX_INIT_CAPACITY &&
        f->numele <= f->capacity / 2)
    {
        uint16_t new_cap = f->capacity / 2;
        if (new_cap < FLAX_INIT_CAPACITY) new_cap = FLAX_INIT_CAPACITY;
        flax_resize(f, new_cap);
    }

    return 1;
}

/* Find a key in the flax, returning 1 if found, 0 otherwise. If the key
 * is found and 'value' is not NULL, the associated data pointer is stored
 * at that address. */
int flaxFind(flax *f, uint8_t key, void **value) {
    if (!f || f->numele == 0) {
        if (value) *value = NULL;
        return 0;
    }
    int16_t idx;
    if (flax_search(flax_keys(f), f->numele, key, &idx)) {
        if (value) *value = flax_values(f)[idx];
        return 1;
    }
    if (value) *value = NULL;
    return 0;
}

/* Free a whole flax. */
void flaxFree(flax *f) {
    if (!f) return;
    flax_free(f->data);
    flax_free(f);
}

/* Free a whole flax, calling the specified callback with a context
 * argument in order to free the auxiliary data. */
void flaxFreeWithCallback(flax *f,
                          void (*free_callback)(void *item, void *ctx),
                          void *ctx) {
    if (!f) return;
    if (free_callback && f->data && f->numele > 0) {
        void **vals = flax_values(f);
        for (uint32_t i = 0; i < f->numele; i++)
            free_callback(vals[i], ctx);
    }
    flax_free(f->data);
    flax_free(f);
}

/* Return the number of elements inside the flax. */
uint16_t flaxSize(flax *f) {
    return f->numele;
}

/* Return the total heap memory used by the flax struct and its data block.
 * O(1): the value is maintained incrementally by alloc/resize operations. */
size_t flaxAllocSize(flax *f) {
    if (!f) return 0;
    return f->alloc_size;
}

/* Shrink the internal storage to fit the current number of elements,
 * releasing unused memory. No-op when the flax is empty (the caller should
 * flaxFree() the whole structure instead) or already at exact capacity. */
void flaxShrink(flax *f) {
    if (f->numele > 0 && f->numele < f->capacity)
        flax_resize(f, f->numele);
}

/* ------------------------------- Iterator --------------------------------- */

/* Initialize a flax iterator. This call should be performed a single time
 * to initialize the iterator, and must be followed by a flaxSeek() call,
 * otherwise the flaxPrev()/flaxNext() functions will just return EOF. */
void flaxStart(flaxIterator *it, flax *f) {
    it->f = f;
    it->idx = -1;
    it->key = 0;
    it->data = NULL;
}

/* Seek an iterator at the specified element. The 'op' argument selects the
 * seek mode: "^" for the first element, "$" for the last, ">=" for greater
 * or equal, ">" for strictly greater, "<=" for less or equal, "<" for
 * strictly less, and "=" for exact match. Return 0 if no matching element
 * was found, otherwise 1 is returned. */
int flaxSeek(flaxIterator *it, const char *op, uint8_t key) {
    if (!it->f || it->f->numele == 0) {
        it->idx = -1;
        it->key = 0;
        it->data = NULL;
        return 0;
    }

    if (op[0] == '^') {
        it->idx = 0;
        flaxIterRefresh(it);
        return 1;
    }

    if (op[0] == '$') {
        it->idx = it->f->numele - 1;
        flaxIterRefresh(it);
        return 1;
    }

    if (op[0] == '>' && op[1] == '=') {
        int16_t idx;
        flax_search(flax_keys(it->f), it->f->numele, key, &idx);
        if (idx >= it->f->numele) {
            it->idx = -1;
            it->key = 0;
            it->data = NULL;
            return 0;
        }
        it->idx = idx;
        flaxIterRefresh(it);
        return 1;
    }

    if (op[0] == '>' && op[1] == '\0') {
        int16_t idx;
        int found = flax_search(flax_keys(it->f), it->f->numele, key, &idx);
        if (found) idx++;
        if (idx >= it->f->numele) {
            it->idx = -1;
            it->key = 0;
            it->data = NULL;
            return 0;
        }
        it->idx = idx;
        flaxIterRefresh(it);
        return 1;
    }

    if (op[0] == '<' && op[1] == '=') {
        int16_t idx;
        int found = flax_search(flax_keys(it->f), it->f->numele, key, &idx);
        if (found) {
            it->idx = idx;
        } else {
            if (idx == 0) {
                it->idx = -1;
                it->key = 0;
                it->data = NULL;
                return 0;
            }
            it->idx = idx - 1;
        }
        flaxIterRefresh(it);
        return 1;
    }

    if (op[0] == '<' && op[1] == '\0') {
        int16_t idx;
        flax_search(flax_keys(it->f), it->f->numele, key, &idx);
        if (idx == 0) {
            it->idx = -1;
            it->key = 0;
            it->data = NULL;
            return 0;
        }
        it->idx = idx - 1;
        flaxIterRefresh(it);
        return 1;
    }

    if (op[0] == '=' && op[1] == '\0') {
        int16_t idx;
        if (!flax_search(flax_keys(it->f), it->f->numele, key, &idx)) {
            it->idx = -1;
            it->key = 0;
            it->data = NULL;
            return 0;
        }
        it->idx = idx;
        flaxIterRefresh(it);
        return 1;
    }

    assert(0 && "flaxSeek: unrecognized op");
    it->idx = -1;
    it->key = 0;
    it->data = NULL;
    return 0;
}

/* Go to the next element in the scope of the iterator 'it'.
 * If EOF is reached, 0 is returned, otherwise 1 is returned. */
int flaxNext(flaxIterator *it) {
    if (it->idx < 0) return 0;
    it->idx++;
    if (it->idx >= it->f->numele) {
        it->idx = -1;
        it->key = 0;
        it->data = NULL;
        return 0;
    }
    flaxIterRefresh(it);
    return 1;
}

/* Go to the previous element in the scope of the iterator 'it'.
 * If EOF is reached, 0 is returned, otherwise 1 is returned. */
int flaxPrev(flaxIterator *it) {
    if (it->idx < 0) return 0;
    it->idx--;
    if (it->idx < 0) {
        it->key = 0;
        it->data = NULL;
        return 0;
    }
    flaxIterRefresh(it);
    return 1;
}

/* Stop the iterator (no-op, included for API symmetry with rax). */
void flaxStop(flaxIterator *it) {
    (void)it;
}

/* Return if the iterator is in an EOF state. This happens when flaxSeek()
 * failed to seek an appropriate element, so that flaxNext() or flaxPrev()
 * will return zero, or when an EOF condition was reached while iterating
 * with flaxNext() and flaxPrev(). */
int flaxEOF(flaxIterator *it) {
    return it->idx < 0 || it->idx >= it->f->numele;
}

/* ----------------------------- Unit tests --------------------------------- */

#ifdef REDIS_TEST
#include "testhelp.h"
#include <stdio.h>
#include <string.h>

#define UNUSED(x) (void)(x)

#define ERR(x, ...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __func__, __LINE__);                   \
        printf("ERROR! " x "\n", __VA_ARGS__);                                \
        err++;                                                                 \
    } while (0)

#define TEST(name) printf("test — %s\n", name);

static int flax_test_free_count;

static void flax_test_counting_free(void *p, void *ctx) {
    (void)ctx;
    flax_test_free_count++;
    flax_free(p);
}

static void flax_test_ctx_free(void *p, void *ctx) {
    (void)p;
    int *cnt = ctx;
    (*cnt)++;
}

int flaxTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int err = 0;

    TEST("new and free empty") {
        flax *a = flaxNew();
        assert(a != NULL);
        assert(a->numele == 0);
        assert(a->capacity == FLAX_INIT_CAPACITY);
        assert(a->data != NULL);
        flaxFree(a);
    }

    TEST("find on empty flax") {
        flax *a = flaxNew();
        void *val;
        assert(flaxFind(a, 42, &val) == 0);
        assert(val == NULL);
        assert(flaxFind(a, 1, &val) == 0);
        flaxFree(a);
    }

    TEST("insert and find") {
        flax *a = flaxNew();
        void *old, *val;

        flaxInsert(a, 30, "thirty", &old);
        assert(old == NULL);
        flaxInsert(a, 10, "ten", &old);
        assert(old == NULL);
        flaxInsert(a, 50, "fifty", &old);
        assert(old == NULL);
        flaxInsert(a, 20, "twenty", &old);
        assert(old == NULL);
        flaxInsert(a, 40, "forty", &old);
        assert(old == NULL);
        assert(flaxSize(a) == 5);

        assert(flaxFind(a, 10, &val) == 1);
        assert(strcmp(val, "ten") == 0);
        assert(flaxFind(a, 20, &val) == 1);
        assert(strcmp(val, "twenty") == 0);
        assert(flaxFind(a, 30, &val) == 1);
        assert(strcmp(val, "thirty") == 0);
        assert(flaxFind(a, 40, &val) == 1);
        assert(strcmp(val, "forty") == 0);
        assert(flaxFind(a, 50, &val) == 1);
        assert(strcmp(val, "fifty") == 0);
        assert(flaxFind(a, 99, &val) == 0);
        assert(flaxFind(a, 0, &val) == 0);

        flaxFree(a);
    }

    TEST("insert duplicate replaces value") {
        flax *a = flaxNew();
        flaxInsert(a, 5, "old_five", NULL);
        flaxInsert(a, 10, "old_ten", NULL);

        void *old, *val;
        flaxInsert(a, 5, "new_five", &old);
        assert(old != NULL);
        assert(strcmp(old, "old_five") == 0);
        assert(flaxSize(a) == 2);
        assert(flaxFind(a, 5, &val) == 1);
        assert(strcmp(val, "new_five") == 0);

        flaxInsert(a, 10, "new_ten", &old);
        assert(strcmp(old, "old_ten") == 0);
        assert(flaxSize(a) == 2);
        assert(flaxFind(a, 10, &val) == 1);
        assert(strcmp(val, "new_ten") == 0);

        flaxFree(a);
    }

    TEST("remove basic") {
        flax *a = flaxNew();
        flaxInsert(a, 1, "one", NULL);
        flaxInsert(a, 2, "two", NULL);
        flaxInsert(a, 3, "three", NULL);

        void *old, *val;
        assert(flaxRemove(a, 2, &old) == 1);
        assert(strcmp(old, "two") == 0);
        assert(flaxSize(a) == 2);
        assert(flaxFind(a, 2, &val) == 0);
        assert(flaxFind(a, 1, &val) == 1);
        assert(strcmp(val, "one") == 0);
        assert(flaxFind(a, 3, &val) == 1);
        assert(strcmp(val, "three") == 0);

        flaxFree(a);
    }

    TEST("remove not found") {
        flax *a = flaxNew();
        flaxInsert(a, 1, "one", NULL);
        void *old;
        assert(flaxRemove(a, 99, &old) == 0);
        assert(old == NULL);
        assert(flaxSize(a) == 1);

        flax *b = flaxNew();
        assert(flaxRemove(b, 1, &old) == 0);
        flaxFree(b);

        flaxFree(a);
    }

    TEST("remove only element") {
        flax *a = flaxNew();
        flaxInsert(a, 42, "answer", NULL);
        void *old, *val;
        assert(flaxRemove(a, 42, &old) == 1);
        assert(strcmp(old, "answer") == 0);
        assert(flaxSize(a) == 0);
        assert(flaxFind(a, 42, &val) == 0);

        flaxFree(a);
    }

    TEST("insert at beginning and end") {
        flax *a = flaxNew();
        flaxInsert(a, 50, "middle", NULL);
        flaxInsert(a, 100, "end", NULL);
        flaxInsert(a, 1, "begin", NULL);

        void *val;
        assert(flaxSize(a) == 3);
        assert(flaxFind(a, 1, &val) == 1);
        assert(strcmp(val, "begin") == 0);
        assert(flaxFind(a, 50, &val) == 1);
        assert(strcmp(val, "middle") == 0);
        assert(flaxFind(a, 100, &val) == 1);
        assert(strcmp(val, "end") == 0);

        flaxFree(a);
    }

    TEST("grow beyond initial capacity") {
        flax *a = flaxNew();
        for (int i = 0; i < 128; i++) {
            char *buf = flax_malloc(16);
            snprintf(buf, 16, "v%d", i);
            flaxInsert(a, (uint8_t)(i * 2), buf, NULL);
        }
        assert(flaxSize(a) == 128);
        assert(a->capacity >= 128);

        for (int i = 0; i < 128; i++) {
            char expected[16];
            snprintf(expected, sizeof(expected), "v%d", i);
            void *val;
            assert(flaxFind(a, (uint8_t)(i * 2), &val) == 1);
            if (strcmp(val, expected) != 0) {
                ERR("grow: key %d expected '%s' got '%s'",
                    i * 2, expected, (char *)val);
            }
        }

        flaxFreeWithCallback(a, flax_test_counting_free, NULL);
    }

    TEST("auto-shrink on remove") {
        flax *a = flaxNew();
        for (int i = 0; i < 64; i++)
            flaxInsert(a, (uint8_t)i, "x", NULL);

        assert(flaxSize(a) == 64);
        uint16_t cap_full = a->capacity;

        for (int i = 0; i < 56; i++)
            flaxRemove(a, (uint8_t)i, NULL);

        assert(flaxSize(a) == 8);
        if (a->capacity >= cap_full) {
            ERR("auto-shrink: capacity %u should be less than %u after removals",
                a->capacity, cap_full);
        }

        for (int i = 56; i < 64; i++) {
            void *val;
            assert(flaxFind(a, (uint8_t)i, &val) == 1);
            assert(strcmp(val, "x") == 0);
        }

        flaxFree(a);
    }

    TEST("explicit shrink after removals") {
        flax *a = flaxNew();
        for (int i = 0; i < 64; i++)
            flaxInsert(a, (uint8_t)i, "x", NULL);

        for (int i = 0; i < 56; i++)
            flaxRemove(a, (uint8_t)i, NULL);

        assert(flaxSize(a) == 8);
        uint16_t cap_after_remove = a->capacity;
        flaxShrink(a);
        assert(a->capacity <= cap_after_remove);
        assert(a->capacity >= a->numele);

        for (int i = 56; i < 64; i++) {
            void *val;
            assert(flaxFind(a, (uint8_t)i, &val) == 1);
            assert(strcmp(val, "x") == 0);
        }

        flaxFree(a);
    }

    TEST("no shrink below FLAX_INIT_CAPACITY") {
        flax *a = flaxNew();
        for (int i = 0; i < 4; i++)
            flaxInsert(a, (uint8_t)i, "x", NULL);

        assert(a->capacity == FLAX_INIT_CAPACITY);
        for (int i = 0; i < 3; i++)
            flaxRemove(a, (uint8_t)i, NULL);

        assert(flaxSize(a) == 1);
        assert(a->capacity == FLAX_INIT_CAPACITY);

        flaxFree(a);
    }

    TEST("flaxFreeWithCallback invokes callback") {
        flax_test_free_count = 0;
        flax *a = flaxNew();
        for (int i = 0; i < 5; i++) {
            char *s = flax_malloc(8);
            snprintf(s, 8, "str%d", i);
            flaxInsert(a, i, s, NULL);
        }
        flaxFreeWithCallback(a, flax_test_counting_free, NULL);
        if (flax_test_free_count != 5) {
            ERR("freeWithCallback: expected 5 frees, got %d",
                flax_test_free_count);
        }
    }

    TEST("flaxFreeWithCallback on empty flax") {
        flax_test_free_count = 0;
        flax *a = flaxNew();
        flaxFreeWithCallback(a, flax_test_counting_free, NULL);
        if (flax_test_free_count != 0) {
            ERR("freeWithCallback empty: expected 0 frees, got %d",
                flax_test_free_count);
        }
    }

    TEST("keys near uint8 boundaries") {
        flax *a = flaxNew();
        flaxInsert(a, 0, "zero", NULL);
        flaxInsert(a, 255, "max", NULL);
        flaxInsert(a, 254, "max-1", NULL);
        flaxInsert(a, 100, "hundred", NULL);

        void *val;
        assert(flaxSize(a) == 4);
        assert(flaxFind(a, 0, &val) == 1);
        assert(strcmp(val, "zero") == 0);
        assert(flaxFind(a, 100, &val) == 1);
        assert(strcmp(val, "hundred") == 0);
        assert(flaxFind(a, 254, &val) == 1);
        assert(strcmp(val, "max-1") == 0);
        assert(flaxFind(a, 255, &val) == 1);
        assert(strcmp(val, "max") == 0);

        flaxFree(a);
    }

    TEST("flaxTryInsert does not overwrite") {
        flax *a = flaxNew();
        assert(flaxTryInsert(a, 10, "ten", NULL) == 1);
        assert(flaxTryInsert(a, 20, "twenty", NULL) == 1);
        assert(flaxSize(a) == 2);

        void *old, *val;
        assert(flaxTryInsert(a, 10, "new_ten", &old) == 0);
        assert(strcmp(old, "ten") == 0);
        assert(flaxSize(a) == 2);
        assert(flaxFind(a, 10, &val) == 1);
        assert(strcmp(val, "ten") == 0);

        flaxFree(a);
    }

    TEST("iterator on empty flax") {
        flax *a = flaxNew();
        flaxIterator it;
        flaxStart(&it, a);
        assert(flaxSeek(&it, "^", 0) == 0);
        assert(flaxEOF(&it) == 1);
        assert(flaxSeek(&it, "$", 0) == 0);
        assert(flaxSeek(&it, ">=", 42) == 0);
        flaxStop(&it);

        flaxFree(a);
    }

    TEST("iterator forward") {
        flax *a = flaxNew();
        flaxInsert(a, 10, "ten", NULL);
        flaxInsert(a, 30, "thirty", NULL);
        flaxInsert(a, 20, "twenty", NULL);
        flaxInsert(a, 40, "forty", NULL);

        flaxIterator it;
        flaxStart(&it, a);
        assert(flaxSeek(&it, "^", 0));
        assert(it.key == 10);
        assert(strcmp(it.data, "ten") == 0);
        assert(flaxNext(&it));
        assert(it.key == 20);
        assert(flaxNext(&it));
        assert(it.key == 30);
        assert(flaxNext(&it));
        assert(it.key == 40);
        assert(flaxNext(&it) == 0);
        assert(flaxEOF(&it) == 1);
        flaxStop(&it);

        flaxFree(a);
    }

    TEST("iterator backward") {
        flax *a = flaxNew();
        flaxInsert(a, 10, "ten", NULL);
        flaxInsert(a, 20, "twenty", NULL);
        flaxInsert(a, 30, "thirty", NULL);

        flaxIterator it;
        flaxStart(&it, a);
        assert(flaxSeek(&it, "$", 0));
        assert(it.key == 30);
        assert(flaxPrev(&it));
        assert(it.key == 20);
        assert(flaxPrev(&it));
        assert(it.key == 10);
        assert(flaxPrev(&it) == 0);
        flaxStop(&it);

        flaxFree(a);
    }

    TEST("iterator seek >=") {
        flax *a = flaxNew();
        flaxInsert(a, 10, "ten", NULL);
        flaxInsert(a, 20, "twenty", NULL);
        flaxInsert(a, 30, "thirty", NULL);
        flaxInsert(a, 40, "forty", NULL);

        flaxIterator it;
        flaxStart(&it, a);

        assert(flaxSeek(&it, ">=", 20));
        assert(it.key == 20);

        assert(flaxSeek(&it, ">=", 25));
        assert(it.key == 30);

        assert(flaxSeek(&it, ">=", 5));
        assert(it.key == 10);

        assert(flaxSeek(&it, ">=", 41) == 0);
        assert(flaxEOF(&it) == 1);
        flaxStop(&it);

        flaxFree(a);
    }

    TEST("iterator on single element") {
        flax *a = flaxNew();
        flaxInsert(a, 42, "answer", NULL);

        flaxIterator it;
        flaxStart(&it, a);
        assert(flaxSeek(&it, "^", 0));
        assert(it.key == 42);
        assert(strcmp(it.data, "answer") == 0);
        assert(flaxNext(&it) == 0);

        flaxStart(&it, a);
        assert(flaxSeek(&it, "$", 0));
        assert(it.key == 42);
        assert(flaxPrev(&it) == 0);
        flaxStop(&it);

        flaxFree(a);
    }

    TEST("flaxFreeWithCallback") {
        int ctx_free_count = 0;
        flax *a = flaxNew();
        flaxInsert(a, 1, "one", NULL);
        flaxInsert(a, 2, "two", NULL);
        flaxInsert(a, 3, "three", NULL);
        flaxFreeWithCallback(a, flax_test_ctx_free, &ctx_free_count);
        if (ctx_free_count != 3) {
            ERR("freeWithCallback: expected 3 frees, got %d",
                ctx_free_count);
        }
    }

    TEST("iterator seek >") {
        flax *a = flaxNew();
        flaxInsert(a, 10, "ten", NULL);
        flaxInsert(a, 20, "twenty", NULL);
        flaxInsert(a, 30, "thirty", NULL);
        flaxInsert(a, 40, "forty", NULL);

        flaxIterator it;
        flaxStart(&it, a);

        /* ">" on existing key skips to the next one. */
        assert(flaxSeek(&it, ">", 20));
        assert(it.key == 30);

        /* ">" on non-existing key lands on the first key greater. */
        assert(flaxSeek(&it, ">", 25));
        assert(it.key == 30);

        /* ">" on a key smaller than all elements returns the first. */
        assert(flaxSeek(&it, ">", 5));
        assert(it.key == 10);

        /* ">" on the largest key returns EOF. */
        assert(flaxSeek(&it, ">", 40) == 0);
        assert(flaxEOF(&it) == 1);

        /* ">" on a key beyond all elements returns EOF. */
        assert(flaxSeek(&it, ">", 50) == 0);
        assert(flaxEOF(&it) == 1);

        flaxStop(&it);
        flaxFree(a);
    }

    TEST("iterator seek <=") {
        flax *a = flaxNew();
        flaxInsert(a, 10, "ten", NULL);
        flaxInsert(a, 20, "twenty", NULL);
        flaxInsert(a, 30, "thirty", NULL);
        flaxInsert(a, 40, "forty", NULL);

        flaxIterator it;
        flaxStart(&it, a);

        /* "<=" on existing key lands on that key. */
        assert(flaxSeek(&it, "<=", 20));
        assert(it.key == 20);

        /* "<=" on non-existing key lands on the greatest smaller key. */
        assert(flaxSeek(&it, "<=", 25));
        assert(it.key == 20);

        /* "<=" on the largest key lands on it. */
        assert(flaxSeek(&it, "<=", 40));
        assert(it.key == 40);

        /* "<=" on a key beyond all elements lands on the last. */
        assert(flaxSeek(&it, "<=", 100));
        assert(it.key == 40);

        /* "<=" on a key smaller than all returns EOF. */
        assert(flaxSeek(&it, "<=", 5) == 0);
        assert(flaxEOF(&it) == 1);

        flaxStop(&it);
        flaxFree(a);
    }

    TEST("iterator seek <") {
        flax *a = flaxNew();
        flaxInsert(a, 10, "ten", NULL);
        flaxInsert(a, 20, "twenty", NULL);
        flaxInsert(a, 30, "thirty", NULL);
        flaxInsert(a, 40, "forty", NULL);

        flaxIterator it;
        flaxStart(&it, a);

        /* "<" on existing key lands on the previous one. */
        assert(flaxSeek(&it, "<", 20));
        assert(it.key == 10);

        /* "<" on non-existing key lands on the greatest smaller key. */
        assert(flaxSeek(&it, "<", 25));
        assert(it.key == 20);

        /* "<" on a key beyond all elements lands on the last. */
        assert(flaxSeek(&it, "<", 100));
        assert(it.key == 40);

        /* "<" on the smallest key returns EOF. */
        assert(flaxSeek(&it, "<", 10) == 0);
        assert(flaxEOF(&it) == 1);

        /* "<" on a key smaller than all returns EOF. */
        assert(flaxSeek(&it, "<", 5) == 0);
        assert(flaxEOF(&it) == 1);

        flaxStop(&it);
        flaxFree(a);
    }

    TEST("iterator seek =") {
        flax *a = flaxNew();
        flaxInsert(a, 10, "ten", NULL);
        flaxInsert(a, 20, "twenty", NULL);
        flaxInsert(a, 30, "thirty", NULL);

        flaxIterator it;
        flaxStart(&it, a);

        /* "=" on existing key succeeds. */
        assert(flaxSeek(&it, "=", 20));
        assert(it.key == 20);
        assert(strcmp(it.data, "twenty") == 0);

        /* "=" on first key. */
        assert(flaxSeek(&it, "=", 10));
        assert(it.key == 10);

        /* "=" on last key. */
        assert(flaxSeek(&it, "=", 30));
        assert(it.key == 30);

        /* "=" on non-existing key returns EOF. */
        assert(flaxSeek(&it, "=", 15) == 0);
        assert(flaxEOF(&it) == 1);

        assert(flaxSeek(&it, "=", 0) == 0);
        assert(flaxSeek(&it, "=", 255) == 0);

        flaxStop(&it);
        flaxFree(a);
    }

    TEST("flaxAllocSize tracks allocations") {
        flax *a = flaxNew();
        size_t sz0 = flaxAllocSize(a);
        assert(sz0 > 0);

        for (int i = 0; i < 64; i++)
            flaxInsert(a, (uint8_t)i, "x", NULL);

        size_t sz1 = flaxAllocSize(a);
        assert(sz1 >= sz0);

        flaxShrink(a);
        size_t sz2 = flaxAllocSize(a);
        assert(sz2 <= sz1);
        assert(sz2 > 0);

        assert(flaxAllocSize(NULL) == 0);

        flaxFree(a);
    }

    TEST("flaxFree and flaxFreeWithCallback on NULL") {
        flaxFree(NULL);
        flaxFreeWithCallback(NULL, flax_test_counting_free, NULL);
    }

    TEST("flaxFind and flaxRemove on NULL flax") {
        void *val;
        assert(flaxFind(NULL, 42, &val) == 0);
        assert(val == NULL);
        assert(flaxRemove(NULL, 42, &val) == 0);
        assert(val == NULL);
    }

    TEST("flaxTryInsert with old=NULL on duplicate") {
        flax *a = flaxNew();
        assert(flaxTryInsert(a, 10, "ten", NULL) == 1);
        assert(flaxTryInsert(a, 10, "new_ten", NULL) == 0);
        assert(flaxSize(a) == 1);

        void *val;
        assert(flaxFind(a, 10, &val) == 1);
        assert(strcmp(val, "ten") == 0);

        flaxFree(a);
    }

    TEST("iterator seek with boundary keys 0 and 255") {
        flax *a = flaxNew();
        flaxInsert(a, 0, "zero", NULL);
        flaxInsert(a, 128, "mid", NULL);
        flaxInsert(a, 255, "max", NULL);

        flaxIterator it;
        flaxStart(&it, a);

        assert(flaxSeek(&it, ">=", 0));
        assert(it.key == 0);
        assert(flaxSeek(&it, ">=", 255));
        assert(it.key == 255);

        assert(flaxSeek(&it, ">", 0));
        assert(it.key == 128);
        assert(flaxSeek(&it, ">", 255) == 0);

        assert(flaxSeek(&it, "<=", 255));
        assert(it.key == 255);
        assert(flaxSeek(&it, "<=", 0));
        assert(it.key == 0);

        assert(flaxSeek(&it, "<", 255));
        assert(it.key == 128);
        assert(flaxSeek(&it, "<", 0) == 0);

        assert(flaxSeek(&it, "=", 0));
        assert(it.key == 0);
        assert(flaxSeek(&it, "=", 255));
        assert(it.key == 255);

        flaxStop(&it);
        flaxFree(a);
    }

    TEST("iterator seek on empty flax all operators") {
        flax *a = flaxNew();
        flaxIterator it;
        flaxStart(&it, a);

        assert(flaxSeek(&it, ">", 42) == 0);
        assert(flaxSeek(&it, "<=", 42) == 0);
        assert(flaxSeek(&it, "<", 42) == 0);
        assert(flaxSeek(&it, "=", 42) == 0);

        flaxStop(&it);
        flaxFree(a);
    }

    if (!err)
        printf("ALL TESTS PASSED!\n");
    else
        ERR("Sorry, not all tests passed! In fact, %d tests failed.", err);

    return err;
}

#endif
