/*
 * Copyright Redis Ltd. 2024 - present
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Indexed binary min-heap. See iheap.h for the contract.
 *
 * Every sift uses the hole technique: the entry being placed is carried in a
 * local while parents (or the smaller child) drop into the hole one by one,
 * each getting a setSlot call for its new slot; the carried entry is written
 * once at the end. That makes the setSlot call count equal to the number of
 * entries that moved, plus one for the placed entry when it moved.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h> /* abort(), for redisassert.h's assert() under clang */
#include "zmalloc.h"
#include "redisassert.h"
#include "iheap.h"

/* The array is an implicit binary tree: the children of slot i are 2i+1 and
 * 2i+2, its parent is (i-1)/2. The child macros widen to 64 bits so a slot
 * near UINT32_MAX cannot wrap; callers compare the result with the count
 * before indexing with it. */
#define IHEAP_PARENT(i) (((i) - 1) / 2)
#define IHEAP_LEFT(i) ((uint64_t)(i) * 2 + 1)
#define IHEAP_RIGHT(i) ((uint64_t)(i) * 2 + 2)

/* Heaps created with a NULL type point here, so the hot path has a single
 * NULL check per move, on the callback, and none on the type. */
static const iheapType iheapNoCallbacks = { NULL, NULL };

/* The largest capacity whose allocation size fits in size_t and whose value
 * fits the 32-bit capacity field. On 64-bit targets the field is the limit;
 * on 32-bit ones the arithmetic is, at about 357 million entries, far more
 * than such a process could hold, so this is a guard against a caller's
 * mistake, not a working limit. */
#define IHEAP_MAX_CAPACITY \
    ((SIZE_MAX - sizeof(iheap)) / sizeof(iheapEntry) < UINT32_MAX ? \
     (SIZE_MAX - sizeof(iheap)) / sizeof(iheapEntry) : (size_t)UINT32_MAX)

/* In a function of its own so that a 64-bit build, where any uint32_t
 * capacity fits, does not warn that the comparison is always true. */
static inline int iheapCapacityFits(size_t capacity) {
    return capacity <= IHEAP_MAX_CAPACITY;
}

/* Tell the embedder where the entry at 'slot' now sits, if it asked. */
static inline void iheapSetSlot(iheap *h, uint32_t slot) {
    if (h->type->setSlot) h->type->setSlot(h->e[slot].item, slot);
}

/* (Re)allocate 'h' (NULL for a new heap) for 'capacity' entries. Any slack
 * the allocator hands back becomes capacity too. */
static iheap *iheapRealloc(iheap *h, size_t capacity) {
    size_t usable;
    h = zrealloc_usable(h, sizeof(iheap) + capacity * sizeof(iheapEntry),
                        &usable, NULL);
    capacity = (usable - sizeof(iheap)) / sizeof(iheapEntry);
    if (capacity > IHEAP_MAX_CAPACITY) capacity = IHEAP_MAX_CAPACITY;
    h->capacity = (uint32_t)capacity;
    return h;
}

/* Double the capacity, up to the limit. The current capacity is below the
 * limit, so the doubling cannot wrap size_t. */
static iheap *iheapGrow(iheap *h) {
    assert(h->capacity < IHEAP_MAX_CAPACITY);
    size_t capacity = (size_t)h->capacity * 2;
    if (capacity > IHEAP_MAX_CAPACITY) capacity = IHEAP_MAX_CAPACITY;
    return iheapRealloc(h, capacity);
}

/* Move the hole at 'i' towards the root until 'ent' fits, write 'ent' there
 * and return its slot. Entries that drop into the hole get a setSlot call;
 * the placed entry does not, the caller decides whether it moved. */
static uint32_t iheapSiftUp(iheap *h, uint32_t i, iheapEntry ent) {
    while (i > 0) {
        uint32_t parent = IHEAP_PARENT(i);
        if (h->e[parent].key <= ent.key) break;
        h->e[i] = h->e[parent];
        iheapSetSlot(h, i);
        i = parent;
    }
    h->e[i] = ent;
    return i;
}

/* The same towards the leaves: the smaller child rises into the hole while
 * it is smaller than 'ent'. */
static uint32_t iheapSiftDown(iheap *h, uint32_t i, iheapEntry ent) {
    uint64_t n = h->count;
    for (;;) {
        uint64_t child = IHEAP_LEFT(i);
        if (child >= n) break;
        if (IHEAP_RIGHT(i) < n && h->e[IHEAP_RIGHT(i)].key < h->e[child].key)
            child = IHEAP_RIGHT(i);
        if (ent.key <= h->e[child].key) break;
        h->e[i] = h->e[child];
        iheapSetSlot(h, i);
        i = (uint32_t)child;
    }
    h->e[i] = ent;
    return i;
}

iheap *iheapNew(const iheapType *type, uint32_t capacity) {
    assert(iheapCapacityFits(capacity));
    iheap *h = iheapRealloc(NULL, capacity ? capacity : 1);
    h->type = type ? type : &iheapNoCallbacks;
    h->count = 0;
    return h;
}

void iheapFree(iheap *h) {
    zfree(h);
}

void iheapInsert(iheap **hp, uint64_t key, void *item) {
    iheap *h = *hp;
    if (h->count == h->capacity) h = *hp = iheapGrow(h);
    iheapEntry ent = { key, item };
    uint32_t slot = iheapSiftUp(h, h->count++, ent);
    iheapSetSlot(h, slot);
}

void iheapUpdate(iheap *h, uint32_t slot, uint64_t key) {
    debugAssert(h->type->setSlot && slot < h->count);
    iheapEntry ent = h->e[slot];
    uint64_t old = ent.key;
    ent.key = key;
    uint32_t to = key < old ? iheapSiftUp(h, slot, ent)
                            : iheapSiftDown(h, slot, ent);
    if (to != slot) iheapSetSlot(h, to);
}

void iheapRemove(iheap *h, uint32_t slot) {
    debugAssert(h->type->setSlot && slot < h->count);
    uint32_t last = --h->count;
    if (slot == last) return;
    /* The last entry fills the hole. It may belong above the hole, when it
     * is smaller than the removed key, or below it otherwise. */
    iheapEntry ent = h->e[last];
    uint32_t to = ent.key < h->e[slot].key ? iheapSiftUp(h, slot, ent)
                                           : iheapSiftDown(h, slot, ent);
    iheapSetSlot(h, to);
}

int iheapMin(const iheap *h, uint64_t *key, void **item) {
    if (h->count == 0) return 0;
    if (key) *key = h->e[0].key;
    if (item) *item = h->e[0].item;
    return 1;
}

int iheapPop(iheap *h, uint64_t *key, void **item) {
    if (!iheapMin(h, key, item)) return 0;
    uint32_t last = --h->count;
    if (last != 0) iheapSetSlot(h, iheapSiftDown(h, 0, h->e[last]));
    return 1;
}

void iheapAppend(iheap **hp, uint64_t key, void *item) {
    iheap *h = *hp;
    if (h->count == h->capacity) h = *hp = iheapGrow(h);
    uint32_t slot = h->count++;
    h->e[slot].key = key;
    h->e[slot].item = item;
    iheapSetSlot(h, slot);
}

void iheapBuild(iheap *h) {
    if (h->count < 2) return;
    /* Floyd: sift down every internal node, deepest first. The last internal
     * node is the parent of the last entry. */
    for (uint32_t i = IHEAP_PARENT(h->count - 1) + 1; i-- > 0;) {
        uint32_t to = iheapSiftDown(h, i, h->e[i]);
        if (to != i) iheapSetSlot(h, to);
    }
}

int iheapValidate(const iheap *h) {
    if (h->count > h->capacity) return 0;
    if (zmalloc_usable_size((void*)h) < iheapAllocSize(h)) return 0;
    for (uint32_t i = 0; i < h->count; i++) {
        if (i > 0 && h->e[IHEAP_PARENT(i)].key > h->e[i].key) return 0;
        if (h->type->getSlot && h->type->getSlot(h->e[i].item) != i)
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------
 * Unit test: ./redis-server test iheap [--accurate] [--valgrind]
 * ---------------------------------------------------------------------- */
#ifdef REDIS_TEST
#include <stdio.h>
#include <inttypes.h>
#include "testhelp.h"

#define UNUSED(x) (void)(x)
#define TEST(name) printf("test — %s\n", name);

/* The test item records what the heap told it, so the model can check it
 * against the heap's array. */
typedef struct testItem {
    uint64_t key;   /* key the model believes the heap holds for the item */
    uint32_t slot;  /* last slot reported through setSlot */
    uint32_t pos;   /* position in testModel.order[] */
} testItem;

static uint64_t testSetSlotCalls;

static void testSetSlot(void *item, uint32_t slot) {
    ((testItem*)item)->slot = slot;
    testSetSlotCalls++;
}

static uint32_t testGetSlot(void *item) {
    return ((testItem*)item)->slot;
}

static const iheapType testType = { testSetSlot, testGetSlot };

/* xorshift64*, seeded per case so every run is reproducible. */
static uint64_t testRngState = 1;

static uint64_t testRand(void) {
    uint64_t x = testRngState;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    testRngState = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static uint64_t testRandBelow(uint64_t n) {
    return testRand() % n;
}

/* floor(log2(n)) + 1: the setSlot call bound for one operation on a heap of
 * n entries (n counted after an insert, before an update or a remove). */
static uint64_t testCallBound(uint64_t n) {
    uint64_t bound = 1;
    while (n > 1) {
        n >>= 1;
        bound++;
    }
    return bound;
}

/* Reference model: a pool of items, of which the first 'nlive' entries of
 * order[] are in the heap. The minimum is found by a linear scan. */
typedef struct testModel {
    testItem *items;
    uint32_t *order;
    uint32_t nitems;
    uint32_t nlive;
} testModel;

static void testModelInit(testModel *m, uint32_t n) {
    m->items = zcalloc(sizeof(testItem) * n);
    m->order = zmalloc(sizeof(uint32_t) * n);
    for (uint32_t i = 0; i < n; i++) {
        m->order[i] = i;
        m->items[i].pos = i;
    }
    m->nitems = n;
    m->nlive = 0;
}

static void testModelFree(testModel *m) {
    zfree(m->items);
    zfree(m->order);
}

static testItem *testModelAdd(testModel *m, uint64_t key) {
    testItem *it = &m->items[m->order[m->nlive++]];
    it->key = key;
    return it;
}

static void testModelDel(testModel *m, testItem *it) {
    uint32_t p = it->pos, q = --m->nlive;
    uint32_t other = m->order[q];
    m->order[p] = other;
    m->items[other].pos = p;
    m->order[q] = (uint32_t)(it - m->items);
    it->pos = q;
}

static testItem *testModelRandomLive(testModel *m) {
    return &m->items[m->order[testRandBelow(m->nlive)]];
}

static int testModelMin(testModel *m, uint64_t *key) {
    if (m->nlive == 0) return 0;
    uint64_t min = UINT64_MAX;
    for (uint32_t i = 0; i < m->nlive; i++) {
        uint64_t k = m->items[m->order[i]].key;
        if (k < min) min = k;
    }
    *key = min;
    return 1;
}

/* The item's reported slot must point back at it with the key the model
 * holds. */
static int testItemConsistent(iheap *h, testItem *it) {
    return it->slot < h->count && h->e[it->slot].item == it &&
           h->e[it->slot].key == it->key;
}

/* Full check: invariants, count, and the minimum against the model. */
static int testCheck(iheap *h, testModel *m) {
    uint64_t key, mkey;
    void *item;
    if (!iheapValidate(h)) return 0;
    if (iheapCount(h) != m->nlive) return 0;
    int has = iheapMin(h, &key, &item);
    if (has != testModelMin(m, &mkey)) return 0;
    if (!has) return 1;
    testItem *it = item;
    return key == mkey && it->pos < m->nlive && testItemConsistent(h, it);
}

/* Insert keys 0..n-1 in order: ascending inserts never move an entry, so
 * key i sits at slot i and the array is a complete tree we can reason
 * about. */
static void testFillAscending(iheap **hp, testModel *m, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        testItem *it = testModelAdd(m, i);
        iheapInsert(hp, i, it);
    }
}

/* Random operations against the model. Full checks run every 'checkEvery'
 * operations; the touched item and the setSlot call bound are checked on
 * every operation. Returns 1 on success. */
static int testFuzz(const char *name, uint64_t seed, uint32_t maxEntries,
                    uint64_t ops, uint64_t checkEvery) {
    testModel m;
    testModelInit(&m, maxEntries);
    iheap *h = iheapNew(&testType, 1);
    testRngState = seed;
    const char *err = NULL;
    uint64_t op;

    for (op = 0; op < ops && !err; op++) {
        /* update larger 50%, update smaller 10%, insert 20%, remove 15%,
         * set key to UINT64_MAX 5%. */
        uint64_t r = testRandBelow(100);
        int kind = r < 50 ? 0 : r < 60 ? 1 : r < 80 ? 2 : r < 95 ? 3 : 4;
        if (m.nlive == 0) kind = 2;
        else if (m.nlive == m.nitems && kind == 2) kind = 0;

        uint32_t n = iheapCount(h);
        uint64_t bound, key;
        testItem *it;
        testSetSlotCalls = 0;
        switch (kind) {
        case 0:
        case 1:
        case 4:
            it = testModelRandomLive(&m);
            key = it->key;
            if (kind == 4) {
                key = UINT64_MAX;
            } else if (key == UINT64_MAX) {
                key = testRandBelow(1 << 20);
            } else if (kind == 0) {
                key += 1 + testRandBelow(1000);
            } else {
                key -= key < 1000 ? testRandBelow(key + 1)
                                  : 1 + testRandBelow(1000);
            }
            it->key = key;
            iheapUpdate(h, it->slot, key);
            bound = testCallBound(n);
            break;
        case 2:
            it = testModelAdd(&m, testRandBelow(1 << 20));
            iheapInsert(&h, it->key, it);
            bound = testCallBound(n + 1);
            break;
        default:
            it = testModelRandomLive(&m);
            iheapRemove(h, it->slot);
            testModelDel(&m, it);
            bound = testCallBound(n);
            it = NULL;
            break;
        }
        if (testSetSlotCalls > bound)
            err = "setSlot call bound exceeded";
        else if (it && !testItemConsistent(h, it))
            err = "touched item is not where its slot says";
        else if (op % checkEvery == 0 && !testCheck(h, &m))
            err = "heap disagrees with the model";
    }
    if (!err && !testCheck(h, &m)) err = "final check failed";

    /* Drain through remove, in random order, down to the empty heap, with
     * the same check cadence. */
    for (uint64_t i = 0; !err && m.nlive > 0; i++) {
        testItem *it = testModelRandomLive(&m);
        iheapRemove(h, it->slot);
        testModelDel(&m, it);
        if (i % checkEvery == 0 && !testCheck(h, &m))
            err = "drain check failed";
    }
    if (!err && !testCheck(h, &m)) err = "empty heap check failed";
    if (err) {
        printf("iheap fuzz '%s' failed at operation %" PRIu64
               " (seed %" PRIx64 "): %s\n", name, op, seed, err);
    }
    iheapFree(h);
    testModelFree(&m);
    return err == NULL;
}

/* Insert and pop only, on a heap without callbacks, against a multiset of
 * keys. The item is the key itself, so pop can be checked to return the
 * matching pair. Returns 1 on success. */
static int testQueueFuzz(uint64_t seed, uint32_t maxEntries, uint64_t ops) {
    uint64_t *keys = zmalloc(sizeof(uint64_t) * maxEntries);
    uint32_t n = 0;
    iheap *h = iheapNew(NULL, 4);
    testRngState = seed;
    const char *err = NULL;
    uint64_t op;

    for (op = 0; op < ops && !err; op++) {
        if (n == 0 || (n < maxEntries && testRandBelow(100) < 55)) {
            uint64_t key = testRandBelow(1 << 16);
            keys[n++] = key;
            iheapInsert(&h, key, (void*)(uintptr_t)key);
        } else {
            uint32_t j = 0;
            for (uint32_t i = 1; i < n; i++)
                if (keys[i] < keys[j]) j = i;
            uint64_t key;
            void *item;
            if (!iheapPop(h, &key, &item) || key != keys[j] ||
                item != (void*)(uintptr_t)key)
                err = "pop disagrees with the model";
            keys[j] = keys[--n];
        }
        if (!err && (!iheapValidate(h) || iheapCount(h) != n))
            err = "heap invalid or count differs";
    }

    /* Drain: popped keys must be non-decreasing and the heap must empty. */
    uint64_t prev = 0, key;
    while (!err && iheapPop(h, &key, NULL)) {
        if (key < prev) err = "drain is not sorted";
        prev = key;
        n--;
    }
    if (!err && (n != 0 || iheapMin(h, NULL, NULL)))
        err = "heap did not drain to empty";
    if (err) {
        printf("iheap queue fuzz failed at operation %" PRIu64
               " (seed %" PRIx64 "): %s\n", op, seed, err);
    }
    iheapFree(h);
    zfree(keys);
    return err == NULL;
}

int iheapTest(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    int accurate = flags & REDIS_TEST_ACCURATE;
    /* Valgrind is slow; the fuzz regimes run a tenth of their operations. */
    uint64_t scale = (flags & REDIS_TEST_VALGRIND) ? 10 : 1;
    int ok;

    TEST("empty heap") {
        iheap *h = iheapNew(&testType, 0);
        uint64_t key = 1;
        void *item = (void*)1;
        ok = iheapCount(h) == 0 && h->capacity >= 1 &&
             !iheapMin(h, &key, &item) && !iheapPop(h, &key, &item) &&
             key == 1 && item == (void*)1 && iheapValidate(h);
        test_cond("empty heap: count 0, min and pop report empty", ok);
        iheapFree(h);
    }

    TEST("one entry") {
        testModel m;
        testModelInit(&m, 1);
        iheap *h = iheapNew(&testType, 1);
        testItem *it = testModelAdd(&m, 10);
        testSetSlotCalls = 0;
        iheapInsert(&h, 10, it);
        ok = testSetSlotCalls == 1 && it->slot == 0 && testCheck(h, &m);
        testSetSlotCalls = 0;
        it->key = 20;
        iheapUpdate(h, it->slot, 20);
        ok = ok && testSetSlotCalls == 0 && testCheck(h, &m);
        it->key = 5;
        iheapUpdate(h, it->slot, 5);
        ok = ok && testSetSlotCalls == 0 && testCheck(h, &m);
        iheapUpdate(h, it->slot, 5);
        ok = ok && testSetSlotCalls == 0 && testCheck(h, &m);
        iheapRemove(h, it->slot);
        testModelDel(&m, it);
        ok = ok && testSetSlotCalls == 0 && testCheck(h, &m);
        test_cond("one entry: insert, update larger/smaller/equal, remove",
                  ok);
        iheapFree(h);
        testModelFree(&m);
    }

    TEST("insert in ascending, descending and random order") {
        ok = 1;
        for (uint32_t n = 1; n <= 64 && ok; n++) {
            for (int order = 0; order < 3 && ok; order++) {
                testModel m;
                testModelInit(&m, n);
                iheap *h = iheapNew(&testType, 1);
                testRngState = 1000 + n;
                for (uint32_t i = 0; i < n && ok; i++) {
                    uint64_t key = order == 0 ? i :
                                   order == 1 ? n - i : testRandBelow(n);
                    testItem *it = testModelAdd(&m, key);
                    testSetSlotCalls = 0;
                    iheapInsert(&h, key, it);
                    /* Ascending never moves anything; descending always
                     * climbs to the root, hitting the bound exactly. */
                    uint64_t bound = testCallBound(i + 1);
                    ok = testCheck(h, &m) &&
                         (order == 0 ? testSetSlotCalls == 1 :
                          order == 1 ? testSetSlotCalls == bound :
                          testSetSlotCalls <= bound);
                }
                iheapFree(h);
                testModelFree(&m);
            }
        }
        test_cond("inserts in every order of 1 to 64 entries validate", ok);
    }

    TEST("update: stays, sinks, rises") {
        testModel m;
        testModelInit(&m, 15);
        iheap *h = iheapNew(&testType, 1);
        testFillAscending(&h, &m, 15);
        /* Slot 3 (key 3) has parent key 1 and children keys 7 and 8:
         * any key in 1..7 leaves it in place, ties included. */
        testItem *it = h->e[3].item;
        testSetSlotCalls = 0;
        it->key = 1;
        iheapUpdate(h, 3, 1);
        ok = testSetSlotCalls == 0 && it->slot == 3 && testCheck(h, &m);
        it->key = 7;
        iheapUpdate(h, 3, 7);
        ok = ok && testSetSlotCalls == 0 && it->slot == 3 &&
             testCheck(h, &m);
        it->key = 6;
        iheapUpdate(h, 3, 6);
        ok = ok && testSetSlotCalls == 0 && it->slot == 3 &&
             h->e[3].key == 6 && testCheck(h, &m);
        /* A leaf set to its own key is the no-op case. */
        it = h->e[14].item;
        iheapUpdate(h, 14, it->key);
        ok = ok && testSetSlotCalls == 0 && testCheck(h, &m);
        /* The root raised above everything sinks to a leaf: three moves
         * plus its own placement, the bound for 15 entries. */
        it = h->e[0].item;
        it->key = 100;
        iheapUpdate(h, 0, 100);
        ok = ok && testSetSlotCalls == 4 && it->slot >= 7 &&
             testCheck(h, &m);
        /* The deepest leaf lowered below everything rises to the root. */
        testSetSlotCalls = 0;
        it = h->e[14].item;
        it->key = 0;
        iheapUpdate(h, 14, 0);
        ok = ok && testSetSlotCalls == 4 && it->slot == 0 &&
             testCheck(h, &m);
        test_cond("update in place makes no call, sinks and rises are "
                  "bounded", ok);
        iheapFree(h);
        testModelFree(&m);
    }

    TEST("remove: root, interior, leaf, last, only") {
        testModel m;
        testModelInit(&m, 15);
        iheap *h = iheapNew(&testType, 1);
        testFillAscending(&h, &m, 15);
        uint32_t slots[] = { 0, 1, 9, 11 }; /* root, interior, leaf, last */
        ok = 1;
        for (int i = 0; i < 4 && ok; i++) {
            testItem *it = h->e[slots[i]].item;
            uint32_t n = iheapCount(h);
            testSetSlotCalls = 0;
            iheapRemove(h, slots[i]);
            testModelDel(&m, it);
            ok = testSetSlotCalls <= testCallBound(n) && testCheck(h, &m);
            /* Removing the last slot moves nothing. */
            if (slots[i] == n - 1) ok = ok && testSetSlotCalls == 0;
        }
        while (ok && iheapCount(h) > 1) {
            testItem *it = h->e[iheapCount(h) / 2].item;
            iheapRemove(h, it->slot);
            testModelDel(&m, it);
            ok = testCheck(h, &m);
        }
        testItem *it = h->e[0].item;
        testSetSlotCalls = 0;
        iheapRemove(h, 0);
        testModelDel(&m, it);
        ok = ok && testSetSlotCalls == 0 && iheapCount(h) == 0 &&
             testCheck(h, &m);
        test_cond("removes at every position keep the heap valid", ok);
        iheapFree(h);
        testModelFree(&m);
    }

    TEST("equal keys") {
        testModel m;
        testModelInit(&m, 12);
        iheap *h = iheapNew(&testType, 1);
        for (int i = 0; i < 12; i++) {
            uint64_t key = i < 2 ? 41 : i < 10 ? 42 : 43;
            iheapInsert(&h, key, testModelAdd(&m, key));
        }
        ok = testCheck(h, &m);
        /* Remove the two 41s: one of the 42s must take the root. */
        for (int i = 0; i < 2 && ok; i++) {
            testItem *it = h->e[0].item;
            iheapRemove(h, 0);
            testModelDel(&m, it);
            ok = testCheck(h, &m) &&
                 h->e[0].key == (uint64_t)(i == 0 ? 41 : 42);
        }
        /* Remove an interior 42, then update another 42 to 42. */
        testItem *it = h->e[3].item;
        iheapRemove(h, 3);
        testModelDel(&m, it);
        ok = ok && testCheck(h, &m);
        it = h->e[2].item;
        testSetSlotCalls = 0;
        iheapUpdate(h, 2, it->key);
        ok = ok && testSetSlotCalls == 0 && testCheck(h, &m);
        test_cond("equal keys: removes and updates keep every slot right",
                  ok);
        iheapFree(h);
        testModelFree(&m);
    }

    TEST("UINT64_MAX keys sort last") {
        testModel m;
        testModelInit(&m, 6);
        iheap *h = iheapNew(&testType, 1);
        for (int i = 0; i < 5; i++)
            iheapInsert(&h, UINT64_MAX, testModelAdd(&m, UINT64_MAX));
        uint64_t key;
        void *item;
        ok = iheapMin(h, &key, &item) && key == UINT64_MAX &&
             testCheck(h, &m);
        testItem *it = testModelAdd(&m, 7);
        iheapInsert(&h, 7, it);
        ok = ok && it->slot == 0 && iheapMin(h, &key, NULL) && key == 7 &&
             testCheck(h, &m);
        it->key = UINT64_MAX;
        iheapUpdate(h, 0, UINT64_MAX);
        ok = ok && iheapMin(h, &key, NULL) && key == UINT64_MAX &&
             testCheck(h, &m);
        it = h->e[4].item;
        it->key = 3;
        iheapUpdate(h, 4, 3);
        ok = ok && it->slot == 0 && iheapMin(h, &key, &item) && key == 3 &&
             item == it && testCheck(h, &m);
        test_cond("UINT64_MAX keys never hide a smaller key", ok);
        iheapFree(h);
        testModelFree(&m);
    }

    TEST("NULL items") {
        iheap *h = iheapNew(NULL, 1);
        uint64_t key;
        void *item = (void*)1;
        iheapInsert(&h, 5, NULL);
        iheapInsert(&h, 3, NULL);
        ok = iheapValidate(h) && iheapMin(h, &key, &item) && key == 3 &&
             item == NULL;
        item = (void*)1;
        ok = ok && iheapPop(h, &key, &item) && key == 3 && item == NULL;
        ok = ok && iheapPop(h, &key, &item) && key == 5 && item == NULL;
        ok = ok && !iheapPop(h, &key, &item) && iheapCount(h) == 0;
        test_cond("NULL items are stored and returned", ok);
        iheapFree(h);
    }

    TEST("priority queue without callbacks") {
        static const iheapType noCallbacks = { NULL, NULL };
        const iheapType *types[] = { NULL, &noCallbacks };
        ok = 1;
        for (int t = 0; t < 2 && ok; t++) {
            iheap *h = iheapNew(types[t], 1);
            testRngState = 77 + t;
            for (int i = 0; i < 1000 && ok; i++) {
                uint64_t key = testRandBelow(500);
                iheapInsert(&h, key, (void*)(uintptr_t)key);
                ok = iheapValidate(h);
            }
            uint64_t prev = 0, key;
            void *item;
            int n = 0;
            while (ok && iheapPop(h, &key, &item)) {
                ok = key >= prev && item == (void*)(uintptr_t)key &&
                     iheapValidate(h);
                prev = key;
                n++;
            }
            ok = ok && n == 1000 && iheapCount(h) == 0;
            iheapFree(h);
        }
        test_cond("insert and pop without callbacks drain in order", ok);
    }

    TEST("growth across capacity boundaries") {
        testModel m;
        testModelInit(&m, 4096);
        iheap *h = iheapNew(&testType, 1);
        testRngState = 4096;
        int moved = 0, grows = 0;
        ok = 1;
        for (uint32_t i = 0; i < 4096 && ok; i++) {
            iheap *before = h;
            uint32_t capacity = h->capacity;
            int full = h->count == capacity;
            uint64_t key = testRandBelow(1 << 20);
            iheapInsert(&h, key, testModelAdd(&m, key));
            if (full) {
                grows++;
                if (h != before) moved = 1;
                ok = h->capacity > capacity;
            } else {
                ok = h == before && h->capacity == capacity;
            }
            ok = ok && testCheck(h, &m);
        }
        /* Doubling from 1 reaches 4096 in 12 steps; allocator slack can
         * only make that fewer. The array must have moved at least once. */
        ok = ok && grows >= 1 && grows <= 12 && moved && h->capacity >= 4096;
        test_cond("growth keeps every slot right and tracks *h", ok);
        iheapFree(h);
        testModelFree(&m);
    }

    TEST("append and build") {
        uint32_t sizes[] = { 1, 2, 3, 7, 8, 1000, 4097 };
        ok = 1;
        for (int s = 0; s < 7 && ok; s++) {
            uint32_t n = sizes[s];
            testModel m;
            testModelInit(&m, n);
            iheap *h = iheapNew(&testType, 1);
            testRngState = 500 + n;
            for (uint32_t i = 0; i < n && ok; i++) {
                testItem *it = testModelAdd(&m, testRandBelow(n * 4));
                testSetSlotCalls = 0;
                iheapAppend(&h, it->key, it);
                ok = testSetSlotCalls == 1 && it->slot == i;
            }
            testSetSlotCalls = 0;
            iheapBuild(h);
            ok = ok && testSetSlotCalls <= 2 * (uint64_t)n &&
                 testCheck(h, &m);
            iheapFree(h);
            testModelFree(&m);
        }
        test_cond("append then build yields a valid heap within 2n calls",
                  ok);
    }

    TEST("alloc size") {
        uint32_t capacities[] = { 1, 2, 3, 5, 8, 100, 1000 };
        ok = 1;
        for (int c = 0; c < 7 && ok; c++) {
            iheap *h = iheapNew(&testType, capacities[c]);
            size_t usable = zmalloc_usable_size(h);
            ok = h->capacity >= capacities[c] &&
                 iheapAllocSize(h) <= usable &&
                 usable - iheapAllocSize(h) < sizeof(iheapEntry);
            iheapFree(h);
        }
        test_cond("alloc size covers the allocation to within one entry",
                  ok);
    }

    TEST("fuzz: small heaps") {
        test_cond("small heaps agree with the model after every operation",
                  testFuzz("small", 0x5EED0001, 16, 200000 / scale, 1));
    }

    TEST("fuzz: medium heaps") {
        test_cond("medium heaps agree with the model after every operation",
                  testFuzz("medium", 0x5EED0002, 1000, 200000 / scale, 1));
    }

    TEST("fuzz: priority queue") {
        test_cond("queue mode agrees with the model",
                  testQueueFuzz(0x5EED0003, 1000, 200000 / scale));
    }

    if (accurate) {
        TEST("fuzz: large heaps") {
            test_cond("large heaps hold the call bound at depth",
                      testFuzz("large", 0x5EED0004, 100000,
                               2000000 / scale, 1024));
        }
    }

    return 0;
}
#endif
