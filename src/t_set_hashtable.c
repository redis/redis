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

/* Hash table (dict) backend for the set type. See t_set_encoding.h. */

#include "server.h"
#include "t_set_encoding.h"

static int htResolveEncodingForAdd(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(str);
    UNUSED(len);
    UNUSED(llval);
    UNUSED(str_is_sds);
    /* The hash table encoding is the final one: it never needs to grow into
     * a different encoding. */
    return set->encoding;
}

static int htRawAdd(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(llval);
    /* Avoid duping the string if it is an sds string. */
    sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
    dict *ht = set->ptr;
    dictEntryLink bucket, link = dictFindLink(ht, sdsval, &bucket);
    if (link == NULL) {
        /* Key doesn't already exist in the set. Add it but dup the key. */
        if (sdsval == str) sdsval = sdsdup(sdsval);
        dictSetKeyAtLink(ht, sdsval, &bucket, 1);
        *htGetMetadataSize(ht) += sdsAllocSize(sdsval);
        return 1;
    }
    /* String is already a member. Free our temporary sds copy, if any. */
    if (sdsval != str) sdsfree(sdsval);
    return 0;
}

static int htRawRemove(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(llval);
    sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
    int deleted = (dictDelete(set->ptr, sdsval) == DICT_OK);
    if (sdsval != str) sdsfree(sdsval); /* free temp copy */
    return deleted;
}

static int htIsMember(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(llval);
    if (str_is_sds) return dictFind(set->ptr, (sds)str) != NULL;
    sds sdsval = sdsnewlen(str, len);
    int result = dictFind(set->ptr, sdsval) != NULL;
    sdsfree(sdsval);
    return result;
}

static void htIterInit(setTypeIterator *si) {
    dictInitIterator(&si->di, si->subject->ptr);
}

static void htIterReset(setTypeIterator *si) {
    dictResetIterator(&si->di);
}

static int htIterNext(setTypeIterator *si, char **str, size_t *len, int64_t *llele) {
    dictEntry *de = dictNext(&si->di);
    if (de == NULL) return -1;
    *str = dictGetKey(de);
    *len = sdslen(*str);
    *llele = -123456789; /* Not needed. Defensive. */
    return 0;
}

static void htRandomElement(robj *set, char **str, size_t *len, int64_t *llele) {
    dictEntry *de = dictGetFairRandomKey(set->ptr);
    *str = dictGetKey(de);
    *len = sdslen(*str);
    *llele = -123456789; /* Not needed. Defensive. */
}

static unsigned long htSize(const robj *set) {
    return dictSize((const dict *)set->ptr);
}

static size_t htAllocSize(const robj *set) {
    dict *d = set->ptr;
    return sizeof(dict) + dictMemUsage(d) + *htGetMetadataSize(d);
}

static robj *htDup(robj *o) {
    robj *set = createSetObject();
    dict *d = o->ptr;
    dictExpand(set->ptr, dictSize(d));
    setTypeIterator si;
    setTypeInitIterator(&si, o);
    char *str;
    size_t len = 0;
    int64_t intobj = 0;
    while (setTypeNext(&si, &str, &len, &intobj) != -1) {
        setTypeAdd(set, (sds)str);
    }
    setTypeResetIterator(&si);
    return set;
}

const setTypeOps setTypeOpsHT = {
    .resolveEncodingForAdd = htResolveEncodingForAdd,
    .rawAdd = htRawAdd,
    .rawRemove = htRawRemove,
    .isMember = htIsMember,
    .iterInit = htIterInit,
    .iterReset = htIterReset,
    .iterNext = htIterNext,
    .randomElement = htRandomElement,
    .size = htSize,
    .allocSize = htAllocSize,
    .dup = htDup,
};
