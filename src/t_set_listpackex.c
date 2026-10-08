/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* Listpack backend with member expiration for the set type. See
 * t_set_encoding.h.
 *
 * The set object points to a listpackEx (the wrapper that holds the ExpireMeta
 * used to register the set in db->subexpires, and the listpack). The listpack
 * holds (member, ttl) pairs ordered by ttl. A member without a ttl has the
 * ttl LPEX_NO_TTL (0) and is placed after all the members that have one. This
 * way the member that expires first is always the first one, and the expired
 * members are always a prefix of the listpack. The ttl is an absolute UNIX time
 * in milliseconds, stored as an integer, so a member without a ttl costs two
 * more bytes. */

#include "server.h"
#include "t_set_encoding.h"

#define LPEX_NO_TTL 0

static inline listpackEx *lpexOf(const robj *set) {
    return set->ptr;
}

/* Frees a listpackEx wrapper and its listpack. */
static void lpexFreeWrapper(listpackEx *lpt) {
    lpFree(lpt->lp);
    zfree(lpt);
}

/* Creates an empty wrapper that is not registered in any ebuckets. */
static listpackEx *lpexCreateWrapper(unsigned char *lp) {
    listpackEx *lpt = zcalloc(sizeof(*lpt));
    lpt->meta.trash = 1;
    lpt->lp = lp;
    return lpt;
}

/* Reads the ttl element that follows the member element 'p'. */
static inline uint64_t lpexTtlAfter(unsigned char *lp, unsigned char *p) {
    unsigned char *t = lpNext(lp, p);
    serverAssert(t != NULL);
    long long ttl;
    serverAssert(lpGetIntegerValue(t, &ttl));
    return (uint64_t)ttl;
}

static inline uint64_t lpexTtlToExpire(uint64_t ttl) {
    return ttl == LPEX_NO_TTL ? EB_EXPIRE_TIME_INVALID : ttl;
}

/* Finds the member. Returns the pointer to its member element or NULL. */
static unsigned char *lpexFind(unsigned char *lp, char *str, size_t len) {
    unsigned char *p = lpFirst(lp);
    if (p == NULL) return NULL;
    /* skip=1: compare only the member of each (member, ttl) pair. */
    return lpFind(lp, p, (unsigned char *)str, len, 1);
}

/* Arguments of the callback below. */
struct lpexFindArgs {
    uint64_t expire;       /* [in] Find the first pair with a ttl above this one */
    unsigned char *p;      /* [out] Member element of that pair, NULL if none */
    int index;             /* Internal */
    unsigned char *mptr;   /* Internal */
};

/* Callback for lpFindCb(): finds the position where a pair with the given ttl
 * has to be inserted, that is, the first pair that has no ttl or a larger one. */
static int lpexCbFindPos(const unsigned char *lp, unsigned char *p, void *user,
                         unsigned char *s, long long slen)
{
    UNUSED(lp);
    struct lpexFindArgs *r = user;
    r->index++;
    if (r->index % 2 == 1) {
        r->mptr = p;               /* Member element of the pair. */
    } else {
        serverAssert(!s);          /* The ttl is an integer. */
        if (slen == LPEX_NO_TTL || (uint64_t)slen >= r->expire) {
            r->p = r->mptr;
            return 0;              /* Stop here. */
        }
    }
    return 1;
}

/* Inserts a (member, ttl) pair keeping the listpack ordered by ttl. The member
 * is given as a string, or as an integer if llvalp is not NULL. */
static unsigned char *lpexInsertPair(unsigned char *lp, char *str, size_t len,
                                     int64_t *llvalp, uint64_t ttl)
{
    listpackEntry ent[2];
    if (llvalp) {
        ent[0].sval = NULL;
        ent[0].lval = *llvalp;
    } else {
        ent[0].sval = (unsigned char *)str;
        ent[0].slen = len;
    }
    ent[1].sval = NULL;
    ent[1].lval = (long long)ttl;

    /* A member without a ttl always goes to the end. */
    if (ttl == LPEX_NO_TTL)
        return lpBatchAppend(lp, ent, 2);

    struct lpexFindArgs r = {.expire = ttl};
    lpFindCb(lp, NULL, &r, lpexCbFindPos, 0);
    if (r.p)
        return lpBatchInsert(lp, r.p, LP_BEFORE, ent, 2, NULL);
    return lpBatchAppend(lp, ent, 2);
}

static int lpexRawAddCommon(robj *set, char *str, size_t len, int64_t *llvalp, int after_convert,
                            int *target_enc, uint64_t expire)
{
    listpackEx *lpt = lpexOf(set);
    unsigned char *lp = lpt->lp;
    uint64_t ttl = expire == EB_EXPIRE_TIME_INVALID ? LPEX_NO_TTL : expire;
    *target_enc = OBJ_ENCODING_LISTPACK_EX;

    if (!after_convert) {
        unsigned char *p = lpexFind(lp, str, len);
        if (p != NULL) {
            if (!setTypeExpireTimeElapsed(lpexTtlToExpire(lpexTtlAfter(lp, p))))
                return 0; /* Already a (live) member. */
            /* The member is logically expired but not removed yet: it is as if
             * it was not there. Replace it by a fresh one, which does not change
             * the number of members, so there are no limits to check. */
            lpt->lp = lp = lpDeleteRangeWithEntry(lp, &p, 2);
            lpt->lp = lpexInsertPair(lp, str, len, llvalp, ttl);
            return 1;
        }
    }

    /* The limit counts members, not listpack elements. */
    if (lpLength(lp) / 2 < server.set_max_listpack_entries &&
        len <= server.set_max_listpack_value &&
        lpSafeToAdd(lp, len + 16))
    {
        lpt->lp = lpexInsertPair(lp, str, len, llvalp, ttl);
        return 1;
    }

    /* Size limit reached: the caller must convert to a bigger encoding. */
    *target_enc = OBJ_ENCODING_HT;
    return -1;
}

static int lpexRawAdd(robj *set, char *str, size_t len, int64_t *llvalp, int str_is_sds,
                      int after_convert, int *target_enc)
{
    UNUSED(str_is_sds);
    return lpexRawAddCommon(set, str, len, llvalp, after_convert, target_enc, EB_EXPIRE_TIME_INVALID);
}

static int lpexRawAddEx(robj *set, char *str, size_t len, int64_t *llvalp, int str_is_sds,
                        int after_convert, int *target_enc, uint64_t expire)
{
    UNUSED(str_is_sds);
    return lpexRawAddCommon(set, str, len, llvalp, after_convert, target_enc, expire);
}

static int lpexRawRemove(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(llval);
    UNUSED(str_is_sds);
    listpackEx *lpt = lpexOf(set);
    unsigned char *p = lpexFind(lpt->lp, str, len);
    if (p == NULL) return 0;
    lpt->lp = lpDeleteRangeWithEntry(lpt->lp, &p, 2);
    return 1;
}

static int lpexIsMember(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    UNUSED(llval);
    UNUSED(str_is_sds);
    return lpexFind(lpexOf(set)->lp, str, len) != NULL;
}

static int lpexGetExpire(robj *set, char *str, size_t len, int64_t llval, int str_is_sds,
                         uint64_t *expire)
{
    UNUSED(llval);
    UNUSED(str_is_sds);
    unsigned char *lp = lpexOf(set)->lp;
    unsigned char *p = lpexFind(lp, str, len);
    if (p == NULL) return 0;
    *expire = lpexTtlToExpire(lpexTtlAfter(lp, p));
    return 1;
}

static int lpexSetExpire(robj *set, char *str, size_t len, int64_t llval, int str_is_sds,
                         uint64_t expire)
{
    UNUSED(llval);
    UNUSED(str_is_sds);
    listpackEx *lpt = lpexOf(set);
    unsigned char *p = lpexFind(lpt->lp, str, len);
    if (p == NULL) return 0;

    uint64_t ttl = expire == EB_EXPIRE_TIME_INVALID ? LPEX_NO_TTL : expire;
    if (lpexTtlAfter(lpt->lp, p) == ttl) return 1;

    /* Take the member out and put it back at the position of its new ttl. The
     * member is copied first since deleting it invalidates the pointer. */
    unsigned int mlen;
    long long mll;
    unsigned char *mval = lpGetValue(p, &mlen, &mll);
    sds mcopy = NULL;
    int64_t ll = mll;
    if (mval) mcopy = sdsnewlen(mval, mlen);

    lpt->lp = lpDeleteRangeWithEntry(lpt->lp, &p, 2);
    lpt->lp = lpexInsertPair(lpt->lp, mcopy, mcopy ? mlen : 0, mcopy ? NULL : &ll, ttl);
    sdsfree(mcopy);
    return 1;
}

static uint64_t lpexMinExpire(robj *set, int accurate) {
    UNUSED(accurate); /* The members are ordered by ttl, so this is always exact. */
    unsigned char *lp = lpexOf(set)->lp;
    /* The ttl of the first pair is the second element. */
    unsigned char *p = lpSeek(lp, 1);
    if (p == NULL) return EB_EXPIRE_TIME_INVALID;
    long long ttl;
    serverAssert(lpGetIntegerValue(p, &ttl));
    return lpexTtlToExpire((uint64_t)ttl);
}

static unsigned long lpexExpire(robj *set, uint64_t now, unsigned long max, setTypeExpireCb cb, void *ctx) {
    listpackEx *lpt = lpexOf(set);
    unsigned long expired = 0;
    unsigned char *p = lpFirst(lpt->lp);

    while (p != NULL && expired < max) {
        uint64_t ttl = lpexTtlAfter(lpt->lp, p);
        /* The members are ordered by ttl: from the first one that has no ttl or
         * is not expired yet, none of the rest is expired. A member is expired
         * iff its ttl is strictly before now, as everywhere else. */
        if (ttl == LPEX_NO_TTL || ttl >= now) break;

        if (cb) {
            unsigned int mlen;
            long long mll;
            unsigned char *mval = lpGetValue(p, &mlen, &mll);
            cb(ctx, (char *)mval, mval ? mlen : 0, mll);
        }
        p = lpNext(lpt->lp, lpNext(lpt->lp, p));
        expired++;
    }

    if (expired)
        lpt->lp = lpDeleteRange(lpt->lp, 0, expired * 2);
    return expired;
}

static void lpexIterInit(setTypeIterator *si) {
    si->lpi = NULL;
}

static void lpexIterReset(setTypeIterator *si) {
    UNUSED(si);
}

static int lpexIterNext(setTypeIterator *si, char **str, size_t *len, int64_t *llele) {
    unsigned char *lp = lpexOf(si->subject)->lp;
    unsigned char *lpi = si->lpi;
    if (lpi == NULL) {
        lpi = lpFirst(lp);
    } else {
        /* Skip the ttl of the previous member. */
        lpi = lpNext(lp, lpNext(lp, lpi));
    }
    if (lpi == NULL) return -1;
    si->lpi = lpi;
    si->expire = lpexTtlToExpire(lpexTtlAfter(lp, lpi));
    unsigned int l = 0;
    *str = (char *)lpGetValue(lpi, &l, (long long *)llele);
    *len = (size_t)l;
    return 0;
}

static void lpexRandomElement(robj *set, char **str, size_t *len, int64_t *llele) {
    unsigned char *lp = lpexOf(set)->lp;
    listpackEntry key;
    lpRandomPair(lp, lpLength(lp) / 2, &key, NULL, 2);
    if (key.sval) {
        *str = (char *)key.sval;
        *len = key.slen;
    } else {
        *str = NULL;
        *llele = key.lval;
    }
}

static unsigned long lpexSize(const robj *set) {
    return lpLength(lpexOf(set)->lp) / 2;
}

static size_t lpexAllocSize(const robj *set) {
    return sizeof(listpackEx) + lpBytes(lpexOf(set)->lp);
}

static robj *lpexSetDup(robj *o) {
    listpackEx *lpt = lpexOf(o);
    size_t sz = lpBytes(lpt->lp);
    unsigned char *new_lp = zmalloc(sz);
    memcpy(new_lp, lpt->lp, sz);
    robj *set = createObject(OBJ_SET, lpexCreateWrapper(new_lp));
    set->encoding = OBJ_ENCODING_LISTPACK_EX;
    return set;
}

static void lpexSetFree(robj *set) {
    lpexFreeWrapper(lpexOf(set));
}

/* Builds a listpack with expiration from a set without member expirations (a
 * plain listpack or an intset), keeping the order of its members. */
static void *lpexConvertFrom(robj *set, unsigned long cap, int panic) {
    UNUSED(panic); /* lpNew() always panics on OOM. */
    serverAssert(set->encoding == OBJ_ENCODING_LISTPACK ||
                 set->encoding == OBJ_ENCODING_INTSET);

    /* Preallocate the minimum two bytes per element: the member and the ttl. */
    unsigned char *lp = lpNew(cap * 4);
    char *str;
    size_t len = 0;
    int64_t llele = 0;
    setTypeIterator si;
    setTypeInitIterator(&si, set, SET_ITER_RAW);
    while (setTypeNext(&si, &str, &len, &llele) != -1) {
        if (str != NULL)
            lp = lpAppend(lp, (unsigned char *)str, len);
        else
            lp = lpAppendInteger(lp, llele);
        lp = lpAppendInteger(lp, LPEX_NO_TTL);
    }
    setTypeResetIterator(&si);
    lp = lpShrinkToFit(lp);
    return lpexCreateWrapper(lp);
}

/* Returns the wrapper of a listpack-with-expiration set, for the code that
 * registers the set in db->subexpires. */
ExpireMeta *setListpackExGetExpireMeta(const robj *set) {
    serverAssert(set->encoding == OBJ_ENCODING_LISTPACK_EX);
    return &lpexOf(set)->meta;
}

const setTypeOps setTypeOpsListpackEx = {
    .rawAdd = lpexRawAdd,
    .rawRemove = lpexRawRemove,
    .isMember = lpexIsMember,
    .iterInit = lpexIterInit,
    .iterReset = lpexIterReset,
    .iterNext = lpexIterNext,
    .randomElement = lpexRandomElement,
    .size = lpexSize,
    .allocSize = lpexAllocSize,
    .dup = lpexSetDup,
    .free = lpexSetFree,
    .convertFrom = lpexConvertFrom,
    .getExpire = lpexGetExpire,
    .setExpire = lpexSetExpire,
    .rawAddEx = lpexRawAddEx,
    .minExpire = lpexMinExpire,
    .expire = lpexExpire,
};
