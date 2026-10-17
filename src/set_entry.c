/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* Hashtable-encoded set member with an optional expiration. See set_entry.h. */

#include "server.h"
#include "redisassert.h"
#include "set_entry.h"

/* SDS aux bit. If set, the entry has an ExpireMeta located right before the
 * member sds. */
#define SET_ENTRY_AUX_BIT_HAS_EXPIRY 0

int sEntryHasExpiry(const SetEntry *entry) {
    return sdsGetAuxBit(sEntryGetMember(entry), SET_ENTRY_AUX_BIT_HAS_EXPIRY);
}

void *sEntryGetAllocPtr(const SetEntry *entry) {
    char *buf = sdsAllocPtr(sEntryGetMember(entry));
    if (sEntryHasExpiry(entry)) buf -= sizeof(ExpireMeta);
    return buf;
}

ExpireMeta *sEntryRefExpiryMeta(SetEntry *entry) {
    return sEntryHasExpiry(entry) ? (ExpireMeta *)sEntryGetAllocPtr(entry) : NULL;
}

uint64_t sEntryGetExpiry(const SetEntry *entry) {
    if (!sEntryHasExpiry(entry))
        return EB_EXPIRE_TIME_INVALID;

    ExpireMeta *expireMeta = (ExpireMeta *)sEntryGetAllocPtr(entry);
    if (expireMeta->trash)
        return EB_EXPIRE_TIME_INVALID;

    return ebGetMetaExpTime(expireMeta);
}

int sEntryIsExpired(const SetEntry *entry) {
    return setTypeExpireTimeElapsed(sEntryGetExpiry(entry));
}

size_t sEntryMemUsage(const SetEntry *entry) {
    size_t size = sdsAllocSize(sEntryGetMember(entry));
    if (sEntryHasExpiry(entry)) size += sizeof(ExpireMeta);
    return size;
}

/* Allocates an entry with an ExpireMeta and a copy of the member. */
static SetEntry *sEntryCreateWithExpiry(const char *member, size_t len) {
    /* SDS_TYPE_5 has no aux bits, so the member needs at least an SDS_TYPE_8
     * header to record the presence of the ExpireMeta. */
    char type = sdsReqType(len);
    if (type == SDS_TYPE_5) type = SDS_TYPE_8;
    size_t memberSize = sdsReqSize(len, type);

    char *buf = zmalloc(sizeof(ExpireMeta) + memberSize);

    /* Not registered in any ebuckets tree yet, which is what trash means. */
    ExpireMeta *expireMeta = (ExpireMeta *)buf;
    memset(expireMeta, 0, sizeof(*expireMeta));
    expireMeta->trash = 1;

    sds s = sdsnewplacement(buf + sizeof(ExpireMeta), memberSize, type, member, len);
    sdsSetAuxBit(s, SET_ENTRY_AUX_BIT_HAS_EXPIRY, 1);
    debugServerAssert(sEntryHasExpiry((SetEntry *)s));
    return (SetEntry *)s;
}

SetEntry *sEntryCreate(const char *member, size_t len, int withExpiry) {
    if (!withExpiry)
        return (SetEntry *)sdsnewlen(member, len);
    return sEntryCreateWithExpiry(member, len);
}

SetEntry *sEntryAddExpiry(SetEntry *entry, ssize_t *usableDiff) {
    serverAssert(!sEntryHasExpiry(entry));
    sds member = sEntryGetMember(entry);
    size_t oldUsable = sEntryMemUsage(entry);

    SetEntry *newEntry = sEntryCreateWithExpiry(member, sdslen(member));
    sdsfree(member);

    if (usableDiff)
        *usableDiff = (ssize_t)sEntryMemUsage(newEntry) - (ssize_t)oldUsable;
    return newEntry;
}

SetEntry *sEntryRemoveExpiry(SetEntry *entry, ssize_t *usableDiff) {
    serverAssert(sEntryHasExpiry(entry));
    /* The entry must not be linked in an ebuckets tree, otherwise freeing it
     * would leave the tree pointing at freed memory. */
    serverAssert(sEntryRefExpiryMeta(entry)->trash);
    sds member = sEntryGetMember(entry);
    size_t oldUsable = sEntryMemUsage(entry);

    SetEntry *newEntry = (SetEntry *)sdsnewlen(member, sdslen(member));
    zfree(sEntryGetAllocPtr(entry));

    if (usableDiff)
        *usableDiff = (ssize_t)sEntryMemUsage(newEntry) - (ssize_t)oldUsable;
    return newEntry;
}

void sEntryFree(SetEntry *entry, size_t *usable) {
    if (usable) *usable = sEntryMemUsage(entry);

    if (sEntryHasExpiry(entry))
        zfree(sEntryGetAllocPtr(entry));
    else
        sdsfree(sEntryGetMember(entry));
}

SetEntry *sEntryDefrag(SetEntry *entry, void *(*defragfn)(void *), sds (*sdsdefragfn)(sds)) {
    if (!sEntryHasExpiry(entry))
        return (SetEntry *)sdsdefragfn(sEntryGetMember(entry));

    char *allocation = sEntryGetAllocPtr(entry);
    char *newAllocation = defragfn(allocation);
    if (newAllocation != NULL) {
        /* Return the same offset into the new allocation as the entry's offset
         * in the old allocation. */
        ptrdiff_t entryPointerOffset = (char *)entry - allocation;
        return (SetEntry *)(newAllocation + entryPointerOffset);
    }
    return NULL;
}

void sEntryDismissMemory(SetEntry *entry) {
    if (sEntryHasExpiry(entry))
        dismissMemory(sEntryGetAllocPtr(entry), sEntryMemUsage(entry));
    else
        dismissSds(sEntryGetMember(entry));
}

/* ------------------------------ Unit tests -------------------------------- */

#ifdef REDIS_TEST
#include <stdio.h>
#include "testhelp.h"

#define TEST(name) printf("test — %s\n", name);

static ExpireMeta *setEntryTestGetMeta(const eItem item) {
    return sEntryRefExpiryMeta((SetEntry *)item);
}

static EbucketsType setEntryTestBucketsType = {
    .getExpireMeta = setEntryTestGetMeta,
    .onDeleteItem = NULL,
    .itemsAddrAreOdd = 1,
};

/* A defrag function that always moves the allocation. */
static void *testDefragMove(void *ptr) {
    size_t sz = zmalloc_usable_size(ptr);
    void *p = zmalloc(sz);
    memcpy(p, ptr, sz);
    zfree(ptr);
    return p;
}

static void *testDefragNoMove(void *ptr) {
    UNUSED(ptr);
    return NULL;
}

/* An sds defrag function that always moves the sds. */
static sds testSdsDefragMove(sds s) {
    sds n = sdsdup(s);
    sdsfree(s);
    return n;
}

static sds testSdsDefragNoMove(sds s) {
    UNUSED(s);
    return NULL;
}

/* Collects the result of many checks, so each test section reports once. */
#define CHECK(c) do { if (!(c)) { ok = 0; printf("  check failed: %s (line %d)\n", #c, __LINE__); } } while (0)

static void fillMember(char *buf, size_t len, char base) {
    for (size_t j = 0; j < len; j++) buf[j] = base + (j % 26);
}

int sEntryTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Member lengths that select every sds header type, and the boundaries. */
    size_t lens[] = {0, 1, 5, 30, 31, 32, 100, 254, 255, 256, 1000, 65534, 65535, 65536, 70000};
    size_t nlens = sizeof(lens) / sizeof(lens[0]);

    TEST("Plain entry: create, read, free") {
        int ok = 1;
        for (size_t i = 0; i < nlens; i++) {
            size_t len = lens[i];
            char *buf = zmalloc(len + 1);
            fillMember(buf, len, 'a');

            SetEntry *e = sEntryCreate(buf, len, 0);
            sds m = sEntryGetMember(e);
            CHECK(sdslen(m) == len && memcmp(m, buf, len) == 0);
            CHECK(!sEntryHasExpiry(e));
            CHECK(sEntryRefExpiryMeta(e) == NULL);
            CHECK(sEntryGetExpiry(e) == EB_EXPIRE_TIME_INVALID);
            CHECK(((uintptr_t)e & 1) == 1);
            CHECK(sEntryGetAllocPtr(e) == sdsAllocPtr(m));
            CHECK(sEntryMemUsage(e) == sdsAllocSize(m));
            size_t usable = 0, expected = sEntryMemUsage(e);
            sEntryFree(e, &usable);
            CHECK(usable == expected);
            zfree(buf);
        }
        test_cond("plain entries: content, no expiry, odd pointer, usage, free", ok);
    }

    TEST("Entry with expiry: create, read, free") {
        int ok = 1;
        for (size_t i = 0; i < nlens; i++) {
            size_t len = lens[i];
            char *buf = zmalloc(len + 1);
            fillMember(buf, len, 'A');

            SetEntry *e = sEntryCreate(buf, len, 1);
            sds m = sEntryGetMember(e);
            CHECK(sdslen(m) == len && memcmp(m, buf, len) == 0);
            CHECK(sdsType(m) != SDS_TYPE_5);
            CHECK(sEntryHasExpiry(e));
            CHECK(((uintptr_t)e & 1) == 1);
            ExpireMeta *meta = sEntryRefExpiryMeta(e);
            CHECK(meta != NULL && (void *)meta == sEntryGetAllocPtr(e));
            CHECK((char *)m - sdsHdrSize(sdsType(m)) == (char *)meta + sizeof(ExpireMeta));
            CHECK(meta->trash == 1);
            CHECK(sEntryGetExpiry(e) == EB_EXPIRE_TIME_INVALID);
            CHECK(sEntryMemUsage(e) == sdsAllocSize(m) + sizeof(ExpireMeta));
            size_t usable = 0, expected = sEntryMemUsage(e);
            sEntryFree(e, &usable);
            CHECK(usable == expected);
            zfree(buf);
        }
        test_cond("entries with expiry: content, header type, layout, trash, usage, free", ok);
    }

    TEST("Register in an ebuckets tree and unregister") {
        int ok = 1;
        ebuckets eb = ebCreate();
        enum { N = 1000 };
        SetEntry *entries[N];
        uint64_t minWhen = UINT64_MAX;
        for (int i = 0; i < N; i++) {
            char member[32];
            int len = snprintf(member, sizeof(member), "member:%d", i);
            entries[i] = sEntryCreate(member, len, 1);
            uint64_t when = 1000000 + (uint64_t)((i * 7919) % 100000);
            if (when < minWhen) minWhen = when;
            CHECK(ebAdd(&eb, &setEntryTestBucketsType, entries[i], when) == 0);
            CHECK(sEntryGetExpiry(entries[i]) == when);
        }
        CHECK(ebGetTotalItems(eb, &setEntryTestBucketsType) == N);
        CHECK(ebGetNextTimeToExpire(eb, &setEntryTestBucketsType) == minWhen);

        for (int i = 0; i < N; i += 2) {
            CHECK(ebRemove(&eb, &setEntryTestBucketsType, entries[i]) == 1);
            CHECK(sEntryGetExpiry(entries[i]) == EB_EXPIRE_TIME_INVALID);
            CHECK(sEntryRefExpiryMeta(entries[i])->trash == 1);
        }
        CHECK(ebGetTotalItems(eb, &setEntryTestBucketsType) == N / 2);

        for (int i = 0; i < N; i++) {
            if (i % 2) CHECK(ebRemove(&eb, &setEntryTestBucketsType, entries[i]) == 1);
            sEntryFree(entries[i], NULL);
        }
        CHECK(ebGetTotalItems(eb, &setEntryTestBucketsType) == 0);
        test_cond("ebuckets: add, expiry visible, minimum, remove, trash again", ok);
    }

    TEST("Add and remove an expiry slot") {
        int ok = 1;
        for (size_t i = 0; i < nlens; i++) {
            size_t len = lens[i];
            char *buf = zmalloc(len + 1);
            fillMember(buf, len, 'k');

            SetEntry *e = sEntryCreate(buf, len, 0);
            size_t before = sEntryMemUsage(e);
            ssize_t diff = 0;
            SetEntry *e2 = sEntryAddExpiry(e, &diff);
            CHECK(sEntryHasExpiry(e2));
            CHECK(sdslen(sEntryGetMember(e2)) == len &&
                  memcmp(sEntryGetMember(e2), buf, len) == 0);
            CHECK((ssize_t)sEntryMemUsage(e2) - (ssize_t)before == diff);
            CHECK(((uintptr_t)e2 & 1) == 1);

            /* Register and unregister, as the set code does around a change. */
            ebuckets eb = ebCreate();
            CHECK(ebAdd(&eb, &setEntryTestBucketsType, e2, 5000) == 0);
            CHECK(sEntryGetExpiry(e2) == 5000);
            CHECK(ebRemove(&eb, &setEntryTestBucketsType, e2) == 1);

            size_t withExpiry = sEntryMemUsage(e2);
            diff = 0;
            SetEntry *e3 = sEntryRemoveExpiry(e2, &diff);
            CHECK(!sEntryHasExpiry(e3));
            CHECK(sdslen(sEntryGetMember(e3)) == len &&
                  memcmp(sEntryGetMember(e3), buf, len) == 0);
            CHECK((ssize_t)sEntryMemUsage(e3) - (ssize_t)withExpiry == diff);
            sEntryFree(e3, NULL);
            zfree(buf);
        }
        test_cond("add and remove an expiry slot: content and usage difference", ok);
    }

    TEST("Defrag") {
        int ok = 1;
        for (int withExpiry = 0; withExpiry <= 1; withExpiry++) {
            SetEntry *e = sEntryCreate("defrag-me", 9, withExpiry);
            CHECK(sEntryDefrag(e, testDefragNoMove, testSdsDefragNoMove) == NULL);
            SetEntry *moved = sEntryDefrag(e, testDefragMove, testSdsDefragMove);
            CHECK(moved != NULL);
            CHECK(sdslen(sEntryGetMember(moved)) == 9 &&
                  memcmp(sEntryGetMember(moved), "defrag-me", 9) == 0);
            CHECK(sEntryHasExpiry(moved) == withExpiry);
            CHECK(((uintptr_t)moved & 1) == 1);
            sEntryFree(moved, NULL);
        }
        test_cond("defrag: not moved returns NULL, moved returns the new entry", ok);
    }

    test_report();
    return 0;
}
#endif
