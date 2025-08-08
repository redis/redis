/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "ebuckets.h"
#include <math.h>

/* Threshold for HEXPIRE and HPERSIST to be considered whether it is worth to
 * update the expiration time of the hash object in global HFE DS. */
#define HASH_NEW_EXPIRE_DIFF_THRESHOLD max(4000, 1<<EB_BUCKET_KEY_PRECISION)

/* Reserve 2 bits out of hash-field expiration time for possible future lightweight
 * indexing/categorizing of fields. It can be achieved by hacking HFE as follows:
 *
 *    HPEXPIREAT key [ 2^47 + USER_INDEX ] FIELDS numfields field [field …]
 *
 * Redis will also need to expose kind of HEXPIRESCAN and HEXPIRECOUNT for this
 * idea. Yet to be better defined.
 *
 * HFE_MAX_ABS_TIME_MSEC constraint must be enforced only at API level. Internally,
 * the expiration time can be up to EB_EXPIRE_TIME_MAX for future readiness.
 */
#define HFE_MAX_ABS_TIME_MSEC (EB_EXPIRE_TIME_MAX >> 2)

typedef enum GetFieldRes {
    /* common (Used by hashTypeGet* value family) */
    GETF_OK = 0,            /* The field was found. */
    GETF_NOT_FOUND,         /* The field was not found. */
    GETF_EXPIRED,           /* Logically expired (Might be lazy deleted or not) */
    GETF_EXPIRED_HASH,      /* Delete hash since retrieved field was expired and
                             * it was the last field in the hash. */
} GetFieldRes;

/* ActiveExpireCtx passed to hashTypeActiveExpire() */
typedef struct ExpireCtx {
    uint32_t fieldsToExpireQuota;
    redisDb *db;
} ExpireCtx;

typedef listpackEntry CommonEntry; /* extend usage beyond lp */

/* hash field expiration (HFE) funcs */
static ExpireAction onFieldExpire(eItem item, void *ctx);
static ExpireMeta* hfieldGetExpireMeta(const eItem field);
static ExpireMeta *hashGetExpireMeta(const eItem hash);
static void hexpireGenericCommand(client *c, long long basetime, int unit);
static ExpireAction hashTypeActiveExpire(eItem hashObj, void *ctx);
static uint64_t hashTypeExpire(kvobj *kv, ExpireCtx *expireCtx, int updateGlobalHFE);
static void hfieldPersist(robj *hashObj, hfield field);
static void propagateHashFieldDeletion(redisDb *db, sds key, char *field, size_t fieldLen);

/* hash dictType funcs */
static int dictHfieldKeyCompare(dictCmpCache *cache, const void *key1, const void *key2);
static uint64_t dictMstrHash(const void *key);
static void dictHfieldDestructor(dict *d, void *field);
static size_t hashDictWithExpireMetadataBytes(dict *d);
static void hashDictWithExpireOnRelease(dict *d);
static kvobj* hashTypeLookupWriteOrCreate(client *c, robj *key);

/*-----------------------------------------------------------------------------
 * Define dictType of hash
 *
 * - Stores fields as mstr strings with optional metadata to attach TTL
 * - Note that small hashes are represented with listpacks
 * - Once expiration is set for a field, the dict instance and corresponding
 *   dictType are replaced with a dict containing metadata for Hash Field
 *   Expiration (HFE) and using dictType `mstrHashDictTypeWithHFE`
 *----------------------------------------------------------------------------*/
dictType mstrHashDictType = {
    dictSdsHash,                                /* lookup hash function */
    NULL,                                       /* key dup */
    NULL,                                       /* val dup */
    dictSdsMstrKeyCompare,                      /* lookup key compare */
    dictHfieldDestructor,                       /* key destructor */
    dictSdsDestructor,                          /* val destructor */
    .storedHashFunction = dictMstrHash,         /* stored hash function */
    .storedKeyCompare = dictHfieldKeyCompare,   /* stored key compare */
};

/* Define alternative dictType of hash with hash-field expiration (HFE) support */
dictType mstrHashDictTypeWithHFE = {
    dictSdsHash,                                /* lookup hash function */
    NULL,                                       /* key dup */
    NULL,                                       /* val dup */
    dictSdsMstrKeyCompare,                      /* lookup key compare */
    dictHfieldDestructor,                       /* key destructor */
    dictSdsDestructor,                          /* val destructor */
    .storedHashFunction = dictMstrHash,         /* stored hash function */
    .storedKeyCompare = dictHfieldKeyCompare,   /* stored key compare */
    .dictMetadataBytes = hashDictWithExpireMetadataBytes,
    .onDictRelease = hashDictWithExpireOnRelease,
};

/*-----------------------------------------------------------------------------
 * Hash Field Expiration (HFE) Feature
 *
 * Each hash instance maintains its own set of hash field expiration within its
 * private ebuckets DS. In order to support HFE active expire cycle across hash
 * instances, hashes with associated HFE will be also registered in a global
 * ebuckets DS with expiration time value that reflects their next minimum
 * time to expire. The global HFE Active expiration will be triggered from
 * activeExpireCycle() function and will invoke "local" HFE Active expiration
 * for each hash instance that has expired fields.
 *
 * hashExpireBucketsType - ebuckets-type to be used at the global space
 * (db->hexpires) to register hashes that have one or more fields with time-Expiration.
 * The hashes will be registered in with the expiration time of the earliest field
 * in the hash.
 *----------------------------------------------------------------------------*/
EbucketsType hashExpireBucketsType = {
    .onDeleteItem = NULL,
    .getExpireMeta = hashGetExpireMeta,   /* get ExpireMeta attached to each hash */
    .itemsAddrAreOdd = 0,                 /* Addresses of dict are even */
};

/* dictExpireMetadata - ebuckets-type for hash fields with time-Expiration. ebuckets
 * instance Will be attached to each hash that has at least one field with expiry
 * time. */
EbucketsType hashFieldExpireBucketsType = {
    .onDeleteItem = NULL,
    .getExpireMeta = hfieldGetExpireMeta, /* get ExpireMeta attached to each field */
    .itemsAddrAreOdd = 1,                 /* Addresses of hfield (mstr) are odd!! */
};

/* OnFieldExpireCtx passed to OnFieldExpire() */
typedef struct OnFieldExpireCtx {
    robj *hashObj;
    redisDb *db;
} OnFieldExpireCtx;

/* The implementation of hashes by dict was modified from storing fields as sds
 * strings to store "mstr" (Immutable string with metadata) in order to be able to
 * attach TTL (ExpireMeta) to the hash-field. This usage of mstr opens up the
 * opportunity for future features to attach additional metadata by need to the
 * fields.
 *
 * The following defines new hfield kind of mstr */
typedef enum HfieldMetaFlags {
    HFIELD_META_EXPIRE = 0,
} HfieldMetaFlags;

mstrKind mstrFieldKind = {
    .name = "hField",

    /* Taking care that all metaSize[*] values are even ensures that all
     * addresses of hfield instances will be odd. */
    .metaSize[HFIELD_META_EXPIRE] = sizeof(ExpireMeta),
};
static_assert(sizeof(struct ExpireMeta ) % 2 == 0, "must be even!");

/* Used by hpersistCommand() */
typedef enum SetPersistRes {
    HFE_PERSIST_NO_FIELD =     -2,   /* No such hash-field */
    HFE_PERSIST_NO_TTL =       -1,   /* No TTL attached to the field */
    HFE_PERSIST_OK =            1
} SetPersistRes;

static inline int isDictWithMetaHFE(dict *d) {
    return d->type == &mstrHashDictTypeWithHFE;
}

/*-----------------------------------------------------------------------------
 * setex* - Set field's expiration
 *
 * Setting expiration time to fields might be time-consuming and complex since
 * each update of expiration time, not only updates `ebuckets` of corresponding
 * hash, but also might update `ebuckets` of global HFE DS. It is required to opt
 * sequence of field updates with expirartion for a given hash, such that only
 * once done, the global HFE DS will get updated.
 *
 * To do so, follow the scheme:
 * 1. Call hashTypeSetExInit() to initialize the HashTypeSetEx struct.
 * 2. Call hashTypeSetEx() one time or more, for each field/expiration update.
 * 3. Call hashTypeSetExDone() for notification and update of global HFE.
 *----------------------------------------------------------------------------*/

/* Returned value of hashTypeSetEx() */
typedef enum SetExRes {
    HSETEX_OK =                1,   /* Expiration time set/updated as expected */
    HSETEX_NO_FIELD =         -2,   /* No such hash-field */
    HSETEX_NO_CONDITION_MET =  0,   /* Specified NX | XX | GT | LT condition not met */
    HSETEX_DELETED =           2,   /* Field deleted because the specified time is in the past */
} SetExRes;

/* Used by httlGenericCommand() */
typedef enum GetExpireTimeRes {
    HFE_GET_NO_FIELD =          -2, /* No such hash-field */
    HFE_GET_NO_TTL =            -1, /* No TTL attached to the field */
} GetExpireTimeRes;

typedef enum ExpireSetCond {
    HFE_NX = 1<<0,
    HFE_XX = 1<<1,
    HFE_GT = 1<<2,
    HFE_LT = 1<<3
} ExpireSetCond;

/* Used by hashTypeSetEx() for setting fields or their expiry  */
typedef struct HashTypeSetEx {

    /*** config ***/
    ExpireSetCond expireSetCond;        /* [XX | NX | GT | LT] */

    /*** metadata ***/
    uint64_t minExpire;                 /* if uninit EB_EXPIRE_TIME_INVALID */
    redisDb *db;
    robj *key, *hashObj;
    uint64_t minExpireFields;           /* Trace updated fields and their previous/new
                                         * minimum expiration time. If minimum recorded
                                         * is above minExpire of the hash, then we don't
                                         * have to update global HFE DS */

    /* Optionally provide client for notification */
    client *c;
    const char *cmd;
} HashTypeSetEx;

int hashTypeSetExInit(robj *key, kvobj *kvo, client *c, redisDb *db,
                      ExpireSetCond expireSetCond, HashTypeSetEx *ex);

SetExRes hashTypeSetEx(robj *o, sds field, uint64_t expireAt, HashTypeSetEx *exInfo);

void hashTypeSetExDone(HashTypeSetEx *e);

/*-----------------------------------------------------------------------------
 * Accessor functions for dictType of hash
 *----------------------------------------------------------------------------*/

static int dictHfieldKeyCompare(dictCmpCache *cache, const void *key1, const void *key2)
{
    int l1,l2;
    UNUSED(cache);

    l1 = hfieldlen((hfield)key1);
    l2 = hfieldlen((hfield)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

static uint64_t dictMstrHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, mstrlen((char*)key));
}

static void dictHfieldDestructor(dict *d, void *field) {

    /* If attached TTL to the field, then remove it from hash's private ebuckets. */
    if (hfieldGetExpireTime(field) != EB_EXPIRE_TIME_INVALID) {
        dictExpireMetadata *dictExpireMeta = (dictExpireMetadata *) dictMetadata(d);
        ebRemove(&dictExpireMeta->hfe, &hashFieldExpireBucketsType, field);
    }

    hfieldFree(field);

    /* Don't have to update global HFE DS. It's unnecessary. Implementing this
     * would introduce significant complexity and overhead for an operation that
     * isn't critical. In the worst case scenario, the hash will be efficiently
     * updated later by an active-expire operation, or it will be removed by the
     * hash's dbGenericDelete() function. */
}

static size_t hashDictWithExpireMetadataBytes(dict *d) {
    UNUSED(d);
    /* expireMeta of the hash, ref to ebuckets and pointer to hash's key */
    return sizeof(dictExpireMetadata);
}

static void hashDictWithExpireOnRelease(dict *d) {
    /* for sure allocated with metadata. Otherwise, this func won't be registered */
    dictExpireMetadata *dictExpireMeta = (dictExpireMetadata *) dictMetadata(d);
    ebDestroy(&dictExpireMeta->hfe, &hashFieldExpireBucketsType, NULL);
}

/*-----------------------------------------------------------------------------
 * listpackEx functions
 *----------------------------------------------------------------------------*/
/*
 * If any of hash field expiration command is called on a listpack hash object
 * for the first time, we convert it to OBJ_ENCODING_LISTPACK_EX encoding.
 * We allocate "struct listpackEx" which holds listpack pointer and expiry
 * metadata. In the listpack string, we append another TTL entry for each field
 * value pair. From now on, listpack will have triplets in it: field-value-ttl.
 * If TTL is not set for a field, we store 'zero' as the TTL value. 'zero' is
 * encoded as two bytes in the listpack. Memory overhead of a non-existing TTL
 * will be two bytes per field.
 *
 * Fields in the listpack will be ordered by TTL. Field with the smallest expiry
 * time will be the first item. Fields without TTL will be at the end of the
 * listpack. This way, it is easier/faster to find expired items.
 */

#define HASH_LP_NO_TTL 0

struct listpackEx *listpackExCreate(void) {
    listpackEx *lpt = zcalloc(sizeof(*lpt));
    lpt->meta.trash = 1;
    lpt->lp = NULL;
    return lpt;
}

static void listpackExFree(listpackEx *lpt) {
    lpFree(lpt->lp);
    zfree(lpt);
}

struct lpFingArgs {
    uint64_t max_to_search; /* [in] Max number of tuples to search */
    uint64_t expire_time;   /* [in] Find the tuple that has a TTL larger than expire_time */
    unsigned char *p;       /* [out] First item of the tuple that has a TTL larger than expire_time */
    int expired;            /* [out] Number of tuples that have TTLs less than expire_time */
    int index;              /* Internally used */
    unsigned char *fptr;    /* Internally used, temp ptr */
};

/* Callback for lpFindCb(). Used to find number of expired fields as part of
 * active expiry or when trying to find the position for the new field according
 * to its expiry time.*/
static int cbFindInListpack(const unsigned char *lp, unsigned char *p,
                            void *user, unsigned char *s, long long slen)
{
    (void) lp;
    struct lpFingArgs *r = user;

    r->index++;

    if (r->max_to_search == 0)
        return 0; /* Break the loop and return */

    if (r->index % 3 == 1) {
        r->fptr = p;  /* First item of the tuple. */
    } else if (r->index % 3 == 0) {
        serverAssert(!s);

        /* Third item of a tuple is expiry time */
        if (slen == HASH_LP_NO_TTL || (uint64_t) slen >= r->expire_time) {
            r->p = r->fptr;
            return 0; /* Break the loop and return */
        }
        r->expired++;
        r->max_to_search--;
    }

    return 1;
}

/* Returns number of expired fields. */
static uint64_t listpackExExpireDryRun(const robj *o) {
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK_EX);

    listpackEx *lpt = o->ptr;

    struct lpFingArgs r = {
        .max_to_search = UINT64_MAX,
        .expire_time = commandTimeSnapshot(),
    };

    lpFindCb(lpt->lp, NULL, &r, cbFindInListpack, 0);
    return r.expired;
}

/* Returns the expiration time of the item with the nearest expiration. */
static uint64_t listpackExGetMinExpire(robj *o) {
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK_EX);

    long long expireAt;
    unsigned char *fptr;
    listpackEx *lpt = o->ptr;

    /* As fields are ordered by expire time, first field will have the smallest
     * expiry time. Third element is the expiry time of the first field */
    fptr = lpSeek(lpt->lp, 2);
    if (fptr != NULL) {
        serverAssert(lpGetIntegerValue(fptr, &expireAt));

        /* Check if this is a non-volatile field. */
        if (expireAt != HASH_LP_NO_TTL)
            return expireAt;
    }

    return EB_EXPIRE_TIME_INVALID;
}

/* Walk over fields and delete the expired ones. */
void listpackExExpire(redisDb *db, kvobj *kv, ExpireInfo *info) {
    serverAssert(kv->encoding == OBJ_ENCODING_LISTPACK_EX);
    uint64_t expired = 0, min = EB_EXPIRE_TIME_INVALID;
    unsigned char *ptr;
    listpackEx *lpt = kv->ptr;

    ptr = lpFirst(lpt->lp);

    sds key = kvobjGetKey(kv);

    while (ptr != NULL && (info->itemsExpired < info->maxToExpire)) {
        long long val;
        int64_t flen;
        unsigned char intbuf[LP_INTBUF_SIZE], *fref;

        fref = lpGet(ptr, &flen, intbuf);

        ptr = lpNext(lpt->lp, ptr);
        serverAssert(ptr);
        ptr = lpNext(lpt->lp, ptr);
        serverAssert(ptr && lpGetIntegerValue(ptr, &val));

        /* Fields are ordered by expiry time. If we reached to a non-expired
         * or a non-volatile field, we know rest is not yet expired. */
        if (val == HASH_LP_NO_TTL || (uint64_t) val > info->now)
            break;

        propagateHashFieldDeletion(db, key, (char *)((fref) ? fref : intbuf), flen);
        server.stat_expired_subkeys++;

        ptr = lpNext(lpt->lp, ptr);

        info->itemsExpired++;
        expired++;
    }

    if (expired) {
        lpt->lp = lpDeleteRange(lpt->lp, 0, expired * 3);
        
        /* update keysizes */
        unsigned long l = lpLength(lpt->lp) / 3;
        updateKeysizesHist(db, getKeySlot(key), OBJ_HASH, l + expired, l);
    }

    min = hashTypeGetMinExpire(kv, 1 /*accurate*/);
    info->nextExpireTime = min;
}

static void listpackExAddInternal(robj *o, listpackEntry ent[3]) {
    listpackEx *lpt = o->ptr;

    /* Shortcut, just append at the end if this is a non-volatile field. */
    if (ent[2].lval == HASH_LP_NO_TTL) {
        lpt->lp = lpBatchAppend(lpt->lp, ent, 3);
        return;
    }

    struct lpFingArgs r = {
            .max_to_search = UINT64_MAX,
            .expire_time = ent[2].lval,
    };

    /* Check if there is a field with a larger TTL. */
    lpFindCb(lpt->lp, NULL, &r, cbFindInListpack, 0);

    /* If list is empty or there is no field with a larger TTL, result will be
     * NULL. Otherwise, just insert before the found item.*/
    if (r.p)
        lpt->lp = lpBatchInsert(lpt->lp, r.p, LP_BEFORE, ent, 3, NULL);
    else
        lpt->lp = lpBatchAppend(lpt->lp, ent, 3);
}

/* Add new field ordered by expire time. */
void listpackExAddNew(robj *o, char *field, size_t flen,
                      char *value, size_t vlen, uint64_t expireAt) {
    listpackEntry ent[3] = {
        {.sval = (unsigned char*) field, .slen = flen},
        {.sval = (unsigned char*) value, .slen = vlen},
        {.lval = expireAt}
    };

    listpackExAddInternal(o, ent);
}

/* If expiry time is changed, this function will place field into the correct
 * position. First, it deletes the field and re-inserts to the listpack ordered
 * by expiry time. */
static void listpackExUpdateExpiry(robj *o, sds field,
                                   unsigned char *fptr,
                                   unsigned char *vptr,
                                   uint64_t expire_at) {
    unsigned int slen = 0;
    long long val = 0;
    unsigned char tmp[512] = {0};
    unsigned char *valstr;
    sds tmpval = NULL;
    listpackEx *lpt = o->ptr;

    /* Copy value */
    valstr = lpGetValue(vptr, &slen, &val);
    if (valstr) {
        /* Normally, item length in the listpack is limited by
         * 'hash-max-listpack-value' config. It is unlikely, but it might be
         * larger than sizeof(tmp). */
        if (slen > sizeof(tmp))
            tmpval = sdsnewlen(valstr, slen);
        else
            memcpy(tmp, valstr, slen);
    }

    /* Delete field name, value and expiry time */
    lpt->lp = lpDeleteRangeWithEntry(lpt->lp, &fptr, 3);

    listpackEntry ent[3] = {{0}};

    ent[0].sval = (unsigned char*) field;
    ent[0].slen = sdslen(field);

    if (valstr) {
        ent[1].sval = tmpval ? (unsigned char *) tmpval : tmp;
        ent[1].slen = slen;
    } else {
        ent[1].lval = val;
    }
    ent[2].lval = expire_at;

    listpackExAddInternal(o, ent);
    sdsfree(tmpval);
}

/* Update field expire time. */
SetExRes hashTypeSetExpiryListpack(HashTypeSetEx *ex, sds field,
                                   unsigned char *fptr, unsigned char *vptr,
                                   unsigned char *tptr, uint64_t expireAt)
{
    long long expireTime;
    uint64_t prevExpire = EB_EXPIRE_TIME_INVALID;

    serverAssert(lpGetIntegerValue(tptr, &expireTime));

    if (expireTime != HASH_LP_NO_TTL) {
        prevExpire = (uint64_t) expireTime;
    }

    /* Special value of EXPIRE_TIME_INVALID indicates field should be persisted.*/
    if (expireAt == EB_EXPIRE_TIME_INVALID) {
        /* Return error if already there is no ttl. */
        if (prevExpire == EB_EXPIRE_TIME_INVALID)
            return HSETEX_NO_CONDITION_MET;
        listpackExUpdateExpiry(ex->hashObj, field, fptr, vptr, HASH_LP_NO_TTL);
        return HSETEX_OK;
    }

    if (prevExpire == EB_EXPIRE_TIME_INVALID) {
        /* For fields without expiry, LT condition is considered valid */
        if (ex->expireSetCond & (HFE_XX | HFE_GT))
            return HSETEX_NO_CONDITION_MET;
    } else {
        if (((ex->expireSetCond == HFE_GT) && (prevExpire >= expireAt)) ||
            ((ex->expireSetCond == HFE_LT) && (prevExpire <= expireAt)) ||
            (ex->expireSetCond == HFE_NX) )
            return HSETEX_NO_CONDITION_MET;

        /* Track of minimum expiration time (only later update global HFE DS) */
        if (ex->minExpireFields > prevExpire)
            ex->minExpireFields = prevExpire;
    }

    /* If expired, then delete the field and propagate the deletion.
     * If replica, continue like the field is valid */
    if (unlikely(checkAlreadyExpired(expireAt))) {
        propagateHashFieldDeletion(ex->db, ex->key->ptr, field, sdslen(field));
        hashTypeDelete(ex->hashObj, field, 1);
        server.stat_expired_subkeys++;
        return HSETEX_DELETED;
    }

    if (ex->minExpireFields > expireAt)
        ex->minExpireFields = expireAt;

    listpackExUpdateExpiry(ex->hashObj, field, fptr, vptr, expireAt);
    return HSETEX_OK;
}

/* Returns 1 if expired */
int hashTypeIsExpired(const robj *o, uint64_t expireAt) {
    if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        if (expireAt == HASH_LP_NO_TTL)
            return 0;
    } else if (o->encoding == OBJ_ENCODING_HT) {
        if (expireAt == EB_EXPIRE_TIME_INVALID)
            return 0;
    } else {
        serverPanic("Unknown encoding: %d", o->encoding);
    }

    return (mstime_t) expireAt < commandTimeSnapshot();
}

/* Returns listpack pointer of the object. */
unsigned char *hashTypeListpackGetLp(robj *o) {
    if (o->encoding == OBJ_ENCODING_LISTPACK)
        return o->ptr;
    else if (o->encoding == OBJ_ENCODING_LISTPACK_EX)
        return ((listpackEx*)o->ptr)->lp;

    serverPanic("Unknown encoding: %d", o->encoding);
}

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/

/* Check the length of a number of objects to see if we need to convert a
 * listpack to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
void hashTypeTryConversion(redisDb *db, robj *o, robj **argv, int start, int end) {
    int i;
    size_t sum = 0;

    if (o->encoding != OBJ_ENCODING_LISTPACK && o->encoding != OBJ_ENCODING_LISTPACK_EX)
        return;

    /* We guess that most of the values in the input are unique, so
     * if there are enough arguments we create a pre-sized hash, which
     * might over allocate memory if there are duplicates. */
    size_t new_fields = (end - start + 1) / 2;
    if (new_fields > server.hash_max_listpack_entries) {
        hashTypeConvert(o, OBJ_ENCODING_HT, &db->hexpires);
        dictExpand(o->ptr, new_fields);
        return;
    }

    for (i = start; i <= end; i++) {
        if (!sdsEncodedObject(argv[i]))
            continue;
        size_t len = sdslen(argv[i]->ptr);
        if (len > server.hash_max_listpack_value) {
            hashTypeConvert(o, OBJ_ENCODING_HT, &db->hexpires);
            return;
        }
        sum += len;
    }
    if (!lpSafeToAdd(hashTypeListpackGetLp(o), sum))
        hashTypeConvert(o, OBJ_ENCODING_HT, &db->hexpires);
}

/* Get the value from a listpack encoded hash, identified by field. */
GetFieldRes hashTypeGetFromListpack(robj *o, sds field,
                            unsigned char **vstr,
                            unsigned int *vlen,
                            long long *vll,
                            uint64_t *expiredAt)
{
    *expiredAt = EB_EXPIRE_TIME_INVALID;
    unsigned char *zl, *fptr = NULL, *vptr = NULL;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char*)field, sdslen(field), 1);
            if (fptr != NULL) {
                /* Grab pointer to the value (fptr points to the field) */
                vptr = lpNext(zl, fptr);
                serverAssert(vptr != NULL);
            }
        }
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        long long expire;
        unsigned char *h;
        listpackEx *lpt = o->ptr;

        fptr = lpFirst(lpt->lp);
        if (fptr != NULL) {
            fptr = lpFind(lpt->lp, fptr, (unsigned char*)field, sdslen(field), 2);
            if (fptr != NULL) {
                vptr = lpNext(lpt->lp, fptr);
                serverAssert(vptr != NULL);

                h = lpNext(lpt->lp, vptr);
                serverAssert(h && lpGetIntegerValue(h, &expire));
                if (expire != HASH_LP_NO_TTL)
                    *expiredAt = expire;
            }
        }
    } else {
        serverPanic("Unknown hash encoding: %d", o->encoding);
    }

    if (vptr != NULL) {
        *vstr = lpGetValue(vptr, vlen, vll);
        return GETF_OK;
    }

    return GETF_NOT_FOUND;
}

/* Get the value from a hash table encoded hash, identified by field.
 * Returns NULL when the field cannot be found, otherwise the SDS value
 * is returned. */
GetFieldRes hashTypeGetFromHashTable(robj *o, sds field, sds *value, uint64_t *expiredAt) {
    dictEntry *de;

    *expiredAt = EB_EXPIRE_TIME_INVALID;

    serverAssert(o->encoding == OBJ_ENCODING_HT);

    de = dictFind(o->ptr, field);

    if (de == NULL)
        return GETF_NOT_FOUND;

    *expiredAt = hfieldGetExpireTime(dictGetKey(de));
    *value = (sds) dictGetVal(de);
    return GETF_OK;
}

/* Higher level function of hashTypeGet*() that returns the hash value
 * associated with the specified field.
 * Arguments:
 * hfeFlags      - Lookup for HFE_LAZY_* flags
 *
 * Returned:
 * GetFieldRes  - Result of get operation
 * vstr, vlen   - if string, ref in either *vstr and *vlen if it's
 *                returned in string form,
 * vll          - or stored in *vll if it's returned as a number.
 *                If *vll is populated *vstr is set to NULL, so the caller can
 *                always check the function return by checking the return value
 *                for GETF_OK and checking if vll (or vstr) is NULL.
 * expiredAt    - if the field has an expiration time, it will be set to the expiration 
 *                time of the field. Otherwise, will be set to EB_EXPIRE_TIME_INVALID.
 */
GetFieldRes hashTypeGetValue(redisDb *db, kvobj *o, sds field, unsigned char **vstr,
                                   unsigned int *vlen, long long *vll, 
                                   int hfeFlags, uint64_t *expiredAt)
{
    sds key;
    GetFieldRes res;
    uint64_t dummy;
    if (expiredAt == NULL) expiredAt = &dummy;
    if (o->encoding == OBJ_ENCODING_LISTPACK ||
        o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        *vstr = NULL;
        res = hashTypeGetFromListpack(o, field, vstr, vlen, vll, expiredAt);

        if (res == GETF_NOT_FOUND)
            return GETF_NOT_FOUND;

    } else if (o->encoding == OBJ_ENCODING_HT) {
        sds value = NULL;
        res = hashTypeGetFromHashTable(o, field, &value, expiredAt);

        if (res == GETF_NOT_FOUND)
            return GETF_NOT_FOUND;

        *vstr = (unsigned char*) value;
        *vlen = sdslen(value);
    } else {
        serverPanic("Unknown hash encoding");
    }

    if ((*expiredAt >= (uint64_t) commandTimeSnapshot()) || 
        (hfeFlags & HFE_LAZY_ACCESS_EXPIRED))
        return GETF_OK;

    if (server.masterhost) {
        /* If CLIENT_MASTER, assume valid as long as it didn't get delete */
        if (server.current_client && (server.current_client->flags & CLIENT_MASTER))
            return GETF_OK;

        /* If user client, then act as if expired, but don't delete! */
        return GETF_EXPIRED;
    }

    if ((server.loading) ||
        (server.allow_access_expired) ||
        (hfeFlags & HFE_LAZY_AVOID_FIELD_DEL) ||
        (isPausedActionsWithUpdate(PAUSE_ACTION_EXPIRE)))
        return GETF_EXPIRED;

    key = kvobjGetKey(o);

    /* delete the field and propagate the deletion */
    serverAssert(hashTypeDelete(o, field, 1) == 1);
    propagateHashFieldDeletion(db, key, field, sdslen(field));
    server.stat_expired_subkeys++;

    if (!(hfeFlags & HFE_LAZY_NO_UPDATE_KEYSIZES)) {
        uint64_t l = hashTypeLength(o, 0);
        updateKeysizesHist(db, getKeySlot(key), OBJ_HASH, l+1, l);
    }

    /* If the field is the last one in the hash, then the hash will be deleted */
    res = GETF_EXPIRED;
    robj *keyObj = createStringObject(key, sdslen(key));
    if (!(hfeFlags & HFE_LAZY_NO_NOTIFICATION))
        notifyKeyspaceEvent(NOTIFY_HASH, "hexpired", keyObj, db->id);
    if ((hashTypeLength(o, 0) == 0) && (!(hfeFlags & HFE_LAZY_AVOID_HASH_DEL))) {
        if (!(hfeFlags & HFE_LAZY_NO_NOTIFICATION))
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", keyObj, db->id);
        dbDelete(db,keyObj);
        res = GETF_EXPIRED_HASH;
    }
    if (!(hfeFlags & HFE_LAZY_NO_SIGNAL))
        signalModifiedKey(NULL, db, keyObj);
    decrRefCount(keyObj);
    return res;
}

/* Like hashTypeGetValue() but returns a Redis object, which is useful for
 * interaction with the hash type outside t_hash.c.
 * The function returns NULL if the field is not found in the hash. Otherwise
 * a newly allocated string object with the value is returned.
 *
 * hfeFlags      - Lookup HFE_LAZY_* flags
 * isHashDeleted - If attempted to access expired field and it's the last field
 *                 in the hash, then the hash will as well be deleted. In this case,
 *                 isHashDeleted will be set to 1.
 * val           - If the field is found, then val will be set to the value object.
 * expireTime    - If the field exists (`GETF_OK`) then expireTime will be set to  
 *                 the expiration time of the field. Otherwise, it will be set to 0.
 *                 
 * Returns 1 if the field exists, and 0 when it doesn't.
 */
int hashTypeGetValueObject(redisDb *db, kvobj *o, sds field, int hfeFlags,
                           robj **val, uint64_t *expireTime, int *isHashDeleted) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    if (isHashDeleted) *isHashDeleted = 0;
    if (val) *val = NULL;
    GetFieldRes res = hashTypeGetValue(db,o,field,&vstr,&vlen,&vll, 
                                                   hfeFlags, expireTime);

    if (res == GETF_OK) {
        /* expireTime set to 0 if the field has no expiration time */ 
        if (expireTime && (*expireTime == EB_EXPIRE_TIME_INVALID))
            *expireTime = 0;
        
        /* If expected to return the value, then create a new object */
        if (val) {
            if (vstr) *val = createStringObject((char *) vstr, vlen);
            else *val = createStringObjectFromLongLong(vll);
        }
        return 1;
    }

    if ((res == GETF_EXPIRED_HASH) && (isHashDeleted))
        *isHashDeleted = 1;

    /* GETF_EXPIRED_HASH, GETF_EXPIRED, GETF_NOT_FOUND */
    return 0;
}

/* Test if the specified field exists in the given hash. If the field is
 * expired (HFE), then it will be lazy deleted unless HFE_LAZY_AVOID_FIELD_DEL 
 * hfeFlags is set.
 *
 * hfeFlags      - Lookup HFE_LAZY_* flags
 * isHashDeleted - If attempted to access expired field and it is the last field
 *                 in the hash, then the hash will as well be deleted. In this case,
 *                 isHashDeleted will be set to 1.
 *
 * Returns 1 if the field exists, and 0 when it doesn't.
 */
int hashTypeExists(redisDb *db, kvobj *o, sds field, int hfeFlags, int *isHashDeleted) {
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    GetFieldRes res = hashTypeGetValue(db, o, field, &vstr, &vlen, &vll, 
                                             hfeFlags, NULL);
    if (isHashDeleted)
        *isHashDeleted = (res == GETF_EXPIRED_HASH) ? 1 : 0;
    return (res == GETF_OK) ? 1 : 0;
}

/* Add a new field, overwrite the old with the new value if it already exists.
 * Return 0 on insert and 1 on update.
 *
 * By default, the key and value SDS strings are copied if needed, so the
 * caller retains ownership of the strings passed. However this behavior
 * can be effected by passing appropriate flags (possibly bitwise OR-ed):
 *
 * HASH_SET_TAKE_FIELD  -- The SDS field ownership passes to the function.
 * HASH_SET_TAKE_VALUE  -- The SDS value ownership passes to the function.
 * HASH_SET_KEEP_TTL --  keep original TTL if field already exists
 *
 * When the flags are used the caller does not need to release the passed
 * SDS string(s). It's up to the function to use the string to create a new
 * entry or to free the SDS string before returning to the caller.
 *
 * HASH_SET_COPY corresponds to no flags passed, and means the default
 * semantics of copying the values if needed.
 *
 */
#define HASH_SET_TAKE_FIELD  (1<<0)
#define HASH_SET_TAKE_VALUE  (1<<1)
#define HASH_SET_KEEP_TTL (1<<2)
int hashTypeSet(redisDb *db, robj *o, sds field, sds value, int flags) {
    int update = 0;

    /* Check if the field is too long for listpack, and convert before adding the item.
     * This is needed for HINCRBY* case since in other commands this is handled early by
     * hashTypeTryConversion, so this check will be a NOP. */
    if (o->encoding == OBJ_ENCODING_LISTPACK  ||
        o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        if (sdslen(field) > server.hash_max_listpack_value || sdslen(value) > server.hash_max_listpack_value)
            hashTypeConvert(o, OBJ_ENCODING_HT, &db->hexpires);
    }

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl, *fptr, *vptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char*)field, sdslen(field), 1);
            if (fptr != NULL) {
                /* Grab pointer to the value (fptr points to the field) */
                vptr = lpNext(zl, fptr);
                serverAssert(vptr != NULL);

                /* Replace value */
                zl = lpReplace(zl, &vptr, (unsigned char*)value, sdslen(value));
                update = 1;
            }
        }

        if (!update) {
            listpackEntry entries[2] = {
                {.sval = (unsigned char*) field, .slen = sdslen(field)},
                {.sval = (unsigned char*) value, .slen = sdslen(value)},
            };

            /* Push new field/value pair onto the tail of the listpack */
            zl = lpBatchAppend(zl, entries, 2);
        }
        o->ptr = zl;

        /* Check if the listpack needs to be converted to a hash table */
        if (hashTypeLength(o, 0) > server.hash_max_listpack_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT, &db->hexpires);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        unsigned char *fptr = NULL, *vptr = NULL, *tptr = NULL;
        listpackEx *lpt = o->ptr;
        long long expireTime = HASH_LP_NO_TTL;

        fptr = lpFirst(lpt->lp);
        if (fptr != NULL) {
            fptr = lpFind(lpt->lp, fptr, (unsigned char*)field, sdslen(field), 2);
            if (fptr != NULL) {
                /* Grab pointer to the value (fptr points to the field) */
                vptr = lpNext(lpt->lp, fptr);
                serverAssert(vptr != NULL);

                /* Replace value */
                lpt->lp = lpReplace(lpt->lp, &vptr, (unsigned char *) value, sdslen(value));
                update = 1;

                fptr = lpPrev(lpt->lp, vptr);
                serverAssert(fptr != NULL);

                tptr = lpNext(lpt->lp, vptr);
                serverAssert(tptr && lpGetIntegerValue(tptr, &expireTime));

                if (flags & HASH_SET_KEEP_TTL) {
                    /* keep old field along with TTL */
                } else if (expireTime != HASH_LP_NO_TTL) {
                    /* re-insert field and override TTL */
                    listpackExUpdateExpiry(o, field, fptr, vptr, HASH_LP_NO_TTL);
                }
            }
        }

        if (!update)
            listpackExAddNew(o, field, sdslen(field), value, sdslen(value),
                             HASH_LP_NO_TTL);

        /* Check if the listpack needs to be converted to a hash table */
        if (hashTypeLength(o, 0) > server.hash_max_listpack_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT, &db->hexpires);

    } else if (o->encoding == OBJ_ENCODING_HT) {
        dict *ht = o->ptr;
        dictEntry *de;
        /* check if field already exists */
        dictEntryLink bucket, link = dictFindLink(ht, field, &bucket);
        /* check if field already exists */
        if (link == NULL) {
            hfield newField = hfieldNew(field, sdslen(field), 0);
            dictSetKeyAtLink(ht, newField, &bucket, 1);
            de = *bucket;
        } else {
            /* If attached TTL to the old field, then remove it from hash's
             * private ebuckets when HASH_SET_KEEP_TTL is not set. */
            if (!(flags & HASH_SET_KEEP_TTL)) {
                hfield oldField = dictGetKey(*link);
                hfieldPersist(o, oldField);
            }
            /* Free the old value */
            sdsfree(dictGetVal(*link));
            update = 1;
            de = *link;
        }

        if (flags & HASH_SET_TAKE_VALUE) {
            dictSetVal(ht, de, value);
            flags &= ~HASH_SET_TAKE_VALUE;
        } else {
            dictSetVal(ht, de, sdsdup(value));
        }
    } else {
        serverPanic("Unknown hash encoding");
    }

    /* Free SDS strings we did not referenced elsewhere if the flags
     * want this function to be responsible. */
    if (flags & HASH_SET_TAKE_FIELD && field) sdsfree(field);
    if (flags & HASH_SET_TAKE_VALUE && value) sdsfree(value);
    return update;
}

SetExRes hashTypeSetExpiryHT(HashTypeSetEx *exInfo, sds field, uint64_t expireAt) {
    dict *ht = exInfo->hashObj->ptr;
    dictEntry *existingEntry = NULL;
    hfield hfNew = NULL;

    if ((existingEntry = dictFind(ht, field)) == NULL)
        return HSETEX_NO_FIELD;

    hfield hfOld = dictGetKey(existingEntry);
    /* Special value of EXPIRE_TIME_INVALID indicates field should be persisted.*/
    if (expireAt == EB_EXPIRE_TIME_INVALID) {
        /* Return error if already there is no ttl. */
        if (hfieldGetExpireTime(hfOld) == EB_EXPIRE_TIME_INVALID)
            return HSETEX_NO_CONDITION_MET;

        hfieldPersist(exInfo->hashObj, hfOld);
        return HSETEX_OK;
    }

    /* If field doesn't have expiry metadata attached */
    if (!hfieldIsExpireAttached(hfOld)) {
        /* For fields without expiry, LT condition is considered valid */
        if (exInfo->expireSetCond & (HFE_XX | HFE_GT))
            return HSETEX_NO_CONDITION_MET;

        /* Delete old field. Below goanna dictSetKey(..,hfNew) */
        hfieldFree(hfOld);
        /* New field with expiration metadata */
        hfNew = hfieldNew(field, sdslen(field), 1);
    } else { /* field has ExpireMeta struct attached */
        uint64_t prevExpire = hfieldGetExpireTime(hfOld);

        /* If field has valid expiration time, then check GT|LT|NX */
        if (prevExpire != EB_EXPIRE_TIME_INVALID) {
            if (((exInfo->expireSetCond == HFE_GT) && (prevExpire >= expireAt)) ||
                ((exInfo->expireSetCond == HFE_LT) && (prevExpire <= expireAt)) ||
                (exInfo->expireSetCond == HFE_NX) )
                return HSETEX_NO_CONDITION_MET;

            /* remove old expiry time from hash's private ebuckets */
            dictExpireMetadata *dm = (dictExpireMetadata *) dictMetadata(ht);
            ebRemove(&dm->hfe, &hashFieldExpireBucketsType, hfOld);

            /* Track of minimum expiration time (only later update global HFE DS) */
            if (exInfo->minExpireFields > prevExpire)
                exInfo->minExpireFields = prevExpire;

        } else {
            /* field has invalid expiry. No need to ebRemove() */

            /* Check XX|LT|GT */
            if (exInfo->expireSetCond & (HFE_XX | HFE_GT))
                return HSETEX_NO_CONDITION_MET;
        }

        /* Reuse hfOld as hfNew and rewrite its expiry with ebAdd() */
        hfNew = hfOld;
    }

    dictSetKey(ht, existingEntry, hfNew);


    /* If expired, then delete the field and propagate the deletion.
     * If replica, continue like the field is valid */
    if (unlikely(checkAlreadyExpired(expireAt))) {
        /* replicas should not initiate deletion of fields */
        propagateHashFieldDeletion(exInfo->db, exInfo->key->ptr, field, sdslen(field));
        hashTypeDelete(exInfo->hashObj, field, 1);
        server.stat_expired_subkeys++;
        return HSETEX_DELETED;
    }

    if (exInfo->minExpireFields > expireAt)
        exInfo->minExpireFields = expireAt;

    dictExpireMetadata *dm = (dictExpireMetadata *) dictMetadata(ht);
    ebAdd(&dm->hfe, &hashFieldExpireBucketsType, hfNew, expireAt);
    return HSETEX_OK;
}

/*
 * Set field expiration
 *
 * Take care to call first hashTypeSetExInit() and then call this function.
 * Finally, call hashTypeSetExDone() to notify and update global HFE DS.
 *
 * Special value of EB_EXPIRE_TIME_INVALID for 'expireAt' argument will persist
 * the field.
 */
SetExRes hashTypeSetEx(robj *o, sds field, uint64_t expireAt, HashTypeSetEx *exInfo) {
    if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        unsigned char *fptr = NULL, *vptr = NULL, *tptr = NULL;
        listpackEx *lpt = o->ptr;

        fptr = lpFirst(lpt->lp);
        if (fptr)
            fptr = lpFind(lpt->lp, fptr, (unsigned char*)field, sdslen(field), 2);

        if (!fptr)
            return HSETEX_NO_FIELD;

        /* Grab pointer to the value (fptr points to the field) */
        vptr = lpNext(lpt->lp, fptr);
        serverAssert(vptr != NULL);

        tptr = lpNext(lpt->lp, vptr);
        serverAssert(tptr);

        /* update TTL */
        return hashTypeSetExpiryListpack(exInfo, field, fptr, vptr, tptr, expireAt);
    } else if (o->encoding == OBJ_ENCODING_HT) {
        /* If needed to set the field along with expiry */
        return hashTypeSetExpiryHT(exInfo, field, expireAt);
    } else {
        serverPanic("Unknown hash encoding");
    }

    return HSETEX_OK; /* never reach here */
}

void initDictExpireMetadata(robj *o) {
    dict *ht = o->ptr;

    dictExpireMetadata *m = (dictExpireMetadata *) dictMetadata(ht);
    m->hfe = ebCreate();     /* Allocate HFE DS */
    m->expireMeta.trash = 1; /* mark as trash (as long it wasn't ebAdd()) */
}

/* Init HashTypeSetEx struct before calling hashTypeSetEx() */
int hashTypeSetExInit(robj *key, kvobj *o, client *c, redisDb *db,
                      ExpireSetCond expireSetCond, HashTypeSetEx *ex)
{
    dict *ht = o->ptr;
    ex->expireSetCond = expireSetCond;
    ex->minExpire = EB_EXPIRE_TIME_INVALID;
    ex->c = c;
    ex->db = db;
    ex->key = key;
    ex->hashObj = o;
    ex->minExpireFields = EB_EXPIRE_TIME_INVALID;

    /* Take care that HASH support expiration */
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        hashTypeConvert(o, OBJ_ENCODING_LISTPACK_EX, &c->db->hexpires);
    } else if (o->encoding == OBJ_ENCODING_HT) {
        /* Take care dict has HFE metadata */
        if (!isDictWithMetaHFE(ht)) {
            /* Realloc (only header of dict) with metadata for hash-field expiration */
            dictTypeAddMeta(&ht, &mstrHashDictTypeWithHFE);
            dictExpireMetadata *m = (dictExpireMetadata *) dictMetadata(ht);
            o->ptr = ht;

            /* Find the key in the keyspace. Need to keep reference to the key for
             * notifications or even removal of the hash */

            /* Fillup dict HFE metadata */
            m->hfe = ebCreate();     /* Allocate HFE DS */
            m->expireMeta.trash = 1; /* mark as trash (as long it wasn't ebAdd()) */
        }
    }

    /* Read minExpire from attached ExpireMeta to the hash */
    ex->minExpire = hashTypeGetMinExpire(o, 0);
    return C_OK;
}

/*
 * After calling hashTypeSetEx() for setting fields or their expiry, call this
 * function to update global HFE DS.
 */
void hashTypeSetExDone(HashTypeSetEx *ex) {

    if (hashTypeLength(ex->hashObj, 0) == 0)
        return;

    /* If minimum HFE of the hash is smaller than expiration time of the
     * specified fields in the command as well as it is smaller or equal
     * than expiration time provided in the command, then the minimum
     * HFE of the hash won't change following this command. */
    if ((ex->minExpire < ex->minExpireFields))
        return;

    /* Retrieve new expired time. It might have changed. */
    uint64_t newMinExpire = hashTypeGetMinExpire(ex->hashObj, 1 /*accurate*/);

    /* Calculate the diff between old minExpire and newMinExpire. If it is
     * only few seconds, then don't have to update global HFE DS. At the worst
     * case fields of hash will be active-expired up to few seconds later.
     *
     * In any case, active-expire operation will know to update global
     * HFE DS more efficiently than here for a single item.
     */
    uint64_t diff = (ex->minExpire > newMinExpire) ?
                    (ex->minExpire - newMinExpire) : (newMinExpire - ex->minExpire);
    if (diff < HASH_NEW_EXPIRE_DIFF_THRESHOLD) return;

    if (ex->minExpire != EB_EXPIRE_TIME_INVALID)
        ebRemove(&ex->db->hexpires, &hashExpireBucketsType, ex->hashObj);
    if (newMinExpire != EB_EXPIRE_TIME_INVALID)
        ebAdd(&ex->db->hexpires, &hashExpireBucketsType, ex->hashObj, newMinExpire);
}

/* Delete an element from a hash.
 *
 * Return 1 on deleted and 0 on not found.
 * isSdsField - 1 if the field is sds, 0 if it is hfield */
int hashTypeDelete(robj *o, void *field, int isSdsField) {
    int deleted = 0;
    int fieldLen = (isSdsField) ? sdslen((sds)field) : hfieldlen((hfield)field);

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl, *fptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char*)field, fieldLen, 1);
            if (fptr != NULL) {
                /* Delete both of the key and the value. */
                zl = lpDeleteRangeWithEntry(zl,&fptr,2);
                o->ptr = zl;
                deleted = 1;
            }
        }
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        unsigned char *fptr;
        listpackEx *lpt = o->ptr;

        fptr = lpFirst(lpt->lp);
        if (fptr != NULL) {
            fptr = lpFind(lpt->lp, fptr, (unsigned char*)field, fieldLen, 2);
            if (fptr != NULL) {
                /* Delete field, value and ttl */
                lpt->lp = lpDeleteRangeWithEntry(lpt->lp, &fptr, 3);
                deleted = 1;
            }
        }
    } else if (o->encoding == OBJ_ENCODING_HT) {
        /* dictDelete() will call dictHfieldDestructor() */
        dictUseStoredKeyApi((dict*)o->ptr, isSdsField ? 0 : 1);
        if (dictDelete((dict*)o->ptr, field) == C_OK) {
            deleted = 1;
        }
        dictUseStoredKeyApi((dict*)o->ptr, 0);

    } else {
        serverPanic("Unknown hash encoding");
    }
    return deleted;
}

/* Return the number of elements in a hash.
 *
 * Note, subtractExpiredFields=1 might be pricy in case there are many HFEs
 */
unsigned long hashTypeLength(const robj *o, int subtractExpiredFields) {
    unsigned long length = ULONG_MAX;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        length = lpLength(o->ptr) / 2;
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        listpackEx *lpt = o->ptr;
        length = lpLength(lpt->lp) / 3;

        if (subtractExpiredFields && lpt->meta.trash == 0)
            length -= listpackExExpireDryRun(o);
    } else if (o->encoding == OBJ_ENCODING_HT) {
        uint64_t expiredItems = 0;
        dict *d = (dict*)o->ptr;
        if (subtractExpiredFields && isDictWithMetaHFE(d)) {
            dictExpireMetadata *meta = (dictExpireMetadata *) dictMetadata(d);
            /* If dict registered in global HFE DS */
            if (meta->expireMeta.trash == 0)
                expiredItems = ebExpireDryRun(meta->hfe,
                                              &hashFieldExpireBucketsType,
                                              commandTimeSnapshot());
        }
        length = dictSize(d) - expiredItems;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return length;
}

hashTypeIterator *hashTypeInitIterator(robj *subject) {
    hashTypeIterator *hi = zmalloc(sizeof(hashTypeIterator));
    hi->subject = subject;
    hi->encoding = subject->encoding;

    if (hi->encoding == OBJ_ENCODING_LISTPACK ||
        hi->encoding == OBJ_ENCODING_LISTPACK_EX)
    {
        hi->fptr = NULL;
        hi->vptr = NULL;
        hi->tptr = NULL;
        hi->expire_time = EB_EXPIRE_TIME_INVALID;
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        hi->di = dictGetIterator(subject->ptr);
    } else {
        serverPanic("Unknown hash encoding");
    }
    return hi;
}

void hashTypeReleaseIterator(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_HT)
        dictReleaseIterator(hi->di);
    zfree(hi);
}

/* Move to the next entry in the hash. Return C_OK when the next entry
 * could be found and C_ERR when the iterator reaches the end. */
int hashTypeNext(hashTypeIterator *hi, int skipExpiredFields) {
    hi->expire_time = EB_EXPIRE_TIME_INVALID;
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl;
        unsigned char *fptr, *vptr;

        zl = hi->subject->ptr;
        fptr = hi->fptr;
        vptr = hi->vptr;

        if (fptr == NULL) {
            /* Initialize cursor */
            serverAssert(vptr == NULL);
            fptr = lpFirst(zl);
        } else {
            /* Advance cursor */
            serverAssert(vptr != NULL);
            fptr = lpNext(zl, vptr);
        }
        if (fptr == NULL) return C_ERR;

        /* Grab pointer to the value (fptr points to the field) */
        vptr = lpNext(zl, fptr);
        serverAssert(vptr != NULL);

        /* fptr, vptr now point to the first or next pair */
        hi->fptr = fptr;
        hi->vptr = vptr;
    } else if (hi->encoding == OBJ_ENCODING_LISTPACK_EX) {
        long long expire_time;
        unsigned char *zl = hashTypeListpackGetLp(hi->subject);
        unsigned char *fptr, *vptr, *tptr;

        fptr = hi->fptr;
        vptr = hi->vptr;
        tptr = hi->tptr;

        if (fptr == NULL) {
            /* Initialize cursor */
            serverAssert(vptr == NULL);
            fptr = lpFirst(zl);
        } else {
            /* Advance cursor */
            serverAssert(tptr != NULL);
            fptr = lpNext(zl, tptr);
        }
        if (fptr == NULL) return C_ERR;

        while (fptr != NULL) {
            /* Grab pointer to the value (fptr points to the field) */
            vptr = lpNext(zl, fptr);
            serverAssert(vptr != NULL);

            tptr = lpNext(zl, vptr);
            serverAssert(tptr && lpGetIntegerValue(tptr, &expire_time));

            if (!skipExpiredFields || !hashTypeIsExpired(hi->subject, expire_time))
                break;

            fptr = lpNext(zl, tptr);
        }
        if (fptr == NULL) return C_ERR;

        /* fptr, vptr now point to the first or next pair */
        hi->fptr = fptr;
        hi->vptr = vptr;
        hi->tptr = tptr;
        hi->expire_time = (expire_time != HASH_LP_NO_TTL) ? (uint64_t) expire_time : EB_EXPIRE_TIME_INVALID;
    } else if (hi->encoding == OBJ_ENCODING_HT) {

        while ((hi->de = dictNext(hi->di)) != NULL) {
            hi->expire_time = hfieldGetExpireTime(dictGetKey(hi->de));
            /* this condition still valid if expire_time equals EB_EXPIRE_TIME_INVALID */
            if (skipExpiredFields && ((mstime_t)hi->expire_time < commandTimeSnapshot()))
                continue;
            return C_OK;
        }
        return C_ERR;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return C_OK;
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a listpack. Prototype is similar to `hashTypeGetFromListpack`. */
void hashTypeCurrentFromListpack(hashTypeIterator *hi, int what,
                                 unsigned char **vstr,
                                 unsigned int *vlen,
                                 long long *vll,
                                 uint64_t *expireTime)
{
    serverAssert(hi->encoding == OBJ_ENCODING_LISTPACK ||
                 hi->encoding == OBJ_ENCODING_LISTPACK_EX);

    if (what & OBJ_HASH_KEY) {
        *vstr = lpGetValue(hi->fptr, vlen, vll);
    } else {
        *vstr = lpGetValue(hi->vptr, vlen, vll);
    }

    if (expireTime)
        *expireTime = hi->expire_time;
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a hash table. Prototype is similar to
 * `hashTypeGetFromHashTable`.
 *
 * expireTime - If parameter is not null, then the function will return the expire
 *              time of the field. If expiry not set, return EB_EXPIRE_TIME_INVALID
 */
void hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what, char **str, size_t *len, uint64_t *expireTime) {
    serverAssert(hi->encoding == OBJ_ENCODING_HT);
    hfield key = NULL;

    if (what & OBJ_HASH_KEY) {
        key = dictGetKey(hi->de);
        *str = key;
        *len = hfieldlen(key);
    } else {
        sds val = dictGetVal(hi->de);
        *str = val;
        *len = sdslen(val);
    }

    if (expireTime)
        *expireTime = hi->expire_time;
}

/* Higher level function of hashTypeCurrent*() that returns the hash value
 * at current iterator position.
 *
 * The returned element is returned by reference in either *vstr and *vlen if
 * it's returned in string form, or stored in *vll if it's returned as
 * a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * type checking if vstr == NULL. */
void hashTypeCurrentObject(hashTypeIterator *hi,
                           int what,
                           unsigned char **vstr,
                           unsigned int *vlen,
                           long long *vll,
                           uint64_t *expireTime)
{
    if (hi->encoding == OBJ_ENCODING_LISTPACK ||
        hi->encoding == OBJ_ENCODING_LISTPACK_EX)
    {
        *vstr = NULL;
        hashTypeCurrentFromListpack(hi, what, vstr, vlen, vll, expireTime);
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        char *ele;
        size_t eleLen;
        hashTypeCurrentFromHashTable(hi, what, &ele, &eleLen, expireTime);
        *vstr = (unsigned char*) ele;
        *vlen = eleLen;
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* Return the key or value at the current iterator position as a new
 * SDS string. */
sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    hashTypeCurrentObject(hi,what,&vstr,&vlen,&vll, NULL);
    if (vstr) return sdsnewlen(vstr,vlen);
    return sdsfromlonglong(vll);
}

/* Return the key at the current iterator position as a new hfield string. */
hfield hashTypeCurrentObjectNewHfield(hashTypeIterator *hi) {
    char buf[LONG_STR_SIZE];
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;
    uint64_t expireTime;
    hfield hf;

    hashTypeCurrentObject(hi,OBJ_HASH_KEY,&vstr,&vlen,&vll, &expireTime);

    if (!vstr) {
        vlen = ll2string(buf, sizeof(buf), vll);
        vstr = (unsigned char *) buf;
    }

    hf = hfieldNew(vstr,vlen, expireTime != EB_EXPIRE_TIME_INVALID);
    return hf;
}

static kvobj *hashTypeLookupWriteOrCreate(client *c, robj *key) {
    dictEntryLink link;
    kvobj *kv = lookupKeyWriteWithLink(c->db, key, &link);
    if (checkType(c, kv, OBJ_HASH)) return NULL;

    if (kv == NULL) {
        robj *o = createHashObject();
        kv = dbAddByLink(c->db, key, &o, &link);
    }
    return kv;
}


void hashTypeConvertListpack(robj *o, int enc) {
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK);

    if (enc == OBJ_ENCODING_LISTPACK) {
        /* Nothing to do... */

    } else if (enc == OBJ_ENCODING_LISTPACK_EX) {
        unsigned char *p;

        /* Append HASH_LP_NO_TTL to each field name - value pair. */
        p = lpFirst(o->ptr);
        while (p != NULL) {
            p = lpNext(o->ptr, p);
            serverAssert(p);

            o->ptr = lpInsertInteger(o->ptr, HASH_LP_NO_TTL, p, LP_AFTER, &p);
            p = lpNext(o->ptr, p);
        }

        listpackEx *lpt = listpackExCreate();
        lpt->lp = o->ptr;
        o->encoding = OBJ_ENCODING_LISTPACK_EX;
        o->ptr = lpt;
    } else if (enc == OBJ_ENCODING_HT) {
        hashTypeIterator *hi;
        dict *dict;
        int ret;

        hi = hashTypeInitIterator(o);
        dict = dictCreate(&mstrHashDictType);

        /* Presize the dict to avoid rehashing */
        dictExpand(dict,hashTypeLength(o, 0));

        while (hashTypeNext(hi, 0) != C_ERR) {

            hfield key = hashTypeCurrentObjectNewHfield(hi);
            sds value = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_VALUE);
            dictUseStoredKeyApi(dict, 1);
            ret = dictAdd(dict, key, value);
            dictUseStoredKeyApi(dict, 0);
            if (ret != DICT_OK) {
                hfieldFree(key); sdsfree(value); /* Needed for gcc ASAN */
                hashTypeReleaseIterator(hi);  /* Needed for gcc ASAN */
                serverLogHexDump(LL_WARNING,"listpack with dup elements dump",
                    o->ptr,lpBytes(o->ptr));
                serverPanic("Listpack corruption detected");
            }
        }
        hashTypeReleaseIterator(hi);
        zfree(o->ptr);
        o->encoding = OBJ_ENCODING_HT;
        o->ptr = dict;
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void hashTypeConvertListpackEx(robj *o, int enc, ebuckets *hexpires) {
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK_EX);

    if (enc == OBJ_ENCODING_LISTPACK_EX) {
        return;
    } else if (enc == OBJ_ENCODING_HT) {
        int ret;
        hashTypeIterator *hi;
        dict *dict;
        dictExpireMetadata *dictExpireMeta;
        listpackEx *lpt = o->ptr;
        uint64_t minExpire = hashTypeGetMinExpire(o, 0);

        if (hexpires && lpt->meta.trash != 1)
            ebRemove(hexpires, &hashExpireBucketsType, o);

        dict = dictCreate(&mstrHashDictTypeWithHFE);
        dictExpand(dict,hashTypeLength(o, 0));
        dictExpireMeta = (dictExpireMetadata *) dictMetadata(dict);

        /* Fillup dict HFE metadata */
        dictExpireMeta->hfe = ebCreate();     /* Allocate HFE DS */
        dictExpireMeta->expireMeta.trash = 1; /* mark as trash (as long it wasn't ebAdd()) */

        hi = hashTypeInitIterator(o);

        while (hashTypeNext(hi, 0) != C_ERR) {
            hfield key = hashTypeCurrentObjectNewHfield(hi);
            sds value = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_VALUE);
            dictUseStoredKeyApi(dict, 1);
            ret = dictAdd(dict, key, value);
            dictUseStoredKeyApi(dict, 0);
            if (ret != DICT_OK) {
                hfieldFree(key); sdsfree(value); /* Needed for gcc ASAN */
                hashTypeReleaseIterator(hi);  /* Needed for gcc ASAN */
                serverLogHexDump(LL_WARNING,"listpack with dup elements dump",
                                 lpt->lp,lpBytes(lpt->lp));
                serverPanic("Listpack corruption detected");
            }

            if (hi->expire_time != EB_EXPIRE_TIME_INVALID)
                ebAdd(&dictExpireMeta->hfe, &hashFieldExpireBucketsType, key, hi->expire_time);
        }
        hashTypeReleaseIterator(hi);
        listpackExFree(lpt);

        o->encoding = OBJ_ENCODING_HT;
        o->ptr = dict;

        if (hexpires && minExpire != EB_EXPIRE_TIME_INVALID)
            ebAdd(hexpires, &hashExpireBucketsType, o, minExpire);
    } else {
        serverPanic("Unknown hash encoding: %d", enc);
    }
}

/* NOTE: hexpires can be NULL (Won't register in global HFE DS) */
void hashTypeConvert(robj *o, int enc, ebuckets *hexpires) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        hashTypeConvertListpack(o, enc);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        hashTypeConvertListpackEx(o, enc, hexpires);
    } else if (o->encoding == OBJ_ENCODING_HT) {
        serverPanic("Not implemented");
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* This is a helper function for the COPY command.
 * Duplicate a hash object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *hashTypeDup(kvobj *o, uint64_t *minHashExpire) {
    robj *hobj;
    hashTypeIterator *hi;

    serverAssert(o->type == OBJ_HASH);

    if(o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = o->ptr;
        size_t sz = lpBytes(zl);
        unsigned char *new_zl = zmalloc(sz);
        memcpy(new_zl, zl, sz);
        hobj = createObject(OBJ_HASH, new_zl);
        hobj->encoding = OBJ_ENCODING_LISTPACK;
    } else if(o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        listpackEx *lpt = o->ptr;

        if (lpt->meta.trash == 0)
            *minHashExpire = ebGetMetaExpTime(&lpt->meta);

        listpackEx *dup = listpackExCreate();

        size_t sz = lpBytes(lpt->lp);
        dup->lp = lpNew(sz);
        memcpy(dup->lp, lpt->lp, sz);

        hobj = createObject(OBJ_HASH, dup);
        hobj->encoding = OBJ_ENCODING_LISTPACK_EX;
    } else if(o->encoding == OBJ_ENCODING_HT) {
        dictExpireMetadata *dictExpireMetaSrc, *dictExpireMetaDst = NULL;
        dict *d;

        /* If dict doesn't have HFE metadata, then create a new dict without it */
        if (!isDictWithMetaHFE(o->ptr)) {
            d = dictCreate(&mstrHashDictType);
        } else {
            /* Create a new dict with HFE metadata */
            d = dictCreate(&mstrHashDictTypeWithHFE);
            dictExpireMetaSrc = (dictExpireMetadata *) dictMetadata((dict *) o->ptr);
            dictExpireMetaDst = (dictExpireMetadata *) dictMetadata(d);
            dictExpireMetaDst->hfe = ebCreate();     /* Allocate HFE DS */
            dictExpireMetaDst->expireMeta.trash = 1; /* mark as trash (as long it wasn't ebAdd()) */

            /* Extract the minimum expire time of the source hash (Will be used by caller
             * to register the new hash in the global ebuckets, i.e db->hexpires) */
            if (dictExpireMetaSrc->expireMeta.trash == 0)
                *minHashExpire = ebGetMetaExpTime(&dictExpireMetaSrc->expireMeta);
        }
        dictExpand(d, dictSize((const dict*)o->ptr));

        hi = hashTypeInitIterator(o);
        while (hashTypeNext(hi, 0) != C_ERR) {
            uint64_t expireTime;
            sds newfield, newvalue;
            /* Extract a field-value pair from an original hash object.*/
            char *field, *value;
            size_t fieldLen, valueLen;
            hashTypeCurrentFromHashTable(hi, OBJ_HASH_KEY, &field, &fieldLen, &expireTime);
            if (expireTime == EB_EXPIRE_TIME_INVALID) {
                newfield = hfieldNew(field, fieldLen, 0);
            } else {
                newfield = hfieldNew(field, fieldLen, 1);
                ebAdd(&dictExpireMetaDst->hfe, &hashFieldExpireBucketsType, newfield, expireTime);
            }

            hashTypeCurrentFromHashTable(hi, OBJ_HASH_VALUE, &value, &valueLen, NULL);
            newvalue = sdsnewlen(value, valueLen);

            /* Add a field-value pair to a new hash object. */
            dictUseStoredKeyApi(d, 1);
            dictAdd(d,newfield,newvalue);
            dictUseStoredKeyApi(d, 0);
        }
        hashTypeReleaseIterator(hi);

        hobj = createObject(OBJ_HASH, d);
        hobj->encoding = OBJ_ENCODING_HT;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return hobj;
}

/* Create a new sds string from the listpack entry. */
sds hashSdsFromListpackEntry(listpackEntry *e) {
    return e->sval ? sdsnewlen(e->sval, e->slen) : sdsfromlonglong(e->lval);
}

/* Reply with bulk string from the listpack entry. */
void hashReplyFromListpackEntry(client *c, listpackEntry *e) {
    if (e->sval)
        addReplyBulkCBuffer(c, e->sval, e->slen);
    else
        addReplyBulkLongLong(c, e->lval);
}

/* Return random element from a non empty hash.
 * 'key' and 'val' will be set to hold the element.
 * The memory in them is not to be freed or modified by the caller.
 * 'val' can be NULL in which case it's not extracted. */
void hashTypeRandomElement(robj *hashobj, unsigned long hashsize, CommonEntry *key, CommonEntry *val) {
    if (hashobj->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = dictGetFairRandomKey(hashobj->ptr);
        hfield field = dictGetKey(de);
        key->sval = (unsigned char*)field;
        key->slen = hfieldlen(field);
        if (val) {
            sds s = dictGetVal(de);
            val->sval = (unsigned char*)s;
            val->slen = sdslen(s);
        }
    } else if (hashobj->encoding == OBJ_ENCODING_LISTPACK) {
        lpRandomPair(hashobj->ptr, hashsize, (listpackEntry *) key, (listpackEntry *) val, 2);
    } else if (hashobj->encoding == OBJ_ENCODING_LISTPACK_EX) {
        lpRandomPair(hashTypeListpackGetLp(hashobj), hashsize, (listpackEntry *) key,
                     (listpackEntry *) val, 3);
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/*
 * Active expiration of fields in hash
 *
 * Called by hashTypeDbActiveExpire() for each hash registered in the HFE DB
 * (db->hexpires) with an expiration-time less than or equal current time.
 *
 * This callback performs the following actions for each hash:
 * - Delete expired fields as by calling ebExpire(hash)
 * - If afterward there are future fields to expire, it will update the hash in
 *   HFE DB with the next hash-field minimum expiration time by returning
 *   ACT_UPDATE_EXP_ITEM.
 * - If the hash has no more fields to expire, it is removed from the HFE DB
 *   by returning ACT_REMOVE_EXP_ITEM.
 * - If hash has no more fields afterward, it will remove the hash from keyspace.
 */
static ExpireAction hashTypeActiveExpire(eItem item, void *ctx) {
    ExpireCtx *expireCtx = ctx;

    /* If no more quota left for this callback, stop */
    if (expireCtx->fieldsToExpireQuota == 0)
        return ACT_STOP_ACTIVE_EXP;

    uint64_t nextExpTime = hashTypeExpire((kvobj *) item, expireCtx, 0);

    /* If hash has no more fields to expire or got deleted, indicate
     * to remove it from HFE DB to the caller ebExpire() */
    if (nextExpTime == EB_EXPIRE_TIME_INVALID || nextExpTime == 0) {
        return ACT_REMOVE_EXP_ITEM;
    } else {
        /* Hash has more fields to expire. Update next expiration time of the hash
         * and indicate to add it back to global HFE DS */
        ebSetMetaExpTime(hashGetExpireMeta(item), nextExpTime);
        return ACT_UPDATE_EXP_ITEM;
    }
}

/* Delete all expired fields from the hash and delete the hash if left empty.
 *
 * updateGlobalHFE - If the hash should be updated in the global HFE DS with new
 *                   expiration time in case expired fields were deleted.
 *
 * Return next Expire time of the hash
 * - 0 if hash got deleted
 * - EB_EXPIRE_TIME_INVALID if no more fields to expire
 */
static uint64_t hashTypeExpire(kvobj *o, ExpireCtx *expireCtx, int updateGlobalHFE) {
    uint64_t noExpireLeftRes = EB_EXPIRE_TIME_INVALID;
    redisDb *db = expireCtx->db;
    ExpireInfo info = {0};

    if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        info = (ExpireInfo) {
                .maxToExpire = expireCtx->fieldsToExpireQuota,
                .now = commandTimeSnapshot(),
                .itemsExpired = 0};

        listpackExExpire(db, o, &info);
    } else {
        serverAssert(o->encoding == OBJ_ENCODING_HT);

        dict *d = o->ptr;
        dictExpireMetadata *dictExpireMeta = (dictExpireMetadata *) dictMetadata(d);

        OnFieldExpireCtx onFieldExpireCtx = { .hashObj = o, .db = db };

        info = (ExpireInfo){
            .maxToExpire = expireCtx->fieldsToExpireQuota,
            .onExpireItem = onFieldExpire,
            .ctx = &onFieldExpireCtx,
            .now = commandTimeSnapshot()
        };

        ebExpire(&dictExpireMeta->hfe, &hashFieldExpireBucketsType, &info);
    }

    /* Update quota left */
    expireCtx->fieldsToExpireQuota -= info.itemsExpired;

    /* In some cases, a field might have been deleted without updating the global DS.
     * As a result, active-expire might not expire any fields, in such cases,
     * we don't need to send notifications or perform other operations for this key. */
    if (info.itemsExpired) {
        sds keystr = kvobjGetKey(o);
        robj *key = createStringObject(keystr, sdslen(keystr));
        notifyKeyspaceEvent(NOTIFY_HASH, "hexpired", key, db->id);

        if (updateGlobalHFE)
            ebRemove(&db->hexpires, &hashExpireBucketsType, o);

        if (hashTypeLength(o, 0) == 0) {
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, db->id);
            dbDelete(db, key);
            noExpireLeftRes = 0;
        } else {
            if ((updateGlobalHFE) && (info.nextExpireTime != EB_EXPIRE_TIME_INVALID))
                ebAdd(&db->hexpires, &hashExpireBucketsType, o, info.nextExpireTime);
        }

        signalModifiedKey(NULL, db, key);
        decrRefCount(key);
    }

    /* return 0 if hash got deleted, EB_EXPIRE_TIME_INVALID if no more fields
     * with expiration. Else return next expiration time */
    return (info.nextExpireTime == EB_EXPIRE_TIME_INVALID) ? noExpireLeftRes : info.nextExpireTime;
}

/* Delete all expired fields in hash if needed (Currently used only by HRANDFIELD)
 *
 * Return 1 if the entire hash was deleted, 0 otherwise.
 * This function might be pricy in case there are many expired fields.
 */
static int hashTypeExpireIfNeeded(redisDb *db, kvobj *o) {
    uint64_t nextExpireTime;
    uint64_t minExpire = hashTypeGetMinExpire(o, 1 /*accurate*/);

    /* Nothing to expire */
    if ((mstime_t) minExpire >= commandTimeSnapshot())
        return 0;

    /* Follow expireIfNeeded() conditions of when not lazy-expire */
    if ( (server.loading) ||
         (server.allow_access_expired) ||
         (server.masterhost) ||  /* master-client or user-client, don't delete */
         (isPausedActionsWithUpdate(PAUSE_ACTION_EXPIRE)))
        return 0;

    /* Take care to expire all the fields */
    ExpireCtx expireCtx = { .db = db, .fieldsToExpireQuota = UINT32_MAX };
    nextExpireTime = hashTypeExpire(o, &expireCtx, 1);
    /* return 1 if the entire hash was deleted */
    return nextExpireTime == 0;
}

/* Return the next/minimum expiry time of the hash-field.
 * accurate=1 - Return the exact time by looking into the object DS.
 * accurate=0 - Return the minimum expiration time maintained in expireMeta
 *              (Verify it is not trash before using it) which might not be
 *              accurate due to optimization reasons.
 *
 * If not found, return EB_EXPIRE_TIME_INVALID
 */
uint64_t hashTypeGetMinExpire(robj *o, int accurate) {
    ExpireMeta *expireMeta = NULL;

    if (!accurate) {
        if (o->encoding == OBJ_ENCODING_LISTPACK) {
            return EB_EXPIRE_TIME_INVALID;
        } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
            listpackEx *lpt = o->ptr;
            expireMeta = &lpt->meta;
        } else {
            serverAssert(o->encoding == OBJ_ENCODING_HT);

            dict *d = o->ptr;
            if (!isDictWithMetaHFE(d))
                return EB_EXPIRE_TIME_INVALID;

            expireMeta = &((dictExpireMetadata *) dictMetadata(d))->expireMeta;
        }

        /* Keep aside next hash-field expiry before updating HFE DS. Verify it is not trash */
        if (expireMeta->trash == 1)
            return EB_EXPIRE_TIME_INVALID;

        return ebGetMetaExpTime(expireMeta);
    }

    /* accurate == 1 */

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        return EB_EXPIRE_TIME_INVALID;
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        return listpackExGetMinExpire(o);
    } else {
        serverAssert(o->encoding == OBJ_ENCODING_HT);

        dict *d = o->ptr;
        if (!isDictWithMetaHFE(d))
            return EB_EXPIRE_TIME_INVALID;

        dictExpireMetadata *expireMeta = (dictExpireMetadata *) dictMetadata(d);
        return ebGetNextTimeToExpire(expireMeta->hfe, &hashFieldExpireBucketsType);
    }
}

uint64_t hashTypeRemoveFromExpires(ebuckets *hexpires, robj *o) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        return EB_EXPIRE_TIME_INVALID;
    } else if (o->encoding == OBJ_ENCODING_HT) {
        /* If dict doesn't holds HFE metadata */
        if (!isDictWithMetaHFE(o->ptr))
            return EB_EXPIRE_TIME_INVALID;
    }

    uint64_t expireTime = ebGetExpireTime(&hashExpireBucketsType, o);

    /* If registered in global HFE DS then remove it (not trash) */
    if (expireTime != EB_EXPIRE_TIME_INVALID)
        ebRemove(hexpires, &hashExpireBucketsType, o);

    return expireTime;
}

int hashTypeIsFieldsWithExpire(robj *o) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        return 0;
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        return EB_EXPIRE_TIME_INVALID != listpackExGetMinExpire(o);
    } else { /* o->encoding == OBJ_ENCODING_HT */
        dict *d = o->ptr;
        /* If dict doesn't holds HFE metadata */
        if (!isDictWithMetaHFE(d))
            return 0;
        dictExpireMetadata *meta = (dictExpireMetadata *) dictMetadata(d);
        return ebGetTotalItems(meta->hfe, &hashFieldExpireBucketsType) != 0;
    }
}

/* Add hash to global HFE DS and update key for notifications.
 *
 * expireTime  - expiration in msec.
 *               If eq. 0 then the hash will be added to the global HFE DS with
 *               the minimum expiration time that is already written in advance
 *               to attached metadata (which considered as trash as long as it is
 *               not attached to global HFE DS).
 *
 * Precondition: It is a hash of type listpackex or HT with HFE metadata.
 */
void hashTypeAddToExpires(redisDb *db, kvobj *hashObj, uint64_t expireTime) {
    if (expireTime > EB_EXPIRE_TIME_MAX)
         return;

    if (hashObj->encoding == OBJ_ENCODING_LISTPACK_EX) {
        listpackEx *lpt = hashObj->ptr;
        expireTime = (expireTime) ? expireTime : ebGetMetaExpTime(&lpt->meta);
        ebAdd(&db->hexpires, &hashExpireBucketsType, hashObj, expireTime);
    } else if (hashObj->encoding == OBJ_ENCODING_HT) {
        dict *d = hashObj->ptr;
        if (isDictWithMetaHFE(d)) {
            dictExpireMetadata *meta = (dictExpireMetadata *) dictMetadata(d);
            expireTime = (expireTime) ? expireTime : ebGetMetaExpTime(&meta->expireMeta);
            ebAdd(&db->hexpires, &hashExpireBucketsType, hashObj, expireTime);
        }
    }
}

/* DB active expire and update hashes with time-expiration on fields.
 *
 * The callback function hashTypeActiveExpire() is invoked for each hash registered
 * in the HFE DB (db->hexpires) with an expiration-time less than or equal to the
 * current time. This callback performs the following actions for each hash:
 * - If the hash has one or more fields to expire, it will delete those fields.
 * - If there are more fields to expire, it will update the hash with the next
 *   expiration time in HFE DB.
 * - If the hash has no more fields to expire, it is removed from the HFE DB.
 * - If the hash has no more fields, it is removed from the main DB.
 *
 * Returns number of fields active-expired.
 */
uint64_t hashTypeDbActiveExpire(redisDb *db, uint32_t maxFieldsToExpire) {
    ExpireCtx ctx = { .db = db, .fieldsToExpireQuota = maxFieldsToExpire };
    ExpireInfo info = {
            .maxToExpire = UINT64_MAX, /* Only maxFieldsToExpire play a role */
            .onExpireItem = hashTypeActiveExpire,
            .ctx = &ctx,
            .now = commandTimeSnapshot(),
            .itemsExpired = 0};

    ebExpire(&db->hexpires, &hashExpireBucketsType, &info);

    /* Return number of fields active-expired */
    return maxFieldsToExpire - ctx.fieldsToExpireQuota;
}

void hashTypeFree(robj *o) {
    switch (o->encoding) {
        case OBJ_ENCODING_HT:
            /* Verify hash is not registered in global HFE ds */
            if (isDictWithMetaHFE((dict*)o->ptr)) {
                dictExpireMetadata *m = (dictExpireMetadata *)dictMetadata((dict*)o->ptr);
                serverAssert(m->expireMeta.trash == 1);
            }
            dictRelease((dict*) o->ptr);
            break;
        case OBJ_ENCODING_LISTPACK:
            lpFree(o->ptr);
            break;
        case OBJ_ENCODING_LISTPACK_EX:
            /* Verify hash is not registered in global HFE ds */
            serverAssert(((listpackEx *) o->ptr)->meta.trash == 1);
            listpackExFree(o->ptr);
            break;
        default:
            serverPanic("Unknown hash encoding type");
            break;
    }
}

ebuckets *hashTypeGetDictMetaHFE(dict *d) {
    dictExpireMetadata *dictExpireMeta = (dictExpireMetadata *) dictMetadata(d);
    return &dictExpireMeta->hfe;
}

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

void hsetnxCommand(client *c) {
    unsigned long hlen;
    int isHashDeleted;
    robj *kv = hashTypeLookupWriteOrCreate(c,c->argv[1]);
    if (kv == NULL) return;

    if (hashTypeExists(c->db, kv, c->argv[2]->ptr, HFE_LAZY_EXPIRE, &isHashDeleted)) {
        addReply(c, shared.czero);
        return;
    }

    /* Field expired and in turn hash deleted. Create new one! */
    if (isHashDeleted) {
        robj *o = createHashObject();
        kv = dbAdd(c->db,c->argv[1],&o);
    }

    hashTypeTryConversion(c->db, kv, c->argv, 2, 3);
    hashTypeSet(c->db, kv, c->argv[2]->ptr, c->argv[3]->ptr, HASH_SET_COPY);
    addReply(c, shared.cone);
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH,"hset",c->argv[1],c->db->id);
    hlen = hashTypeLength(kv, 0);
    updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_HASH, hlen - 1, hlen);
    server.dirty++;
}

void hsetCommand(client *c) {
    int i, created = 0;
    kvobj *kv;

    if ((c->argc % 2) == 1) {
        addReplyErrorArity(c);
        return;
    }

    if ((kv = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTypeTryConversion(c->db, kv, c->argv, 2, c->argc-1);

    for (i = 2; i < c->argc; i += 2)
        created += !hashTypeSet(c->db, kv, c->argv[i]->ptr, c->argv[i+1]->ptr, HASH_SET_COPY);

    /* HMSET (deprecated) and HSET return value is different. */
    char *cmdname = c->argv[0]->ptr;
    if (cmdname[1] == 's' || cmdname[1] == 'S') {
        /* HSET */
        addReplyLongLong(c, created);
    } else {
        /* HMSET */
        addReply(c, shared.ok);
    }
    signalModifiedKey(c,c->db,c->argv[1]);
    unsigned long l = hashTypeLength(kv, 0);
    updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_HASH, l - created, l);
    notifyKeyspaceEvent(NOTIFY_HASH,"hset",c->argv[1],c->db->id);
    server.dirty += (c->argc - 2)/2;
}

/* Parse expire time from argument and do boundary checks. */
static int parseExpireTime(client *c, robj *o, int unit, long long basetime,
                           long long *expire)
{
    long long val;

    /* Read the expiry time from command */
    if (getLongLongFromObjectOrReply(c, o, &val, NULL) != C_OK)
        return C_ERR;

    if (val < 0) {
        addReplyError(c,"invalid expire time, must be >= 0");
        return C_ERR;
    }

    if (unit == UNIT_SECONDS) {
        if (val > (long long) HFE_MAX_ABS_TIME_MSEC / 1000) {
            addReplyErrorExpireTime(c);
            return C_ERR;
        }
        val *= 1000;
    }

    if (val > (long long) HFE_MAX_ABS_TIME_MSEC - basetime) {
        addReplyErrorExpireTime(c);
        return C_ERR;
    }
    val += basetime;
    *expire = val;
    return C_OK;
}

/* Flags that are used as part of HGETEX and HSETEX commands. */
#define HFE_EX       (1<<0) /* Expiration time in seconds */
#define HFE_PX       (1<<1) /* Expiration time in milliseconds */
#define HFE_EXAT     (1<<2) /* Expiration time in unix seconds */
#define HFE_PXAT     (1<<3) /* Expiration time in unix milliseconds */
#define HFE_PERSIST  (1<<4) /* Persist fields */
#define HFE_KEEPTTL  (1<<5) /* Do not discard field ttl on set op */
#define HFE_FXX      (1<<6) /* Set fields if all the fields already exist */
#define HFE_FNX      (1<<7) /* Set fields if none of the fields exist */

/* Parse hsetex command arguments.
 * HSETEX <key>
 *  [FNX|FXX]
 *  [EX seconds|PX milliseconds|EXAT unix-time-seconds|PXAT unix-time-milliseconds|KEEPTTL]
 *  FIELDS <numfields> field value [field value ...]
*/
static int hsetexParseArgs(client *c, int *flags,
                           long long *expire_time, int *expire_time_pos,
                           int *first_field_pos, int *field_count) {
    *flags = 0;
    *first_field_pos = -1;
    *field_count = -1;
    *expire_time_pos = -1;

    for (int i = 2; i < c->argc; i++) {
        if (!strcasecmp(c->argv[i]->ptr, "fields")) {
            long val;

            if (i >= c->argc - 3) {
                addReplyErrorArity(c);
                return C_ERR;
            }

            if (getRangeLongFromObjectOrReply(c, c->argv[i + 1], 1, INT_MAX, &val,
                                              "invalid number of fields") != C_OK)
                return C_ERR;

            int remaining = (c->argc - i  - 2);
            if (remaining % 2 != 0 || val != remaining / 2) {
                addReplyErrorArity(c);
                return C_ERR;
            }

            *first_field_pos = i + 2;
            *field_count = (int) val;
            return C_OK;
        } else if (!strcasecmp(c->argv[i]->ptr, "EX")) {
            if (*flags & (HFE_EX | HFE_EXAT | HFE_PX | HFE_PXAT | HFE_KEEPTTL))
                goto err_expiration;

            if (i >= c->argc - 1)
                goto err_missing_expire;

            *flags |= HFE_EX;
            i++;

            if (parseExpireTime(c, c->argv[i], UNIT_SECONDS,
                                commandTimeSnapshot(), expire_time) != C_OK)
                return C_ERR;

            *expire_time_pos = i;
        } else if (!strcasecmp(c->argv[i]->ptr, "PX")) {
            if (*flags & (HFE_EX | HFE_EXAT | HFE_PX | HFE_PXAT | HFE_KEEPTTL))
                goto err_expiration;

            if (i >= c->argc - 1)
                goto err_missing_expire;

            *flags |= HFE_PX;
            i++;
            if (parseExpireTime(c, c->argv[i], UNIT_MILLISECONDS,
                                commandTimeSnapshot(), expire_time) != C_OK)
                return C_ERR;

            *expire_time_pos = i;
        } else if (!strcasecmp(c->argv[i]->ptr, "EXAT")) {
            if (*flags & (HFE_EX | HFE_EXAT | HFE_PX | HFE_PXAT | HFE_KEEPTTL))
                goto err_expiration;

            if (i >= c->argc - 1)
                goto err_missing_expire;

            *flags |= HFE_EXAT;
            i++;
            if (parseExpireTime(c, c->argv[i], UNIT_SECONDS, 0, expire_time) != C_OK)
                return C_ERR;

            *expire_time_pos = i;
        } else if (!strcasecmp(c->argv[i]->ptr, "PXAT")) {
            if (*flags & (HFE_EX | HFE_EXAT | HFE_PX | HFE_PXAT | HFE_KEEPTTL))
                goto err_expiration;

            if (i >= c->argc - 1)
                goto err_missing_expire;

            *flags |= HFE_PXAT;
            i++;
            if (parseExpireTime(c, c->argv[i], UNIT_MILLISECONDS, 0,
                                expire_time) != C_OK)
                return C_ERR;

            *expire_time_pos = i;
        } else if (!strcasecmp(c->argv[i]->ptr, "KEEPTTL")) {
            if (*flags & (HFE_EX | HFE_EXAT | HFE_PX | HFE_PXAT | HFE_KEEPTTL))
                goto err_expiration;
            *flags |= HFE_KEEPTTL;
        } else if (!strcasecmp(c->argv[i]->ptr, "FXX")) {
            if (*flags & (HFE_FXX | HFE_FNX))
                goto err_condition;
            *flags |= HFE_FXX;
        } else if (!strcasecmp(c->argv[i]->ptr, "FNX")) {
            if (*flags & (HFE_FXX | HFE_FNX))
                goto err_condition;
            *flags |= HFE_FNX;
        } else {
            addReplyErrorFormat(c, "unknown argument: %s", (char*) c->argv[i]->ptr);
            return C_ERR;
        }
    }

    serverAssert(0);

err_missing_expire:
    addReplyError(c, "missing expire time");
    return C_ERR;
err_condition:
    addReplyError(c, "Only one of FXX or FNX arguments can be specified");
    return C_ERR;
err_expiration:
    addReplyError(c, "Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments can be specified");
    return C_ERR;
}

/* Set the value of one or more fields of a given hash key, and optionally set
 * their expiration.
 *
 * HSETEX key
 *  [FNX | FXX]
 *  [EX seconds | PX milliseconds | EXAT unix-time-seconds | PXAT unix-time-milliseconds | KEEPTTL]
 *  FIELDS <numfields> field value [field value...]
 *
 * Reply:
 *   Integer reply: 0 if no fields were set (due to FXX/FNX args)
 *   Integer reply: 1 if all the fields were set
 */
void hsetexCommand(client *c) {
    int flags = 0, first_field_pos = 0, field_count = 0, expire_time_pos = -1;
    int updated = 0, deleted = 0, set_expiry;
    int expired = 0, fields_set = 0;
    long long expire_time = EB_EXPIRE_TIME_INVALID;
    int64_t oldlen, newlen;
    HashTypeSetEx setex;
    dictEntryLink link;

    if (hsetexParseArgs(c, &flags, &expire_time, &expire_time_pos,
                        &first_field_pos, &field_count) != C_OK)
        return;

    kvobj *o = lookupKeyWriteWithLink(c->db, c->argv[1], &link);
    if (checkType(c, o, OBJ_HASH))
        return;

    if (!o) {
        if (flags & HFE_FXX) {
            addReplyLongLong(c, 0);
            return;
        }
        o = createHashObject();
        dbAddByLink(c->db, c->argv[1], &o, &link);
    }
    oldlen = (int64_t) hashTypeLength(o, 0);

    if (flags & (HFE_FXX | HFE_FNX)) {
        int found = 0;
        for (int i = 0; i < field_count; i++) {
            sds field = c->argv[first_field_pos + (i * 2)]->ptr;
            unsigned char *vstr = NULL;
            unsigned int vlen = UINT_MAX;
            long long vll = LLONG_MAX;
            const int opt = HFE_LAZY_NO_NOTIFICATION |
                            HFE_LAZY_NO_SIGNAL |
                            HFE_LAZY_AVOID_HASH_DEL |
                            HFE_LAZY_NO_UPDATE_KEYSIZES;

            GetFieldRes res = hashTypeGetValue(c->db, o, field, &vstr, &vlen, &vll, opt, NULL);
            int exists = (res == GETF_OK);
            expired += (res == GETF_EXPIRED);
            found += exists;

            /* Check for early exit if the condition is already invalid. */
            if (((flags & HFE_FXX) && !exists) ||
                ((flags & HFE_FNX) && exists))
                break;
        }

        int all_exists = (found == field_count);
        int non_exists = (found == 0);

        if (((flags & HFE_FNX) && !non_exists) ||
            ((flags & HFE_FXX) && !all_exists))
        {
            addReplyLongLong(c, 0);
            goto out;
        }
    }
    hashTypeTryConversion(c->db, o,c->argv, first_field_pos, c->argc - 1);

    /* Check if we will set the expiration time. */
    set_expiry = flags & (HFE_EX | HFE_PX | HFE_EXAT | HFE_PXAT);
    if (set_expiry)
        hashTypeSetExInit(c->argv[1], o, c, c->db, 0, &setex);


    for (int i = 0; i < field_count; i++) {
        sds field = c->argv[first_field_pos + (i * 2)]->ptr;
        sds value = c->argv[first_field_pos + (i * 2) + 1]->ptr;

        int opt = HASH_SET_COPY;
        /* If we are going to set the expiration time later, no need to discard
         * it as part of set operation now. */
        if (flags & (HFE_EX | HFE_PX | HFE_EXAT | HFE_PXAT | HFE_KEEPTTL))
            opt |= HASH_SET_KEEP_TTL;

        hashTypeSet(c->db, o, field, value, opt);
        fields_set = 1;
        /* Update the expiration time. */
        if (set_expiry) {
            int ret = hashTypeSetEx(o, field, expire_time, &setex);
            updated += (ret == HSETEX_OK);
            deleted += (ret == HSETEX_DELETED);
        }
    }

    if (set_expiry)
        hashTypeSetExDone(&setex);

    server.dirty += field_count;

    if (deleted) {
        /* If fields are deleted due to timestamp is being in the past, hdel's
         * are already propagated. No need to propagate the command itself. */
        preventCommandPropagation(c);
    } else if (set_expiry && !(flags & HFE_PXAT)) {
        /* Propagate as 'HSETEX <key> PXAT ..' if there is EX/EXAT/PX flag*/

        /* Replace EX/EXAT/PX with PXAT */
        rewriteClientCommandArgument(c, expire_time_pos - 1, shared.pxat);
        /* Replace timestamp with unix timestamp milliseconds. */
        robj *expire = createStringObjectFromLongLong(expire_time);
        rewriteClientCommandArgument(c, expire_time_pos, expire);
        decrRefCount(expire);
    }

    addReplyLongLong(c, 1);

out:
    /* Emit keyspace notifications based on field expiry, mutation, or key deletion */
    if (fields_set || expired) {
        signalModifiedKey(c, c->db, c->argv[1]);
        if (expired)
            notifyKeyspaceEvent(NOTIFY_HASH, "hexpired", c->argv[1], c->db->id);
        if (fields_set) {
            notifyKeyspaceEvent(NOTIFY_HASH, "hset", c->argv[1], c->db->id);
            if (deleted || updated)
                notifyKeyspaceEvent(NOTIFY_HASH, deleted ? "hdel" : "hexpire", c->argv[1], c->db->id);
        }
    }
    /* Key may become empty due to lazy expiry in hashTypeExists()
     * or the new expiration time is in the past.*/
    newlen = (int64_t) hashTypeLength(o, 0);
    if (newlen == 0) {
        newlen = -1;
        /* Del key but don't update KEYSIZES. else it will decr wrong bin in histogram */
        dbDeleteSkipKeysizesUpdate(c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    }
    if (oldlen != newlen)
        updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_HASH,
                           oldlen, newlen);
}

void hincrbyCommand(client *c) {
    long long value, incr, oldvalue;
    kvobj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if (getLongLongFromObjectOrReply(c,c->argv[3],&incr,NULL) != C_OK) return;
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;

    GetFieldRes res = hashTypeGetValue(c->db,o,c->argv[2]->ptr,&vstr,&vlen,&value,
                                       HFE_LAZY_EXPIRE, NULL);
    if (res == GETF_OK) {
        if (vstr) {
            if (string2ll((char*)vstr,vlen,&value) == 0) {
                addReplyError(c,"hash value is not an integer");
                return;
            }
        } /* Else hashTypeGetValue() already stored it into &value */
    } else if ((res == GETF_NOT_FOUND) || (res == GETF_EXPIRED)) {
        value = 0;
        unsigned long l = hashTypeLength(o, 0);
        updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_HASH, l, l + 1);
    } else {
        /* Field expired and in turn hash deleted. Create new one! */
        o = createHashObject();
        dbAdd(c->db,c->argv[1],&o);
        value = 0;
        updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_HASH, 0, 1);
    }

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr;
    new = sdsfromlonglong(value);
    hashTypeSet(c->db, o,c->argv[2]->ptr,new,HASH_SET_TAKE_VALUE | HASH_SET_KEEP_TTL);
    addReplyLongLong(c,value);
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH,"hincrby",c->argv[1],c->db->id);
    server.dirty++;
}

void hincrbyfloatCommand(client *c) {
    long double value, incr;
    long long ll;
    kvobj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if (getLongDoubleFromObjectOrReply(c,c->argv[3],&incr,NULL) != C_OK) return;
    if (isnan(incr) || isinf(incr)) {
        addReplyError(c,"value is NaN or Infinity");
        return;
    }
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    GetFieldRes res = hashTypeGetValue(c->db, o,c->argv[2]->ptr,&vstr,&vlen,&ll,
                                       HFE_LAZY_EXPIRE, NULL);
    if (res == GETF_OK) {
        if (vstr) {
            if (string2ld((char*)vstr,vlen,&value) == 0) {
                addReplyError(c,"hash value is not a float");
                return;
            }
        } else {
            value = (long double)ll;
        }
    } else if ((res == GETF_NOT_FOUND) || (res == GETF_EXPIRED)) {
        value = 0;
        unsigned long l = hashTypeLength(o, 0);
        updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_HASH, l, l + 1);
    } else {
        /* Field expired and in turn hash deleted. Create new one! */
        o = createHashObject();
        dbAdd(c->db, c->argv[1], &o);
        value = 0;
        updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_HASH, 0, 1);
    }

    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }

    char buf[MAX_LONG_DOUBLE_CHARS];
    int len = ld2string(buf,sizeof(buf),value,LD_STR_HUMAN);
    new = sdsnewlen(buf,len);
    hashTypeSet(c->db, o,c->argv[2]->ptr,new,HASH_SET_TAKE_VALUE | HASH_SET_KEEP_TTL);
    addReplyBulkCBuffer(c,buf,len);
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH,"hincrbyfloat",c->argv[1],c->db->id);
    server.dirty++;

    /* Always replicate HINCRBYFLOAT as an HSETEX command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart.
     * The KEEPTTL flag is used to make sure the field TTL is preserved. */
    robj *newobj;
    newobj = createRawStringObject(buf,len);
    rewriteClientCommandVector(c, 7, shared.hsetex, c->argv[1], shared.keepttl,
                        shared.fields, shared.integers[1], c->argv[2], newobj);
    decrRefCount(newobj);
}

static GetFieldRes addHashFieldToReply(client *c, kvobj *o, sds field, int hfeFlags) {
    if (o == NULL) {
        addReplyNull(c);
        return GETF_NOT_FOUND;
    }

    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    GetFieldRes res = hashTypeGetValue(c->db, o, field, &vstr, &vlen, &vll, hfeFlags, NULL);
    if (res == GETF_OK) {
        if (vstr) {
            addReplyBulkCBuffer(c, vstr, vlen);
        } else {
            addReplyBulkLongLong(c, vll);
        }
    } else {
        addReplyNull(c);
    }
    return res;
}

void hgetCommand(client *c) {
    kvobj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    addHashFieldToReply(c, o, c->argv[2]->ptr, HFE_LAZY_EXPIRE);
}

void hmgetCommand(client *c) {
    GetFieldRes res = GETF_OK;
    int i;
    int expired = 0, deleted = 0;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HMGET should respond with a series of null bulks. */
    kvobj *o = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c,o,OBJ_HASH)) return;

    addReplyArrayLen(c, c->argc-2);
    for (i = 2; i < c->argc ; i++) {
        if (!deleted) {
            res = addHashFieldToReply(c, o, c->argv[i]->ptr, HFE_LAZY_NO_NOTIFICATION);
            expired += (res == GETF_EXPIRED);
            deleted += (res == GETF_EXPIRED_HASH);
        } else {
            /* If hash got lazy expired since all fields are expired (o is invalid),
             * then fill the rest with trivial nulls and return. */
            addReplyNull(c);
        }
    }

    if (expired) {
        notifyKeyspaceEvent(NOTIFY_HASH, "hexpired", c->argv[1], c->db->id);
        if (deleted)
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id); 
    }
}

/* Get and delete the value of one or more fields of a given hash key.
 * HGETDEL <key> FIELDS <numfields> field1 field2 ...
 * Reply: list of the value associated with each field or nil if the field
 *        doesn’t exist.
 */
void hgetdelCommand(client *c) {
    int res = 0, hfe = 0, deleted = 0, expired = 0;
    int64_t oldlen = -1; /* not exists as long as it is not set */
    long num_fields = 0;

    kvobj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_HASH))
        return;

    if (strcasecmp(c->argv[2]->ptr, "FIELDS") != 0) {
        addReplyError(c, "Mandatory argument FIELDS is missing or not at the right position");
        return;
    }

    /* Read number of fields */
    if (getRangeLongFromObjectOrReply(c, c->argv[3], 1, LONG_MAX, &num_fields,
                                      "Number of fields must be a positive integer") != C_OK)
        return;

    /* Verify `numFields` is consistent with number of arguments */
    if (num_fields != c->argc - 4) {
        addReplyError(c, "The `numfields` parameter must match the number of arguments");
        return;
    }

    /* Hash field expiration is optimized to avoid frequent update global HFE DS
     * for each field deletion. Eventually active-expiration will run and update
     * or remove the hash from global HFE DS gracefully. Nevertheless, statistic
     * "subexpiry" might reflect wrong number of hashes with HFE to the user if
     * it is the last field with expiration. The following logic checks if this
     * is the last field with expiration and removes it from global HFE DS. */
    if (o) {
        hfe = hashTypeIsFieldsWithExpire(o);
        oldlen = hashTypeLength(o, 0);
    }

    addReplyArrayLen(c, num_fields);
    for (int i = 4; i < c->argc; i++) {
        const int flags = HFE_LAZY_NO_NOTIFICATION |
                          HFE_LAZY_NO_SIGNAL |
                          HFE_LAZY_AVOID_HASH_DEL |
                          HFE_LAZY_NO_UPDATE_KEYSIZES;
        res = addHashFieldToReply(c, o, c->argv[i]->ptr, flags);
        expired += (res == GETF_EXPIRED);
        /* Try to delete only if it's found and not expired lazily. */
        if (res == GETF_OK) {
            deleted++;
            serverAssert(hashTypeDelete(o, c->argv[i]->ptr, 1) == 1);
        }
    }

    /* Return if no modification has been made. */
    if (expired == 0 && deleted == 0)
        return;

    signalModifiedKey(c, c->db, c->argv[1]);

    if (expired)
        notifyKeyspaceEvent(NOTIFY_HASH, "hexpired", c->argv[1], c->db->id);
    if (deleted) {
        notifyKeyspaceEvent(NOTIFY_HASH, "hdel", c->argv[1], c->db->id);
        server.dirty += deleted;

        /* Propagate as HDEL command.
         * Orig: HGETDEL <key> FIELDS <numfields> field1 field2 ...
         * Repl: HDEL <key> field1 field2 ... */
        rewriteClientCommandArgument(c, 0, shared.hdel);
        rewriteClientCommandArgument(c, 2, NULL);  /* Delete FIELDS arg */
        rewriteClientCommandArgument(c, 2, NULL);  /* Delete <numfields> arg */
    }

    /* Key may have become empty because of deleting fields or lazy expire. */
    int64_t newlen = (int64_t) hashTypeLength(o, 0);
    if (newlen == 0) {
        newlen = -1;
        /* Del key but don't update KEYSIZES. else it will decr wrong bin in histogram */
        dbDeleteSkipKeysizesUpdate(c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    } else if (hfe && (hashTypeIsFieldsWithExpire(o) == 0)) { /*is it last HFE*/
        ebRemove(&c->db->hexpires, &hashExpireBucketsType, o);
    }

    if (oldlen != newlen)
        updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_HASH,
                           oldlen, newlen);
}

/* Get and delete the value of one or more fields of a given hash key.
 *
 * HGETEX <key>
 *   [EX seconds | PX milliseconds | EXAT unix-time-seconds | PXAT unix-time-milliseconds | PERSIST]
 *   FIELDS <numfields> field1 field2 ...
 *
 * Reply: list of the value associated with each field or nil if the field
 *        doesn’t exist.
 */
void hgetexCommand(client *c) {
    int expired = 0, deleted = 0, updated = 0;
    int num_fields_pos = 3, cond = 0;
    long num_fields;
    int64_t oldlen = 0, newlen = -1;
    long long expire_time = 0;
    HashTypeSetEx setex;

    kvobj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_HASH))
        return;

    /* Read optional arg */
    if (!strcasecmp(c->argv[2]->ptr, "ex"))
        cond = HFE_EX;
    else if (!strcasecmp(c->argv[2]->ptr, "px"))
        cond = HFE_PX;
    else if (!strcasecmp(c->argv[2]->ptr, "exat"))
        cond = HFE_EXAT;
    else if (!strcasecmp(c->argv[2]->ptr, "pxat"))
        cond = HFE_PXAT;
    else if (!strcasecmp(c->argv[2]->ptr, "persist"))
        cond = HFE_PERSIST;

    /* Parse expiration time */
    if (cond & (HFE_EX | HFE_PX | HFE_EXAT | HFE_PXAT)) {
        num_fields_pos += 2;
        int unit = (cond & (HFE_EX | HFE_EXAT)) ? UNIT_SECONDS : UNIT_MILLISECONDS;
        long long basetime = cond & (HFE_EX | HFE_PX) ? commandTimeSnapshot() : 0;
        if (parseExpireTime(c, c->argv[3], unit, basetime, &expire_time) != C_OK)
            return;
    } else if (cond & HFE_PERSIST) {
        num_fields_pos += 1;
    }

    if (strcasecmp(c->argv[num_fields_pos - 1]->ptr, "FIELDS") != 0) {
        addReplyError(c, "Mandatory argument FIELDS is missing or not at the right position");
        return;
    }

    /* Read number of fields */
    if (getRangeLongFromObjectOrReply(c, c->argv[num_fields_pos], 1, LONG_MAX, &num_fields,
                                      "Number of fields must be a positive integer") != C_OK)
        return;

    /* Check number of fields is consistent with number of arguments */
    if (num_fields != c->argc - num_fields_pos - 1) {
        addReplyError(c, "The `numfields` parameter must match the number of arguments");
        return;
    }

    /* Non-existing keys and empty hashes are the same thing. Reply null if the
     * key does not exist.*/
    if (!o) {
        addReplyArrayLen(c, num_fields);
        for (int i = 0; i < num_fields; i++)
            addReplyNull(c);
        return;
    }

    oldlen = hashTypeLength(o, 0);
    if (cond)
        hashTypeSetExInit(c->argv[1], o, c, c->db, 0, &setex);

    addReplyArrayLen(c, num_fields);
    for (int i = num_fields_pos + 1; i < c->argc; i++) {
        const int flags = HFE_LAZY_NO_NOTIFICATION |
                          HFE_LAZY_NO_SIGNAL |
                          HFE_LAZY_AVOID_HASH_DEL |
                          HFE_LAZY_NO_UPDATE_KEYSIZES;
        sds field = c->argv[i]->ptr;
        int res = addHashFieldToReply(c, o, field, flags);
        expired += (res == GETF_EXPIRED);

        /* Set expiration only if the field exists and not expired lazily. */
        if (res == GETF_OK && cond) {
            if (cond & HFE_PERSIST)
                expire_time = EB_EXPIRE_TIME_INVALID;

            res = hashTypeSetEx(o, field, expire_time, &setex);
            deleted += (res == HSETEX_DELETED);
            updated += (res == HSETEX_OK);
        }
    }

    if (cond)
        hashTypeSetExDone(&setex);

    /* Exit early if no modification has been made. */
    if (expired == 0 && deleted == 0 && updated == 0)
        return;

    server.dirty += deleted + updated;
    signalModifiedKey(c, c->db, c->argv[1]);

    /* This command will never be propagated as it is. It will be propagated as
     * HDELs when fields are lazily expired or deleted, if the new timestamp is
     * in the past. HDEL's will be emitted as part of addHashFieldToReply()
     * or hashTypeSetEx() in this case.
     *
     * If PERSIST flags is used, it will be propagated as HPERSIST command.
     * IF EX/EXAT/PX/PXAT flags are used, it will be replicated as HPEXPRITEAT.
     */
    if (expired)
        notifyKeyspaceEvent(NOTIFY_HASH, "hexpired", c->argv[1], c->db->id);
    if (updated) {
        if (cond & HFE_PERSIST) {
            notifyKeyspaceEvent(NOTIFY_HASH, "hpersist", c->argv[1], c->db->id);

            /* Propagate as HPERSIST command.
             * Orig: HGETEX <key> PERSIST FIELDS <numfields> field1 field2 ...
             * Repl: HPERSIST <key> FIELDS <numfields> field1 field2 ... */
            rewriteClientCommandArgument(c, 0, shared.hpersist);
            rewriteClientCommandArgument(c, 2, NULL); /* Delete PERSIST arg */
        } else {
            notifyKeyspaceEvent(NOTIFY_HASH, "hexpire", c->argv[1], c->db->id);

            /* Propagate as HPEXPIREAT command.
             * Orig: HGETEX <key> [EX|PX|EXAT|PXAT] ttl FIELDS <numfields> field1 field2 ...
             * Repl: HPEXPIREAT <key> ttl FIELDS <numfields> field1 field2 ... */
            rewriteClientCommandArgument(c, 0, shared.hpexpireat);
            rewriteClientCommandArgument(c, 2, NULL); /* Del [EX|PX|EXAT|PXAT]*/

            /* Rewrite TTL if it is not unix time milliseconds already. */
            if (!(cond & HFE_PXAT)) {
                robj *expire = createStringObjectFromLongLong(expire_time);
                rewriteClientCommandArgument(c, 2, expire);
                decrRefCount(expire);
            }
        }
    } else if (deleted) {
        /* If we are here, fields are deleted because new timestamp was in the
         * past. HDELs are already propagated as part of hashTypeSetEx(). */
        notifyKeyspaceEvent(NOTIFY_HASH, "hdel", c->argv[1], c->db->id);
        preventCommandPropagation(c);
    }

    /* Key may become empty due to lazy expiry in addHashFieldToReply()
     * or the new expiration time is in the past.*/
    newlen = hashTypeLength(o, 0);

    updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_HASH, oldlen, newlen);
    if (newlen == 0) {
        dbDelete(c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    }
}

void hdelCommand(client *c) {
    kvobj *o;
    int j, deleted = 0, keyremoved = 0;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    int64_t oldLen = (int64_t) hashTypeLength(o, 0);
    
    /* Hash field expiration is optimized to avoid frequent update global HFE DS for
     * each field deletion. Eventually active-expiration will run and update or remove
     * the hash from global HFE DS gracefully. Nevertheless, statistic "subexpiry"
     * might reflect wrong number of hashes with HFE to the user if it is the last
     * field with expiration. The following logic checks if this is indeed the last
     * field with expiration and removes it from global HFE DS. */
    int isHFE = hashTypeIsFieldsWithExpire(o);

    for (j = 2; j < c->argc; j++) {
        if (hashTypeDelete(o,c->argv[j]->ptr,1)) {
            deleted++;
            if (hashTypeLength(o, 0) == 0) {
                /* del key but don't update KEYSIZES. Else it will decr wrong bin in histogram */
                dbDeleteSkipKeysizesUpdate(c->db, c->argv[1]);
                keyremoved = 1;
                break;
            }
        }
    }
    if (deleted) {
        int64_t newLen = -1; /* The value -1 indicates that the key is deleted. */
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_HASH,"hdel",c->argv[1],c->db->id);
        if (keyremoved) {
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        } else {
            if (isHFE && (hashTypeIsFieldsWithExpire(o) == 0)) /* is it last HFE */
                ebRemove(&c->db->hexpires, &hashExpireBucketsType, o);
            newLen = oldLen - deleted;
        }
        updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_HASH, oldLen, newLen);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);
}

void hlenCommand(client *c) {
    kvobj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    addReplyLongLong(c,hashTypeLength(o, 0));
}

void hstrlenCommand(client *c) {
    kvobj *o;
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    GetFieldRes res = hashTypeGetValue(c->db, o, c->argv[2]->ptr, &vstr,
                                       &vlen, &vll, HFE_LAZY_EXPIRE, NULL);

    if (res == GETF_NOT_FOUND || res == GETF_EXPIRED || res == GETF_EXPIRED_HASH) {
        addReply(c, shared.czero);
        return;
    }

    size_t len = vstr ? vlen : sdigits10(vll);
    addReplyLongLong(c,len);
}

static void addHashIteratorCursorToReply(client *c, hashTypeIterator *hi, int what) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK ||
        hi->encoding == OBJ_ENCODING_LISTPACK_EX)
    {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromListpack(hi, what, &vstr, &vlen, &vll, NULL);
        if (vstr)
            addReplyBulkCBuffer(c, vstr, vlen);
        else
            addReplyBulkLongLong(c, vll);
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        char *value;
        size_t len;
        hashTypeCurrentFromHashTable(hi, what, &value, &len, NULL);
        addReplyBulkCBuffer(c, value, len);
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void genericHgetallCommand(client *c, int flags) {
    kvobj *o;
    hashTypeIterator *hi;
    int length, count = 0;

    robj *emptyResp = (flags & OBJ_HASH_KEY && flags & OBJ_HASH_VALUE) ?
        shared.emptymap[c->resp] : shared.emptyarray;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],emptyResp))
        == NULL || checkType(c,o,OBJ_HASH)) return;

    /* We return a map if the user requested keys and values, like in the
     * HGETALL case. Otherwise to use a flat array makes more sense. */
    if ((length = hashTypeLength(o, 1 /*subtractExpiredFields*/)) == 0) {
        addReply(c, emptyResp);
        return;
    }

    if (flags & OBJ_HASH_KEY && flags & OBJ_HASH_VALUE) {
        addReplyMapLen(c, length);
    } else {
        addReplyArrayLen(c, length);
    }

    hi = hashTypeInitIterator(o);

    while (hashTypeNext(hi, 1 /*skipExpiredFields*/) != C_ERR) {
        if (flags & OBJ_HASH_KEY) {
            addHashIteratorCursorToReply(c, hi, OBJ_HASH_KEY);
            count++;
        }
        if (flags & OBJ_HASH_VALUE) {
            addHashIteratorCursorToReply(c, hi, OBJ_HASH_VALUE);
            count++;
        }
    }

    hashTypeReleaseIterator(hi);

    /* Make sure we returned the right number of elements. */
    if (flags & OBJ_HASH_KEY && flags & OBJ_HASH_VALUE) count /= 2;
    serverAssert(count == length);
}

void hkeysCommand(client *c) {
    genericHgetallCommand(c,OBJ_HASH_KEY);
}

void hvalsCommand(client *c) {
    genericHgetallCommand(c,OBJ_HASH_VALUE);
}

void hgetallCommand(client *c) {
    genericHgetallCommand(c,OBJ_HASH_KEY|OBJ_HASH_VALUE);
}

void hexistsCommand(client *c) {
    kvobj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    addReply(c,hashTypeExists(c->db,o,c->argv[2]->ptr,HFE_LAZY_EXPIRE, NULL) ?
                                shared.cone : shared.czero);
}

void hscanCommand(client *c) {
    kvobj *o;
    unsigned long long cursor;

    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    scanGenericCommand(c,o,cursor);
}

static void hrandfieldReplyWithListpack(client *c, unsigned int count, listpackEntry *keys, listpackEntry *vals) {
    for (unsigned long i = 0; i < count; i++) {
        if (vals && c->resp > 2)
            addReplyArrayLen(c,2);
        if (keys[i].sval)
            addReplyBulkCBuffer(c, keys[i].sval, keys[i].slen);
        else
            addReplyBulkLongLong(c, keys[i].lval);
        if (vals) {
            if (vals[i].sval)
                addReplyBulkCBuffer(c, vals[i].sval, vals[i].slen);
            else
                addReplyBulkLongLong(c, vals[i].lval);
        }
    }
}

/* How many times bigger should be the hash compared to the requested size
 * for us to not use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define HRANDFIELD_SUB_STRATEGY_MUL 3

/* If client is trying to ask for a very large number of random elements,
 * queuing may consume an unlimited amount of memory, so we want to limit
 * the number of randoms per time. */
#define HRANDFIELD_RANDOM_SAMPLE_LIMIT 1000

void hrandfieldWithCountCommand(client *c, long l, int withvalues) {
    unsigned long count, size;
    int uniq = 1;
    kvobj *hash;

    if ((hash = lookupKeyReadOrReply(c,c->argv[1],shared.emptyarray))
        == NULL || checkType(c,hash,OBJ_HASH)) return;

    if(l >= 0) {
        count = (unsigned long) l;
    } else {
        count = -l;
        uniq = 0;
    }

    /* Delete all expired fields. If the entire hash got deleted then return empty array. */
    if (hashTypeExpireIfNeeded(c->db, hash)) {
        addReply(c, shared.emptyarray);
        return;
    }

    /* Delete expired fields */
    size = hashTypeLength(hash, 0);

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
        if (withvalues && c->resp == 2)
            addReplyArrayLen(c, count*2);
        else
            addReplyArrayLen(c, count);
        if (hash->encoding == OBJ_ENCODING_HT) {
            while (count--) {
                dictEntry *de = dictGetFairRandomKey(hash->ptr);
                hfield field = dictGetKey(de);
                sds value = dictGetVal(de);
                if (withvalues && c->resp > 2)
                    addReplyArrayLen(c,2);
                addReplyBulkCBuffer(c, field, hfieldlen(field));
                if (withvalues)
                    addReplyBulkCBuffer(c, value, sdslen(value));
                if (c->flags & CLIENT_CLOSE_ASAP)
                    break;
            }
        } else if (hash->encoding == OBJ_ENCODING_LISTPACK ||
                   hash->encoding == OBJ_ENCODING_LISTPACK_EX)
        {
            listpackEntry *keys, *vals = NULL;
            unsigned long limit, sample_count;
            unsigned char *lp = hashTypeListpackGetLp(hash);
            int tuple_len = hash->encoding == OBJ_ENCODING_LISTPACK ? 2 : 3;

            limit = count > HRANDFIELD_RANDOM_SAMPLE_LIMIT ? HRANDFIELD_RANDOM_SAMPLE_LIMIT : count;
            keys = zmalloc(sizeof(listpackEntry)*limit);
            if (withvalues)
                vals = zmalloc(sizeof(listpackEntry)*limit);
            while (count) {
                sample_count = count > limit ? limit : count;
                count -= sample_count;
                lpRandomPairs(lp, sample_count, keys, vals, tuple_len);
                hrandfieldReplyWithListpack(c, sample_count, keys, vals);
                if (c->flags & CLIENT_CLOSE_ASAP)
                    break;
            }
            zfree(keys);
            zfree(vals);
        }
        return;
    }

    /* Initiate reply count, RESP3 responds with nested array, RESP2 with flat one. */
    long reply_size = count < size ? count : size;
    if (withvalues && c->resp == 2)
        addReplyArrayLen(c, reply_size*2);
    else
        addReplyArrayLen(c, reply_size);

    /* CASE 2:
    * The number of requested elements is greater than the number of
    * elements inside the hash: simply return the whole hash. */
    if(count >= size) {
        hashTypeIterator *hi = hashTypeInitIterator(hash);
        while (hashTypeNext(hi, 0) != C_ERR) {
            if (withvalues && c->resp > 2)
                addReplyArrayLen(c,2);
            addHashIteratorCursorToReply(c, hi, OBJ_HASH_KEY);
            if (withvalues)
                addHashIteratorCursorToReply(c, hi, OBJ_HASH_VALUE);
        }
        hashTypeReleaseIterator(hi);
        return;
    }

    /* CASE 2.5 listpack only. Sampling unique elements, in non-random order.
     * Listpack encoded hashes are meant to be relatively small, so
     * HRANDFIELD_SUB_STRATEGY_MUL isn't necessary and we rather not make
     * copies of the entries. Instead, we emit them directly to the output
     * buffer.
     *
     * And it is inefficient to repeatedly pick one random element from a
     * listpack in CASE 4. So we use this instead. */
    if (hash->encoding == OBJ_ENCODING_LISTPACK ||
        hash->encoding == OBJ_ENCODING_LISTPACK_EX)
    {
        unsigned char *lp = hashTypeListpackGetLp(hash);
        int tuple_len = hash->encoding == OBJ_ENCODING_LISTPACK ? 2 : 3;
        listpackEntry *keys, *vals = NULL;
        keys = zmalloc(sizeof(listpackEntry)*count);
        if (withvalues)
            vals = zmalloc(sizeof(listpackEntry)*count);
        serverAssert(lpRandomPairsUnique(lp, count, keys, vals, tuple_len) == count);
        hrandfieldReplyWithListpack(c, count, keys, vals);
        zfree(keys);
        zfree(vals);
        return;
    }

    /* CASE 3:
     * The number of elements inside the hash of type dict is not greater than
     * HRANDFIELD_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create an array of dictEntry pointers from the original hash,
     * and subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requested elements is just
     * a bit less than the number of elements in the hash, the natural approach
     * used into CASE 4 is highly inefficient. */
    if (count*HRANDFIELD_SUB_STRATEGY_MUL > size) {
        /* Hashtable encoding (generic implementation) */
        dict *ht = hash->ptr;
        dictIterator di;
        dictEntry *de;
        unsigned long idx = 0;

        /* Allocate a temporary array of pointers to stored key-values in dict and
         * assist it to remove random elements to reach the right count. */
        struct FieldValPair {
            hfield field;
            sds value;
        } *pairs = zmalloc(sizeof(struct FieldValPair) * size);

        /* Add all the elements into the temporary array. */
        dictInitIterator(&di, ht);
        while((de = dictNext(&di)) != NULL)
              pairs[idx++] = (struct FieldValPair) {dictGetKey(de), dictGetVal(de)};
        dictResetIterator(&di);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            unsigned long toDiscardIdx = rand() % size;
            pairs[toDiscardIdx] = pairs[--size];
        }

        /* Reply with what's in the array */
        for (idx = 0; idx < size; idx++) {
            if (withvalues && c->resp > 2)
                addReplyArrayLen(c,2);
            addReplyBulkCBuffer(c, pairs[idx].field, hfieldlen(pairs[idx].field));
            if (withvalues)
                addReplyBulkCBuffer(c, pairs[idx].value, sdslen(pairs[idx].value));
        }

        zfree(pairs);
    }

    /* CASE 4: We have a big hash compared to the requested number of elements.
     * In this case we can simply get random elements from the hash and add
     * to the temporary hash, trying to eventually get enough unique elements
     * to reach the specified count. */
    else {
        /* Allocate temporary dictUnique to find unique elements. Just keep ref
         * to key-value from the original hash. This dict relaxes hash function
         * to be based on field's pointer */
        dictType uniqueDictType = { .hashFunction =  dictPtrHash };
        dict *dictUnique = dictCreate(&uniqueDictType);
        dictExpand(dictUnique, count);

        /* Hashtable encoding (generic implementation) */
        unsigned long added = 0;

        while(added < count) {
            dictEntry *de = dictGetFairRandomKey(hash->ptr);
            serverAssert(de != NULL);
            hfield field = dictGetKey(de);
            sds value = dictGetVal(de);

            /* Try to add the object to the dictionary. If it already exists
            * free it, otherwise increment the number of objects we have
            * in the result dictionary. */
            if (dictAdd(dictUnique, field, value) != DICT_OK)
                continue;

            added++;

            /* We can reply right away, so that we don't need to store the value in the dict. */
            if (withvalues && c->resp > 2)
                addReplyArrayLen(c,2);

            addReplyBulkCBuffer(c, field, hfieldlen(field));
            if (withvalues)
                addReplyBulkCBuffer(c, value, sdslen(value));
        }

        /* Release memory */
        dictRelease(dictUnique);
    }
}

/*
 * HRANDFIELD - Return a random field from the hash value stored at key.
 * CLI usage: HRANDFIELD key [<count> [WITHVALUES]]
 *
 * Considerations for the current imp of HRANDFIELD & HFE feature:
 *  HRANDFIELD might access any of the fields in the hash as some of them might
 *  be expired. And so the Implementation of HRANDFIELD along with HFEs
 *  might be one of the two options:
 *  1. Expire hash-fields before diving into handling HRANDFIELD.
 *  2. Refine HRANDFIELD cases to deal with expired fields.
 *
 *  Regarding the first option, as reference, the command RANDOMKEY also declares
 *  on O(1) complexity, yet might be stuck on a very long (but not infinite) loop
 *  trying to find non-expired keys. Furthermore RANDOMKEY also evicts expired keys
 *  along the way even though it is categorized as a read-only command. Note that
 *  the case of HRANDFIELD is more lightweight versus RANDOMKEY since HFEs have
 *  much more effective and aggressive active-expiration for fields behind.
 *
 *  The second option introduces additional implementation complexity to HRANDFIELD.
 *  We could further refine HRANDFIELD cases to differentiate between scenarios
 *  with many expired fields versus few expired fields, and adjust based on the
 *  percentage of expired fields. However, this approach could still lead to long
 *  loops or necessitate expiring fields before selecting them. For the “lightweight”
 *  cases it is also expected to have a lightweight expiration.
 *
 *  Considering the pros and cons, and the fact that HRANDFIELD is an infrequent
 *  command (particularly with HFEs) and the fact we have effective active-expiration
 *  behind for hash-fields, it is better to keep it simple and choose the option #1.
 */
void hrandfieldCommand(client *c) {
    long l;
    int withvalues = 0;
    kvobj *hash;
    CommonEntry ele;

    if (c->argc >= 3) {
        if (getRangeLongFromObjectOrReply(c,c->argv[2],-LONG_MAX,LONG_MAX,&l,NULL) != C_OK) return;
        if (c->argc > 4 || (c->argc == 4 && strcasecmp(c->argv[3]->ptr,"withvalues"))) {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        } else if (c->argc == 4) {
            withvalues = 1;
            if (l < -LONG_MAX/2 || l > LONG_MAX/2) {
                addReplyError(c,"value is out of range");
                return;
            }
        }
        hrandfieldWithCountCommand(c, l, withvalues);
        return;
    }

    /* Handle variant without <count> argument. Reply with simple bulk string */
    if ((hash = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]))== NULL ||
        checkType(c,hash,OBJ_HASH)) {
        return;
    }

    /* Delete all expired fields. If the entire hash got deleted then return null. */
    if (hashTypeExpireIfNeeded(c->db, hash)) {
        addReply(c,shared.null[c->resp]);
        return;
    }

    hashTypeRandomElement(hash,hashTypeLength(hash, 0),&ele,NULL);

    if (ele.sval)
        addReplyBulkCBuffer(c, ele.sval, ele.slen);
    else
        addReplyBulkLongLong(c, ele.lval);
}

/*-----------------------------------------------------------------------------
 * Hash Field with optional expiry (based on mstr)
 *----------------------------------------------------------------------------*/
static hfield _hfieldNew(const void *field, size_t fieldlen, int withExpireMeta,
                         int trymalloc)
{
    if (!withExpireMeta)
        return mstrNew(field, fieldlen, trymalloc);

    hfield hf = mstrNewWithMeta(&mstrFieldKind, field, fieldlen,
                                (mstrFlags) 1 << HFIELD_META_EXPIRE, trymalloc);

    if (!hf) return NULL;

    ExpireMeta *expireMeta = mstrMetaRef(hf, &mstrFieldKind, HFIELD_META_EXPIRE);

    /* as long as it is not inside ebuckets, it is considered trash */
    expireMeta->trash = 1;
    return hf;
}

/* if expireAt is 0, then expireAt is ignored and no metadata is attached */
hfield hfieldNew(const void *field, size_t fieldlen, int withExpireMeta) {
    return _hfieldNew(field, fieldlen, withExpireMeta, 0);
}

hfield hfieldTryNew(const void *field, size_t fieldlen, int withExpireMeta) {
    return _hfieldNew(field, fieldlen, withExpireMeta, 1);
}

int hfieldIsExpireAttached(hfield field) {
    return mstrIsMetaAttached(field) && mstrGetFlag(field, (int) HFIELD_META_EXPIRE);
}

static ExpireMeta* hfieldGetExpireMeta(const eItem field) {
    /* extract the expireMeta from the field of type mstr */
    return mstrMetaRef(field, &mstrFieldKind, (int) HFIELD_META_EXPIRE);
}

/* returned value is unix time in milliseconds */
uint64_t hfieldGetExpireTime(hfield field) {
    if (!hfieldIsExpireAttached(field))
        return EB_EXPIRE_TIME_INVALID;

    ExpireMeta *expireMeta = mstrMetaRef(field, &mstrFieldKind, (int) HFIELD_META_EXPIRE);
    if (expireMeta->trash)
        return EB_EXPIRE_TIME_INVALID;

    return ebGetMetaExpTime(expireMeta);
}

/* Remove TTL from the field. Assumed ExpireMeta is attached and has valid value */
static void hfieldPersist(robj *hashObj, hfield field) {
    uint64_t fieldExpireTime = hfieldGetExpireTime(field);
    if (fieldExpireTime == EB_EXPIRE_TIME_INVALID)
        return;

    /* if field is set with expire, then dict must has HFE metadata attached */
    dict *d = hashObj->ptr;
    dictExpireMetadata *dictExpireMeta = (dictExpireMetadata *)dictMetadata(d);

    /* If field has valid expiry then dict must have valid metadata as well */
    serverAssert(dictExpireMeta->expireMeta.trash == 0);

    /* Remove field from private HFE DS */
    ebRemove(&dictExpireMeta->hfe, &hashFieldExpireBucketsType, field);

    /* Don't have to update global HFE DS. It's unnecessary. Implementing this
     * would introduce significant complexity and overhead for an operation that
     * isn't critical. In the worst case scenario, the hash will be efficiently
     * updated later by an active-expire operation, or it will be removed by the
     * hash's dbGenericDelete() function. */
}

int hfieldIsExpired(hfield field) {
    /* Condition remains valid even if hfieldGetExpireTime() returns EB_EXPIRE_TIME_INVALID,
     * as the constant is equivalent to (EB_EXPIRE_TIME_MAX + 1). */
    return ( (mstime_t)hfieldGetExpireTime(field) < commandTimeSnapshot());
}

/*-----------------------------------------------------------------------------
 * Hash Field Expiration (HFE)
 *----------------------------------------------------------------------------*/
/*  Can be called either by active-expire cron job or query from the client */
static void propagateHashFieldDeletion(redisDb *db, sds key, char *field, size_t fieldLen) {
    robj *argv[] = {
        shared.hdel,
        createStringObject((char*) key, sdslen(key)),
        createStringObject(field, fieldLen)
    };

    enterExecutionUnit(1, 0);
    int prev_replication_allowed = server.replication_allowed;
    server.replication_allowed = 1;
    alsoPropagate(db->id,argv, 3, PROPAGATE_AOF|PROPAGATE_REPL);
    server.replication_allowed = prev_replication_allowed;
    exitExecutionUnit();

    /* Propagate the HDEL command */
    postExecutionUnitOperations();

    decrRefCount(argv[1]);
    decrRefCount(argv[2]);
}

/* Called during active expiration of hash-fields. Propagate to replica & Delete. */
static ExpireAction onFieldExpire(eItem item, void *ctx) {
    OnFieldExpireCtx *expCtx = ctx;
    hfield hf = item;
    kvobj *kv = expCtx->hashObj;
    sds key = kvobjGetKey(kv);
    propagateHashFieldDeletion(expCtx->db, key, hf, hfieldlen(hf));

    /* update keysizes */
    unsigned long l = hashTypeLength(expCtx->hashObj, 0);
    updateKeysizesHist(expCtx->db, getKeySlot(key), OBJ_HASH, l, l - 1);
    
    serverAssert(hashTypeDelete(expCtx->hashObj, hf, 0) == 1);
    server.stat_expired_subkeys++;
    return ACT_REMOVE_EXP_ITEM;
}

/* Retrieve the ExpireMeta associated with the hash.
 * The caller is responsible for ensuring that it is indeed attached. */
static ExpireMeta *hashGetExpireMeta(const eItem hash) {
    robj *hashObj = (robj *)hash;
    if (hashObj->encoding == OBJ_ENCODING_LISTPACK_EX) {
        listpackEx *lpt = hashObj->ptr;
        return &lpt->meta;
    } else if (hashObj->encoding == OBJ_ENCODING_HT) {
        dict *d = hashObj->ptr;
        dictExpireMetadata *dictExpireMeta = (dictExpireMetadata *) dictMetadata(d);
        return &dictExpireMeta->expireMeta;
    } else {
        serverPanic("Unknown encoding: %d", hashObj->encoding);
    }
}

/* HTTL key <FIELDS count field [field ...]>  */
static void httlGenericCommand(client *c, const char *cmd, long long basetime, int unit) {
    UNUSED(cmd);
    kvobj *hashObj;
    long numFields = 0, numFieldsAt = 3;

    /* Read the hash object */
    hashObj = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, hashObj, OBJ_HASH))
        return;

    if (strcasecmp(c->argv[numFieldsAt-1]->ptr, "FIELDS")) {
        addReplyError(c, "Mandatory argument FIELDS is missing or not at the right position");
        return;
    }

    /* Read number of fields */
    if (getRangeLongFromObjectOrReply(c, c->argv[numFieldsAt], 1, LONG_MAX,
                                      &numFields, "Number of fields must be a positive integer") != C_OK)
        return;

    /* Verify `numFields` is consistent with number of arguments */
    if (numFields != (c->argc - numFieldsAt - 1)) {
        addReplyError(c, "The `numfields` parameter must match the number of arguments");
        return;
    }

    /* Non-existing keys and empty hashes are the same thing. It also means
     * fields in the command don't exist in the hash key. */
    if (!hashObj) {
        addReplyArrayLen(c, numFields);
        for (int i = 0; i < numFields; i++) {
            addReplyLongLong(c, HFE_GET_NO_FIELD);
        }
        return;
    }

    if (hashObj->encoding == OBJ_ENCODING_LISTPACK) {
        void *lp = hashObj->ptr;

        addReplyArrayLen(c, numFields);
        for (int i = 0 ; i < numFields ; i++) {
            sds field = c->argv[numFieldsAt+1+i]->ptr;
            void *fptr = lpFirst(lp);
            if (fptr != NULL)
                fptr = lpFind(lp, fptr, (unsigned char *) field, sdslen(field), 1);

            if (!fptr)
                addReplyLongLong(c, HFE_GET_NO_FIELD);
            else
                addReplyLongLong(c, HFE_GET_NO_TTL);
        }
        return;
    } else if (hashObj->encoding == OBJ_ENCODING_LISTPACK_EX) {
        listpackEx *lpt = hashObj->ptr;

        addReplyArrayLen(c, numFields);
        for (int i = 0 ; i < numFields ; i++) {
            long long expire;
            sds field = c->argv[numFieldsAt+1+i]->ptr;
            void *fptr = lpFirst(lpt->lp);
            if (fptr != NULL)
                fptr = lpFind(lpt->lp, fptr, (unsigned char *) field, sdslen(field), 2);

            if (!fptr) {
                addReplyLongLong(c, HFE_GET_NO_FIELD);
                continue;
            }

            fptr = lpNext(lpt->lp, fptr);
            serverAssert(fptr);
            fptr = lpNext(lpt->lp, fptr);
            serverAssert(fptr && lpGetIntegerValue(fptr, &expire));

            if (expire == HASH_LP_NO_TTL) {
                addReplyLongLong(c, HFE_GET_NO_TTL);
                continue;
            }

            if (expire <= commandTimeSnapshot()) {
                addReplyLongLong(c, HFE_GET_NO_FIELD);
                continue;
            }

            if (unit == UNIT_SECONDS)
                addReplyLongLong(c, (expire + 999 - basetime) / 1000);
            else
                addReplyLongLong(c, (expire - basetime));
        }
        return;
    } else if (hashObj->encoding == OBJ_ENCODING_HT) {
        dict *d = hashObj->ptr;

        addReplyArrayLen(c, numFields);
        for (int i = 0 ; i < numFields ; i++) {
            sds field = c->argv[numFieldsAt+1+i]->ptr;
            dictEntry *de = dictFind(d, field);
            if (de == NULL) {
                addReplyLongLong(c, HFE_GET_NO_FIELD);
                continue;
            }

            hfield hf = dictGetKey(de);
            uint64_t expire = hfieldGetExpireTime(hf);
            if (expire == EB_EXPIRE_TIME_INVALID) {
                addReplyLongLong(c, HFE_GET_NO_TTL); /* no ttl */
                continue;
            }

            if ( (long long) expire < commandTimeSnapshot()) {
                addReplyLongLong(c, HFE_GET_NO_FIELD);
                continue;
            }

            if (unit == UNIT_SECONDS)
                addReplyLongLong(c, (expire + 999 - basetime) / 1000);
            else
                addReplyLongLong(c, (expire - basetime));
        }
        return;
    } else {
        serverPanic("Unknown encoding: %d", hashObj->encoding);
    }
}

/* This is the generic command implementation for HEXPIRE, HPEXPIRE, HEXPIREAT
 * and HPEXPIREAT. Because the command second argument may be relative or absolute
 * the "basetime" argument is used to signal what the base time is (either 0
 * for *AT variants of the command, or the current time for relative expires).
 *
 * unit is either UNIT_SECONDS or UNIT_MILLISECONDS, and is only used for
 * the argv[2] parameter. The basetime is always specified in milliseconds.
 *
 * PROPAGATE TO REPLICA:
 *   The command will be translated into HPEXPIREAT and the expiration time will be
 *   converted to absolute time in milliseconds.
 *
 *   As we need to propagate H(P)EXPIRE(AT) command to the replica, each field that
 *   is mentioned in the command should be categorized into one of the four options:
 *   1. Field’s expiration time updated successfully - Propagate it to replica as
 *      part of the HPEXPIREAT command.
 *   2. The field got deleted since the time is in the past - propagate also HDEL
 *      command to delete the field. Also remove the field from the propagated
 *      HPEXPIREAT command.
 *   3. Condition not met for the field - Remove the field from the propagated
 *      HPEXPIREAT command.
 *   4. Field doesn't exists - Remove the field from propagated HPEXPIREAT command.
 *
 *   If none of the provided fields match option #1, that is provided time of the
 *   command is in the past, then avoid propagating the HPEXPIREAT command to the
 *   replica.
 *
 *   This approach is aligned with existing EXPIRE command. If a given key has already
 *   expired, then DEL will be propagated instead of EXPIRE command. If condition
 *   not met, then command will be rejected. Otherwise, EXPIRE command will be
 *   propagated for given key.
 */
static void hexpireGenericCommand(client *c, long long basetime, int unit) {
    long numFields = 0, numFieldsAt = 4;
    long long expire; /* unix time in msec */
    int fieldAt, fieldsNotSet = 0, expireSetCond = 0, updated = 0, deleted = 0;
    int64_t oldlen, newlen;
    robj *keyArg = c->argv[1], *expireArg = c->argv[2];

    /* Read the hash object */
    kvobj *hashObj = lookupKeyWrite(c->db, keyArg);
    if (checkType(c, hashObj, OBJ_HASH))
        return;

    /* Read the expiry time from command */
    if (parseExpireTime(c, expireArg, unit, basetime, &expire) != C_OK)
        return;

    /* Read optional expireSetCond [NX|XX|GT|LT] */
    char *optArg = c->argv[3]->ptr;
    if (!strcasecmp(optArg, "nx")) {
        expireSetCond = HFE_NX; ++numFieldsAt;
    } else if (!strcasecmp(optArg, "xx")) {
        expireSetCond = HFE_XX; ++numFieldsAt;
    } else if (!strcasecmp(optArg, "gt")) {
        expireSetCond = HFE_GT; ++numFieldsAt;
    } else if (!strcasecmp(optArg, "lt")) {
        expireSetCond = HFE_LT; ++numFieldsAt;
    }

    if (strcasecmp(c->argv[numFieldsAt-1]->ptr, "FIELDS")) {
        addReplyError(c, "Mandatory argument FIELDS is missing or not at the right position");
        return;
    }

    /* Read number of fields */
    if (getRangeLongFromObjectOrReply(c, c->argv[numFieldsAt], 1, LONG_MAX,
                                      &numFields, "Parameter `numFields` should be greater than 0") != C_OK)
        return;

    /* Verify `numFields` is consistent with number of arguments */
    if (numFields != (c->argc - numFieldsAt - 1)) {
        addReplyError(c, "The `numfields` parameter must match the number of arguments");
        return;
    }

    /* Non-existing keys and empty hashes are the same thing. It also means
     * fields in the command don't exist in the hash key. */
    if (!hashObj) {
        addReplyArrayLen(c, numFields);
        for (int i = 0; i < numFields; i++) {
            addReplyLongLong(c, HSETEX_NO_FIELD);
        }
        return;
    }

    oldlen = hashTypeLength(hashObj, 0);

    HashTypeSetEx exCtx;
    hashTypeSetExInit(keyArg, hashObj, c, c->db, expireSetCond, &exCtx);
    addReplyArrayLen(c, numFields);

    fieldAt = numFieldsAt + 1;
    while (fieldAt < c->argc) {
        sds field = c->argv[fieldAt]->ptr;
        SetExRes res = hashTypeSetEx(hashObj, field, expire, &exCtx);
        updated += (res == HSETEX_OK);
        deleted += (res == HSETEX_DELETED);

        if (unlikely(res != HSETEX_OK)) {
            /* If the field was not set, prevent field propagation */
            rewriteClientCommandArgument(c, fieldAt, NULL);
            fieldsNotSet = 1;
        } else {
            ++fieldAt;
        }

        addReplyLongLong(c,res);
    }

    hashTypeSetExDone(&exCtx);

    if (deleted + updated > 0) {
        server.dirty += deleted + updated;
        signalModifiedKey(c, c->db, keyArg);
        notifyKeyspaceEvent(NOTIFY_HASH, deleted ? "hdel" : "hexpire",
                            keyArg, c->db->id);
    }

    newlen = (int64_t) hashTypeLength(hashObj, 0);
    if (newlen == 0) {
        newlen = -1;
        /* Del key but don't update KEYSIZES. Else it will decr wrong bin in histogram */
        dbDeleteSkipKeysizesUpdate(c->db, keyArg);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", keyArg, c->db->id);
    }

    if (oldlen != newlen)
        updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_HASH,
                           oldlen, newlen);

    /* Avoid propagating command if not even one field was updated (Either because
     * the time is in the past, and corresponding HDELs were sent, or conditions
     * not met) then it is useless and invalid to propagate command with no fields */
    if (updated == 0) {
        preventCommandPropagation(c);
        return;
    }

    /* If some fields were dropped, rewrite the number of fields */
    if (fieldsNotSet) {
        robj *numFieldsObj = createStringObjectFromLongLong(updated);
        rewriteClientCommandArgument(c, numFieldsAt, numFieldsObj);
        decrRefCount(numFieldsObj);
    }

    /* Propagate as HPEXPIREAT millisecond-timestamp. Rewrite only if not already */
    if (c->cmd->proc != hpexpireatCommand) {
        rewriteClientCommandArgument(c,0,shared.hpexpireat);
    }

    /* rewrite expiration time to unix time in msec  */
    if (basetime != 0 || unit == UNIT_SECONDS) {
        robj *expireObj = createStringObjectFromLongLong(expire);
        rewriteClientCommandArgument(c, 2, expireObj);
        decrRefCount(expireObj);
    }
}

/* HPEXPIRE key milliseconds [ NX | XX | GT | LT] FIELDS numfields <field [field ...]> */
void hpexpireCommand(client *c) {
    hexpireGenericCommand(c,commandTimeSnapshot(),UNIT_MILLISECONDS);
}

/* HEXPIRE key seconds [NX | XX | GT | LT] FIELDS numfields <field [field ...]> */
void hexpireCommand(client *c) {
    hexpireGenericCommand(c,commandTimeSnapshot(),UNIT_SECONDS);
}

/* HEXPIREAT key unix-time-seconds [NX | XX | GT | LT] FIELDS numfields <field [field ...]> */
void hexpireatCommand(client *c) {
    hexpireGenericCommand(c,0,UNIT_SECONDS);
}

/* HPEXPIREAT key unix-time-milliseconds [NX | XX | GT | LT] FIELDS numfields <field [field ...]> */
void hpexpireatCommand(client *c) {
    hexpireGenericCommand(c,0,UNIT_MILLISECONDS);
}

/* for each specified field: get the remaining time to live in seconds*/
/* HTTL key FIELDS numfields <field [field ...]> */
void httlCommand(client *c) {
    httlGenericCommand(c, "httl", commandTimeSnapshot(), UNIT_SECONDS);
}

/* HPTTL key FIELDS numfields <field [field ...]> */
void hpttlCommand(client *c) {
    httlGenericCommand(c, "hpttl", commandTimeSnapshot(), UNIT_MILLISECONDS);
}

/* HEXPIRETIME key FIELDS numfields <field [field ...]> */
void hexpiretimeCommand(client *c) {
    httlGenericCommand(c, "hexpiretime", 0, UNIT_SECONDS);
}

/* HPEXPIRETIME key FIELDS numfields <field [field ...]> */
void hpexpiretimeCommand(client *c) {
    httlGenericCommand(c, "hexpiretime", 0, UNIT_MILLISECONDS);
}

/* HPERSIST key FIELDS numfields <field [field ...]> */
void hpersistCommand(client *c) {
    long numFields = 0, numFieldsAt = 3;
    int changed = 0; /* Used to determine whether to send a notification. */

    /* Read the hash object */
    kvobj *hashObj = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, hashObj, OBJ_HASH))
        return;

    if (strcasecmp(c->argv[numFieldsAt-1]->ptr, "FIELDS")) {
        addReplyError(c, "Mandatory argument FIELDS is missing or not at the right position");
        return;
    }

    /* Read number of fields */
    if (getRangeLongFromObjectOrReply(c, c->argv[numFieldsAt], 1, LONG_MAX,
                                      &numFields, "Number of fields must be a positive integer") != C_OK)
        return;

    /* Verify `numFields` is consistent with number of arguments */
    if (numFields != (c->argc - numFieldsAt - 1)) {
        addReplyError(c, "The `numfields` parameter must match the number of arguments");
        return;
    }

    /* Non-existing keys and empty hashes are the same thing. It also means
     * fields in the command don't exist in the hash key. */
    if (!hashObj) {
        addReplyArrayLen(c, numFields);
        for (int i = 0; i < numFields; i++) {
            addReplyLongLong(c, HFE_PERSIST_NO_FIELD);
        }
        return;
    }

    if (hashObj->encoding == OBJ_ENCODING_LISTPACK) {
        addReplyArrayLen(c, numFields);
        for (int i = 0 ; i < numFields ; i++) {
            sds field = c->argv[numFieldsAt + 1 + i]->ptr;
            unsigned char *fptr, *zl = hashObj->ptr;

            fptr = lpFirst(zl);
            if (fptr != NULL)
                fptr = lpFind(zl, fptr, (unsigned char *) field, sdslen(field), 1);

            if (!fptr)
                addReplyLongLong(c, HFE_PERSIST_NO_FIELD);
            else
                addReplyLongLong(c, HFE_PERSIST_NO_TTL);
        }
        return;
    } else if (hashObj->encoding == OBJ_ENCODING_LISTPACK_EX) {
        long long prevExpire;
        unsigned char *fptr, *vptr, *tptr;
        listpackEx *lpt = hashObj->ptr;

        addReplyArrayLen(c, numFields);
        for (int i = 0 ; i < numFields ; i++) {
            sds field = c->argv[numFieldsAt + 1 + i]->ptr;

            fptr = lpFirst(lpt->lp);
            if (fptr != NULL)
                fptr = lpFind(lpt->lp, fptr, (unsigned char*)field, sdslen(field), 2);

            if (!fptr) {
                addReplyLongLong(c, HFE_PERSIST_NO_FIELD);
                continue;
            }

            vptr = lpNext(lpt->lp, fptr);
            serverAssert(vptr);
            tptr = lpNext(lpt->lp, vptr);
            serverAssert(tptr && lpGetIntegerValue(tptr, &prevExpire));

            if (prevExpire == HASH_LP_NO_TTL) {
                addReplyLongLong(c, HFE_PERSIST_NO_TTL);
                continue;
            }

            if (prevExpire < commandTimeSnapshot()) {
                addReplyLongLong(c, HFE_PERSIST_NO_FIELD);
                continue;
            }

            listpackExUpdateExpiry(hashObj, field, fptr, vptr, HASH_LP_NO_TTL);
            addReplyLongLong(c, HFE_PERSIST_OK);
            changed = 1;
        }
    } else if (hashObj->encoding == OBJ_ENCODING_HT) {
        dict *d = hashObj->ptr;

        addReplyArrayLen(c, numFields);
        for (int i = 0 ; i < numFields ; i++) {
            sds field = c->argv[numFieldsAt + 1 + i]->ptr;
            dictEntry *de = dictFind(d, field);
            if (de == NULL) {
                addReplyLongLong(c, HFE_PERSIST_NO_FIELD);
                continue;
            }

            hfield hf = dictGetKey(de);
            uint64_t expire = hfieldGetExpireTime(hf);
            if (expire == EB_EXPIRE_TIME_INVALID) {
                addReplyLongLong(c, HFE_PERSIST_NO_TTL);
                continue;
            }

            /* Already expired. Pretend there is no such field */
            if ( (long long) expire < commandTimeSnapshot()) {
                addReplyLongLong(c, HFE_PERSIST_NO_FIELD);
                continue;
            }

            hfieldPersist(hashObj, hf);
            addReplyLongLong(c, HFE_PERSIST_OK);
            changed = 1;
        }
    } else {
        serverPanic("Unknown encoding: %d", hashObj->encoding);
    }

    /* Generates a hpersist event if the expiry time associated with any field
     * has been successfully deleted. */
    if (changed) {
        notifyKeyspaceEvent(NOTIFY_HASH, "hpersist", c->argv[1], c->db->id);
        signalModifiedKey(c, c->db, c->argv[1]);
        server.dirty++;
    }
}
