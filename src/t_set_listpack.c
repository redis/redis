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

/* Listpack backend for the set type. See t_set_encoding.h. */

#include "server.h"
#include "t_set_encoding.h"

static int lpRawAdd(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(llval);
    UNUSED(str_is_sds);
    unsigned char *lp = set->ptr;
    unsigned char *p = lpFirst(lp);
    if (p != NULL) p = lpFind(lp, p, (unsigned char *)str, len, 0);
    if (p != NULL) {
        /* Already a member. */
        return 0;
    }
    if (lpLength(lp) < server.set_max_listpack_entries && len <= server.set_max_listpack_value &&
        lpSafeToAdd(lp, len)) {
        set->ptr = lpAppend(lp, (unsigned char *)str, len);
        return 1;
    }
    /* Size limit reached: the caller must convert to a bigger encoding
     * (see resolveEncodingForAdd) and retry there. */
    return -1;
}

static int lpResolveEncodingForAdd(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(set);
    UNUSED(str);
    UNUSED(len);
    UNUSED(llval);
    UNUSED(str_is_sds);
    /* A listpack that's full (or that hit a per-value size limit) always
     * grows into a hash table. */
    return OBJ_ENCODING_HT;
}

static int lpRawRemove(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(llval);
    UNUSED(str_is_sds);
    unsigned char *lp = set->ptr;
    unsigned char *p = lpFirst(lp);
    if (p == NULL) return 0;
    p = lpFind(lp, p, (unsigned char *)str, len, 0);
    if (p == NULL) return 0;
    set->ptr = lpDelete(lp, p, NULL);
    return 1;
}

static int lpIsMember(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(llval);
    UNUSED(str_is_sds);
    unsigned char *lp = set->ptr;
    unsigned char *p = lpFirst(lp);
    return p && lpFind(lp, p, (unsigned char *)str, len, 0);
}

static void lpIterInit(setTypeIterator *si) {
    si->lpi = NULL;
}

static void lpIterReset(setTypeIterator *si) {
    UNUSED(si);
    /* Nothing to release for a listpack iterator. */
}

static int lpIterNext(setTypeIterator *si, char **str, size_t *len, int64_t *llele) {
    unsigned char *lp = si->subject->ptr;
    unsigned char *lpi = si->lpi;
    if (lpi == NULL) {
        lpi = lpFirst(lp);
    } else {
        lpi = lpNext(lp, lpi);
    }
    if (lpi == NULL) return -1;
    si->lpi = lpi;
    unsigned int l = 0;
    *str = (char *)lpGetValue(lpi, &l, (long long *)llele);
    *len = (size_t)l;
    return 0;
}

static void lpRandomElement(robj *set, char **str, size_t *len, int64_t *llele) {
    unsigned char *lp = set->ptr;
    int r = rand() % lpLength(lp);
    unsigned char *p = lpSeek(lp, r);
    unsigned int l;
    *str = (char *)lpGetValue(p, &l, (long long *)llele);
    *len = (size_t)l;
}

static unsigned long lpSize(const robj *set) {
    return lpLength((unsigned char *)set->ptr);
}

static size_t lpAllocSize(const robj *set) {
    return lpBytes(set->ptr);
}

static robj *lpSetDup(robj *o) {
    unsigned char *lp = o->ptr;
    size_t sz = lpBytes(lp);
    unsigned char *new_lp = zmalloc(sz);
    memcpy(new_lp, lp, sz);
    robj *set = createObject(OBJ_SET, new_lp);
    set->encoding = OBJ_ENCODING_LISTPACK;
    return set;
}

void setTypeListpackShrinkToFit(robj *set) {
    set->ptr = lpShrinkToFit(set->ptr);
}

static void *lpBuildFromIterator(setTypeIterator *si, unsigned long cap, int panic) {
    UNUSED(panic); /* lpNew() always panics on OOM, regardless of 'panic'. */
    unsigned char *lp = lpNew(cap);
    char *str;
    size_t len = 0;
    int64_t llele = 0;
    while (setTypeNext(si, &str, &len, &llele) != -1) {
        if (str != NULL)
            lp = lpAppend(lp, (unsigned char *)str, len);
        else
            lp = lpAppendInteger(lp, llele);
    }
    return lp;
}

const setTypeOps setTypeOpsListpack = {
    .rawAdd = lpRawAdd,
    .resolveEncodingForAdd = lpResolveEncodingForAdd,
    .rawRemove = lpRawRemove,
    .isMember = lpIsMember,
    .iterInit = lpIterInit,
    .iterReset = lpIterReset,
    .iterNext = lpIterNext,
    .randomElement = lpRandomElement,
    .size = lpSize,
    .allocSize = lpAllocSize,
    .dup = lpSetDup,
    .buildFromIterator = lpBuildFromIterator,
};
