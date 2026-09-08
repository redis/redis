/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

/* Intset backend for the set type. See t_set_encoding.h. */

#include "server.h"
#include "intset.h"
#include "t_set_encoding.h"

/* A NULL str means the caller already has the value as an integer (llval)
 * and there's nothing to parse. This is intset's only encoding-selection
 * fast path: it's the sole encoding that can consume a bare integer
 * without ever looking at a string form of it. */
static int isRawAdd(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(str_is_sds);
    long long value;
    if (str == NULL) {
        value = llval;
    } else if (!string2ll(str, len, &value)) {
        /* Not representable as an integer: intset can't hold it at all. */
        return -1;
    }
    uint8_t success = 0;
    set->ptr = intsetAdd(set->ptr, value, &success);
    return success ? 1 : 0;
}

/* Only called after rawAdd() returns -1, i.e. the value isn't an integer.
 * Decides whether the resulting set is still small enough to become a
 * listpack, or must go straight to a hash table. */
static int isResolveEncodingForAdd(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(str);
    UNUSED(llval);
    UNUSED(str_is_sds);
    size_t maxelelen = 0, totsize = 0;
    unsigned long n = intsetLen(set->ptr);
    if (n != 0) {
        size_t elelen1 = sdigits10(intsetMax(set->ptr));
        size_t elelen2 = sdigits10(intsetMin(set->ptr));
        maxelelen = max(elelen1, elelen2);
        size_t s1 = lpEstimateBytesRepeatedInteger(intsetMax(set->ptr), n);
        size_t s2 = lpEstimateBytesRepeatedInteger(intsetMin(set->ptr), n);
        totsize = max(s1, s2);
    }
    if (n < server.set_max_listpack_entries && len <= server.set_max_listpack_value &&
        maxelelen <= server.set_max_listpack_value && lpSafeToAdd(NULL, totsize + len)) {
        /* In the "safe to add" check above we assumed all elements in the
         * intset are of size maxelelen. This is an upper bound. */
        return OBJ_ENCODING_LISTPACK;
    }
    return OBJ_ENCODING_HT;
}

static int isRawRemove(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(str_is_sds);
    long long value;
    if (str == NULL) {
        value = llval;
    } else if (!string2ll(str, len, &value)) {
        return 0;
    }
    int success;
    set->ptr = intsetRemove(set->ptr, value, &success);
    return success;
}

static int isIsMember(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(str_is_sds);
    long long value;
    if (str == NULL) {
        value = llval;
    } else if (!string2ll(str, len, &value)) {
        return 0;
    }
    return intsetFind(set->ptr, value);
}

static void isIterInit(setTypeIterator *si) {
    si->ii = 0;
}

static void isIterReset(setTypeIterator *si) {
    UNUSED(si);
    /* Nothing to release for an intset iterator. */
}

static int isIterNext(setTypeIterator *si, char **str, size_t *len, int64_t *llele) {
    UNUSED(len);
    if (!intsetGet(si->subject->ptr, si->ii++, llele)) return -1;
    *str = NULL;
    return 0;
}

static void isRandomElement(robj *set, char **str, size_t *len, int64_t *llele) {
    UNUSED(len);
    *llele = intsetRandom(set->ptr);
    *str = NULL; /* Not needed. Defensive. */
}

static unsigned long isSize(const robj *set) {
    return intsetLen((const intset *)set->ptr);
}

static size_t isAllocSize(const robj *set) {
    return intsetAllocSize(set->ptr);
}

static robj *isSetDup(robj *o) {
    intset *is = o->ptr;
    size_t size = intsetBlobLen(is);
    intset *newis = zmalloc(size);
    memcpy(newis, is, size);
    robj *set = createObject(OBJ_SET, newis);
    set->encoding = OBJ_ENCODING_INTSET;
    return set;
}

const setTypeOps setTypeOpsIntset = {
    .rawAdd = isRawAdd,
    .resolveEncodingForAdd = isResolveEncodingForAdd,
    .rawRemove = isRawRemove,
    .isMember = isIsMember,
    .iterInit = isIterInit,
    .iterReset = isIterReset,
    .iterNext = isIterNext,
    .randomElement = isRandomElement,
    .size = isSize,
    .allocSize = isAllocSize,
    .dup = isSetDup,
};
