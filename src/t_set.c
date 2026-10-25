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
#include "server.h"
#include "intset.h"  /* Compact integer set structure */
#include "hyperloglog.h"
#include "vector.h"

/*=============================================================================
 * Set type: structure of this file
 * The whole implementation of the set type is in this file, in these parts:
 *
 *   1. The interface of the encodings (setTypeOps).
 *   2. The shared primitives of the set type (setType*), which are independent of
 *      the encoding and dispatch to it through setTypeOps, and the member
 *      expiration logic that is shared by all the encodings.
 *   3. The implementation of each encoding: intset, listpack, listpack with
 *      member expirations (listpackex) and hashtable.
 *   4. The set commands.
 *   5. The member expiration commands (SEXPIRE, STTL, SPERSIST, SADDEX...).
 *=============================================================================*/

/*=============================================================================
 * 1. Encodings interface
 *=============================================================================*/

/* Per-encoding operations for the set type. Each function operates on a robj
 * of type OBJ_SET encoded with whichever encoding the table instance below
 * belongs to (intset, listpack or hash table). This lets t_set.c dispatch to
 * the right implementation without an if/else chain on robj->encoding.
 *
 * rawAdd() attempts to insert a member under the object's *current*
 * encoding. It returns 1 if added, 0 if the member already existed, or -1
 * if the value cannot be added as-is (a configured limit would be
 * exceeded, or the encoding structurally can't represent the value) - in
 * which case it does not mutate the object at all.
 * Note that after_convert is set to 1 when rawAdd is called after set encoding
 * conversion, so it is guaranteed that the item to add doesn't already exist in
 * the set, and therefore there is no need to search for it. Also, for listpack,
 * the listpack may be shrink after adding the new item if too much space was
 * allocated for it during conversion.
 * target_enc is set to the encoding that the caller should convert the set to
 * if rawAdd returns -1.
 * llvalp is NULL if the value was received as string (this does not necessarily
 * mean that the value can't be converted to an integer), and points to the integer
 * value if the value was received as an integer.
 */
/* Called by the expire() op for each member it removes. */
typedef void (*setTypeExpireCb)(void *ctx, char *str, size_t len, int64_t llval);

struct _setTypeOps {
    /* return 0 if the member already exists, 1 if added, -1 if it cannot be added (due to encoding mismatch or size limits)*/
    int (*rawAdd)(robj *set, char *str, size_t len, int64_t *llvalp, int str_is_sds, int after_convert, int *target_enc);
    /* return 0 if the member doesn't exist, 1 if removed */
    int (*rawRemove)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);
    /* return 1 if the member exists, 0 if not */
    int (*isMember)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);

    void (*iterInit)(setTypeIterator *si);
    void (*iterReset)(setTypeIterator *si);
    /* return 0 if there is a next element, -1 if not */
    int (*iterNext)(setTypeIterator *si, char **str, size_t *len, int64_t *llele);

    void (*randomElement)(robj *set, char **str, size_t *len, int64_t *llele);

    unsigned long (*size)(const robj *set);
    size_t (*allocSize)(const robj *set);

    robj *(*dup)(robj *set);
    void (*free)(robj *set);

    /* Builds a fresh instance of this encoding from the contents of an
     * existing set, to be assigned to the converted object's ptr by the
     * caller (see setTypeConvertAndExpand()). Iterates 'set'
     * internally (initializing and resetting its own iterator). 'cap' is
     * the number of elements to presize for; encodings that need a
     * different unit internally (e.g. the listpack's byte size estimate)
     * convert it themselves. Returns NULL if 'panic' is false and
     * allocation failed; the encodings that always panic on OOM (i.e.
     * everything but the hash table) ignore 'panic'. NULL for encodings
     * that setTypeConvertAndExpand() never targets (currently intset - see
     * maybeConvertToIntset() instead). */
    void *(*convertFrom)(robj *set, unsigned long cap, int panic);

    /* Member expiration. The following are NULL for the encodings that cannot
     * hold member expirations (every encoding but the "ex" variants), and
     * setTypeHasExpireSupport() tells the two kinds apart. Expiration times
     * are absolute UNIX times in milliseconds, and EB_EXPIRE_TIME_INVALID means
     * "no expiration". The members are passed like in rawAdd(). */

    /* Looks up a member. Returns 0 if it does not exist and 1 if it does, and
     * then sets *expire to its expiration time. A member that is logically
     * expired but not yet removed is still found. */
    int (*getExpire)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds, uint64_t *expire);
    /* Sets the expiration of an existing member, or clears it when 'expire' is
     * EB_EXPIRE_TIME_INVALID. Returns 0 if the member does not exist, 1 if done. */
    int (*setExpire)(robj *set, char *str, size_t len, int64_t llval, int str_is_sds, uint64_t expire);
    /* Same as rawAdd(), and the new member gets the given expiration. An
     * existing member that is logically expired is replaced, as in rawAdd(). */
    int (*rawAddEx)(robj *set, char *str, size_t len, int64_t *llvalp, int str_is_sds, int after_convert, int *target_enc, uint64_t expire);
    /* The earliest expiration among the members, EB_EXPIRE_TIME_INVALID if none
     * has one. 'accurate' asks for an exact answer instead of a cached one. */
    uint64_t (*minExpire)(robj *set, int accurate);
    /* Removes up to 'max' members whose expiration is before 'now', the
     * earliest first, calling 'cb' for each one right before removing it. A
     * member is passed as a string (str, len) or, if str is NULL, as an integer
     * (llval). Returns the number of removed members. */
    unsigned long (*expire)(robj *set, uint64_t now, unsigned long max, setTypeExpireCb cb, void *ctx);
};

/* Returns the ops of the given set encoding (OBJ_ENCODING_*). */
const setTypeOps *setTypeGetOps(int encoding);

/* One instance per encoding, defined in the section of the encoding below. */
extern const setTypeOps setTypeOpsIntset;
extern const setTypeOps setTypeOpsListpack;
extern const setTypeOps setTypeOpsListpackEx;
extern const setTypeOps setTypeOpsHT;

/*=============================================================================
 * 2. Shared primitives of the set type
 * Encoding independent functions, which dispatch to the encoding through
 * setTypeOps (see setTypeGetOps()).
 *=============================================================================*/

/* Returns the setTypeOps table for the given OBJ_ENCODING_* value. This is
 * the single place that maps a set's encoding to its backend implementation;
 * every other function below dispatches through it instead of switching on
 * robj->encoding itself. */
const setTypeOps *setTypeGetOps(int encoding) {
    switch (encoding) {
    case OBJ_ENCODING_INTSET: return &setTypeOpsIntset;
    case OBJ_ENCODING_LISTPACK: return &setTypeOpsListpack;
    case OBJ_ENCODING_LISTPACK_EX: return &setTypeOpsListpackEx;
    case OBJ_ENCODING_HT: return &setTypeOpsHT;
    default: serverPanic("Unknown set encoding");
    }
}

/* Compute the UNION or DIFF (per 'op') of the 'setnum' sets named in 'setkeys'.
 *   dstkey          - if non-NULL, store the result into this key (…STORE) and
 *                     reply with its cardinality; otherwise reply to the client.
 *   cardinality_only - reply only with the result cardinality (SUNIONCARD),
 *                      without materializing the elements to the client.
 *   approx           - with cardinality_only, return an approximate cardinality
 *                      computed with a HyperLogLog (UNION only).
 *   limit            - with cardinality_only, stop once the cardinality reaches
 *                      'limit' (0 means no limit). */
void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,
                              robj *dstkey, int op,
                              int cardinality_only, int approx, long limit);

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a listpack
 * or a regular hash table.
 *
 * The size hint indicates approximately how many items will be added which is
 * used to determine the initial representation. */
robj *setTypeCreate(sds value, size_t size_hint) {
    if (isSdsRepresentableAsLongLong(value,NULL) == C_OK && size_hint <= server.set_max_intset_entries)
        return createIntsetObject();
    if (size_hint <= server.set_max_listpack_entries)
        return createSetListpackObject();

    /* We may oversize the set by using the hint if the hint is not accurate,
     * but we will assume this is acceptable to maximize performance. */
    robj *o = createSetObject();
    dictExpand(o->ptr, size_hint);
    return o;
}

/* Check if the existing set should be converted to another encoding based off the
 * the size hint. */
void setTypeMaybeConvert(robj *set, size_t size_hint) {
    if (((set->encoding == OBJ_ENCODING_LISTPACK || set->encoding == OBJ_ENCODING_LISTPACK_EX) &&
         size_hint > server.set_max_listpack_entries)
        || (set->encoding == OBJ_ENCODING_INTSET && size_hint > server.set_max_intset_entries))
    {
        setTypeConvertAndExpand(set, OBJ_ENCODING_HT, size_hint, 1);
    }
}

/* Return the maximum number of entries to store in an intset. */
static size_t intsetMaxEntries(void) {
    size_t max_entries = server.set_max_intset_entries;
    /* limit to 1G entries due to intset internals. */
    if (max_entries >= 1<<30) max_entries = 1<<30;
    return max_entries;
}

/* Converts intset to HT if it contains too many entries. */
static void maybeConvertIntset(robj *subject) {
    serverAssert(subject->encoding == OBJ_ENCODING_INTSET);
    if (intsetLen(subject->ptr) > intsetMaxEntries())
        setTypeConvert(subject,OBJ_ENCODING_HT);
}

/* When you know all set elements are integers, call this to convert the set to
 * an intset. No conversion happens if the set contains too many entries for an
 * intset. */
static void maybeConvertToIntset(robj *set) {
    if (set->encoding == OBJ_ENCODING_INTSET) return; /* already intset */
    if (setTypeSize(set) > intsetMaxEntries()) return; /* can't use intset */
    intset *is = intsetNew();
    char *str;
    size_t len = 0;
    int64_t llval = 0;
    setTypeIterator si;
    setTypeInitIterator(&si, set, SET_ITER_RAW);
    while (setTypeNext(&si, &str, &len, &llval) != -1) {
        if (str) {
            /* If the element is returned as a string, we may be able to convert
             * it to integer. This happens for OBJ_ENCODING_HT. */
            serverAssert(string2ll(str, len, (long long *)&llval));
        }
        uint8_t success = 0;
        is = intsetAdd(is, llval, &success);
        serverAssert(success);
    }
    setTypeResetIterator(&si);
    setTypeFree(set); /* frees the internals but not robj itself */
    set->ptr = is;
    set->encoding = OBJ_ENCODING_INTSET;
}

/* Add the specified sds value into a set.
 *
 * If the value was already member of the set, nothing is done and 0 is
 * returned, otherwise the new element is added and 1 is returned. */
int setTypeAdd(robj *subject, sds value) {
    return setTypeAddAux(subject, value, sdslen(value), 0, 1);
}

/* Add member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was added and 0 if it was already a member. */
int setTypeAddAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str && set->encoding != OBJ_ENCODING_INTSET) {
        /* Only intset can consume a bare integer directly; other encodings
         * need it stringified first. */
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    const setTypeOps *ops = setTypeGetOps(set->encoding);
    int64_t *llvalp = (str == NULL) || (str == tmpbuf) ? &llval : NULL;
    int target_enc, added = ops->rawAdd(set, str, len, llvalp, str_is_sds, 0, &target_enc);
    if (added == -1) {
        serverAssert(str); /* make sure a string value is valid before conversion */
        /* Doesn't fit under the current encoding: convert encoding and retry there. */
        setTypeConvertAndExpand(set, target_enc, ops->size(set) + 1, 1);
        added = setTypeGetOps(target_enc)->rawAdd(set, str, len, NULL, str_is_sds, 1, &target_enc);
        serverAssert(added == 1);
        return added;
    }

    /* Post-insert overflow check: an intset that grew past the configured
     * limit converts to a hash table. */
    if (added && set->encoding == OBJ_ENCODING_INTSET) maybeConvertIntset(set);
    return added;
}

/* Like setTypeAddAux(), and the member gets the given expiration, or none if it
 * is EB_EXPIRE_TIME_INVALID. The set must be able to hold member expirations
 * if an expiration is given. If the member exists already, 0 is returned and its
 * expiration is set to the given one. */
int setTypeAddExAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds, uint64_t expire) {
    if (expire == EB_EXPIRE_TIME_INVALID)
        return setTypeAddAux(set, str, len, llval, str_is_sds);

    serverAssert(setTypeHasExpireSupport(set));
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    const setTypeOps *ops = setTypeGetOps(set->encoding);
    int64_t *llvalp = (str == tmpbuf) ? &llval : NULL;
    int target_enc;
    int added = ops->rawAddEx(set, str, len, llvalp, str_is_sds, 0, &target_enc, expire);
    if (added == -1) {
        /* Doesn't fit under the current encoding: convert it, keeping the
         * expirations, and retry there. */
        serverAssert(str);
        setTypeConvertAndExpand(set, target_enc, ops->size(set) + 1, 1);
        added = setTypeGetOps(target_enc)->rawAddEx(set, str, len, NULL, str_is_sds, 1, &target_enc, expire);
        serverAssert(added == 1);
        return added;
    }
    if (added == 0)
        ops->setExpire(set, str, len, llval, str_is_sds, expire);
    return added;
}

/* Like setTypeIsMemberAux(), but reports the expiration of the member. Returns
 * 1 if the member exists (even if it is logically expired), 0 otherwise. The
 * expiration is EB_EXPIRE_TIME_INVALID if the member has none. */
int setTypeGetExpireAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds, uint64_t *expire) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str && set->encoding != OBJ_ENCODING_INTSET) {
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    const setTypeOps *ops = setTypeGetOps(set->encoding);
    if (!setTypeHasExpireSupport(set)) {
        *expire = EB_EXPIRE_TIME_INVALID;
        return ops->isMember(set, str, len, llval, str_is_sds);
    }
    return ops->getExpire(set, str, len, llval, str_is_sds, expire);
}

/* Adds a member to the destination set of a STORE command, merging the
 * expiration with member's expiration if it is there already. In the expiration
 * merge process, a source without an expiration is ignored: the member gets the
 * one of the expiration values from sources that have expiration. If more than
 * one source has expiration, the values is set by the 'farthest' flag:  the
 * nearest value if it is false (as in SINTERSTORE), or the farthest with if it
 * is true (as in SUNIONSTORE).
 * Returns 1 if the member was added, 0 if it was already there. */
static int setTypeAddMergeExpire(robj *dst, char *str, size_t len, int64_t llval,
                                 int str_is_sds, uint64_t expire, int farthest)
{
    if (expire == EB_EXPIRE_TIME_INVALID)
        return setTypeAddAux(dst, str, len, llval, str_is_sds);

    if (!setTypeHasExpireSupport(dst)) setTypeConvertToExpireEncoding(dst);
    uint64_t cur;
    if (setTypeGetExpireAux(dst, str, len, llval, str_is_sds, &cur) &&
        cur != EB_EXPIRE_TIME_INVALID && (farthest ? cur >= expire : cur <= expire))
        return 0;
    return setTypeAddExAux(dst, str, len, llval, str_is_sds, expire);
}

/* Deletes a value provided as an sds string from the set. Returns 1 if the
 * value was deleted and 0 if it was not a member of the set. */
int setTypeRemove(robj *setobj, sds value) {
    return setTypeRemoveAux(setobj, value, sdslen(value), 0, 1);
}

/* Remove a member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was deleted and 0 if it was not a member of the set. */
int setTypeRemoveAux(robj *setobj, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str && setobj->encoding != OBJ_ENCODING_INTSET) {
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    return setTypeGetOps(setobj->encoding)->rawRemove(setobj, str, len, llval, str_is_sds);
}

/* Check if an sds string is a member of the set. Returns 1 if the value is a
 * member of the set and 0 if it isn't. */
int setTypeIsMember(robj *subject, sds value) {
    return setTypeIsMemberAux(subject, value, sdslen(value), 0, 1);
}

/* Membership checking optimized for the different encodings. The value can be
 * provided as an sds string (indicated by passing str_is_sds = 1), as string
 * and length (str_is_sds = 0) or as an integer in which case str is set to NULL
 * and llval is provided instead.
 *
 * Returns 1 if the value is a member of the set and 0 if it isn't. */
int setTypeIsMemberAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str && set->encoding != OBJ_ENCODING_INTSET) {
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    const setTypeOps *ops = setTypeGetOps(set->encoding);
    if (unlikely(setTypeHasExpireSupport(set))) {
        /* A member that is logically expired but not removed yet is not a member
         * for the callers of this function. It is not removed either. */
        uint64_t expire;
        if (!ops->getExpire(set, str, len, llval, str_is_sds, &expire)) return 0;
        return !setTypeExpireTimeElapsed(expire);
    }
    return ops->isMember(set, str, len, llval, str_is_sds);
}

void setTypeInitIterator(setTypeIterator *si, robj *subject, int mode) {
    si->subject = subject;
    si->mode = mode;
    si->expire = EB_EXPIRE_TIME_INVALID;
    si->encoding = subject->encoding;
    si->typeOps = setTypeGetOps(si->encoding);
    si->typeOps->iterInit(si);
}

void setTypeResetIterator(setTypeIterator *si) {
    si->subject = NULL;
    si->typeOps->iterReset(si);
}

/* Move to the next entry in the set. Returns the object at the current
 * position, as a string or as an integer.
 *
 * Since set elements can be internally be stored as SDS strings, char buffers or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointers
 * (str and len) or (llele) depending on whether the value is stored as a string
 * or as an integer internally.
 *
 * If OBJ_ENCODING_HT is returned, then str points to an sds string and can be
 * used as such. If OBJ_ENCODING_INTSET, then llele is populated and str is
 * pointed to NULL. If OBJ_ENCODING_LISTPACK is returned, the value can be
 * either a string or an integer. If *str is not NULL, then str and len are
 * populated with the string content and length. Otherwise, llele populated with
 * an integer value.
 *
 * Note that str, len and llele pointers should all be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused.
 *
 * When there are no more elements -1 is returned. */
int setTypeNext(setTypeIterator *si, char **str, size_t *len, int64_t *llele) {
    while (si->typeOps->iterNext(si, str, len, llele) != -1) {
        /* In skip-expired mode, the members that are logically expired but not
         * yet removed are not returned (and not removed either). */
        if (si->expire != EB_EXPIRE_TIME_INVALID &&
            si->mode == SET_ITER_SKIP_EXPIRED &&
            setTypeExpireTimeElapsed(si->expire))
            continue;
        return si->encoding;
    }
    return -1;
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new SDS
 * strings. So if you don't retain a pointer to this object you should call
 * sdsfree() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue. */
sds setTypeNextObject(setTypeIterator *si) {
    int64_t intele = 0;
    char *str;
    size_t len = 0;

    if (setTypeNext(si, &str, &len, &intele) == -1) return NULL;
    if (str != NULL) return sdsnewlen(str, len);
    return sdsfromlonglong(intele);
}

/* Return random element from a non empty set.
 * The returned element can be an int64_t value if the set is encoded
 * as an "intset" blob of integers, or an string.
 *
 * The caller provides three pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and can be used by the caller to check if the
 * int64_t pointer or the str and len pointers were populated, as for
 * setTypeNext. If OBJ_ENCODING_HT is returned, str is pointed to a
 * string which is actually an sds string and it can be used as such.
 *
 * Note that both the str, len and llele pointers should be passed and cannot
 * be NULL. If str is set to NULL, the value is an integer stored in llele. */
int setTypeRandomElement(robj *setobj, char **str, size_t *len, int64_t *llele) {
    setTypeGetOps(setobj->encoding)->randomElement(setobj, str, len, llele);
    return setobj->encoding;
}

/* Pops a random element and returns it as an object. */
robj *setTypePopRandom(robj *set) {
    robj *obj;
    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        /* Find random and delete it without re-seeking the listpack. */
        unsigned int i = 0;
        unsigned char *p = lpNextRandom(set->ptr, lpFirst(set->ptr), &i, 1, 1);
        unsigned int len = 0; /* initialize to silence warning */
        long long llele = 0; /* initialize to silence warning */
        char *str = (char *)lpGetValue(p, &len, &llele);
        if (str)
            obj = createStringObject(str, len);
        else
            obj = createStringObjectFromLongLong(llele);
        set->ptr = lpDelete(set->ptr, p, NULL);
    } else {
        char *str;
        size_t len = 0;
        int64_t llele = 0;
        int encoding = setTypeRandomElement(set, &str, &len, &llele);
        if (str)
            obj = createStringObject(str, len);
        else
            obj = createStringObjectFromLongLong(llele);
        setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HT);
    }
    return obj;
}

unsigned long setTypeSize(const robj *subject) {
    return setTypeGetOps(subject->encoding)->size(subject);
}

size_t setTypeAllocSize(const robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_SET);
    return setTypeGetOps(o->encoding)->allocSize(o);
}

/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hash table) is presized to hold the number of elements in the original
 * set. */
void setTypeConvert(robj *setobj, int enc) {
    setTypeConvertAndExpand(setobj, enc, setTypeSize(setobj), 1);
}

/* Converts a set to the specified encoding, pre-sizing it for 'cap' elements.
 * The 'panic' argument controls whether to panic on OOM (panic=1) or return
 * C_ERR on OOM (panic=0). If panic=1 is given, this function always returns
 * C_OK. */
int setTypeConvertAndExpand(robj *setobj, int enc, unsigned long cap, int panic) {
    serverAssertWithInfo(NULL,setobj,setobj->type == OBJ_SET &&
                             setobj->encoding != enc);
    if (enc != OBJ_ENCODING_HT && enc != OBJ_ENCODING_LISTPACK &&
        enc != OBJ_ENCODING_LISTPACK_EX)
    {
        serverPanic("Unsupported set conversion");
    }
    /* A set with member expirations keeps them: it can only grow to a hashtable. */
    serverAssert(setobj->encoding != OBJ_ENCODING_LISTPACK_EX || enc == OBJ_ENCODING_HT);

    void *newptr = setTypeGetOps(enc)->convertFrom(setobj, cap, panic);
    if (newptr == NULL) {
        serverAssert(!panic);
        return C_ERR;
    }

    /* The set is registered in db->subexpires through the ExpireMeta that lives in
     * its encoding, which is freed here: unregister it, and register it again with
     * the new one. Only sets that are in a database are registered, and the
     * command that converts it is the one executing. */
    uint64_t registered = EB_EXPIRE_TIME_INVALID;
    redisDb *regdb = NULL;
    if (setobj->encoding == OBJ_ENCODING_LISTPACK_EX || setobj->encoding == OBJ_ENCODING_HT) {
        if (setHasSubexpiry(setobj))
            registered = ebGetExpireTime(&subexpiresBucketsType, setobj);
        if (registered != EB_EXPIRE_TIME_INVALID) {
            serverAssert(server.executing_client && server.executing_client->db);
            regdb = server.executing_client->db;
            estoreRemove(regdb->subexpires, getKeySlot(kvobjGetKey(setobj)), setobj);
        }
    }

    setTypeFree(setobj); /* frees the internals but not setobj itself */
    setobj->encoding = enc;
    setobj->ptr = newptr;
    if (regdb) setTypeUpdateSubexpiry(regdb, setobj);
    return C_OK;
}

/* This is a helper function for the COPY command.
 * Duplicate a set object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *setTypeDup(robj *o) {
    serverAssert(o->type == OBJ_SET);
    return setTypeGetOps(o->encoding)->dup(o);
}

/* Frees the internals of a set object (but not the robj itself). Called by
 * freeSetObject() in object.c, mirroring how freeHashObject() there just
 * delegates to hashTypeFree() in t_hash.c. */
void setTypeFree(robj *o) {
    setTypeGetOps(o->encoding)->free(o);
}

/*=============================================================================
 * 2b. Member expiration: encoding independent logic
 * Set member expiration (SME). Every set can hold member expirations: a set that
 * cannot as it is, is converted to the listpack with expirations or to a hashtable
 * (see setTypeConvertToExpireEncoding()).
 *=============================================================================*/

/* Retrieve the ExpireMeta associated with the set, used by db->subexpires.
 * The caller is responsible for ensuring that it is indeed attached. */
ExpireMeta *setGetExpireMeta(const eItem set) {
    const robj *o = (const robj *)set;
    if (o->encoding == OBJ_ENCODING_LISTPACK_EX)
        return setListpackExGetExpireMeta(o);
    if (o->encoding == OBJ_ENCODING_HT)
        return setHashtableGetExpireMeta(o);
    serverPanic("Unexpected set encoding in subexpires: %d", o->encoding);
}

/* Returns the earliest member expiration time of the set, or
 * EB_EXPIRE_TIME_INVALID if no member has one. 'accurate' requests an exact
 * answer instead of the cached one. */
uint64_t setTypeGetMinExpire(robj *o, int accurate) {
    serverAssert(o->type == OBJ_SET);
    if (!setTypeHasExpireSupport(o)) return EB_EXPIRE_TIME_INVALID;
    return setTypeGetOps(o->encoding)->minExpire(o, accurate);
}

/* Returns 1 if the set currently has an ExpireMeta attached and can be
 * registered in db->subexpires. */
int setHasSubexpiry(const kvobj *o) {
    serverAssert(o->type == OBJ_SET);
    return o->encoding == OBJ_ENCODING_LISTPACK_EX ||
           (o->encoding == OBJ_ENCODING_HT && setHashtableHasExpire(o));
}

/* Returns 1 if the set is registered in db->subexpires, which then holds a
 * pointer to the key object (the active defrag must update it when it moves). */
int setIsRegisteredInSubexpires(const kvobj *set) {
    return setHasSubexpiry(set) &&
           ebGetExpireTime(&subexpiresBucketsType, (kvobj *)set) != EB_EXPIRE_TIME_INVALID;
}

/* Returns 1 if the set can hold member expirations as it is: it is a listpack
 * with expirations, or a hashtable that has the metadata of the expirations. */
int setTypeHasExpireSupport(const robj *set) {
    if (set->encoding == OBJ_ENCODING_HT)
        return setHashtableHasExpire(set);
    return setTypeGetOps(set->encoding)->getExpire != NULL;
}

/* Returns 1 if an expiration time (absolute, in milliseconds) has passed and the
 * member it belongs to must be treated as if it did not exist. Like for hash
 * fields, the expiration time itself is not yet expired, and nothing is expired
 * when access to expired data is allowed (a debug switch). */
int setTypeExpireTimeElapsed(uint64_t expire) {
    if (expire == EB_EXPIRE_TIME_INVALID) return 0;
    if (server.allow_access_expired) return 0;
    return expire < (uint64_t)commandTimeSnapshot();
}

/* Looks up a member. Returns 0 if it does not exist and 1 if it does, and then
 * sets *expire to its expiration time, EB_EXPIRE_TIME_INVALID if it has none.
 * A member that is logically expired but not removed yet is found. */
int setTypeGetExpire(robj *set, sds member, uint64_t *expire) {
    if (!setTypeHasExpireSupport(set)) {
        *expire = EB_EXPIRE_TIME_INVALID;
        return setTypeIsMember(set, member);
    }
    return setTypeGetOps(set->encoding)->getExpire(set, member, sdslen(member), 0, 1, expire);
}

/* Returns 1 if the set can hold member expirations now or after being converted.
 * Every set can: a hashtable gets the metadata of the expirations, and a set that
 * is too big for a listpack is converted to a hashtable. */
int setTypeCanHoldExpire(const robj *set) {
    UNUSED(set);
    return 1;
}

/* Prepares a set to hold member expirations: a set that cannot hold them as it is
 * (an intset or a listpack) is converted to the listpack with expirations
 * encoding if its members fit in a listpack, or to a hashtable otherwise, and a
 * hashtable gets the metadata of the expirations. */
void setTypeConvertToExpireEncoding(robj *set) {
    if (setTypeHasExpireSupport(set)) return;

    if (set->encoding != OBJ_ENCODING_HT) {
        /* The listpack limits apply: members count, not listpack elements. */
        int fitsListpack = setTypeSize(set) <= server.set_max_listpack_entries;
        /* An intset member is at most 20 characters long. */
        if (set->encoding == OBJ_ENCODING_INTSET && server.set_max_listpack_value < 20)
            fitsListpack = 0;
        if (fitsListpack) {
            setTypeConvertAndExpand(set, OBJ_ENCODING_LISTPACK_EX, setTypeSize(set), 1);
            return;
        }
        setTypeConvert(set, OBJ_ENCODING_HT);
    }
    setHashtableAddExpireSupport(set);
}

/*-----------------------------------------------------------------------------
 * Flags and reply codes
 *----------------------------------------------------------------------------*/

/* Conditions of the SEXPIRE family (evaluated per member). */
#define SME_NX       (1<<0) /* Set only if the member has no expiration */
#define SME_XX       (1<<1) /* Set only if the member has an expiration */
#define SME_GT       (1<<2) /* Set only if the new expiration is greater */
#define SME_LT       (1<<3) /* Set only if the new expiration is less */

/* Flags of SADDEX. */
#define SME_EX       (1<<0) /* Expiration time in seconds */
#define SME_PX       (1<<1) /* Expiration time in milliseconds */
#define SME_EXAT     (1<<2) /* Expiration time in unix seconds */
#define SME_PXAT     (1<<3) /* Expiration time in unix milliseconds */
#define SME_KEEPTTL  (1<<4) /* Do not discard the member ttl on add */
#define SME_MXX      (1<<5) /* Add only if all the members already exist */
#define SME_MNX      (1<<6) /* Add only if none of the members exist */

/* Reply codes per member, the same as the hash field expiration commands. */
#define SME_NO_MEMBER   (-2) /* The member does not exist */
#define SME_NO_TTL      (-1) /* The member exists but has no expiration */
#define SME_COND_NOT_MET 0   /* The condition (NX, XX, GT, LT) was not met */
#define SME_UPDATED      1   /* The expiration was set or updated */
#define SME_DELETED      2   /* The member was deleted: the time is in the past */
#define SME_PERSISTED    1   /* The expiration was removed */

/*-----------------------------------------------------------------------------
 * Helpers
 *----------------------------------------------------------------------------*/

#define SME_STACK_SIZE 64

/* A vec with an embedded stack buffer, used to collect the member robj pointers
 * for subkey notifications without heap allocation in the common case. */
typedef struct memvec { vec v; void *buf[SME_STACK_SIZE]; } memvec;

static inline vec *memvecInit(memvec *mv, size_t cap) {
    vecInit(&mv->v, mv->buf, SME_STACK_SIZE);
    vecReserve(&mv->v, cap);
    return &mv->v;
}

/* Propagates the removal of a member as an explicit SREM, whatever the reason
 * for the removal is (an expiration time in the past, lazy or active expiry). A
 * replica never decides on its own that a member expired, it only applies the
 * SREM of its master. */
static void propagateSetMemberDeletion(redisDb *db, sds key, char *member, size_t len) {
    robj *argv[] = {
        shared.srem,
        createStringObject((char*) key, sdslen(key)),
        createStringObject(member, len)
    };

    enterExecutionUnit(1, 0);
    /* The expiration is decided by the server, so it must be propagated even if
     * the command that triggered it asked not to propagate. */
    alsoPropagateForced(db->id, argv, 3, PROPAGATE_AOF|PROPAGATE_REPL);
    exitExecutionUnit();
    postExecutionUnitOperations();

    decrRefCount(argv[1]);
    decrRefCount(argv[2]);
}

/* The reply for a member that is logically expired but was not removed yet:
 * treated as if the member did not exist. */
static inline int setMemberIsGone(int exists, uint64_t expire) {
    return !exists || (expire != EB_EXPIRE_TIME_INVALID && (long long)expire < commandTimeSnapshot());
}

/*-----------------------------------------------------------------------------
 * Lazy expiry
 *
 * Like for hash fields, there is no sweep before a command: a command that
 * touches a member checks whether it expired, and a command that samples
 * members removes all the expired ones first.
 *----------------------------------------------------------------------------*/

/* Returns 1 if this node must not remove expired members itself right now: a
 * replica waits for the SREM of its master, nothing is removed while loading or
 * while the expiration actions are paused, and a master link is trusted. *deletable
 * is set to 0 if the expired member must be reported as missing but kept, and
 * *trusted to 1 if it must be reported as present (the master link applying
 * commands the master already decided on). */
static void setLazyExpiryGuard(int *deletable, int *trusted) {
    *deletable = 1;
    *trusted = 0;
    if (server.masterhost || server.cluster_enabled) {
        /* If CLIENT_MASTER, assume valid as long as it was not deleted. In
         * cluster mode, also while importing data from the source (a fake master
         * client with the CLIENT_MASTER flag), to not delete members still in use. */
        if (server.current_client && (server.current_client->flags & CLIENT_MASTER)) {
            *trusted = 1;
            return;
        }
        /* For a replica, if a user client, act as if expired but do not delete. */
        if (server.masterhost) {
            *deletable = 0;
            return;
        }
    }
    if (server.loading || isPausedActionsWithUpdate(PAUSE_ACTION_EXPIRE))
        *deletable = 0;
}

/* Removes a member that was found logically expired: removes it, propagates the
 * SREM, and does the bookkeeping, as per the flags. Deletes the key if the set
 * became empty, unless SET_LAZY_AVOID_SET_DEL. Returns 1 if the key was deleted. */
static int setLazyDeleteExpiredMember(redisDb *db, kvobj *set, sds member, int flags) {
    size_t oldsize = 0;
    sds key = kvobjGetKey(set);
    int64_t oldlen = (int64_t)setTypeSize(set);

    if (server.memory_tracking_enabled && !(flags & SET_LAZY_NO_UPDATE_ALLOCSIZES))
        oldsize = kvobjAllocSize(set);
    serverAssert(setTypeRemove(set, member) == 1);
    if (server.memory_tracking_enabled && !(flags & SET_LAZY_NO_UPDATE_ALLOCSIZES))
        updateSlotAllocSize(db, getKeySlot(key), set, oldsize, kvobjAllocSize(set));
    propagateSetMemberDeletion(db, key, member, sdslen(member));
    server.stat_expired_subkeys++;

    if (!(flags & SET_LAZY_NO_UPDATE_KEYSIZES))
        updateKeysizesHist(db, OBJ_SET, oldlen, oldlen - 1);

    int deleted = 0;
    robj *keyObj = createStringObject(key, sdslen(key));
    unsigned long length = setTypeSize(set);
    if (length != 0 && !(flags & SET_LAZY_NO_NOTIFICATION)) {
        robj mobj, *marr[1] = {&mobj};
        initStaticStringObject(mobj, member);
        notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "sexpired", keyObj, db->id, marr, 1);
    }
    if (length == 0 && !(flags & SET_LAZY_AVOID_SET_DEL)) {
        if (!(flags & SET_LAZY_NO_NOTIFICATION))
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", keyObj, db->id);
        dbDelete(db, keyObj);
        set = NULL;
        deleted = 1;
    }
    keyModified(NULL, db, keyObj, set, !(flags & SET_LAZY_NO_SIGNAL));
    decrRefCount(keyObj);
    return deleted;
}

/* Checks whether a member is in the set, removing it if it is logically expired
 * and this node is allowed to. Returns 1 if the member is in the set, 0 if it is
 * not or is expired. *setDeleted (if not NULL) is set to 1 if the whole key got
 * deleted, because the expired member was the last one; the set must not be used
 * afterwards. This function also updates the allocation size accounting (a read
 * can move memory, for example by rehashing) unless suppressed. */
int setTypeIsMemberLazy(redisDb *db, kvobj *set, sds member, int flags, int *setDeleted) {
    size_t oldsize = 0;
    const int track = server.memory_tracking_enabled && !(flags & SET_LAZY_NO_UPDATE_ALLOCSIZES);
    if (setDeleted) *setDeleted = 0;
    if (track) oldsize = kvobjAllocSize(set);

    uint64_t expire = EB_EXPIRE_TIME_INVALID;
    int res = setTypeGetExpire(set, member, &expire);
    int remove = 0;

    if (res && setTypeExpireTimeElapsed(expire)) {
        int deletable, trusted;
        setLazyExpiryGuard(&deletable, &trusted);
        if (trusted) {
            res = 1;
        } else {
            res = 0;
            remove = deletable;
        }
    }

    if (remove) {
        int deleted = setLazyDeleteExpiredMember(db, set, member,
                                                 flags | SET_LAZY_NO_UPDATE_ALLOCSIZES);
        /* The accounting is updated before the key is deleted, if it is. */
        if (track && !deleted)
            updateSlotAllocSize(db, getKeySlot(kvobjGetKey(set)), set, oldsize, kvobjAllocSize(set));
        if (setDeleted) *setDeleted = deleted;
        return 0;
    }

    if (track)
        updateSlotAllocSize(db, getKeySlot(kvobjGetKey(set)), set, oldsize, kvobjAllocSize(set));
    return res;
}

/* Callback of the expire() op: propagates the removal of a member and collects
 * it for the keyspace event. */
typedef struct SetExpireCtx {
    redisDb *db;
    sds key;
    vec *vexpired;     /* Collects the expired members, NULL if not needed */
    int activeEx;      /* The expiration is done by the active expire cycle */
} SetExpireCtx;

static void setExpireMemberCb(void *c, char *str, size_t len, int64_t llval) {
    SetExpireCtx *ctx = c;
    char buf[LONG_STR_SIZE];
    if (str == NULL) {
        len = ll2string(buf, sizeof(buf), llval);
        str = buf;
    }
    if (ctx->vexpired)
        vecPush(ctx->vexpired, createStringObject(str, len));
    propagateSetMemberDeletion(ctx->db, ctx->key, str, len);
    server.stat_expired_subkeys++;
    if (ctx->activeEx) server.stat_expired_subkeys_active++;
}

/* Deletes the expired members of the set, up to the quota, and deletes the set
 * if it is left empty.
 *
 * Returns the next expiration time of the set:
 * - 0 if the set was deleted
 * - EB_EXPIRE_TIME_INVALID if no more members have an expiration */
uint64_t setTypeExpire(redisDb *db, kvobj *set, uint32_t *quota, int updateSubexpires, int activeEx) {
    const setTypeOps *ops = setTypeGetOps(set->encoding);
    serverAssert(ops->expire != NULL);
    sds keystr = kvobjGetKey(set);

    /* Collect the expired members for a batched subkey notification, unless the
     * subkey notifications are disabled. */
    memvec mvexpired;
    vec *vexpired = isSubkeyNotifyEnabled(NOTIFY_SET) ?
                        memvecInit(&mvexpired, SME_STACK_SIZE) : NULL;
    SetExpireCtx ctx = {.db = db, .key = keystr, .vexpired = vexpired, .activeEx = activeEx};

    size_t oldsize = 0;
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(set);
    int64_t oldlen = (int64_t)setTypeSize(set);

    unsigned long expired = ops->expire(set, (uint64_t)commandTimeSnapshot(), *quota,
                                        setExpireMemberCb, &ctx);
    *quota -= expired;

    uint64_t res = EB_EXPIRE_TIME_INVALID;
    if (expired) {
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(db, getKeySlot(keystr), set, oldsize, kvobjAllocSize(set));
        int64_t newlen = (int64_t)setTypeSize(set);
        updateKeysizesHist(db, OBJ_SET, oldlen, newlen);

        robj *key = createStringObject(keystr, sdslen(keystr));
        notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "sexpired", key, db->id,
            vexpired ? (robj**)vecData(vexpired) : NULL, vexpired ? vecSize(vexpired) : 0);

        int deleted = 0;
        if (newlen == 0) {
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, db->id);
            dbDelete(db, key);
            res = 0;
            deleted = 1;
        }
        keyModified(NULL, db, key, deleted ? NULL : set, 1);
        decrRefCount(key);
        if (!deleted) {
            res = ops->minExpire(set, 1);
            /* Not for the active expiry cycle: it updates the registration itself,
             * with the time that this function returns. */
            if (updateSubexpires) setTypeUpdateSubexpiry(db, set);
        }
    } else {
        res = ops->minExpire(set, 1);
    }

    if (vexpired) {
        for (size_t i = 0; i < vecSize(vexpired); i++)
            decrRefCount(vecGet(vexpired, i));
        vecRelease(vexpired);
    }
    return res;
}

/* Deletes all the expired members of the set (and the set itself if it is left
 * empty) unless this node must not delete expired data right now. This is a
 * sweep, used by the commands that sample members, to not pick among many
 * expired members. Returns 1 if the whole set was deleted.
 *
 * Known limitation, as of HRANDFIELD for the hash fields: if this node must not
 * delete expired data right now (a replica, loading, expirations paused, or
 * access to expired data allowed), nothing is swept and the commands that
 * sample (SPOP, SRANDMEMBER) can pick a logically expired member. */
int setTypeExpireIfNeeded(redisDb *db, kvobj *set) {
    const setTypeOps *ops = setTypeGetOps(set->encoding);
    if (ops->minExpire == NULL) return 0;

    uint64_t minExpire = ops->minExpire(set, 1);
    /* Nothing to expire */
    if (minExpire == EB_EXPIRE_TIME_INVALID || (mstime_t)minExpire >= commandTimeSnapshot())
        return 0;

    /* Follow the conditions of when not to lazy-expire a key. */
    if (server.loading || server.allow_access_expired || server.masterhost ||
        isPausedActionsWithUpdate(PAUSE_ACTION_EXPIRE))
        return 0;

    uint32_t quota = UINT32_MAX;
    return setTypeExpire(db, set, &quota, 1, 0) == 0;
}

/* Registers the set in db->subexpires with its earliest member expiration, or
 * updates or removes the registration, to match the set as it is now. Call it
 * after a change that can make a member expire earlier than the registered time,
 * and when the set may not be registered yet (a new TTL, a new object).
 *
 * Changes that can only make the earliest expiration later (removing a member,
 * lazy expiry) do not need it: the active expiry cycle visits the set at the
 * registered time, finds nothing to expire or less than expected, and registers
 * the right time. */
void setTypeUpdateSubexpiry(redisDb *db, kvobj *set) {
    uint64_t current = setTypeGetMinExpire(set, 1);
    uint64_t registered = EB_EXPIRE_TIME_INVALID;
    int slot = getKeySlot(kvobjGetKey(set));

    if (setHasSubexpiry(set))
        registered = ebGetExpireTime(&subexpiresBucketsType, set);
    if (current == registered) return;

    if (registered == EB_EXPIRE_TIME_INVALID)
        estoreAdd(db->subexpires, slot, set, current);
    else if (current == EB_EXPIRE_TIME_INVALID)
        estoreRemove(db->subexpires, slot, set);
    else
        estoreUpdate(db->subexpires, slot, set, current);
}

/*=============================================================================
 * 3a. Encoding: intset
 * Backend of the intset encoding (OBJ_ENCODING_INTSET).
 *=============================================================================*/

/* Decides whether the set is still small enough to become a
 * listpack, or must go straight to a hash table. */
static int isResolveEncodingForAdd(robj *set, size_t len) {
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

/* A NULL str means the caller already has the value as an integer (llval)
 * and there's nothing to parse. This is intset's only encoding-selection
 * fast path: it's the sole encoding that can consume a bare integer
 * without ever looking at a string form of it. */
static int isRawAdd(robj *set, char *str, size_t len, int64_t *llvalp, int str_is_sds, int after_convert, int *target_enc) {
    UNUSED(str_is_sds);
    UNUSED(after_convert);
    long long value;
    if (str == NULL) {
        serverAssert(llvalp);
        value = *llvalp;
    } else if (!string2ll(str, len, &value)) {
        /* Not representable as an integer: intset can't hold it at all. */
        *target_enc = isResolveEncodingForAdd(set, len);
        return -1;
    }
    *target_enc = OBJ_ENCODING_INTSET;
    uint8_t success = 0;
    set->ptr = intsetAdd(set->ptr, value, &success);
    return success ? 1 : 0;
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

static void isFree(robj *set) {
    zfree(set->ptr);
}

const setTypeOps setTypeOpsIntset = {
    .rawAdd = isRawAdd,
    .rawRemove = isRawRemove,
    .isMember = isIsMember,
    .iterInit = isIterInit,
    .iterReset = isIterReset,
    .iterNext = isIterNext,
    .randomElement = isRandomElement,
    .size = isSize,
    .allocSize = isAllocSize,
    .dup = isSetDup,
    .free = isFree,
    .convertFrom = NULL, /* .convertFrom intentionally left NULL: setTypeConvertAndExpand()
                          * never targets intset (see maybeConvertToIntset()). */
};

/*=============================================================================
 * 3b. Encoding: listpack
 * Backend of the listpack encoding (OBJ_ENCODING_LISTPACK).
 *=============================================================================*/

void setTypeListpackShrinkToFit(robj *set) {
    set->ptr = lpShrinkToFit(set->ptr);
}

static int lpRawAdd(robj *set, char *str, size_t len, int64_t *llvalp, int str_is_sds, int after_convert, int *target_enc) {
    UNUSED(str_is_sds);
    unsigned char *lp = set->ptr;
    *target_enc = OBJ_ENCODING_LISTPACK;
    if (!after_convert) {
        /* If this is a normal add (not after a conversion), check if the value
         * is already a member. */
        unsigned char *p = lpFirst(lp);
        if (p != NULL) p = lpFind(lp, p, (unsigned char *)str, len, 0);
        if (p != NULL) {
            /* Already a member. */
            return 0;
        }
    }
    if (lpLength(lp) < server.set_max_listpack_entries && len <= server.set_max_listpack_value &&
        lpSafeToAdd(lp, len))
    {
        if (llvalp) {
            /* If the value is a valid integer, we can add it as an integer
             * to save space. */
            set->ptr = lpAppendInteger(lp, *llvalp);
        } else {
            /* Otherwise, add it as a string. */
            set->ptr = lpAppend(lp, (unsigned char *)str, len);
        }

        if (after_convert) {
            /* Only reachable when converting from intset, whose capacity
             * estimate for the listpack it builds is an upper bound; shrink
             * it down to its actual content now that we're done growing it. */
            setTypeListpackShrinkToFit(set);
        }

        return 1;
    }
    /* Size limit reached: the caller must convert to a bigger encoding
     * (see resolveEncodingForAdd) and retry there. */
    *target_enc = OBJ_ENCODING_HT;
    return -1;
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

static void lpSetFree(robj *set) {
    zfree(set->ptr);
}

static void *lpConvertFrom(robj *set, unsigned long cap, int panic) {
    UNUSED(panic); /* lpNew() always panics on OOM, regardless of 'panic'. */

    /* Preallocate the minimum two bytes per element (enc/value + backlen) */
    size_t byteCap = cap * 2;
    if (set->encoding == OBJ_ENCODING_INTSET && intsetLen(set->ptr) > 0) {
        /* If we're converting from intset, we have a better estimate. */
        size_t s1 = lpEstimateBytesRepeatedInteger(intsetMin(set->ptr), cap);
        size_t s2 = lpEstimateBytesRepeatedInteger(intsetMax(set->ptr), cap);
        byteCap = max(s1, s2);
    }

    unsigned char *lp = lpNew(byteCap);
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
    }
    setTypeResetIterator(&si);
    return lp;
}

const setTypeOps setTypeOpsListpack = {
    .rawAdd = lpRawAdd,
    .rawRemove = lpRawRemove,
    .isMember = lpIsMember,
    .iterInit = lpIterInit,
    .iterReset = lpIterReset,
    .iterNext = lpIterNext,
    .randomElement = lpRandomElement,
    .size = lpSize,
    .allocSize = lpAllocSize,
    .dup = lpSetDup,
    .free = lpSetFree,
    .convertFrom = lpConvertFrom,
};

/*=============================================================================
 * 3c. Encoding: listpack with member expirations (listpackex)
 *=============================================================================*/

/* Backend of the listpackex encoding (OBJ_ENCODING_LISTPACK_EX).
 *
 * The set object points to a listpackEx (the wrapper that holds the ExpireMeta
 * used to register the set in db->subexpires, and the listpack). The listpack
 * holds (member, ttl) pairs ordered by ttl. A member without a ttl has the
 * ttl LPEX_NO_TTL (0) and is placed after all the members that have one. This
 * way the member that expires first is always the first one, and the expired
 * members are always a prefix of the listpack. The ttl is an absolute UNIX time
 * in milliseconds, stored as an integer, so a member without a ttl costs two
 * more bytes. */

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

/* Creates an empty set object encoded as a listpack with expirations. */
robj *createSetListpackExObject(void) {
    robj *o = createObject(OBJ_SET, lpexCreateWrapper(lpNew(0)));
    o->encoding = OBJ_ENCODING_LISTPACK_EX;
    return o;
}

/* Returns the listpack of a listpack-with-expiration set, as saved in the RDB. */
unsigned char *setListpackExGetLp(const robj *set) {
    return lpexOf(set)->lp;
}

/* Makes 'set' (a new object, without a value) a listpack with expirations that
 * owns 'lp'. */
void setListpackExAttach(robj *set, unsigned char *lp) {
    set->type = OBJ_SET;
    set->encoding = OBJ_ENCODING_LISTPACK_EX;
    set->ptr = lpexCreateWrapper(lp);
}

/* Validates a listpack loaded from an RDB file or a DUMP payload. In the deep
 * mode every element is checked and the order of the expirations too: the
 * members that have one come first, by increasing expiration, then the ones
 * that do not. The other code relies on this order. */
int setListpackExValidate(unsigned char *lp, size_t size, int deep) {
    if (!lpValidateIntegrityAndDups(lp, size, deep, 2)) return 0;
    if (!deep) return 1;

    uint64_t prev = 0;
    int no_ttl_seen = 0;
    unsigned char *p = lpFirst(lp);
    while (p) {
        unsigned char *t = lpNext(lp, p);
        long long ttl;
        if (t == NULL || !lpGetIntegerValue(t, &ttl)) return 0;
        if (ttl < 0 || (uint64_t)ttl > EB_EXPIRE_TIME_MAX) return 0;
        if (ttl == LPEX_NO_TTL) {
            no_ttl_seen = 1;
        } else {
            if (no_ttl_seen || (uint64_t)ttl < prev) return 0;
            prev = ttl;
        }
        p = lpNext(lp, t);
    }
    return 1;
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

/*=============================================================================
 * 3d. Encoding: hashtable
 * Backend of the hashtable (dict) encoding (OBJ_ENCODING_HT), with or without
 * member expirations.
 *=============================================================================*/

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
    size_t *alloc_size = htGetMetadataSize(d);

    /* A dict without the metadata of the expirations holds only plain members
     * (they are SetEntry only once the dict has the metadata, see
     * htRawAddCommon()), so there is nothing to unlink: the common case. */
    if (!htHasExpire(d)) {
        *alloc_size -= sdsAllocSize(entry);
        sdsfree(entry);
        return;
    }

    size_t usable;

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

/*=============================================================================
 * 4. Set commands
 *=============================================================================*/

/* When estimating cardinality with HLL and a LIMIT is set, this is the minimum
 * number of additional elements to process between two hllCount() checks. */
#define HLL_CHECK_INTERVAL_FLOOR 1024

void saddCommand(client *c) {
    kvobj *set;
    int j, added = 0;
    dictEntryLink link;
    size_t oldsize = 0;

    set = lookupKeyWriteWithLink(c->db,c->argv[1], &link);
    if (checkType(c,set,OBJ_SET)) return;
    
    if (set == NULL) {
        robj *o = setTypeCreate(c->argv[2]->ptr, c->argc - 2);
        set = dbAddByLink(c->db, c->argv[1], &o, &link);
    } else {
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(set);
        setTypeMaybeConvert(set, c->argc - 2);
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), set, oldsize, kvobjAllocSize(set));
    }

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(set);
    /* A logically expired member that is added again is replaced in place, so
     * the number of added members is not the growth of the set. */
    unsigned long oldlen = setTypeSize(set);
    for (j = 2; j < c->argc; j++) {
        if (setTypeAdd(set,c->argv[j]->ptr)) added++;
    }
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), set, oldsize, kvobjAllocSize(set));
    if (added) {
        unsigned long size = setTypeSize(set);
        if (size != oldlen) updateKeysizesHist(c->db, OBJ_SET, oldlen, size);
        keyModified(c,c->db,c->argv[1],set,1);
        notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->argv[1],c->db->id);
    }
    server.dirty += added;
    addReplyLongLong(c,added);
}

void sremCommand(client *c) {
    int j, deleted = 0, keyremoved = 0;
    size_t oldsize = 0;

    kvobj *set = lookupKeyWriteOrReply(c, c->argv[1], shared.czero);
    if (set == NULL || checkType(c, set, OBJ_SET))
        return;

    unsigned long oldSize = setTypeSize(set);
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(set);

    if (set->encoding == OBJ_ENCODING_HT)
        dictPauseAutoResize((dict*)set->ptr);
    for (j = 2; j < c->argc; j++) {
        if (setTypeRemove(set,c->argv[j]->ptr)) {
            deleted++;
            if (setTypeSize(set) == 0) {
                if (server.memory_tracking_enabled)
                    updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), set, oldsize, kvobjAllocSize(set));
                dbDeleteSkipKeysizesUpdate(c->db, c->argv[1]);
                keyremoved = 1;
                break;
            }
        }
    }
    if (!keyremoved && set->encoding == OBJ_ENCODING_HT) {
        dictResumeAutoResize((dict*)set->ptr);
        dictShrinkIfNeeded((dict*)set->ptr);
    }
    if (server.memory_tracking_enabled && !keyremoved)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), set, oldsize, kvobjAllocSize(set));
    if (deleted) {
        int64_t newSize = oldSize - deleted;

        keyModified(c, c->db, c->argv[1], keyremoved ? NULL : set, 1);
        notifyKeyspaceEvent(NOTIFY_SET,"srem",c->argv[1],c->db->id);
        if (keyremoved) {
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],
                                c->db->id);
            newSize = -1; /* removed */
        }
        updateKeysizesHist(c->db, OBJ_SET, oldSize, newSize);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);
}

void smoveCommand(client *c) {
    robj *srcset, *dstset, *ele;
    size_t oldSrcAllocSize = 0, oldDstAllocSize = 0;
    srcset = lookupKeyWrite(c->db,c->argv[1]);
    dstset = lookupKeyWrite(c->db,c->argv[2]);
    ele = c->argv[3];

    /* If the source key does not exist return 0 */
    if (srcset == NULL) {
        addReply(c,shared.czero);
        return;
    }

    /* If the source key has the wrong type, or the destination key
     * is set and has the wrong type, return with an error. */
    if (checkType(c,srcset,OBJ_SET) ||
        checkType(c,dstset,OBJ_SET)) return;

    /* If srcset and dstset are equal, SMOVE is a no-op */
    if (srcset == dstset) {
        addReply(c,setTypeIsMemberLazy(c->db, srcset, ele->ptr, 0, NULL) ?
            shared.cone : shared.czero);
        return;
    }

    /* The member moves with its expiration. A source member that is logically
     * expired is not a member: it is removed, and nothing is moved. */
    uint64_t srcExpire = EB_EXPIRE_TIME_INVALID;
    if (setTypeHasExpireSupport(srcset)) {
        if (!setTypeIsMemberLazy(c->db, srcset, ele->ptr, 0, NULL)) {
            addReply(c,shared.czero);
            return;
        }
        setTypeGetExpire(srcset, ele->ptr, &srcExpire);
    }

    /* The destination member, if there is one, gets the expiration of the source
     * member, replacing its own. A destination member that is logically expired
     * is removed first, and is then treated as a new member. */
    int dstHadMember = 0;
    uint64_t dstExpire = EB_EXPIRE_TIME_INVALID;
    if (dstset && setTypeHasExpireSupport(dstset)) {
        int dstDeleted = 0;
        dstHadMember = setTypeIsMemberLazy(c->db, dstset, ele->ptr, 0, &dstDeleted);
        if (dstDeleted)
            dstset = NULL; /* The key was deleted: it is created again below. */
        else if (dstHadMember)
            setTypeGetExpire(dstset, ele->ptr, &dstExpire);
    }

    if (server.memory_tracking_enabled)
        oldSrcAllocSize = kvobjAllocSize(srcset);
    int deleted = setTypeRemove(srcset,ele->ptr);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), srcset, oldSrcAllocSize, kvobjAllocSize(srcset));
    /* If the element cannot be removed from the src set, return 0. */
    if (!deleted) {
        addReply(c,shared.czero);
        return;
    }
    notifyKeyspaceEvent(NOTIFY_SET,"srem",c->argv[1],c->db->id);

    /* Update keysizes histogram */
    int64_t srcNewLen = setTypeSize(srcset), srcOldLen = srcNewLen + 1;

    /* Remove the src set from the database when empty */
    if (srcNewLen == 0) {
        dbDeleteSkipKeysizesUpdate(c->db,c->argv[1]);
        srcNewLen = -1; /* removed */
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
    updateKeysizesHist(c->db, OBJ_SET, srcOldLen, srcNewLen);

    /* Create the destination set when it doesn't exist */
    if (!dstset) {
        dstset = setTypeCreate(ele->ptr, 1);
        dbAdd(c->db, c->argv[2], &dstset);
    }

    keyModified(c, c->db, c->argv[1], (srcNewLen > 0) ? srcset : NULL, 1);
    server.dirty++;

    if (server.memory_tracking_enabled)
        oldDstAllocSize = kvobjAllocSize(dstset);
    int added, expireChanged = 0, persisted = 0;
    /* A destination member that is logically expired and could not be removed
     * above is replaced in place, so 'added' is not always a growth of the set. */
    unsigned long dstOldLen = setTypeSize(dstset);
    if (srcExpire != EB_EXPIRE_TIME_INVALID) {
        setTypeConvertToExpireEncoding(dstset);
        added = setTypeAddExAux(dstset, ele->ptr, sdslen(ele->ptr), 0, 1, srcExpire);
        expireChanged = 1;
    } else {
        added = setTypeAdd(dstset,ele->ptr);
        /* The member without an expiration overwrites the expiration of the one
         * that already was in the destination. */
        if (!added && dstExpire != EB_EXPIRE_TIME_INVALID) {
            setTypeGetOps(dstset->encoding)->setExpire(dstset, ele->ptr, sdslen(ele->ptr), 0, 1,
                                                       EB_EXPIRE_TIME_INVALID);
            persisted = 1;
        }
    }
    /* An extra key has changed when ele was successfully added to dstset */
    if (added) {
        unsigned long dstLen = setTypeSize(dstset);
        if (dstLen != dstOldLen) updateKeysizesHist(c->db, OBJ_SET, dstOldLen, dstLen);
    }
    if (added || expireChanged || persisted) {
        if (expireChanged || persisted) setTypeUpdateSubexpiry(c->db, dstset);
        server.dirty++;
        keyModified(c,c->db,c->argv[2],dstset,1);
        if (added)
            notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->argv[2],c->db->id);
        if (expireChanged)
            notifyKeyspaceEvent(NOTIFY_SET,"sexpire",c->argv[2],c->db->id);
        if (persisted)
            notifyKeyspaceEvent(NOTIFY_SET,"spersist",c->argv[2],c->db->id);
    }
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[2]->ptr), dstset, oldDstAllocSize, kvobjAllocSize(dstset));
    addReply(c,shared.cone);
}

void sismemberCommand(client *c) {
    kvobj *set;

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,OBJ_SET)) return;

    /* Removes the member if it is logically expired. Also updates the allocation
     * size accounting (a lookup can move memory, for example by rehashing). */
    if (setTypeIsMemberLazy(c->db, set, c->argv[2]->ptr, 0, NULL))
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}

void smismemberCommand(client *c) {
    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * sets, where SMISMEMBER should respond with a series of zeros. */
    kvobj *set = lookupKeyRead(c->db, c->argv[1]);
    if (set && checkType(c,set,OBJ_SET)) return;

    addReplyArrayLen(c,c->argc - 2);

    for (int j = 2; j < c->argc; j++) {
        int setDeleted = 0;
        if (set && setTypeIsMemberLazy(c->db, set, c->argv[j]->ptr, 0, &setDeleted))
            addReply(c,shared.cone);
        else
            addReply(c,shared.czero);
        /* The key was deleted if the last member was a logically expired one. */
        if (setDeleted) set = NULL;
    }
}

void scardCommand(client *c) {
    kvobj *kv;

    if ((kv = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,kv,OBJ_SET)) return;

    addReplyLongLong(c,setTypeSize(kv));
}

/* Handle the "SPOP key <count>" variant. The normal version of the
 * command is handled by the spopCommand() function itself. */

/* How many times bigger should be the set compared to the remaining size
 * for us to use the "create new set" strategy? Read later in the
 * implementation for more info. */
#define SPOP_MOVE_STRATEGY_MUL 5

void spopWithCountCommand(client *c) {
    long l;
    unsigned long count, size, toRemove;
    size_t oldsize = 0;

    /* Get the count argument */
    if (getPositiveLongFromObjectOrReply(c,c->argv[2],&l,NULL) != C_OK) return;
    count = (unsigned long) l;

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a kv. Otherwise, return nil */
    robj *set = lookupKeyWriteOrReply(c, c->argv[1], shared.emptyset[c->resp]);
    if (set == NULL || checkType(c, set, OBJ_SET)) return;

    /* If count is zero, serve an empty set ASAP to avoid special
     * cases later. */
    if (count == 0) {
        addReply(c,shared.emptyset[c->resp]);
        return;
    }

    /* Remove the members that are logically expired first, so that all of the
     * rest of the command works on a set of live members. If they were all
     * expired, the key is gone. */
    if (setTypeExpireIfNeeded(c->db, set)) {
        addReply(c,shared.emptyset[c->resp]);
        return;
    }

    size = setTypeSize(set);
    toRemove = (count >= size) ? size : count;

    /* Generate an SPOP keyspace notification */
    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->argv[1],c->db->id);
    server.dirty += toRemove;

    /* CASE 1:
     * The number of requested elements is greater than or equal to
     * the number of elements inside the set: simply return the whole set. */
    if (count >= size) {
        /* We just return the entire set */
        sunionDiffGenericCommand(c,c->argv+1,1,NULL,SET_OP_UNION,0,0,0);

        /* Delete the set as it is now empty */
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);

        /* todo: Move the spop notification to be executed after the command logic. */

        /* Propagate this command as a DEL or UNLINK operation */
        robj *aux = server.lazyfree_lazy_server_del ? shared.unlink : shared.del;
        rewriteClientCommandVector(c, 2, aux, c->argv[1]);
        keyModified(c,c->db,c->argv[1],NULL,1);
        return;
    }

    /* Case 2 and 3 require to replicate SPOP as a set of SREM commands.
     * Prepare our replication argument vector. Also send the array length
     * which is common to both the code paths. */
    unsigned long batchsize = count > 1024 ? 1024 : count;
    robj **propargv = zmalloc(sizeof(robj *) * (2 + batchsize));
    propargv[0] = shared.srem;
    propargv[1] = c->argv[1];
    unsigned long propindex = 2;
    addReplySetLen(c,count);

    /* Common iteration vars. */
    char *str;
    size_t len = 0;
    int64_t llele = 0;
    unsigned long remaining = size-count; /* Elements left after SPOP. */

    /* If we are here, the number of requested elements is less than the
     * number of elements inside the set. Also we are sure that count < size.
     * Use two different strategies.
     *
     * CASE 2: The number of elements to return is small compared to the
     * set size. We can just extract random elements and return them to
     * the set. */
    if (remaining*SPOP_MOVE_STRATEGY_MUL > count &&
        set->encoding == OBJ_ENCODING_LISTPACK)
    {
        /* Specialized case for listpack. Traverse it only once. */
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(set);
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        unsigned int index = 0;
        unsigned char **ps = zmalloc(sizeof(char *) * count);
        for (unsigned long i = 0; i < count; i++) {
            p = lpNextRandom(lp, p, &index, count - i, 1);
            unsigned int len = 0;
            str = (char *)lpGetValue(p, &len, (long long *)&llele);

            if (str) {
                addReplyBulkCBuffer(c, str, len);
                propargv[propindex++] = createStringObject(str, len);
            } else {
                addReplyBulkLongLong(c, llele);
                propargv[propindex++] = createStringObjectFromLongLong(llele);
            }
            /* Replicate/AOF this command as an SREM operation */
            if (propindex == 2 + batchsize) {
                alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL);
                for (unsigned long j = 2; j < propindex; j++) {
                    decrRefCount(propargv[j]);
                }
                propindex = 2;
            }

            /* Store pointer for later deletion and move to next. */
            ps[i] = p;
            p = lpNext(lp, p);
            index++;
        }
        lp = lpBatchDelete(lp, ps, count);
        zfree(ps);
        set->ptr = lp;
        updateKeysizesHist(c->db, OBJ_SET, size, size - count);
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), set, oldsize, kvobjAllocSize(set));
    } else if (remaining*SPOP_MOVE_STRATEGY_MUL > count) {
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(set);
        for (unsigned long i = 0; i < count; i++) {
            propargv[propindex] = setTypePopRandom(set);
            addReplyBulk(c, propargv[propindex]);
            propindex++;
            /* Replicate/AOF this command as an SREM operation */
            if (propindex == 2 + batchsize) {
                alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL);
                for (unsigned long j = 2; j < propindex; j++) {
                    decrRefCount(propargv[j]);
                }
                propindex = 2;
            }
        }
        updateKeysizesHist(c->db, OBJ_SET, size, size - count);
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), set, oldsize, kvobjAllocSize(set));
    } else {
    /* CASE 3: The number of elements to return is very big, approaching
     * the size of the set itself. After some time extracting random elements
     * from such a set becomes computationally expensive, so we use
     * a different strategy, we extract random elements that we don't
     * want to return (the elements that will remain part of the set),
     * creating a new set as we do this (that will be stored as the original
     * set). Then we return the elements left in the original set and
     * release it. */
        robj *newset = NULL;
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(set);

        /* Create a new set with just the remaining elements. */
        if (set->encoding == OBJ_ENCODING_LISTPACK) {
            /* Specialized case for listpack. Traverse it only once. */
            newset = createSetListpackObject();
            unsigned char *lp = set->ptr;
            unsigned char *p = lpFirst(lp);
            unsigned int index = 0;
            unsigned char **ps = zmalloc(sizeof(char *) * remaining);
            for (unsigned long i = 0; i < remaining; i++) {
                p = lpNextRandom(lp, p, &index, remaining - i, 1);
                unsigned int len = 0;
                str = (char *)lpGetValue(p, &len, (long long *)&llele);
                setTypeAddAux(newset, str, len, llele, 0);
                ps[i] = p;
                p = lpNext(lp, p);
                index++;
            }
            lp = lpBatchDelete(lp, ps, remaining);
            zfree(ps);
            set->ptr = lp;
        } else {
            const int keepExpire = setTypeHasExpireSupport(set);
            while(remaining--) {
                int encoding = setTypeRandomElement(set, &str, &len, &llele);
                uint64_t expire = EB_EXPIRE_TIME_INVALID;
                if (keepExpire) {
                    /* The remaining members keep their expiration. */
                    setTypeGetExpireAux(set, str, len, llele, encoding == OBJ_ENCODING_HT, &expire);
                    if (!newset) newset = createSetListpackExObject();
                } else if (!newset) {
                    newset = str ? createSetListpackObject() : createIntsetObject();
                }
                setTypeAddExAux(newset, str, len, llele, encoding == OBJ_ENCODING_HT, expire);
                setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HT);
            }
        }

        /* Transfer the old set to the client. */
        setTypeIterator si;
        setTypeInitIterator(&si, set, SET_ITER_SKIP_EXPIRED);
        while (setTypeNext(&si, &str, &len, &llele) != -1) {
            if (str == NULL) {
                addReplyBulkLongLong(c,llele);
                propargv[propindex++] = createStringObjectFromLongLong(llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
                propargv[propindex++] = createStringObject(str, len);
            }
            /* Replicate/AOF this command as an SREM operation */
            if (propindex == 2 + batchsize) {
                alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL);
                for (unsigned long i = 2; i < propindex; i++) {
                    decrRefCount(propargv[i]);
                }
                propindex = 2;
            }
        }
        setTypeResetIterator(&si);

        /* Update key size histogram "explicitly" and not indirectly by dbReplaceValue()
         * since function dbReplaceValue() assumes the entire set is being replaced, 
         * but here we're building the new set from the existing one. As a result, 
         * the size of the old set has already changed by the time we reach this point. */
        updateKeysizesHist(c->db, OBJ_SET, size, size-count);
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), set, oldsize, kvobjAllocSize(set));
        dbReplaceValue(c->db, c->argv[1], &newset, 0);
        set = newset;
        /* The new object is not registered in subexpires yet. */
        if (setTypeHasExpireSupport(set)) setTypeUpdateSubexpiry(c->db, set);
    }

    /* Replicate/AOF the remaining elements as an SREM operation */
    if (propindex != 2) {
        alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL);
        for (unsigned long i = 2; i < propindex; i++) {
            decrRefCount(propargv[i]);
        }
        propindex = 2;
    }
    zfree(propargv);

    /* Don't propagate the command itself even if we incremented the
     * dirty counter. We don't want to propagate an SPOP command since
     * we propagated the command as a set of SREMs operations using
     * the alsoPropagate() API. */
    preventCommandPropagation(c);
    keyModified(c,c->db,c->argv[1],set,1);
}

void spopCommand(client *c) {
    unsigned long size;
    robj *ele;
    size_t oldsize = 0;

    if (c->argc == 3) {
        spopWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a kv */
    kvobj *kv = lookupKeyWriteOrReply(c, c->argv[1], shared.null[c->resp]);
    if (kv == NULL || checkType(c, kv, OBJ_SET)) return;

    /* Remove the members that are logically expired first. If they were all
     * expired, the key is gone. */
    if (setTypeExpireIfNeeded(c->db, kv)) {
        addReplyNull(c);
        return;
    }

    size = setTypeSize(kv);
    updateKeysizesHist(c->db, OBJ_SET, size, size-1);

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(kv);

    /* Pop a random element from the kv */
    ele = setTypePopRandom(kv);

    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), kv, oldsize, kvobjAllocSize(kv));

    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->argv[1],c->db->id);

    /* Replicate/AOF this command as an SREM operation */
    rewriteClientCommandVector(c,3,shared.srem,c->argv[1],ele);

    /* Add the element to the reply */
    addReplyBulk(c, ele);
    decrRefCount(ele);

    /* Delete the kv if it's empty */
    int deleted = 0;
    if (setTypeSize(kv) == 0) {
        deleted = 1;
        dbDelete(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    /* Set has been modified */
    keyModified(c, c->db, c->argv[1], deleted ? NULL : kv, 1);
    server.dirty++;
}

/* handle the "SRANDMEMBER key <count>" variant. The normal version of the
 * command is handled by the srandmemberCommand() function itself. */

/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3

/* If client is trying to ask for a very large number of random elements,
 * queuing may consume an unlimited amount of memory, so we want to limit
 * the number of randoms per time. */
#define SRANDFIELD_RANDOM_SAMPLE_LIMIT 1000

void srandmemberWithCountCommand(client *c) {
    long l;
    unsigned long count, size;
    int uniq = 1;
    kvobj *set;
    char *str;
    size_t len = 0;
    int64_t llele = 0;

    dict *d;

    if (getRangeLongFromObjectOrReply(c,c->argv[2],-LONG_MAX,LONG_MAX,&l,NULL) != C_OK) return;
    if (l >= 0) {
        count = (unsigned long) l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        count = -l;
        uniq = 0;
    }

    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptyarray))
        == NULL || checkType(c,set,OBJ_SET)) return;

    if (setTypeHasExpireSupport(set)) {
        /* Remove the members that are logically expired first. If they were all
         * expired, the key is gone. */
        if (setTypeExpireIfNeeded(c->db, set)) {
            addReply(c,shared.emptyarray);
            return;
        }
    }
    size = setTypeSize(set);

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        addReply(c,shared.emptyarray);
        return;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. This case is the only one that also needs to return the
     * elements in random order. */
    if (!uniq || count == 1) {
        addReplyArrayLen(c,count);

        if (set->encoding == OBJ_ENCODING_LISTPACK && count > 1) {
            /* Specialized case for listpack, traversing it only once. */
            unsigned long limit, sample_count;
            limit = count > SRANDFIELD_RANDOM_SAMPLE_LIMIT ? SRANDFIELD_RANDOM_SAMPLE_LIMIT : count;
            listpackEntry *entries = zmalloc(limit * sizeof(listpackEntry));
            while (count) {
                sample_count = count > limit ? limit : count;
                count -= sample_count;
                lpRandomEntries(set->ptr, sample_count, entries);
                for (unsigned long i = 0; i < sample_count; i++) {
                    if (entries[i].sval)
                        addReplyBulkCBuffer(c, entries[i].sval, entries[i].slen);
                    else
                        addReplyBulkLongLong(c, entries[i].lval);
                }
                if (c->flags & CLIENT_CLOSE_ASAP)
                    break;
            }
            zfree(entries);
            return;
        }

        while(count--) {
            setTypeRandomElement(set, &str, &len, &llele);
            if (str == NULL) {
                addReplyBulkLongLong(c,llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            if (c->flags & CLIENT_CLOSE_ASAP)
                break;
        }
        return;
    }

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set. */
    if (count >= size) {
        setTypeIterator si;
        addReplyArrayLen(c,size);
        setTypeInitIterator(&si, set, SET_ITER_RAW);
        while (setTypeNext(&si, &str, &len, &llele) != -1) {
            if (str == NULL) {
                addReplyBulkLongLong(c,llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            size--;
        }
        setTypeResetIterator(&si);
        serverAssert(size==0);
        return;
    }

    /* CASE 2.5 listpack only. Sampling unique elements, in non-random order.
     * Listpack encoded sets are meant to be relatively small, so
     * SRANDMEMBER_SUB_STRATEGY_MUL isn't necessary and we rather not make
     * copies of the entries. Instead, we emit them directly to the output
     * buffer.
     *
     * And it is inefficient to repeatedly pick one random element from a
     * listpack in CASE 4. So we use this instead. */
    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        unsigned int i = 0;
        addReplyArrayLen(c, count);
        while (count) {
            p = lpNextRandom(lp, p, &i, count--, 1);
            unsigned int len;
            str = (char *)lpGetValue(p, &len, (long long *)&llele);
            if (str == NULL) {
                addReplyBulkLongLong(c, llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            p = lpNext(lp, p);
            i++;
        }
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. */
    d = dictCreate(&sdsReplyDictType);

    /* CASE 3:
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requested elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 4 is highly inefficient. */
    if (count*SRANDMEMBER_SUB_STRATEGY_MUL > size) {
        setTypeIterator si;

        /* Add all the elements into the temporary dictionary. */
        setTypeInitIterator(&si, set, SET_ITER_RAW);
        dictExpand(d, size);
        while (setTypeNext(&si, &str, &len, &llele) != -1) {
            int retval = DICT_ERR;

            if (str == NULL) {
                retval = dictAdd(d,sdsfromlonglong(llele),NULL);
            } else {
                retval = dictAdd(d, sdsnewlen(str, len), NULL);
            }
            serverAssert(retval == DICT_OK);
        }
        setTypeResetIterator(&si);
        serverAssert(dictSize(d) == size);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            dictEntry *de;
            de = dictGetFairRandomKey(d);
            dictUnlink(d,dictGetKey(de));
            sdsfree(dictGetKey(de));
            dictFreeUnlinkedEntry(d,de);
            size--;
        }
    }

    /* CASE 4: We have a big set compared to the requested number of elements.
     * In this case we can simply get random elements from the set and add
     * to the temporary set, trying to eventually get enough unique elements
     * to reach the specified count. */
    else {
        unsigned long added = 0;
        sds sdsele;

        dictExpand(d, count);
        while (added < count) {
            setTypeRandomElement(set, &str, &len, &llele);
            if (str == NULL) {
                sdsele = sdsfromlonglong(llele);
            } else {
                sdsele = sdsnewlen(str, len);
            }
            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. */
            if (dictAdd(d,sdsele,NULL) == DICT_OK)
                added++;
            else
                sdsfree(sdsele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    {
        dictIterator di;
        dictEntry *de;

        addReplyArrayLen(c,count);
        dictInitIterator(&di, d);
        while((de = dictNext(&di)) != NULL)
            addReplyBulkSds(c,dictGetKey(de));
        dictResetIterator(&di);
        dictRelease(d);
    }
}

/* SRANDMEMBER <key> [<count>] */
void srandmemberCommand(client *c) {
    kvobj *set;
    char *str;
    size_t len = 0;
    int64_t llele = 0;
    size_t oldsize = 0;

    if (c->argc == 3) {
        srandmemberWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Handle variant without <count> argument. Reply with simple bulk string */
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]))
        == NULL || checkType(c,set,OBJ_SET)) return;

    if (setTypeHasExpireSupport(set)) {
        /* Same as for the variant with a count. */
        if (setTypeExpireIfNeeded(c->db, set)) {
            addReplyNull(c);
            return;
        }
    }

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(set);
    setTypeRandomElement(set, &str, &len, &llele);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), set, oldsize, kvobjAllocSize(set));
    if (str == NULL) {
        addReplyBulkLongLong(c,llele);
    } else {
        addReplyBulkCBuffer(c, str, len);
    }
}

typedef struct setopsrc {
    robj *set;
    size_t oldsize;
} setopsrc;

int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    robj *o1 = ((setopsrc*)s1)->set, *o2 = ((setopsrc*)s2)->set;
    if (setTypeSize(o1) > setTypeSize(o2)) return 1;
    if (setTypeSize(o1) < setTypeSize(o2)) return -1;
    return 0;
}

/* This is used by SDIFF and in this case we can receive NULL that should
 * be handled as empty sets. */
int qsortCompareSetsByRevCardinality(const void *s1, const void *s2) {
    robj *o1 = ((setopsrc*)s1)->set, *o2 = ((setopsrc*)s2)->set;
    unsigned long first = o1 ? setTypeSize(o1) : 0;
    unsigned long second = o2 ? setTypeSize(o2) : 0;

    if (first < second) return 1;
    if (first > second) return -1;
    return 0;
}

/* SINTER / SINTERSTORE / SINTERCARD
 *
 * 'cardinality_only' work for SINTERCARD, only return the cardinality
 * with minimum processing and memory overheads.
 *
 * 'limit' work for SINTERCARD, stop searching after reaching the limit.
 * Passing a 0 means unlimited.
 */
void sinterGenericCommand(client *c, robj **setkeys,
                          unsigned long setnum, robj *dstkey,
                          int cardinality_only, unsigned long limit) {
    setopsrc *sets = zmalloc(sizeof(setopsrc)*setnum);
    setTypeIterator si;
    robj *dstset = NULL;
    char *str;
    size_t len = 0;
    int64_t intobj = 0;
    void *replylen = NULL;
    unsigned long j, cardinality = 0;
    int encoding, empty = 0;

    for (j = 0; j < setnum; j++) {
        kvobj *kv = lookupKeyRead(c->db, setkeys[j]);
        if (!kv) {
            /* A NULL is considered an empty set */
            empty += 1;
            sets[j].set = NULL;
            sets[j].oldsize = 0;
            continue;
        }
        if (checkType(c, kv, OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j].set = kv;
        if (server.memory_tracking_enabled)
            sets[j].oldsize = kvobjAllocSize(kv);
    }

    /* Set intersection with an empty set always results in an empty set.
     * Return ASAP if there is an empty set. */
    if (empty > 0) {
        zfree(sets);
        if (dstkey) {
            if (dbDelete(c->db,dstkey)) {
                keyModified(c,c->db,dstkey,NULL,1);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
                server.dirty++;
            }
            addReply(c,shared.czero);
        } else if (cardinality_only) {
            addReplyLongLong(c,cardinality);
        } else {
            addReply(c,shared.emptyset[c->resp]);
        }
        return;
    }

    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    qsort(sets,setnum,sizeof(setopsrc),qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (dstkey) {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        if (sets[0].set->encoding == OBJ_ENCODING_INTSET) {
            /* The first set is an intset, so the result is an intset too. The
             * elements are inserted in ascending order which is efficient in an
             * intset. */
            dstset = createIntsetObject();
        } else if (sets[0].set->encoding == OBJ_ENCODING_LISTPACK) {
            /* To avoid many reallocs, we estimate that the result is a listpack
             * of approximately the same size as the first set. Then we shrink
             * it or possibly convert it to intset in the end. */
            unsigned char *lp = lpNew(lpBytes(sets[0].set->ptr));
            dstset = createObject(OBJ_SET, lp);
            dstset->encoding = OBJ_ENCODING_LISTPACK;
        } else {
            /* We start off with a listpack, since it's more efficient to append
             * to than an intset. Later we can convert it to intset or a
             * hashtable. */
            dstset = createSetListpackObject();
        }
    } else if (!cardinality_only) {
        replylen = addReplyDeferredLen(c);
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    int only_integers = 1;
    setTypeInitIterator(&si, sets[0].set, SET_ITER_SKIP_EXPIRED);
    while((encoding = setTypeNext(&si, &str, &len, &intobj)) != -1) {
        for (j = 1; j < setnum; j++) {
            if (sets[j].set == sets[0].set) continue;
            if (!setTypeIsMemberAux(sets[j].set, str, len, intobj,
                                    encoding == OBJ_ENCODING_HT))
                break;
        }

        /* Only take action when all sets contain the member */
        if (j == setnum) {
            /* The stored member gets the nearest expiration of all the sets. */
            uint64_t expire = EB_EXPIRE_TIME_INVALID;
            if (dstkey && !cardinality_only) {
                for (j = 0; j < setnum; j++) {
                    uint64_t e = si.expire;
                    if (j > 0 && sets[j].set != sets[0].set) {
                        if (!setTypeHasExpireSupport(sets[j].set)) continue;
                        setTypeGetExpireAux(sets[j].set, str, len, intobj,
                                            encoding == OBJ_ENCODING_HT, &e);
                    } else if (j > 0) continue;
                    if (e != EB_EXPIRE_TIME_INVALID && e < expire) expire = e;
                }
            }
            if (cardinality_only) {
                cardinality++;

                /* We stop the searching after reaching the limit. */
                if (limit && cardinality >= limit)
                    break;
            } else if (!dstkey) {
                if (str != NULL)
                    addReplyBulkCBuffer(c, str, len);
                else
                    addReplyBulkLongLong(c,intobj);
                cardinality++;
            } else {
                if (str && only_integers) {
                    /* It may be an integer although we got it as a string. */
                    if (encoding == OBJ_ENCODING_HT &&
                        string2ll(str, len, (long long *)&intobj))
                    {
                        if (dstset->encoding == OBJ_ENCODING_LISTPACK ||
                            dstset->encoding == OBJ_ENCODING_INTSET)
                        {
                            /* Adding it as an integer is more efficient. */
                            str = NULL;
                        }
                    } else {
                        /* It's not an integer */
                        only_integers = 0;
                    }
                }
                setTypeAddMergeExpire(dstset, str, len, intobj, encoding == OBJ_ENCODING_HT, expire, 0);
            }
        }
    }
    setTypeResetIterator(&si);

    if (server.memory_tracking_enabled) {
        for (j = 0; j < setnum; j++) {
            robj *obj = sets[j].set;
            if (!obj) continue;
            updateSlotAllocSize(c->db, getKeySlot(setkeys[j]->ptr), obj,
                                sets[j].oldsize, kvobjAllocSize(obj));
        }
    }

    if (cardinality_only) {
        addReplyLongLong(c,cardinality);
    } else if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        if (setTypeSize(dstset) > 0) {
            if (only_integers && !setTypeHasExpireSupport(dstset)) maybeConvertToIntset(dstset);
            if (dstset->encoding == OBJ_ENCODING_LISTPACK) {
                /* We allocated too much memory when we created it to avoid
                 * frequent reallocs. Therefore, we shrink it now. */
                dstset->ptr = lpShrinkToFit(dstset->ptr);
            }
            setKey(c, c->db, dstkey, &dstset, 0);
            if (setTypeHasExpireSupport(dstset)) setTypeUpdateSubexpiry(c->db, dstset);
            addReplyLongLong(c,setTypeSize(dstset));
            notifyKeyspaceEvent(NOTIFY_SET,"sinterstore",
                dstkey,c->db->id);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
            if (dbDelete(c->db,dstkey)) {
                server.dirty++;
                keyModified(c,c->db,dstkey,NULL,1);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
            }
            decrRefCount(dstset);
        }
    } else {
        setDeferredSetLen(c,replylen,cardinality);
    }
    zfree(sets);
}

/* SINTER key [key ...] */
void sinterCommand(client *c) {
    sinterGenericCommand(c, c->argv+1,  c->argc-1, NULL, 0, 0);
}

/* SMEMBERS key */
void smembersCommand(client *c) {
    setTypeIterator si;
    char *str;
    size_t len = 0;
    int64_t intobj = 0;
    size_t oldsize = 0;
    kvobj *setobj = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c,setobj,OBJ_SET)) return;
    if (!setobj) {
        addReply(c, shared.emptyset[c->resp]);
        return;
    }

    /* Prepare the response. The logically expired members are skipped, so the
     * length is not known in advance for the sets that can hold expirations. */
    unsigned long length = setTypeSize(setobj), returned = 0;
    void *replylen = NULL;
    const int deferred_len = setTypeHasExpireSupport(setobj);
    if (deferred_len)
        replylen = addReplyDeferredLen(c);
    else
        addReplySetLen(c,length);
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(setobj);
    /* Iterate through the elements of the set. */
    setTypeInitIterator(&si, setobj, SET_ITER_SKIP_EXPIRED);

    while (setTypeNext(&si, &str, &len, &intobj) != -1) {
        if (str != NULL)
            addReplyBulkCBuffer(c, str, len);
        else
            addReplyBulkLongLong(c, intobj);
        returned++;
    }
    setTypeResetIterator(&si);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), setobj, oldsize, kvobjAllocSize(setobj));
    if (deferred_len)
        setDeferredSetLen(c, replylen, returned);
    else
        serverAssert(returned == length); /* fail on corrupt data */
}

/* SINTERCARD numkeys key [key ...] [LIMIT limit] */
void sinterCardCommand(client *c) {
    long j;
    long numkeys = 0; /* Number of keys. */
    long limit = 0;   /* 0 means not limit. */

    if (getRangeLongFromObjectOrReply(c, c->argv[1], 1, LONG_MAX,
                                      &numkeys, "numkeys should be greater than 0") != C_OK)
        return;
    if (numkeys > (c->argc - 2)) {
        addReplyError(c, "Number of keys can't be greater than number of args");
        return;
    }

    for (j = 2 + numkeys; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc - 1) - j;

        if (!strcasecmp(opt, "LIMIT") && moreargs) {
            j++;
            if (getPositiveLongFromObjectOrReply(c, c->argv[j], &limit,
                                                 "LIMIT can't be negative") != C_OK)
                return;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    sinterGenericCommand(c, c->argv+2, numkeys, NULL, 1, limit);
}

/* SINTERSTORE destination key [key ...] */
void sinterstoreCommand(client *c) {
    sinterGenericCommand(c, c->argv+2, c->argc-2, c->argv[1], 0, 0);
}

void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,
                              robj *dstkey, int op,
                              int cardinality_only, int approx, long limit)
{
    /* Approximate cardinality is only ever requested for SUNIONCARD, i.e. a
     * non-storing UNION that returns a count. */
    serverAssert(!approx || (op == SET_OP_UNION && cardinality_only && dstkey == NULL));

    setopsrc *sets = zmalloc(sizeof(setopsrc)*setnum);
    setTypeIterator si;
    robj *dstset = NULL;
    robj *hllobj = NULL; /* Used only for approximate (HLL) cardinality. */
    int dstset_encoding = OBJ_ENCODING_INTSET;
    char *str;
    size_t len = 0;
    int64_t llval = 0;
    int encoding;
    int j, diff_algo = 1;
    long cardinality = 0;
    int sameset = 0;
    /* Memory tracking is only needed for SET_OP_DIFF. UNION just iterates the
     * source sets; it never calls dictFind/dictAdd/dictDelete on a passed key,
     * so it can't advance a rehash and change a source set's allocation size. */
    int must_track_memory = (op != SET_OP_UNION && server.memory_tracking_enabled);

    for (j = 0; j < setnum; j++) {
        kvobj *setobj = lookupKeyRead(c->db, setkeys[j]);
        if (!setobj) {
            sets[j].set = NULL;
            sets[j].oldsize = 0;
            continue;
        }
        if (checkType(c,setobj,OBJ_SET)) {
            zfree(sets);
            return;
        }
        /* For a SET's encoding, according to the factory method setTypeCreate(), currently have 3 types:
         * 1. OBJ_ENCODING_INTSET
         * 2. OBJ_ENCODING_LISTPACK
         * 3. OBJ_ENCODING_HT
         * 'dstset_encoding' is used to determine which kind of encoding to use when initialize 'dstset'.
         *
         * If all sets are all OBJ_ENCODING_INTSET encoding or 'dstkey' is not null, keep 'dstset'
         * OBJ_ENCODING_INTSET encoding when initialize. Otherwise it is not efficient to create the 'dstset'
         * from intset and then convert to listpack or hashtable.
         *
         * If one of the set is OBJ_ENCODING_LISTPACK, let's set 'dstset' to hashtable default encoding,
         * the hashtable is more efficient when find and compare than the listpack. The corresponding
         * time complexity are O(1) vs O(n). */
        if (!dstkey && dstset_encoding == OBJ_ENCODING_INTSET &&
            (setobj->encoding == OBJ_ENCODING_LISTPACK || setobj->encoding == OBJ_ENCODING_LISTPACK_EX ||
         setobj->encoding == OBJ_ENCODING_HT)) {
            dstset_encoding = OBJ_ENCODING_HT;
        }
        sets[j].set = setobj;
        if (must_track_memory)
            sets[j].oldsize = kvobjAllocSize(setobj);
        if (j > 0 && sets[0].set == sets[j].set) {
            sameset = 1; 
        }
    }

    /* Select what DIFF algorithm to use.
     *
     * Algorithm 1 is O(N*M) where N is the size of the element first set
     * and M the total number of sets.
     *
     * Algorithm 2 is O(N) where N is the total number of elements in all
     * the sets.
     *
     * We compute what is the best bet with the current input here. */
    if (op == SET_OP_DIFF && sets[0].set && !sameset) {
        long long algo_one_work = 0, algo_two_work = 0;

        for (j = 0; j < setnum; j++) {
            if (sets[j].set == NULL) continue;

            algo_one_work += setTypeSize(sets[0].set);
            algo_two_work += setTypeSize(sets[j].set);
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. */
        algo_one_work /= 2;
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;

        if (diff_algo == 1 && setnum > 1) {
            /* With algorithm 1 it is better to order the sets to subtract
             * by decreasing size, so that we are more likely to find
             * duplicated elements ASAP. */
            qsort(sets+1,setnum-1,sizeof(setopsrc),
                qsortCompareSetsByRevCardinality);
        }
    }

    /* We need a temp set object to store our union/diff. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE/SDIFFSTORE operation) then
     * this set object will be the resulting object to set into the target key*/
    if (dstset_encoding == OBJ_ENCODING_INTSET) {
        dstset = createIntsetObject();
    } else {
        dstset = createSetObject();
    }

    if (op == SET_OP_UNION) {
        /* Union is trivial, just add every element of every set to either the
         * temporary set (exact) or, for approximate cardinality, a temporary
         * HLL object (standard sparse→dense encoding, same as PFADD). */
        if (approx) hllobj = createHLLObject();
        int early_exit = 0;
        long elements_processed = 0;
        long check_after = limit; /* For approx: first check after `limit` elements. */
        for (j = 0; j < setnum && !early_exit; j++) {
            if (!sets[j].set) continue; /* non existing keys are like empty sets */

            setTypeInitIterator(&si, sets[j].set, SET_ITER_SKIP_EXPIRED);
            while ((encoding = setTypeNext(&si, &str, &len, &llval)) != -1) {
                if (!approx) {
                    cardinality += setTypeAddMergeExpire(dstset, str, len, llval, encoding == OBJ_ENCODING_HT,
                                                         dstkey ? si.expire : EB_EXPIRE_TIME_INVALID, 1);
                    if (cardinality_only && limit > 0 && cardinality >= limit) {
                        early_exit = 1;
                        break;
                    }
                    continue;
                }

                /* Approximate path: feed the element into the HLL. The estimate
                 * is only ever read, so we can stop early once it is confidently
                 * at/above the requested LIMIT. */
                int retval;
                if (str != NULL) {
                    retval = hllAdd(hllobj, (unsigned char *)str, len);
                } else {
                    char buf[LONG_STR_SIZE];
                    size_t slen = ll2string(buf, sizeof(buf), (long long)llval);
                    retval = hllAdd(hllobj, (unsigned char *)buf, slen);
                }
                /* hllAdd() only fails on a corrupted HLL header, which cannot
                 * happen for an HLL we just created ourselves. */
                serverAssert(retval != -1);

                elements_processed++;
                if (limit > 0 && elements_processed >= check_after) {
                    uint64_t est = hllCount(hllobj->ptr, NULL);
                    if (est >= (uint64_t)limit) {
                        early_exit = 1;
                        break;
                    }
                    long remaining = (long)limit - (long)est;
                    check_after = elements_processed +
                        (remaining > HLL_CHECK_INTERVAL_FLOOR ? remaining : HLL_CHECK_INTERVAL_FLOOR);
                }
            }
            setTypeResetIterator(&si);
        }
    } else if (op == SET_OP_DIFF && sameset) {
        /* At least one of the sets is the same one (same key) as the first one, result must be empty. */
    } else if (op == SET_OP_DIFF && sets[0].set && diff_algo == 1) {
        /* DIFF Algorithm 1:
         *
         * We perform the diff by iterating all the elements of the first set,
         * and only adding it to the target set if the element does not exist
         * into all the other sets.
         *
         * This way we perform at max N*M operations, where N is the size of
         * the first set, and M the number of sets. */
        setTypeInitIterator(&si, sets[0].set, SET_ITER_SKIP_EXPIRED);
        while ((encoding = setTypeNext(&si, &str, &len, &llval)) != -1) {
            for (j = 1; j < setnum; j++) {
                if (!sets[j].set) continue; /* no key is an empty set. */
                if (sets[j].set == sets[0].set) break; /* same set! */
                if (setTypeIsMemberAux(sets[j].set, str, len, llval,
                                       encoding == OBJ_ENCODING_HT))
                    break;
            }
            if (j == setnum) {
                /* There is no other set with this element. Add or count it if cardinality_only. */
                if (cardinality_only) {
                    cardinality++;
                    if (limit > 0 && cardinality >= limit) {
                        break; /* We reached the limit, break from the while loop iterating sets[0] */
                    }
                } else {
                    cardinality += setTypeAddMergeExpire(dstset, str, len, llval, encoding == OBJ_ENCODING_HT,
                                                         dstkey ? si.expire : EB_EXPIRE_TIME_INVALID, 0);
                }
            }
        }
        setTypeResetIterator(&si);
    } else if (op == SET_OP_DIFF && sets[0].set && diff_algo == 2 && cardinality_only && limit > 0) {
        /* DIFF Algorithm 2b (cardinality-only with LIMIT):
         *
         * Collect sets[1..N] into a single auxiliary set and iterate
         * the first set, counting elements absent from it with
         * a single membership lookup each. This lets us stop as
         * soon as LIMIT is reached. */
        for (j = 1; j < setnum; j++) {
            if (!sets[j].set) continue; /* non existing keys are like empty sets */

            setTypeInitIterator(&si, sets[j].set, SET_ITER_SKIP_EXPIRED);
            while ((encoding = setTypeNext(&si, &str, &len, &llval)) != -1)
                setTypeAddAux(dstset, str, len, llval, encoding == OBJ_ENCODING_HT);
            setTypeResetIterator(&si);
        }

        setTypeInitIterator(&si, sets[0].set, SET_ITER_SKIP_EXPIRED);
        while ((encoding = setTypeNext(&si, &str, &len, &llval)) != -1) {
            if (!setTypeIsMemberAux(dstset, str, len, llval, encoding == OBJ_ENCODING_HT)) {
                cardinality++;
                if (cardinality >= limit) break; /* We reached the limit. */
            }
        }
        setTypeResetIterator(&si);
    } else if (op == SET_OP_DIFF && sets[0].set && diff_algo == 2) {
        /* DIFF Algorithm 2:
         *
         * Add all the elements of the first set to the auxiliary set.
         * Then remove all the elements of all the next sets from it.
         *
         * This is O(N) where N is the sum of all the elements in every
         * set. */
        for (j = 0; j < setnum; j++) {
            if (!sets[j].set) continue; /* non existing keys are like empty sets */

            setTypeInitIterator(&si, sets[j].set, SET_ITER_SKIP_EXPIRED);
            while((encoding = setTypeNext(&si, &str, &len, &llval)) != -1) {
                if (j == 0) {
                    cardinality += setTypeAddMergeExpire(dstset, str, len, llval,
                                                         encoding == OBJ_ENCODING_HT,
                                                         dstkey ? si.expire : EB_EXPIRE_TIME_INVALID, 0);
                } else {
                    cardinality -= setTypeRemoveAux(dstset, str, len, llval,
                                                    encoding == OBJ_ENCODING_HT);
                }
            }
            setTypeResetIterator(&si);

            /* Exit if result set is empty as any additional removal
             * of elements will have no effect. */
            if (cardinality == 0) break;
        }
    }
    if (must_track_memory) {
        for (j = 0; j < setnum; j++) {
            robj *obj = sets[j].set;
            if (!obj) continue;
            updateSlotAllocSize(c->db, getKeySlot(setkeys[j]->ptr), obj,
                                sets[j].oldsize, kvobjAllocSize(obj));
        }
    }

    /* Output the content of the resulting set, if not in STORE mode */
    if (cardinality_only) {
        if (approx) {
            cardinality = hllCount(hllobj->ptr, NULL);
            if (limit > 0 && cardinality > limit)
                cardinality = limit;
            decrRefCount(hllobj);
        }
        addReplyLongLong(c, cardinality);
        server.lazyfree_lazy_server_del ? freeObjAsync(NULL, dstset, -1) :
                                          decrRefCount(dstset);
    } else if (!dstkey) {
        addReplySetLen(c,cardinality);
        setTypeInitIterator(&si, dstset, SET_ITER_SKIP_EXPIRED);
        while (setTypeNext(&si, &str, &len, &llval) != -1) {
            if (str)
                addReplyBulkCBuffer(c, str, len);
            else
                addReplyBulkLongLong(c, llval);
        }
        setTypeResetIterator(&si);
        server.lazyfree_lazy_server_del ? freeObjAsync(NULL, dstset, -1) :
                                          decrRefCount(dstset);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        if (setTypeSize(dstset) > 0) {
            setKey(c, c->db, dstkey, &dstset, 0);
            if (setTypeHasExpireSupport(dstset)) setTypeUpdateSubexpiry(c->db, dstset);
            addReplyLongLong(c,setTypeSize(dstset));
            notifyKeyspaceEvent(NOTIFY_SET,
                op == SET_OP_UNION ? "sunionstore" : "sdiffstore",
                dstkey,c->db->id);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
            if (dbDelete(c->db,dstkey)) {
                server.dirty++;
                keyModified(c,c->db,dstkey,NULL,1);
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
            }
            decrRefCount(dstset);
        }
    }
    zfree(sets);
}

/* SUNION key [key ...] */
void sunionCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,SET_OP_UNION,0,0,0);
}

/* SUNIONCARD numkeys key [key ...] [APPROX] [LIMIT limit] */
void sunioncardCommand(client *c) {
    long j;
    long numkeys = 0;
    long limit = 0; /* 0 means no limit. */
    int approx = 0;

    if (getRangeLongFromObjectOrReply(c, c->argv[1], 1, LONG_MAX,
                                      &numkeys, "numkeys should be greater than 0") != C_OK)
        return;
    if (numkeys > (c->argc - 2)) {
        addReplyError(c, "Number of keys can't be greater than number of args");
        return;
    }

    for (j = 2 + numkeys; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc - 1) - j;

        if (!strcasecmp(opt, "LIMIT") && moreargs) {
            j++;
            if (getPositiveLongFromObjectOrReply(c, c->argv[j], &limit,
                                                 "LIMIT can't be negative") != C_OK)
                return;
        } else if (!strcasecmp(opt, "APPROX")) {
            approx = 1;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    /* Both the exact and approximate (HLL) cardinality are computed by the
     * generic union function; `approx` selects between them. */
    sunionDiffGenericCommand(c, c->argv+2, numkeys, NULL,
                             SET_OP_UNION, 1, approx, limit);
}

/* SUNIONSTORE destination key [key ...] */
void sunionstoreCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],SET_OP_UNION,0,0,0);
}

/* SDIFF key [key ...] */
void sdiffCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,SET_OP_DIFF,0,0,0);
}

/* SDIFFSTORE destination key [key ...] */
void sdiffstoreCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],SET_OP_DIFF,0,0,0);
}

/* SDIFFCARD numkeys key [key ...] [LIMIT limit] */
void sdiffcardCommand(client *c) {
    long j;
    long numkeys = 0;
    long limit = 0; /* 0 means no limit. */

    if (getRangeLongFromObjectOrReply(c, c->argv[1], 1, LONG_MAX,
                                      &numkeys, "numkeys should be greater than 0") != C_OK)
        return;
    if (numkeys > (c->argc - 2)) {
        addReplyError(c, "Number of keys can't be greater than number of args");
        return;
    }

    for (j = 2 + numkeys; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc - 1) - j;

        if (!strcasecmp(opt, "LIMIT") && moreargs) {
            j++;
            if (getPositiveLongFromObjectOrReply(c, c->argv[j], &limit,
                                                 "LIMIT can't be negative") != C_OK)
                return;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    sunionDiffGenericCommand(c, c->argv+2, numkeys, NULL, SET_OP_DIFF, 1, 0, limit);
}

void sscanCommand(client *c) {
    kvobj *set;
    unsigned long long cursor;
    size_t oldsize = 0;

    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,set,OBJ_SET)) return;
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(set);
    scanGenericCommand(c,set,cursor);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), set, oldsize, kvobjAllocSize(set));
}

/*=============================================================================
 * 5. Member expiration commands
 * SEXPIRE, SPEXPIRE, SEXPIREAT, SPEXPIREAT, STTL, SPTTL, SEXPIRETIME,
 * SPEXPIRETIME, SPERSIST and SADDEX.
 *=============================================================================*/

/*-----------------------------------------------------------------------------
 * SEXPIRE, SPEXPIRE, SEXPIREAT, SPEXPIREAT
 *
 *   SEXPIRE key seconds [NX | XX | GT | LT] MEMBERS nummembers member [member ...]
 *
 * The keywords can come in any order, as for the HEXPIRE family.
 *----------------------------------------------------------------------------*/

typedef struct {
    int membersPos;         /* Position of the MEMBERS keyword (-1 if not found) */
    int numMembersPos;      /* Position of the nummembers argument */
    int firstMemberPos;     /* Position of the first member */
    int memberCount;        /* Number of members */
    int expireTimePos;      /* Position of the expire time argument */
    long long expireTime;   /* Parsed expire time, absolute in milliseconds */
    int expireCondition;    /* SME_NX, SME_XX, SME_GT, SME_LT */
} SetExpireArgs;

/* Parser for the SEXPIRE family with flexible keyword ordering.
 * Returns C_OK on success, C_ERR on error (with the reply sent). */
static int parseSetExpireArgs(client *c, SetExpireArgs *args, long long basetime, int unit) {
    memset(args, 0, sizeof(*args));
    args->membersPos = -1;
    args->expireTimePos = 2;

    if (parseSubkeyExpireTime(c, c->argv[2], unit, basetime, &args->expireTime) != C_OK)
        return C_ERR;

    /* Parse the remaining arguments starting from position 3 */
    for (int i = 3; i < c->argc; i++) {
        char *arg = c->argv[i]->ptr;

        if (!strcasecmp(arg, "MEMBERS")) {
            if (args->membersPos != -1) {
                addReplyError(c, "MEMBERS keyword specified multiple times");
                return C_ERR;
            }

            if (i >= c->argc - 2) {
                addReplyError(c, "MEMBERS requires at least nummembers and one member argument");
                return C_ERR;
            }

            args->membersPos = i;
            args->numMembersPos = i + 1;
            long numMembers;
            if (getRangeLongFromObjectOrReply(c, c->argv[args->numMembersPos], 1, INT_MAX,
                                              &numMembers,
                                              "Parameter `numMembers` should be greater than 0") != C_OK)
                return C_ERR;

            args->firstMemberPos = i + 2;

            /* We must have exactly the right number of members */
            if (numMembers > c->argc - args->firstMemberPos) {
                addReplyError(c, "wrong number of arguments");
                return C_ERR;
            }

            args->memberCount = (int)numMembers;

            /* Skip over the member arguments */
            i = args->firstMemberPos + args->memberCount - 1;
            continue;
        }

        if (!strcasecmp(arg, "NX")) {
            args->expireCondition |= SME_NX;
        } else if (!strcasecmp(arg, "XX")) {
            args->expireCondition |= SME_XX;
        } else if (!strcasecmp(arg, "GT")) {
            args->expireCondition |= SME_GT;
        } else if (!strcasecmp(arg, "LT")) {
            args->expireCondition |= SME_LT;
        } else {
            addReplyErrorFormat(c, "unknown argument: %s", (char*) c->argv[i]->ptr);
            return C_ERR;
        }
    }

    if (args->membersPos == -1) {
        addReplyError(c, "missing MEMBERS argument");
        return C_ERR;
    }

    if (__builtin_popcount(args->expireCondition & (SME_NX|SME_XX|SME_GT|SME_LT)) > 1) {
        addReplyError(c, "Multiple condition flags specified");
        return C_ERR;
    }

    return C_OK;
}

static void sexpireGenericCommand(client *c, long long basetime, int unit) {
    SetExpireArgs args;
    int64_t oldlen, newlen;
    size_t oldsize = 0;
    robj *keyArg = c->argv[1];
    const int obey = mustObeyClient(c); /* From the master, or loading the AOF. */

    kvobj *set = lookupKeyWrite(c->db, keyArg);
    if (checkType(c, set, OBJ_SET))
        return;

    if (parseSetExpireArgs(c, &args, basetime, unit) != C_OK)
        return;

    /* Non-existing keys and empty sets are the same thing. It also means the
     * members in the command don't exist in the set. */
    if (!set) {
        addReplyArrayLen(c, args.memberCount);
        for (int i = 0; i < args.memberCount; i++)
            addReplyLongLong(c, SME_NO_MEMBER);
        return;
    }

    oldlen = (int64_t)setTypeSize(set);
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(set);

    /* Members are collected per outcome, for the subkey notifications. */
    memvec mvupdated, mvdeleted;
    vec *vupdated = memvecInit(&mvupdated, args.memberCount);
    vec *vdeleted = memvecInit(&mvdeleted, args.memberCount);
    /* Positions of the members that were not set, to remove them from the
     * propagated command. */
    int *notSet = NULL;
    int notSetCount = 0;

    addReplyArrayLen(c, args.memberCount);
    for (int i = 0; i < args.memberCount; i++) {
        int pos = args.firstMemberPos + i;
        sds member = c->argv[pos]->ptr;
        uint64_t cur;
        int res;

        int exists = setTypeGetExpire(set, member, &cur);
        if (obey) {
            /* Applying a command the master (or the AOF) already decided on: the
             * propagated SPEXPIREAT only lists members that were set, so don't
             * re-evaluate the member's old expiration (it may have lapsed on this
             * node's clock because of the replication lag or the loading time) nor
             * the conditions, just set the time, even if it is already in the past. */
            if (!exists) {
                res = SME_NO_MEMBER;
            } else {
                setTypeConvertToExpireEncoding(set);
                const setTypeOps *ops = setTypeGetOps(set->encoding);
                serverAssert(ops->setExpire(set, member, sdslen(member), 0, 1, (uint64_t)args.expireTime));
                vecPush(vupdated, c->argv[pos]);
                res = SME_UPDATED;
            }
        } else if (setMemberIsGone(exists, cur)) {
            res = SME_NO_MEMBER;
        } else if (((args.expireCondition & SME_NX) && cur != EB_EXPIRE_TIME_INVALID) ||
                   ((args.expireCondition & SME_XX) && cur == EB_EXPIRE_TIME_INVALID) ||
                   /* A member without an expiration counts as an infinite one. */
                   ((args.expireCondition & SME_GT) &&
                    (cur == EB_EXPIRE_TIME_INVALID || (uint64_t)args.expireTime <= cur)) ||
                   ((args.expireCondition & SME_LT) &&
                    (cur != EB_EXPIRE_TIME_INVALID && (uint64_t)args.expireTime >= cur)))
        {
            res = SME_COND_NOT_MET;
        } else if (checkAlreadyExpired(args.expireTime)) {
            /* The new expiration is in the past: the member is deleted. */
            setTypeRemove(set, member);
            propagateSetMemberDeletion(c->db, keyArg->ptr, member, sdslen(member));
            vecPush(vdeleted, c->argv[pos]);
            res = SME_DELETED;
        } else {
            setTypeConvertToExpireEncoding(set);
            const setTypeOps *ops = setTypeGetOps(set->encoding);
            serverAssert(ops->setExpire(set, member, sdslen(member), 0, 1, (uint64_t)args.expireTime));
            vecPush(vupdated, c->argv[pos]);
            res = SME_UPDATED;
        }

        if (res != SME_UPDATED) {
            if (notSet == NULL)
                notSet = zmalloc(sizeof(int) * args.memberCount);
            notSet[notSetCount++] = pos;
        }
        addReplyLongLong(c, res);
    }

    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(keyArg->ptr), set, oldsize, kvobjAllocSize(set));

    if (vecSize(vdeleted) + vecSize(vupdated) > 0) {
        server.dirty += vecSize(vdeleted) + vecSize(vupdated);
        keyModified(c, c->db, keyArg, set, 1);
        if (vecSize(vdeleted))
            notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "srem", keyArg, c->db->id,
                                           (robj**)vecData(vdeleted), vecSize(vdeleted));
        if (vecSize(vupdated))
            notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "sexpire", keyArg, c->db->id,
                                           (robj**)vecData(vupdated), vecSize(vupdated));
    }

    newlen = (int64_t)setTypeSize(set);
    if (newlen == 0) {
        newlen = -1;
        /* Delete the key without updating the keysizes, which is done below. */
        dbDeleteSkipKeysizesUpdate(c->db, keyArg);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", keyArg, c->db->id);
    } else if (vecSize(vupdated)) {
        setTypeUpdateSubexpiry(c->db, set);
    }
    if (oldlen != newlen)
        updateKeysizesHist(c->db, OBJ_SET, oldlen, newlen);

    /* If no member was set (the time is in the past and the SREMs were
     * already propagated, or the conditions were not met), propagating the
     * command is useless, and invalid with no members. */
    if (vecSize(vupdated) == 0) {
        preventCommandPropagation(c);
    } else {
        /* Rewrite to the canonical SPEXPIREAT command. */
        if (c->cmd->proc != spexpireatCommand) {
            rewriteClientCommandArgument(c, 0, shared.spexpireat);
            robj *expireTimeObj = createStringObjectFromLongLong(args.expireTime);
            rewriteClientCommandArgument(c, args.expireTimePos, expireTimeObj);
            decrRefCount(expireTimeObj);
        }
        /* For a partial result, remove the members that were not set. */
        if (notSetCount) {
            for (int i = notSetCount - 1; i >= 0; i--)
                rewriteClientCommandArgument(c, notSet[i], NULL);
            robj *count = createStringObjectFromLongLong(vecSize(vupdated));
            rewriteClientCommandArgument(c, args.membersPos + 1, count);
            decrRefCount(count);
        }
    }

    zfree(notSet);
    vecRelease(vupdated);
    vecRelease(vdeleted);
}

/* SEXPIRE key seconds [NX | XX | GT | LT] MEMBERS nummembers member [member ...] */
void sexpireCommand(client *c) {
    sexpireGenericCommand(c, commandTimeSnapshot(), UNIT_SECONDS);
}

/* SPEXPIRE key milliseconds [NX | XX | GT | LT] MEMBERS nummembers member [member ...] */
void spexpireCommand(client *c) {
    sexpireGenericCommand(c, commandTimeSnapshot(), UNIT_MILLISECONDS);
}

/* SEXPIREAT key unix-time-seconds [NX | XX | GT | LT] MEMBERS nummembers member [member ...] */
void sexpireatCommand(client *c) {
    sexpireGenericCommand(c, 0, UNIT_SECONDS);
}

/* SPEXPIREAT key unix-time-milliseconds [NX | XX | GT | LT] MEMBERS nummembers member [member ...] */
void spexpireatCommand(client *c) {
    sexpireGenericCommand(c, 0, UNIT_MILLISECONDS);
}

/*-----------------------------------------------------------------------------
 * STTL, SPTTL, SEXPIRETIME, SPEXPIRETIME and SPERSIST
 *
 *   STTL key MEMBERS nummembers member [member ...]
 *
 * The MEMBERS keyword must be right after the key.
 *----------------------------------------------------------------------------*/

/* Parses and validates the MEMBERS block at its fixed position. Returns C_OK
 * and sets numMembers on success, C_ERR (with the reply sent) otherwise. */
static int parseSetMembersBlock(client *c, long *numMembers) {
    const int numMembersAt = 3;

    if (strcasecmp(c->argv[numMembersAt-1]->ptr, "MEMBERS")) {
        addReplyError(c, "Mandatory argument MEMBERS is missing or not at the right position");
        return C_ERR;
    }

    if (getRangeLongFromObjectOrReply(c, c->argv[numMembersAt], 1, LONG_MAX,
                                      numMembers, "Number of members must be a positive integer") != C_OK)
        return C_ERR;

    /* Verify `numMembers` is consistent with the number of arguments */
    if (*numMembers != (c->argc - numMembersAt - 1)) {
        addReplyError(c, "The `nummembers` parameter must match the number of arguments");
        return C_ERR;
    }
    return C_OK;
}

static void sttlGenericCommand(client *c, long long basetime, int unit) {
    long numMembers = 0;

    kvobj *set = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, set, OBJ_SET))
        return;

    if (parseSetMembersBlock(c, &numMembers) != C_OK)
        return;

    addReplyArrayLen(c, numMembers);
    for (long i = 0; i < numMembers; i++) {
        sds member = c->argv[4 + i]->ptr;
        uint64_t expire = EB_EXPIRE_TIME_INVALID;
        /* Non-existing keys and empty sets are the same thing. This command only
         * reads: a member that is logically expired but not removed yet is
         * reported as missing, and nothing is deleted. */
        int exists = set ? setTypeGetExpire(set, member, &expire) : 0;
        if (!exists) {
            addReplyLongLong(c, SME_NO_MEMBER);
        } else if (expire == EB_EXPIRE_TIME_INVALID) {
            addReplyLongLong(c, SME_NO_TTL);
        } else if ((long long)expire < commandTimeSnapshot()) {
            addReplyLongLong(c, SME_NO_MEMBER);
        } else if (unit == UNIT_SECONDS) {
            addReplyLongLong(c, (expire + 999 - basetime) / 1000);
        } else {
            addReplyLongLong(c, expire - basetime);
        }
    }
}

/* STTL key MEMBERS nummembers member [member ...] */
void sttlCommand(client *c) {
    sttlGenericCommand(c, commandTimeSnapshot(), UNIT_SECONDS);
}

/* SPTTL key MEMBERS nummembers member [member ...] */
void spttlCommand(client *c) {
    sttlGenericCommand(c, commandTimeSnapshot(), UNIT_MILLISECONDS);
}

/* SEXPIRETIME key MEMBERS nummembers member [member ...] */
void sexpiretimeCommand(client *c) {
    sttlGenericCommand(c, 0, UNIT_SECONDS);
}

/* SPEXPIRETIME key MEMBERS nummembers member [member ...] */
void spexpiretimeCommand(client *c) {
    sttlGenericCommand(c, 0, UNIT_MILLISECONDS);
}

/* SPERSIST key MEMBERS nummembers member [member ...] */
void spersistCommand(client *c) {
    long numMembers = 0;
    size_t oldsize = 0;
    robj *keyArg = c->argv[1];
    const int obey = mustObeyClient(c); /* From the master, or loading the AOF. */

    kvobj *set = lookupKeyWrite(c->db, keyArg);
    if (checkType(c, set, OBJ_SET))
        return;

    if (parseSetMembersBlock(c, &numMembers) != C_OK)
        return;

    /* Non-existing keys and empty sets are the same thing. It also means the
     * members in the command don't exist in the set. */
    if (!set) {
        addReplyArrayLen(c, numMembers);
        for (long i = 0; i < numMembers; i++)
            addReplyLongLong(c, SME_NO_MEMBER);
        return;
    }

    /* Track which members were successfully persisted, for the notification. */
    memvec mvpersisted;
    vec *vpersisted = memvecInit(&mvpersisted, numMembers);
    /* Positions of the members that were not persisted, to remove them from the
     * propagated command. */
    int *notPersisted = NULL;
    int notPersistedCount = 0;
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(set);

    addReplyArrayLen(c, numMembers);
    for (long i = 0; i < numMembers; i++) {
        robj *memberObj = c->argv[4 + i];
        sds member = memberObj->ptr;
        uint64_t expire = EB_EXPIRE_TIME_INVALID;

        int exists = setTypeGetExpire(set, member, &expire);
        int res;
        if (!exists) {
            res = SME_NO_MEMBER;
        } else if (expire == EB_EXPIRE_TIME_INVALID) {
            res = SME_NO_TTL;
        } else if (!obey && (long long)expire < commandTimeSnapshot()) {
            /* Already expired. Pretend there is no such member. A master or the
             * AOF is not evaluated against this node's clock: the propagated
             * command only lists the members the master persisted. */
            res = SME_NO_MEMBER;
        } else {
            serverAssert(setTypeGetOps(set->encoding)->setExpire(set, member, sdslen(member), 0, 1,
                                                                 EB_EXPIRE_TIME_INVALID));
            vecPush(vpersisted, memberObj);
            res = SME_PERSISTED;
        }
        if (res != SME_PERSISTED) {
            if (notPersisted == NULL)
                notPersisted = zmalloc(sizeof(int) * numMembers);
            notPersisted[notPersistedCount++] = 4 + i;
        }
        addReplyLongLong(c, res);
    }

    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(keyArg->ptr), set, oldsize, kvobjAllocSize(set));

    if (vecSize(vpersisted)) {
        setTypeUpdateSubexpiry(c->db, set);
        server.dirty += vecSize(vpersisted);
        keyModified(c, c->db, keyArg, set, 1);
        notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "spersist", keyArg, c->db->id,
                                       (robj**)vecData(vpersisted), vecSize(vpersisted));
        /* For a partial result, remove the members that were not persisted, so
         * a replica persists exactly the members the master did. */
        if (notPersistedCount) {
            for (int i = notPersistedCount - 1; i >= 0; i--)
                rewriteClientCommandArgument(c, notPersisted[i], NULL);
            robj *count = createStringObjectFromLongLong(vecSize(vpersisted));
            rewriteClientCommandArgument(c, 3, count);
            decrRefCount(count);
        }
    } else {
        /* Nothing changed: nothing to propagate. */
        preventCommandPropagation(c);
    }
    zfree(notPersisted);
    vecRelease(vpersisted);
}

/*-----------------------------------------------------------------------------
 * SADDEX
 *
 *   SADDEX key [MNX | MXX]
 *       [EX seconds | PX milliseconds | EXAT unix-time-seconds | PXAT unix-time-milliseconds | KEEPTTL]
 *       MEMBERS nummembers member [member ...]
 *
 * Reply: Integer 0 if no member was added, updated or deleted (MNX/MXX condition
 *        not met, or nothing changed), 1 if at least one member was.
 *----------------------------------------------------------------------------*/

/* Parses the SADDEX arguments, in any order of the keywords. Returns C_OK on
 * success, C_ERR (with the reply sent) otherwise. */
static int parseSaddexArgs(client *c, int *flags, long long *expireTime, int *expireTimePos,
                           int *firstMemberPos, int *memberCount) {
    *flags = 0;
    *firstMemberPos = -1;
    *memberCount = -1;
    *expireTimePos = -1;
    const int expireFlags = SME_EX | SME_EXAT | SME_PX | SME_PXAT | SME_KEEPTTL;

    for (int i = 2; i < c->argc; i++) {
        char *arg = c->argv[i]->ptr;

        if (!strcasecmp(arg, "MEMBERS")) {
            /* Ensure only one MEMBERS argument is provided */
            if (*firstMemberPos != -1) {
                addReplyError(c, "MEMBERS keyword specified multiple times");
                return C_ERR;
            }

            long val;
            /* Ensure we have at least the nummembers argument */
            if (i + 1 >= c->argc) {
                addReplyErrorArity(c);
                return C_ERR;
            }

            if (getRangeLongFromObjectOrReply(c, c->argv[i + 1], 1, INT_MAX, &val,
                                              "invalid number of members") != C_OK)
                return C_ERR;

            *firstMemberPos = i + 2;
            *memberCount = (int) val;

            if ((long long)*firstMemberPos + *memberCount > c->argc) {
                addReplyError(c, "wrong number of arguments");
                return C_ERR;
            }

            /* Skip over nummembers and the members. Set i to the last position
             * of the MEMBERS block, the loop will increment past it. */
            i = *firstMemberPos + *memberCount - 1;
            continue;
        }

        int unit = 0, flag = 0;
        long long basetime = 0;
        if (!strcasecmp(arg, "EX")) {
            flag = SME_EX; unit = UNIT_SECONDS; basetime = commandTimeSnapshot();
        } else if (!strcasecmp(arg, "PX")) {
            flag = SME_PX; unit = UNIT_MILLISECONDS; basetime = commandTimeSnapshot();
        } else if (!strcasecmp(arg, "EXAT")) {
            flag = SME_EXAT; unit = UNIT_SECONDS;
        } else if (!strcasecmp(arg, "PXAT")) {
            flag = SME_PXAT; unit = UNIT_MILLISECONDS;
        }

        if (flag) {
            if (*flags & expireFlags) {
                addReplyError(c, "Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments can be specified");
                return C_ERR;
            }
            if (i >= c->argc - 1) {
                addReplyError(c, "missing expire time");
                return C_ERR;
            }
            *flags |= flag;
            i++;
            if (parseSubkeyExpireTime(c, c->argv[i], unit, basetime, expireTime) != C_OK)
                return C_ERR;
            *expireTimePos = i;
        } else if (!strcasecmp(arg, "KEEPTTL")) {
            if (*flags & expireFlags) {
                addReplyError(c, "Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments can be specified");
                return C_ERR;
            }
            *flags |= SME_KEEPTTL;
        } else if (!strcasecmp(arg, "MXX") || !strcasecmp(arg, "MNX")) {
            if (*flags & (SME_MXX | SME_MNX)) {
                addReplyError(c, "Only one of MXX or MNX arguments can be specified");
                return C_ERR;
            }
            *flags |= !strcasecmp(arg, "MXX") ? SME_MXX : SME_MNX;
        } else {
            addReplyErrorFormat(c, "unknown argument: %s", (char*) c->argv[i]->ptr);
            return C_ERR;
        }
    }

    /* Ensure MEMBERS is specified */
    if (*firstMemberPos == -1) {
        addReplyError(c, "missing MEMBERS argument");
        return C_ERR;
    }

    return C_OK;
}

void saddexCommand(client *c) {
    int flags = 0, firstMemberPos = 0, memberCount = 0, expireTimePos = -1;
    const int obey = mustObeyClient(c); /* From the master, or loading the AOF. */
    long long expireTime = EB_EXPIRE_TIME_INVALID;
    int64_t oldlen, newlen;
    size_t oldsize = 0;
    dictEntryLink link;
    robj *keyArg = c->argv[1];

    if (parseSaddexArgs(c, &flags, &expireTime, &expireTimePos, &firstMemberPos, &memberCount) != C_OK)
        return;

    kvobj *set = lookupKeyWriteWithLink(c->db, keyArg, &link);
    if (checkType(c, set, OBJ_SET))
        return;

    /* With MXX all the members must exist, so a missing key means nothing is added. */
    if (!set && (flags & SME_MXX)) {
        addReplyLongLong(c, 0);
        return;
    }

    const int setsExpire = (flags & (SME_EX | SME_PX | SME_EXAT | SME_PXAT)) != 0;
    /* An expiration in the past deletes the members instead of adding them. */
    const int pastTime = setsExpire && checkAlreadyExpired(expireTime);

    /* The conditions are all or nothing: check them before changing anything. A
     * member that is logically expired but not removed yet does not exist. A
     * master or the AOF already evaluated them, and the local clock must not
     * overrule it. */
    if (set && !obey && (flags & (SME_MXX | SME_MNX))) {
        int found = 0;
        for (int i = 0; i < memberCount; i++) {
            uint64_t cur;
            int exists = setTypeGetExpire(set, c->argv[firstMemberPos + i]->ptr, &cur);
            found += !setMemberIsGone(exists, cur);
        }
        if (((flags & SME_MNX) && found != 0) || ((flags & SME_MXX) && found != memberCount)) {
            addReplyLongLong(c, 0);
            return;
        }
    }

    /* Counts the members that were added, deleted or got a different expiration.
     * Only they are counted as changes, notified and propagated: the reply is 0,
     * and nothing is propagated, if there was none, even if the conditions were met. */
    int modified = 0;
    memvec mvset, mvupdated, mvdeleted;
    vec *vset = memvecInit(&mvset, memberCount);
    vec *vupdated = memvecInit(&mvupdated, memberCount);
    vec *vdeleted = memvecInit(&mvdeleted, memberCount);

    if (pastTime) {
        /* The members that exist are deleted, and the ones that do not exist
         * are not added, so a missing key is not even created. */
        if (set) {
            oldlen = (int64_t)setTypeSize(set);
            if (server.memory_tracking_enabled)
                oldsize = kvobjAllocSize(set);
            for (int i = 0; i < memberCount; i++) {
                robj *memberObj = c->argv[firstMemberPos + i];
                sds member = memberObj->ptr;
                if (setTypeRemove(set, member)) {
                    propagateSetMemberDeletion(c->db, keyArg->ptr, member, sdslen(member));
                    vecPush(vdeleted, memberObj);
                    modified++;
                }
            }
        } else {
            oldlen = 0;
        }
    } else {
        if (!set) {
            set = setTypeCreate(c->argv[firstMemberPos]->ptr, memberCount);
            dbAddByLink(c->db, keyArg, &set, &link);
        }
        oldlen = (int64_t)setTypeSize(set);
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(set);

        if (setsExpire)
            setTypeConvertToExpireEncoding(set);

        for (int i = 0; i < memberCount; i++) {
            robj *memberObj = c->argv[firstMemberPos + i];
            sds member = memberObj->ptr;
            uint64_t cur;
            int exists = setTypeGetExpire(set, member, &cur);
            /* A member that is logically expired but not removed yet is added again. */
            int changed = (!obey && setMemberIsGone(exists, cur)) || !exists ||
                          (setsExpire ? cur != (uint64_t)expireTime
                                      : (cur != EB_EXPIRE_TIME_INVALID && !(flags & SME_KEEPTTL)));
            if (!changed) continue;
            modified++;
            if (setsExpire) {
                /* Add or update the member, and set its expiration. The set
                 * grows to a bigger encoding, keeping the expirations, if needed. */
                setTypeAddExAux(set, member, sdslen(member), 0, 1, (uint64_t)expireTime);
                vecPush(vupdated, memberObj);
            } else if (!setTypeAdd(set, member) && setTypeHasExpireSupport(set) &&
                       !(flags & SME_KEEPTTL))
            {
                /* The member exists: without KEEPTTL, its expiration is discarded. */
                setTypeGetOps(set->encoding)->setExpire(set, member, sdslen(member), 0, 1,
                                                        EB_EXPIRE_TIME_INVALID);
            }
            vecPush(vset, memberObj);
        }
    }

    if (server.memory_tracking_enabled && set)
        updateSlotAllocSize(c->db, getKeySlot(keyArg->ptr), set, oldsize, kvobjAllocSize(set));

    if (vecSize(vset) + vecSize(vdeleted) > 0) {
        server.dirty += vecSize(vset) + vecSize(vdeleted);
        keyModified(c, c->db, keyArg, set, 1);
        if (vecSize(vset))
            notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "sadd", keyArg, c->db->id,
                                           (robj**)vecData(vset), vecSize(vset));
        if (vecSize(vdeleted))
            notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "srem", keyArg, c->db->id,
                                           (robj**)vecData(vdeleted), vecSize(vdeleted));
        if (vecSize(vupdated))
            notifyKeyspaceEventWithSubkeys(NOTIFY_SET, "sexpire", keyArg, c->db->id,
                                           (robj**)vecData(vupdated), vecSize(vupdated));
    }

    if (set) {
        newlen = (int64_t)setTypeSize(set);
        if (newlen == 0) {
            newlen = -1;
            /* Delete the key without updating the keysizes, which is done below. */
            dbDeleteSkipKeysizesUpdate(c->db, keyArg);
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", keyArg, c->db->id);
        } else if (setTypeHasExpireSupport(set)) {
            setTypeUpdateSubexpiry(c->db, set);
        }
        if (oldlen != newlen)
            updateKeysizesHist(c->db, OBJ_SET, oldlen, newlen);
    }

    if (pastTime) {
        /* The SREMs of the deleted members were already propagated. */
        preventCommandPropagation(c);
    } else if (modified == 0) {
        preventCommandPropagation(c);
    } else if (setsExpire && !(flags & SME_PXAT)) {
        /* Propagate with an absolute time in milliseconds: PXAT. */
        rewriteClientCommandArgument(c, expireTimePos - 1, shared.pxat);
        robj *expire = createStringObjectFromLongLong(expireTime);
        rewriteClientCommandArgument(c, expireTimePos, expire);
        decrRefCount(expire);
    }

    addReplyLongLong(c, modified > 0);

    vecRelease(vset);
    vecRelease(vupdated);
    vecRelease(vdeleted);
}
