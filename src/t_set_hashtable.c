/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* Hash table (dict) backend for the set type. See t_set_encoding.h. */

#include "server.h"
#include "t_set_encoding.h"

/*-----------------------------------------------------------------------------
 * Dict types of a hashtable set
 *
 * The members are the keys of the dict (it has no values). A member without an
 * expiration is a plain sds, and a member that has one is a SetEntry (see
 * set_entry.h), whose pointer is the member sds as well. Once a member gets an
 * expiration, the dict (and its type) are replaced by one that has the metadata
 * of the expirations: a private ebuckets tree of the members that have one and
 * an ExpireMeta to register the whole set in db->subexpires.
 *
 * Both types must be the same in everything but the metadata and the release
 * callback, since the dict is changed from one to the other in place. In
 * particular, the destructor handles members of both forms.
 *----------------------------------------------------------------------------*/

static void setDictEntryDestructor(dict *d, void *entry);
static size_t setDictMetadataBytes(dict *d);
static size_t setDictWithExpireMetadataBytes(dict *d);
static void setDictWithExpireOnRelease(dict *d);
static ExpireMeta *setMemberGetExpireMeta(const eItem item);

/* ebuckets type of the private tree of a set: the members that have an expiration. */
EbucketsType setMemberExpireBucketsType = {
    .onDeleteItem = NULL,
    .getExpireMeta = setMemberGetExpireMeta, /* get ExpireMeta attached to each member */
    .itemsAddrAreOdd = 1,                    /* Addresses of members (sds) are odd */
};

/* Set dictionary type. Keys are members, values are not used. */
dictType setDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    setDictEntryDestructor,    /* key destructor */
    NULL,                      /* val destructor */
    NULL,                      /* allow to expand */
    .no_value = 1,             /* no values in this dict */
    .keys_are_odd = 1,         /* an SDS string is always an odd pointer */
    .dictMetadataBytes = setDictMetadataBytes,
};

/* The same, for sets that have members with an expiration. */
dictType setDictTypeWithExpire = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    setDictEntryDestructor,    /* key destructor */
    NULL,                      /* val destructor */
    NULL,                      /* allow to expand */
    .no_value = 1,             /* no values in this dict */
    .keys_are_odd = 1,         /* an SDS string is always an odd pointer */
    .dictMetadataBytes = setDictWithExpireMetadataBytes,
    .onDictRelease = setDictWithExpireOnRelease,
};

static inline int htHasExpire(const dict *d) {
    return d->type == &setDictTypeWithExpire;
}

static ExpireMeta *setMemberGetExpireMeta(const eItem item) {
    return sEntryRefExpiryMeta((SetEntry *)item);
}

static void setDictEntryDestructor(dict *d, void *entry) {
    size_t usable;
    size_t *alloc_size = htGetMetadataSize(d);

    /* If the member has an expiration, remove it from the private ebuckets. */
    if (sEntryGetExpiry(entry) != EB_EXPIRE_TIME_INVALID) {
        htMetadataEx *meta = htGetMetadataEx(d);
        ebRemove(&meta->elemExpires, &setMemberExpireBucketsType, entry);
    }

    sEntryFree(entry, &usable);
    *alloc_size -= usable;
}

static size_t setDictMetadataBytes(dict *d) {
    UNUSED(d);
    return sizeof(size_t);
}

static size_t setDictWithExpireMetadataBytes(dict *d) {
    UNUSED(d);
    /* The alloc size, the ExpireMeta of the set and the ref to the private ebuckets. */
    return sizeof(htMetadataEx);
}

static void setDictWithExpireOnRelease(dict *d) {
    /* Allocated with the metadata for sure. Otherwise this would not be registered. */
    htMetadataEx *meta = htGetMetadataEx(d);
    ebDestroy(&meta->elemExpires, &setMemberExpireBucketsType, NULL);
}

/* Returns 1 if the hashtable set has the metadata of the expirations. */
int setHashtableHasExpire(const robj *set) {
    return htHasExpire((const dict *)set->ptr);
}

/* Replaces the dict of a hashtable set by one with the metadata of the
 * expirations, if it does not have it yet. */
void setHashtableAddExpireSupport(robj *set) {
    dict *d = set->ptr;
    if (htHasExpire(d)) return;
    dictTypeAddMeta(&d, &setDictTypeWithExpire); /* The dict may move. */
    set->ptr = d;
    htMetadataEx *meta = htGetMetadataEx(d);
    meta->elemExpires = ebCreate();      /* Allocate the DS of the members with expiration */
    meta->expireMeta.trash = 1;  /* Mark as trash, as long as it was not ebAdd()'ed */
}

ExpireMeta *setHashtableGetExpireMeta(const robj *set) {
    dict *d = set->ptr;
    serverAssert(htHasExpire(d));
    return &htGetMetadataEx(d)->expireMeta;
}

/*-----------------------------------------------------------------------------
 * Ops of the hashtable encoding
 *----------------------------------------------------------------------------*/

static int htRawAddCommon(robj *set, char *str, size_t len, int str_is_sds, uint64_t expire) {
    /* Avoid duping the string if it is an sds string. */
    sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
    dict *ht = set->ptr;
    const int hasExpire = htHasExpire(ht);
    serverAssert(expire == EB_EXPIRE_TIME_INVALID || hasExpire);

    dictEntryLink bucket, link = dictFindLink(ht, sdsval, &bucket);
    if (link != NULL) {
        if (hasExpire) {
            uint64_t prev = sEntryGetExpiry(dictGetKey(*link));
            if (setTypeExpireTimeElapsed(prev)) {
                /* The member is logically expired but not removed yet: replace it by
                 * a new one, as if it was not there. */
                serverAssert(dictDelete(ht, sdsval) == DICT_OK);
                link = dictFindLink(ht, sdsval, &bucket);
                serverAssert(link == NULL);
            }
        }
        if (link != NULL) {
            /* String is already a member. Free our temporary sds copy, if any. */
            if (sdsval != str) sdsfree(sdsval);
            return 0;
        }
    }

    /* Key doesn't already exist in the set. Add it but dup the key. */
    SetEntry *sEntry;
    if (expire != EB_EXPIRE_TIME_INVALID) {
        sEntry = sEntryCreate(sdsval, sdslen(sdsval), 1);
        if (sdsval != str) sdsfree(sdsval);
    } else {
        if (sdsval == str) sdsval = sdsdup(sdsval);
        sEntry = (SetEntry *)sdsval;
    }
    dictSetKeyAtLink(ht, sEntry, &bucket, 1);
    *htGetMetadataSize(ht) += sEntryMemUsage(sEntry);
    if (expire != EB_EXPIRE_TIME_INVALID)
        ebAdd(&htGetMetadataEx(ht)->elemExpires, &setMemberExpireBucketsType, sEntry, expire);
    return 1;
}

static int htRawAdd(robj *set, char *str, size_t len, int64_t *llvalp, int str_is_sds, int after_convert, int *target_enc) {
    UNUSED(llvalp);
    UNUSED(after_convert);
    *target_enc = OBJ_ENCODING_HT; /* Hash table is the final encoding: no further conversion is possible. */
    return htRawAddCommon(set, str, len, str_is_sds, EB_EXPIRE_TIME_INVALID);
}

static int htRawAddEx(robj *set, char *str, size_t len, int64_t *llvalp, int str_is_sds, int after_convert, int *target_enc, uint64_t expire) {
    UNUSED(llvalp);
    UNUSED(after_convert);
    *target_enc = OBJ_ENCODING_HT;
    return htRawAddCommon(set, str, len, str_is_sds, expire);
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

static int htGetExpire(robj *set, char *str, size_t len, int64_t llval, int str_is_sds, uint64_t *expire) {
    UNUSED(llval);
    sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
    dictEntry *de = dictFind(set->ptr, sdsval);
    if (sdsval != str) sdsfree(sdsval);
    if (de == NULL) return 0;
    *expire = sEntryGetExpiry(dictGetKey(de));
    return 1;
}

static int htSetExpire(robj *set, char *str, size_t len, int64_t llval, int str_is_sds, uint64_t expire) {
    UNUSED(llval);
    dict *ht = set->ptr;
    sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
    dictEntryLink link = dictFindLink(ht, sdsval, NULL);
    if (sdsval != str) sdsfree(sdsval);
    if (link == NULL) return 0;

    SetEntry *sOldEntry = dictGetKey(*link), *sEntry = sOldEntry;
    size_t *alloc_size = htGetMetadataSize(ht);

    if (expire == EB_EXPIRE_TIME_INVALID) {
        /* Remove the expiration. A plain member has none. */
        if (!sEntryHasExpiry(sOldEntry)) return 1;
        /* Unlink from the private tree if it is there, then drop the prefix. */
        if (sEntryGetExpiry(sOldEntry) != EB_EXPIRE_TIME_INVALID)
            ebRemove(&htGetMetadataEx(ht)->elemExpires, &setMemberExpireBucketsType, sOldEntry);
        ssize_t usableDiff;
        sEntry = sEntryRemoveExpiry(sOldEntry, &usableDiff);
        *alloc_size += usableDiff;
        dictSetKeyAtLink(ht, sEntry, &link, 0);
        return 1;
    }

    serverAssert(htHasExpire(ht));
    if (!sEntryHasExpiry(sOldEntry)) {
        /* Reallocate the member with room for the expiration. */
        ssize_t usableDiff;
        sEntry = sEntryAddExpiry(sOldEntry, &usableDiff);
        *alloc_size += usableDiff;
    } else {
        uint64_t prev = sEntryGetExpiry(sOldEntry);
        if (prev == expire) return 1;
        /* Remove from the private tree the old expiration time. */
        if (prev != EB_EXPIRE_TIME_INVALID)
            ebRemove(&htGetMetadataEx(ht)->elemExpires, &setMemberExpireBucketsType, sOldEntry);
    }
    dictSetKeyAtLink(ht, sEntry, &link, 0); /* newItem=0: update an existing entry */
    ebAdd(&htGetMetadataEx(ht)->elemExpires, &setMemberExpireBucketsType, sEntry, expire);
    return 1;
}

static uint64_t htMinExpire(robj *set, int accurate) {
    UNUSED(accurate);
    dict *ht = set->ptr;
    if (!htHasExpire(ht)) return EB_EXPIRE_TIME_INVALID;
    return ebGetNextTimeToExpire(htGetMetadataEx(ht)->elemExpires, &setMemberExpireBucketsType);
}

typedef struct htExpireCtx {
    dict *d;
    setTypeExpireCb cb;
    void *ctx;
} htExpireCtx;

/* Called by ebExpire() for each member that expired. */
static ExpireAction htOnMemberExpire(eItem item, void *c) {
    htExpireCtx *ctx = c;
    sds member = sEntryGetMember((SetEntry *)item);
    if (ctx->cb) ctx->cb(ctx->ctx, member, sdslen(member), 0);
    /* ebExpire() already took the item out of the tree, so the destructor of the
     * dict only frees it. */
    serverAssert(dictDelete(ctx->d, member) == DICT_OK);
    return ACT_REMOVE_EXP_ITEM;
}

static unsigned long htExpire(robj *set, uint64_t now, unsigned long max, setTypeExpireCb cb, void *ctx) {
    dict *ht = set->ptr;
    if (!htHasExpire(ht)) return 0;
    htExpireCtx c = {.d = ht, .cb = cb, .ctx = ctx};
    ExpireInfo info = {
        .maxToExpire = max,
        .now = now,
        .onExpireItem = htOnMemberExpire,
        .ctx = &c,
        .itemsExpired = 0,
    };
    ebExpire(&htGetMetadataEx(ht)->elemExpires, &setMemberExpireBucketsType, &info);
    return info.itemsExpired;
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
    if (htHasExpire((dict *)si->subject->ptr))
        si->expire = sEntryGetExpiry((SetEntry *)*str);
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
    if (htHasExpire(d)) setHashtableAddExpireSupport(set);
    setTypeIterator si;
    setTypeInitIterator(&si, o, SET_ITER_RAW);
    char *str;
    size_t len = 0;
    int64_t intobj = 0;
    while (setTypeNext(&si, &str, &len, &intobj) != -1) {
        /* Expired members are copied as well, with their expiration. */
        htRawAddCommon(set, str, len, 1, si.expire);
    }
    setTypeResetIterator(&si);
    return set;
}

static void htFree(robj *set) {
#ifdef DEBUG_ASSERTIONS
    dictEmpty(set->ptr, NULL);
    debugServerAssert(*htGetMetadataSize(set->ptr) == 0);
#endif
    dictRelease((dict *)set->ptr);
}

static void *htConvertFrom(robj *set, unsigned long cap, int panic) {
    dict *d = dictCreate(&setDictType);
    if (panic) {
        dictExpand(d, cap);
    } else if (dictTryExpand(d, cap) != DICT_OK) {
        dictRelease(d);
        return NULL;
    }

    /* The members of a listpack with expirations keep their expiration. */
    const int withExpire = (set->encoding == OBJ_ENCODING_LISTPACK_EX);
    robj holder = {0};
    if (withExpire) {
        /* Use a temporary object to build the dict with the expiration support. */
        holder.type = OBJ_SET;
        holder.encoding = OBJ_ENCODING_HT;
        holder.ptr = d;
        setHashtableAddExpireSupport(&holder);
        d = holder.ptr;
    }

    /* To add the elements we extract integers and create redis objects */
    setTypeIterator si;
    setTypeInitIterator(&si, set, SET_ITER_RAW);
    char *str;
    size_t len = 0;
    int64_t llele = 0;
    while (setTypeNext(&si, &str, &len, &llele) != -1) {
        sds element = str ? sdsnewlen(str, len) : sdsfromlonglong(llele);
        if (withExpire) {
            htRawAddCommon(&holder, element, sdslen(element), 1, si.expire);
            sdsfree(element);
        } else {
            serverAssert(dictAdd(d, element, NULL) == DICT_OK);
            *htGetMetadataSize(d) += sdsAllocSize(element);
        }
    }
    setTypeResetIterator(&si);
    return d;
}

const setTypeOps setTypeOpsHT = {
    .rawAdd = htRawAdd,
    .rawRemove = htRawRemove,
    .isMember = htIsMember,
    .iterInit = htIterInit,
    .iterReset = htIterReset,
    .iterNext = htIterNext,
    .randomElement = htRandomElement,
    .size = htSize,
    .allocSize = htAllocSize,
    .free = htFree,
    .convertFrom = htConvertFrom,
    .dup = htDup,
    .getExpire = htGetExpire,
    .setExpire = htSetExpire,
    .rawAddEx = htRawAddEx,
    .minExpire = htMinExpire,
    .expire = htExpire,
};
