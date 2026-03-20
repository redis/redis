#include "flax.h"
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>

#ifndef flax_malloc
#ifndef FLAX_MALLOC_INCLUDE
#define FLAX_MALLOC_INCLUDE "flax_malloc.h"
#endif
#include FLAX_MALLOC_INCLUDE
#endif

static size_t flax_values_offset(int64_t capacity) {
    size_t raw = (size_t)capacity * sizeof(int64_t);
    size_t align = alignof(void *);
    return (raw + align - 1) & ~(align - 1);
}

static int64_t *flax_keys(flax *f) {
    return (int64_t *)f->data;
}

static void **flax_values(flax *f) {
    return (void **)((char *)f->data + flax_values_offset(f->capacity));
}

/* Linear scan with fast paths for first/last.
 * Returns 1 if key found (out_idx = its index), 0 if not (out_idx = insertion point).
 * Sequential access through the contiguous keys array is cache-friendly and
 * avoids branch-misprediction overhead of binary search at typical flax sizes. */
static int flax_search(const int64_t *keys, int64_t numele, int64_t key, int64_t *out_idx) {
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
    for (int64_t i = 1; i < numele - 1; i++) {
        if (keys[i] < key) continue;
        *out_idx = i;
        return keys[i] == key;
    }

    *out_idx = numele - 1;
    return 0;
}

static void flax_resize(flax *f, int64_t new_capacity) {
    size_t new_voff = flax_values_offset(new_capacity);
    size_t new_alloc = new_voff + (size_t)new_capacity * sizeof(void *);
    void *new_data = flax_malloc(new_alloc);

    if (f->data && f->numele > 0) {
        memcpy(new_data, f->data, (size_t)f->numele * sizeof(int64_t));
        memcpy((char *)new_data + new_voff,
               (char *)f->data + flax_values_offset(f->capacity),
               (size_t)f->numele * sizeof(void *));
    }

    flax_free(f->data);
    f->data = new_data;
    f->capacity = new_capacity;
}

static void flaxIterRefresh(flaxIterator *it) {
    it->key = flax_keys(it->f)[it->idx];
    it->data = flax_values(it->f)[it->idx];
}

flax *flaxNew(void) {
    flax *f = flax_malloc(sizeof(flax));
    f->numele = 0;
    f->capacity = FLAX_MIN_CAPACITY;
    size_t voff = flax_values_offset(FLAX_MIN_CAPACITY);
    f->data = flax_malloc(voff + (size_t)FLAX_MIN_CAPACITY * sizeof(void *));
    return f;
}

int flaxInsert(flax *f, int64_t key, void *data, void **old) {
    if (f->numele == f->capacity) {
        int64_t new_cap = f->capacity == 0 ? FLAX_MIN_CAPACITY : f->capacity * 2;
        flax_resize(f, new_cap);
    }

    int64_t idx;
    if (flax_search(flax_keys(f), f->numele, key, &idx)) {
        void **vals = flax_values(f);
        if (old) *old = vals[idx];
        vals[idx] = data;
        return 1;
    }

    int64_t *keys = flax_keys(f);
    void **vals = flax_values(f);
    int64_t tail = f->numele - idx;

    if (tail > 0) {
        memmove(&keys[idx + 1], &keys[idx], (size_t)tail * sizeof(int64_t));
        memmove(&vals[idx + 1], &vals[idx], (size_t)tail * sizeof(void *));
    }

    keys[idx] = key;
    vals[idx] = data;
    f->numele++;
    if (old) *old = NULL;
    return 1;
}

int flaxTryInsert(flax *f, int64_t key, void *data, void **old) {
    if (f->numele == f->capacity) {
        int64_t new_cap = f->capacity == 0 ? FLAX_MIN_CAPACITY : f->capacity * 2;
        flax_resize(f, new_cap);
    }

    int64_t idx;
    if (flax_search(flax_keys(f), f->numele, key, &idx)) {
        if (old) *old = flax_values(f)[idx];
        return 0;
    }

    int64_t *keys = flax_keys(f);
    void **vals = flax_values(f);
    int64_t tail = f->numele - idx;

    if (tail > 0) {
        memmove(&keys[idx + 1], &keys[idx], (size_t)tail * sizeof(int64_t));
        memmove(&vals[idx + 1], &vals[idx], (size_t)tail * sizeof(void *));
    }

    keys[idx] = key;
    vals[idx] = data;
    f->numele++;
    if (old) *old = NULL;
    return 1;
}

int flaxRemove(flax *f, int64_t key, void **old) {
    if (!f || f->numele == 0) {
        if (old) *old = NULL;
        return 0;
    }

    int64_t idx;
    if (!flax_search(flax_keys(f), f->numele, key, &idx)) {
        if (old) *old = NULL;
        return 0;
    }

    int64_t *keys = flax_keys(f);
    void **vals = flax_values(f);
    if (old) *old = vals[idx];
    int64_t tail = f->numele - idx - 1;

    if (tail > 0) {
        memmove(&keys[idx], &keys[idx + 1], (size_t)tail * sizeof(int64_t));
        memmove(&vals[idx], &vals[idx + 1], (size_t)tail * sizeof(void *));
    }

    f->numele--;

    if (f->capacity > FLAX_MIN_CAPACITY &&
        f->numele < f->capacity / 4 &&
        f->capacity / 2 >= FLAX_MIN_CAPACITY) {
        flax_resize(f, f->capacity / 2);
    }

    return 1;
}

int flaxFind(flax *f, int64_t key, void **value) {
    if (!f || f->numele == 0) {
        if (value) *value = NULL;
        return 0;
    }
    int64_t idx;
    if (flax_search(flax_keys(f), f->numele, key, &idx)) {
        if (value) *value = flax_values(f)[idx];
        return 1;
    }
    if (value) *value = NULL;
    return 0;
}

void flaxFree(flax *f) {
    flaxFreeWithCallback(f, NULL);
}

void flaxFreeWithCallback(flax *f, void (*free_callback)(void *)) {
    if (!f) return;
    if (free_callback && f->data && f->numele > 0) {
        void **vals = flax_values(f);
        for (int64_t i = 0; i < f->numele; i++)
            free_callback(vals[i]);
    }
    flax_free(f->data);
    flax_free(f);
}

void flaxFreeWithCbAndContext(flax *f,
                              void (*free_callback)(void *item, void *ctx),
                              void *ctx) {
    if (!f) return;
    if (free_callback && f->data && f->numele > 0) {
        void **vals = flax_values(f);
        for (int64_t i = 0; i < f->numele; i++)
            free_callback(vals[i], ctx);
    }
    flax_free(f->data);
    flax_free(f);
}

uint64_t flaxSize(flax *f) {
    return (uint64_t)f->numele;
}

/* --- Iterator implementation --- */

void flaxStart(flaxIterator *it, flax *f) {
    it->f = f;
    it->idx = -1;
    it->key = 0;
    it->data = NULL;
}

int flaxSeek(flaxIterator *it, const char *op, int64_t key) {
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
        int64_t idx;
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
        int64_t idx;
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
        int64_t idx;
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
        int64_t idx;
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
        int64_t idx;
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

    it->idx = -1;
    it->key = 0;
    it->data = NULL;
    return 0;
}

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

void flaxStop(flaxIterator *it) {
    (void)it;
}

int flaxEOF(flaxIterator *it) {
    return it->idx < 0 || it->idx >= it->f->numele;
}

#ifdef REDIS_TEST
#include "testhelp.h"
#include <assert.h>
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

static void flax_test_counting_free(void *p) {
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
        assert(a->capacity == FLAX_MIN_CAPACITY);
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
        for (int64_t i = 0; i < 100; i++) {
            char *buf = flax_malloc(16);
            snprintf(buf, 16, "v%lld", (long long)i);
            flaxInsert(a, i * 3, buf, NULL);
        }
        assert(flaxSize(a) == 100);
        assert(a->capacity >= 100);

        for (int64_t i = 0; i < 100; i++) {
            char expected[16];
            snprintf(expected, sizeof(expected), "v%lld", (long long)i);
            void *val;
            assert(flaxFind(a, i * 3, &val) == 1);
            if (strcmp(val, expected) != 0) {
                ERR("grow: key %lld expected '%s' got '%s'",
                    (long long)(i * 3), expected, (char *)val);
            }
        }

        flaxFreeWithCallback(a, flax_free);
    }

    TEST("shrink after many removals") {
        flax *a = flaxNew();
        for (int64_t i = 0; i < 64; i++)
            flaxInsert(a, i, "x", NULL);

        assert(flaxSize(a) == 64);
        int64_t cap_before = a->capacity;

        for (int64_t i = 0; i < 56; i++)
            flaxRemove(a, i, NULL);

        assert(flaxSize(a) == 8);
        if (a->capacity >= cap_before) {
            ERR("shrink: capacity %lld should be less than %lld",
                (long long)a->capacity, (long long)cap_before);
        }

        for (int64_t i = 56; i < 64; i++) {
            void *val;
            assert(flaxFind(a, i, &val) == 1);
            assert(strcmp(val, "x") == 0);
        }

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
        flaxFreeWithCallback(a, flax_test_counting_free);
        if (flax_test_free_count != 5) {
            ERR("freeWithCallback: expected 5 frees, got %d",
                flax_test_free_count);
        }
    }

    TEST("flaxFreeWithCallback on empty flax") {
        flax_test_free_count = 0;
        flax *a = flaxNew();
        flaxFreeWithCallback(a, flax_test_counting_free);
        if (flax_test_free_count != 0) {
            ERR("freeWithCallback empty: expected 0 frees, got %d",
                flax_test_free_count);
        }
    }

    TEST("negative keys") {
        flax *a = flaxNew();
        flaxInsert(a, -100, "neg100", NULL);
        flaxInsert(a, 0, "zero", NULL);
        flaxInsert(a, 100, "pos100", NULL);
        flaxInsert(a, -50, "neg50", NULL);

        void *val;
        assert(flaxSize(a) == 4);
        assert(flaxFind(a, -100, &val) == 1);
        assert(strcmp(val, "neg100") == 0);
        assert(flaxFind(a, -50, &val) == 1);
        assert(strcmp(val, "neg50") == 0);
        assert(flaxFind(a, 0, &val) == 1);
        assert(strcmp(val, "zero") == 0);
        assert(flaxFind(a, 100, &val) == 1);
        assert(strcmp(val, "pos100") == 0);

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

    TEST("flaxFreeWithCbAndContext") {
        int ctx_free_count = 0;
        flax *a = flaxNew();
        flaxInsert(a, 1, "one", NULL);
        flaxInsert(a, 2, "two", NULL);
        flaxInsert(a, 3, "three", NULL);
        flaxFreeWithCbAndContext(a, flax_test_ctx_free, &ctx_free_count);
        if (ctx_free_count != 3) {
            ERR("freeWithCbAndContext: expected 3 frees, got %d",
                ctx_free_count);
        }
    }

    if (!err)
        printf("ALL TESTS PASSED!\n");
    else
        ERR("Sorry, not all tests passed! In fact, %d tests failed.", err);

    return err;
}

#endif
