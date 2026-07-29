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
 */

#include "server.h"
#include "bio.h"
#include "redisassert.h"
#include "ebuckets.h"
#include "entry.h"
#include "vector.h"
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

typedef listpackEntry CommonEntry; /* extend usage beyond lp */

#define FIELDS_STACK_SIZE 16

/* A vec with an embedded stack buffer, used to collect field robj pointers
 * for subkey notifications without heap allocation in the common case. */
typedef struct fieldvec { vec v; void *buf[FIELDS_STACK_SIZE]; } fieldvec;

static inline vec *fieldvecInit(fieldvec *fv, size_t cap) {
    vecInit(&fv->v, fv->buf, FIELDS_STACK_SIZE);
    vecReserve(&fv->v, cap);
    return &fv->v;
}

/* hash field expiration (HFE) funcs */
static ExpireAction onFieldExpire(eItem item, void *ctx);
static ExpireMeta* hentryGetExpireMeta(const eItem field);
static void hexpireGenericCommand(client *c, long long basetime, int unit);
static void hfieldPersist(robj *hashObj, Entry *entry);
static void propagateHashFieldDeletion(redisDb *db, sds key, char *field, size_t fieldLen);

/* hash dictType funcs */
static void dictEntryDestructor(dict *d, void *entry);
static size_t hashDictMetadataBytes(dict *d);
static size_t hashDictWithExpireMetadataBytes(dict *d);
static void hashDictWithExpireOnRelease(dict *d);
static kvobj* hashTypeLookupWriteOrCreate(client *c, robj *key);
static int hashTypeCanConvertTmplToListpack(robj *o);
static void hashTypeConvertTmplToListpackOrHT(robj *o, int lp_enc, int with_hfe);

/*-----------------------------------------------------------------------------
 * Define dictType of hash
 *
 * - Stores fields as entry objects (field-value pairs) with optional expiration
 * - Note that small hashes are represented with listpacks
 * - Once expiration is set for a field, the dict instance and corresponding
 *   dictType are replaced with a dict containing metadata for Hash Field
 *   Expiration (HFE) and using dictType `entryHashDictTypeWithHFE`
 * - Dict uses no_value=1 since entry contains both field and value
 *----------------------------------------------------------------------------*/
dictType entryHashDictType = {
    dictSdsHash,                                /* lookup hash function */
    NULL,                                       /* key dup */
    NULL,                                       /* val dup */
    dictSdsKeyCompare,                          /* lookup key compare */
    dictEntryDestructor,                       /* key destructor */
    NULL,                                       /* val destructor (value is in entry) */
    .dictMetadataBytes = hashDictMetadataBytes,
    .no_value = 1,                              /* entry contains both field and value */
    .keys_are_odd = 1,                          /* entry pointers (SDS) are always odd */
};

/* Define alternative dictType of hash with hash-field expiration (HFE) support */
dictType entryHashDictTypeWithHFE = {
    dictSdsHash,                                /* lookup hash function */
    NULL,                                       /* key dup */
    NULL,                                       /* val dup */
    dictSdsKeyCompare,                          /* lookup key compare */
    dictEntryDestructor,                       /* key destructor */
    NULL,                                       /* val destructor (value is in entry) */
    .dictMetadataBytes = hashDictWithExpireMetadataBytes,
    .onDictRelease = hashDictWithExpireOnRelease,
    .no_value = 1,                              /* entry contains both field and value */
    .keys_are_odd = 1,                          /* entry pointers (SDS) are always odd */
};

/*-----------------------------------------------------------------------------
 * Hash Field Expiration (HFE) Feature
 *
 * Each hash instance maintains its own set of hash field expiration within its
 * private ebuckets DS. In order to support HFE active expire cycle across hash
 * instances, hashes with associated HFE will be also registered in a global
 * ebuckets DS with expiration time value that reflects their next minimum
 * time to expire (db->subexpires). The global HFE Active expiration will be
 * triggered from activeExpireCycle() function and in turn will invoke "local"
 * HFE Active sub-expiration for each hash instance that has expired fields.
 *----------------------------------------------------------------------------*/
EbucketsType subexpiresBucketsType = {
    .onDeleteItem = NULL,
    .getExpireMeta = hashGetExpireMeta,   /* get ExpireMeta attached to each hash */
    .itemsAddrAreOdd = 0,                 /* Addresses of dict are even */
};

/* htExpireMetadata - ebuckets-type for hash fields with time-Expiration. ebuckets
 * instance Will be attached to each hash that has at least one field with expiry
 * time. */
EbucketsType hashFieldExpireBucketsType = {
    .onDeleteItem = NULL,
    .getExpireMeta = hentryGetExpireMeta, /* get ExpireMeta attached to each field */
    .itemsAddrAreOdd = 1,                 /* Addresses of hfield (entry/sds) are odd!! */
};

/* OnFieldExpireCtx passed to OnFieldExpire() */
typedef struct OnFieldExpireCtx {
    robj *hashObj;
    redisDb *db;
    int activeEx; /* 1 for active expire, 0 for lazy expire */
    vec *vexpired; /* Expired fields vector */
} OnFieldExpireCtx;

/* The implementation of hashes by dict was modified from storing fields as sds
 * strings to store "entry" objects (field-value pairs with optional expiration).
 * The entry structure unifies field and value into a single allocation, with
 * optional expiration metadata. This is simpler than the previous mstr approach
 * and provides better memory locality.
 */

/* Used by hpersistCommand() */
typedef enum SetPersistRes {
    HFE_PERSIST_NO_FIELD =     -2,   /* No such hash-field */
    HFE_PERSIST_NO_TTL =       -1,   /* No TTL attached to the field */
    HFE_PERSIST_OK =            1
} SetPersistRes;

static inline int isDictWithMetaHFE(dict *d) {
    return d->type == &entryHashDictTypeWithHFE;
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

static void dictEntryDestructor(dict *d, void *entry) {
    size_t usable;
    size_t *alloc_size = htGetMetadataSize(d);

    /* If attached TTL to the field, then remove it from hash's private ebuckets. */
    if (entryGetExpiry(entry) != EB_EXPIRE_TIME_INVALID) {
        htMetadataEx *dictExpireMeta = htGetMetadataEx(d);
        ebRemove(&dictExpireMeta->hfe, &hashFieldExpireBucketsType, entry);
    }

    entryFree(entry, &usable);
    *alloc_size -= usable;

    /* Don't have to update global HFE DS. It's unnecessary. Implementing this
     * would introduce significant complexity and overhead for an operation that
     * isn't critical. In the worst case scenario, the hash will be efficiently
     * updated later by an active-expire operation, or it will be removed by the
     * hash's dbGenericDelete() function. */
}

static size_t hashDictMetadataBytes(dict *d) {
    UNUSED(d);
    return sizeof(size_t);
}

static size_t hashDictWithExpireMetadataBytes(dict *d) {
    UNUSED(d);
    /* expireMeta of the hash, ref to ebuckets and pointer to hash's key */
    return sizeof(htMetadataEx);
}

static void hashDictWithExpireOnRelease(dict *d) {
    /* for sure allocated with metadata. Otherwise, this func won't be registered */
    htMetadataEx *dictExpireMeta = htGetMetadataEx(d);
    ebDestroy(&dictExpireMeta->hfe, &hashFieldExpireBucketsType, NULL);
}

/*-----------------------------------------------------------------------------
 * Hash template registry internals
 *
 * Many hashes often share the exact same set of field names (e.g. rows of one
 * "schema"). Instead of repeating the field names in every key, a single
 * immutable template stores the names once and every matching hash references
 * it, keeping only its own values. This is a memory optimization aimed at large
 * populations of hashes with a stable, identical field set.
 *
 * A template is identified by its exact set of field names and is never
 * changed once created. So adding a field (HSET) or removing one (HDEL) does
 * not edit the template: the hash just switches to the template for the new
 * field set, reusing it if it already exists or creating it otherwise (see
 * hashTemplateGetOrCreateWithHash). This makes changing the field set more
 * costly than on a plain hash, so the feature is meant for many hashes that
 * keep the same fields mostly stable.
 *
 * Fields are kept sorted inside the template by sdscmplen (length, then bytes;
 * see hashTemplateValidateFields). A field lookup binary-searches the sorted
 * names for its index (hashTemplateFieldIndex), then reads the value stored at
 * that same index.
 *
 * Two value encodings reference a template:
 *   OBJ_ENCODING_TMPL_LP    - o->ptr is a listpack laid out as
 *                             [template_id (varint)][v0][v1]...[vN-1]. Values
 *                             are packed inline and the leading id maps back to
 *                             the template. Compact, used for small hashes.
 *                             Lookup: lpSeek(lp, idx + 1) (the +1 skips the id).
 *   OBJ_ENCODING_TMPL_ARRAY - o->ptr is a hashTemplateArray { tmpl_id; sds
 *                             values[] } holding the values in template field
 *                             order. Used once listpack limits are exceeded (or
 *                             there are many fields). Lookup: values[idx].
 *
 * Registry: htemplates->by_fields maps a sorted field set to its template, and
 * htemplates->by_id maps a small integer id to the template. TMPL_LP stores
 * this id inline rather than an 8-byte pointer, so a key's reference to its
 * template can be as little as ~2 bytes. RDB save uses it the same way: each
 * template is written once and every key just references it by this id.
 *
 * Lifetime and threading: each referencing key holds a key_refcount to its
 * template and a client can also hold a hold_refcount (e.g. a fieldset
 * registered with HIMPORT PREPARE). A single template may back a huge number of
 * keys and those keys are often freed in bulk in the background by a BIO
 * lazyfree thread (e.g. background trim after ASM). The registry is main thread
 * only, so a BIO thread must not touch it: instead the BIO lazyfree thread records
 * per-template key drop counts and publishes them to the main thread, which 
 * applies the drops and frees any template reaching zero refs in cron (see bio_pending_drops).
 *----------------------------------------------------------------------------*/

#define HASH_TMPL_STACK_ENTRIES 128

/* Global template registry; file-local alias of server.htemplates. */
static hashTemplateRegistry *htemplates = NULL;

/* Forward declarations for template helpers. */
static hashTemplate *hashTemplateGetOrCreateWithHash(uint64_t hash, sds *fields, unsigned long long field_count);
static void hashTemplateDecrKeyRef(hashTemplate *tmpl);
static hashTemplate *hashTemplateLpGetTemplate(unsigned char *lp);
static uint64_t hashTemplateLpGetTemplateId(unsigned char *lp);
static unsigned char *hashTemplateLpCreate(hashTemplate *tmpl, sds *values);
static hashTemplateArray *hashTemplateArrayCreate(hashTemplate *tmpl, sds *values, int take);
static hashTemplate *hashTemplateArrayGetTemplate(hashTemplateArray *hta);

/* Keys freed in BIO lazyfree thread sets a dict (template id->key_ref_count). 
 * Later, it is applied on main thread in cron, key refs are actually dropped. */
static redisAtomic uintptr_t bio_pending_drops = 0;

static uint64_t pendingDropHash(const void *key) { return (uint64_t)(uintptr_t)key; }
static dictType pendingDropDictType = { pendingDropHash, NULL, NULL, NULL, NULL, NULL, NULL };

/* BIO thread: Increment deleted key count for the template id. */
static void hashTemplateRecordPendingDrop(uint64_t id) {
    serverAssert(bioIsLazyfreeWorker());

    uintptr_t cur;
    atomicExchangeAcquire(bio_pending_drops, 0, cur);

    dict *d = cur ? (dict *)cur : dictCreate(&pendingDropDictType);
    dictEntry *e;
    dictEntry *de = dictAddRaw(d, (void *)(uintptr_t)id, &e);
    
    /* Increment deleted key count for the template id */
    if (de) 
        dictSetUnsignedIntegerVal(de, 1);
    else    
        dictSetUnsignedIntegerVal(e, dictGetUnsignedIntegerVal(e) + 1);
    atomicSetRelease(bio_pending_drops, (uintptr_t)d);
}

/* Sum of the 'n' sds lengths, or -1 if any exceeds hash-max-listpack-value. */
static ssize_t hashTypeSdsArrayLpBytes(sds *arr, unsigned long long n) {
    size_t sum = 0;
    for (unsigned long long i = 0; i < n; i++) {
        size_t len = sdslen(arr[i]);
        if (len > server.hash_max_listpack_value) return -1;
        sum += len;
    }
    return (ssize_t)sum;
}

/* Do 'n' sds strings fit a listpack? */
static int hashTypeSdsArrayFitsLp(sds *arr, unsigned long long n) {
    if (n > server.hash_max_listpack_entries) return 0;
    ssize_t sum = hashTypeSdsArrayLpBytes(arr, n);
    return sum >= 0 && lpSafeToAdd(NULL, sum);
}

/* by_id is a chunked array: fixed-size chunks of template pointers. */
#define TMPL_CHUNK_SIZE 128  /* template ids per chunk */

typedef struct tmplIdChunk {
    hashTemplate *slots[TMPL_CHUNK_SIZE]; /* id % TMPL_CHUNK -> template, NULL if free */
    unsigned int used;               /* used slots; chunk freed when 0 */
} tmplIdChunk;

/* Get chunk holding 'id', allocating it on demand. */
static tmplIdChunk *tmplIdGetOrCreateChunk(size_t id) {
    size_t chunk_idx = id / TMPL_CHUNK_SIZE;
    if (chunk_idx >= htemplates->by_id_cap) {
        size_t old = htemplates->by_id_cap;
        size_t ncap = old ? old * 2 : 8;
        while (ncap <= chunk_idx) ncap *= 2;

        htemplates->by_id = zrealloc(htemplates->by_id, ncap * sizeof(tmplIdChunk *));
        memset(htemplates->by_id + old, 0, (ncap - old) * sizeof(tmplIdChunk *));
        htemplates->by_id_cap = ncap;
    }
    if (htemplates->by_id[chunk_idx] == NULL) {
        htemplates->by_id[chunk_idx] = zcalloc(sizeof(tmplIdChunk));
        htemplates->by_id_chunks++;
    }
    return htemplates->by_id[chunk_idx];
}

/* Get lowest free id. Caller guarantees a gap exists. */
static size_t tmplIdGetLowestFree(void) {
    size_t chunk_idx = 0;
    while (chunk_idx < htemplates->by_id_cap && htemplates->by_id[chunk_idx] &&
           htemplates->by_id[chunk_idx]->used == TMPL_CHUNK_SIZE) {
        chunk_idx++;
    }
    tmplIdChunk *chunk = chunk_idx < htemplates->by_id_cap ? htemplates->by_id[chunk_idx] : NULL;
    size_t id = chunk_idx * TMPL_CHUNK_SIZE;
    while (chunk && chunk->slots[id % TMPL_CHUNK_SIZE] != NULL) id++;
    return id;
}

/* Store tmpl in by_id under the lowest free id and return that id (the caller
 * sets tmpl->id). Low ids keep the TMPL_LP listpack id ~2 bytes per key. */
static uint64_t tmplIdAllocate(hashTemplate *tmpl) {
    int no_gaps = dictSize(htemplates->by_fields) == htemplates->by_id_next;
    size_t id = no_gaps ? htemplates->by_id_next++ : tmplIdGetLowestFree();
    tmplIdChunk *chunk = tmplIdGetOrCreateChunk(id);
    chunk->slots[id % TMPL_CHUNK_SIZE] = tmpl;
    chunk->used++;
    return id;
}

/* Recycle a template ID when template is freed. */
static void tmplIdRecycle(uint64_t id) {
    size_t chunk_idx = id / TMPL_CHUNK_SIZE;
    tmplIdChunk *chunk = htemplates->by_id[chunk_idx];
    chunk->slots[id % TMPL_CHUNK_SIZE] = NULL;
    /* Free the chunk once it holds no live ids so the id space shrinks. */
    if (--chunk->used == 0) {
        zfree(chunk);
        htemplates->by_id[chunk_idx] = NULL;
        htemplates->by_id_chunks--;
    }

    /* This runs from the registry's key destructor, dictSize()==1 means it is
     * the last one and the whole by_id array can be released. */
    if (dictSize(htemplates->by_fields) == 1) {
        for (size_t i = 0; i < htemplates->by_id_cap; i++)
            zfree(htemplates->by_id[i]);
        zfree(htemplates->by_id);
        htemplates->by_id = NULL;
        htemplates->by_id_cap = 0;
        htemplates->by_id_chunks = 0;
        htemplates->by_id_next = 0;
    }
}

/* Lookup template by ID. Returns NULL if invalid. */
hashTemplate *hashTemplateGetById(uint64_t id) {
    size_t chunk_idx = id / TMPL_CHUNK_SIZE;
    if (chunk_idx >= htemplates->by_id_cap) return NULL;
    tmplIdChunk *chunk = htemplates->by_id[chunk_idx];
    return chunk ? chunk->slots[id % TMPL_CHUNK_SIZE] : NULL;
}

/* Defrag the template struct and re-point every reference
 * to it (by_id slot, by_fields key, by_fields_lp value).*/
hashTemplate *hashTemplateDefrag(hashTemplate *tmpl) {
    /* Field-name array and the strings it holds. */
    sds *newfields = activeDefragAlloc(tmpl->fields);
    if (newfields) tmpl->fields = newfields;
    for (unsigned long long i = 0; i < tmpl->field_count; i++) {
        sds newsds = activeDefragSds(tmpl->fields[i]);
        if (newsds) tmpl->fields[i] = newsds;
    }

    /* Find the entries referencing tmpl (by_fields key) and its blob
     * (by_fields_lp key+value) before any realloc frees the old pointers. */
    uint64_t bf_hash = dictGetHash(htemplates->by_fields, tmpl);
    dictEntry *bf = dictFindByHashAndPtr(htemplates->by_fields, tmpl, bf_hash);
    dictEntry *lp = tmpl->fields_lp ? dictFind(htemplates->by_fields_lp, tmpl->fields_lp) : NULL;

    /* fields_lp blob (the by_fields_lp key). */
    if (tmpl->fields_lp) {
        unsigned char *newlp = activeDefragAlloc(tmpl->fields_lp);
        if (newlp) {
            tmpl->fields_lp = newlp;
            if (lp) dictSetKey(htemplates->by_fields_lp, lp, newlp);
        }
    }

    /* The struct itself: by_id slot, by_fields key, by_fields_lp value. */
    uint64_t id = tmpl->id;
    hashTemplate *newtmpl = activeDefragAlloc(tmpl);
    if (!newtmpl) return tmpl;

    tmplIdChunk *chunk = htemplates->by_id[id / TMPL_CHUNK_SIZE];
    chunk->slots[id % TMPL_CHUNK_SIZE] = newtmpl;

    if (bf) dictSetKey(htemplates->by_fields, bf, newtmpl);
    if (lp) dictSetVal(htemplates->by_fields_lp, lp, newtmpl);
    return newtmpl;
}

/* Defrag by_id top array (once, at idx 0) and the chunk at 'idx'. */
int hashTemplateDefragByIdChunk(unsigned long chunk_idx) {
    if (htemplates->by_id == NULL || chunk_idx >= htemplates->by_id_cap) 
        return 0;
    if (chunk_idx == 0) {
        tmplIdChunk **newarr = activeDefragAlloc(htemplates->by_id);
        if (newarr) htemplates->by_id = newarr;
    }
    tmplIdChunk *chunk = htemplates->by_id[chunk_idx];
    if (chunk) {
        tmplIdChunk *newchunk = activeDefragAlloc(chunk);
        if (newchunk) htemplates->by_id[chunk_idx] = newchunk;
    }
    return 1;
}

/* HIMPORT SET propagation usually sends RESTORE commands with the same few
 * field sets. ASM may send RESTORE commands with thousands of different field
 * sets because keys arrive in arbitrary order. The cache holds at most one blob
 * per template, so its size is naturally bounded by the template count. The
 * explicit cap only protects against misuse that creates an excessive number
 * of templates. */
#define HASH_TMPL_FIELDS_LP_CACHE_MAX_BYTES (16 * 1024 * 1024)

/* Cache 'fields_lp' as tmpl's fields-listpack (blob -> template), taking
 * ownership on success. Returns 0 without consuming fields_lp if tmpl is already
 * indexed or the cache is full. */
int hashTemplateIndexFieldsLp(hashTemplate *tmpl, unsigned char *fields_lp) {
    if (tmpl->fields_lp) return 0;

    size_t cached_bytes = htemplates->fields_lp_cache_bytes;
    serverAssert(cached_bytes <= HASH_TMPL_FIELDS_LP_CACHE_MAX_BYTES);

    size_t bytes = lpBytes(fields_lp);
    if (bytes > HASH_TMPL_FIELDS_LP_CACHE_MAX_BYTES - cached_bytes)
        return 0;

    tmpl->fields_lp = fields_lp;
    tmpl->fields_lp_last_used = server.mstime;
    serverAssert(dictAdd(htemplates->by_fields_lp, fields_lp, tmpl) == DICT_OK);
    htemplates->fields_lp_cache_bytes += bytes;
    htemplates->total_mem_size += bytes;
    return 1;
}

/* Build a fresh fields listpack (caller owns it). */
static unsigned char *hashTemplateBuildFieldsLp(hashTemplate *tmpl) {
    listpackEntry stack_ent[HASH_TMPL_STACK_ENTRIES];
    listpackEntry *ent = (tmpl->field_count <= HASH_TMPL_STACK_ENTRIES) ?
                         stack_ent : zmalloc(sizeof(listpackEntry) * tmpl->field_count);
    for (unsigned long long i = 0; i < tmpl->field_count; i++) {
        ent[i].sval = (unsigned char *)tmpl->fields[i];
        ent[i].slen = sdslen(tmpl->fields[i]);
    }
    unsigned char *lp = lpNewWithEntries(ent, tmpl->field_count);
    if (ent != stack_ent) zfree(ent);
    return lp;
}

/* Return tmpl's fields listpack, building it on first use. '*cache' requests
 * indexing the blob for RESTORE lookup and is cleared if the cache is full,
 * in which case the caller owns the returned blob. */
unsigned char *hashTemplateGetFieldsLp(hashTemplate *tmpl, int *cache) {
    if (!*cache) return hashTemplateBuildFieldsLp(tmpl);
    tmpl->fields_lp_last_used = server.mstime;
    if (tmpl->fields_lp) return tmpl->fields_lp;
    unsigned char *lp = hashTemplateBuildFieldsLp(tmpl);
    if (!hashTemplateIndexFieldsLp(tmpl, lp)) *cache = 0;
    return lp;
}

/* Lookup a template by its fields listpack. Returns NULL if not indexed. */
hashTemplate *hashTemplateGetByFieldsLp(unsigned char *fields_lp) {
    dictEntry *de = dictFind(htemplates->by_fields_lp, fields_lp);
    if (!de) return NULL;
    hashTemplate *tmpl = dictGetVal(de);
    tmpl->fields_lp_last_used = server.mstime;
    return tmpl;
}

/* Compute SipHash for a single field. */
static uint64_t computeFieldHash(sds field) {
    return dictGenHashFunction(field, sdslen(field));
}

/* Compute commutative hash for a set of fields.
 * Uses sum of per-field SipHashes so fields can be
 * incrementally added/removed:
 *   add:    hash += computeFieldHash(new_field)
 *   remove: hash -= computeFieldHash(removed_field) */
static uint64_t computeFieldsHash(sds *fields, unsigned long long field_count) {
    uint64_t hash = 0;
    for (unsigned long long i = 0; i < field_count; i++)
        hash += computeFieldHash(fields[i]);
    return hash;
}

/* Template registry dict callbacks. Key is hashTemplate*, value is same. */
static uint64_t templateFieldsHashFunc(const void *key) {
    const hashTemplate *tmpl = key;
    return tmpl->hash;
}

static int templateFieldsKeyCompare(dictCmpCache *cache, 
                                    const void *k1,
                                    const void *k2)
{
    UNUSED(cache);
    const hashTemplate *t1 = k1;
    const hashTemplate *t2 = k2;

    if (t1->hash != t2->hash) return 0;
    if (t1->field_count != t2->field_count) return 0;

    for (unsigned long long i = 0; i < t1->field_count; i++) {
        if (sdscmplen(t1->fields[i], t2->fields[i]) != 0)
            return 0;
    }
    return 1;
}

/* Drop tmpl's cached fields listpack. */
static void hashTemplateDropFieldsLp(hashTemplate *tmpl) {
    if (tmpl->fields_lp == NULL) return;

    unsigned char *lp = tmpl->fields_lp;
    size_t bytes = lpBytes(lp);
    serverAssert(dictDelete(htemplates->by_fields_lp, lp) == DICT_OK);
    serverAssert(htemplates->fields_lp_cache_bytes >= bytes);
    htemplates->fields_lp_cache_bytes -= bytes;
    htemplates->total_mem_size -= bytes;
    lpFree(lp);
    tmpl->fields_lp = NULL;
}

static void templateFieldsKeyDestructor(dict *d, void *key) {
    UNUSED(d);
    hashTemplate *tmpl = key;
    tmplIdRecycle(tmpl->id);
    hashTemplateDropFieldsLp(tmpl);
    for (unsigned long long i = 0; i < tmpl->field_count; i++)
        sdsfree(tmpl->fields[i]);
    zfree(tmpl->fields);
    htemplates->total_mem_size -= tmpl->mem_size;
    zfree(tmpl);
}

static dictType templateFieldsDictType = {
    templateFieldsHashFunc,        /* hash function */
    NULL,                          /* key dup */
    NULL,                          /* val dup */
    templateFieldsKeyCompare,      /* key compare */
    templateFieldsKeyDestructor,   /* key destructor (key=tmpl) */
    NULL,                          /* val destructor (val=same as key) */
    NULL                           /* allow to expand */
};

/* by_fields_lp dict: key is the fields listpack, value is the hashTemplate* */
static uint64_t templateFieldsLpHashFunc(const void *key) {
    return dictGenHashFunction(key, lpBytes((unsigned char *)key));
}

static int templateFieldsLpKeyCompare(dictCmpCache *cache,
                                      const void *k1, const void *k2)
{
    UNUSED(cache);
    size_t l1 = lpBytes((unsigned char *)k1), l2 = lpBytes((unsigned char *)k2);
    return (l1 == l2) && (memcmp(k1, k2, l1) == 0);
}

static dictType templateFieldsLpDictType = {
    templateFieldsLpHashFunc,      /* hash function */
    NULL,                          /* key dup */
    NULL,                          /* val dup */
    templateFieldsLpKeyCompare,    /* key compare */
    NULL,                          /* key destructor (blob owned by template) */
    NULL,                          /* val destructor */
    NULL                           /* allow to expand */
};

/* Initialize hash templates registry. */
void hashTemplatesInit(void) {
    if (htemplates) return;
    htemplates = zcalloc(sizeof(hashTemplateRegistry));
    htemplates->by_fields = dictCreate(&templateFieldsDictType);
    htemplates->by_fields_lp = dictCreate(&templateFieldsLpDictType);
    server.htemplates = htemplates;
}

/* Create a new hash tmpl, fields must be pre-sorted. */
static hashTemplate *hashTemplateCreateInternal(uint64_t hash, sds *fields,
                                                unsigned long long field_count) 
{
    /* Fields must be strictly ascending; catch unsorted/dup sets in test builds. */
    debugServerAssert(hashTemplateValidateFields(fields, field_count));

    hashTemplate *tmpl = zmalloc(sizeof(*tmpl));
    tmpl->hash = hash;
    tmpl->hold_refcount = 0;
    tmpl->key_refcount = 0;
    tmpl->field_count = field_count;
    tmpl->fields = zmalloc(sizeof(sds) * field_count);
    tmpl->fields_lp = NULL; /* Lazy built on first save/lookup due to RESTORE. */
    tmpl->fields_lp_last_used = 0;
    tmpl->mem_size = sizeof(*tmpl) + sizeof(sds) * field_count;

    for (unsigned long long i = 0; i < field_count; i++) {
        tmpl->fields[i] = sdsdup(fields[i]);
        tmpl->mem_size += sdsZmallocSize(tmpl->fields[i]);
    }
    htemplates->total_mem_size += tmpl->mem_size;

    /* Whether DUMP can serialize the field names as one listpack blob. */
    tmpl->fits_in_listpack = hashTypeSdsArrayFitsLp(tmpl->fields, tmpl->field_count);
    tmpl->id = tmplIdAllocate(tmpl);
    return tmpl;
}

/* Get or create a template (computes the fields-hash). fields must be pre-sorted. */
hashTemplate *hashTemplateGetOrCreate(sds *fields, unsigned long long field_count) {
    return hashTemplateGetOrCreateWithHash(
        computeFieldsHash(fields, field_count), fields, field_count);
}

/* Get or create a template, reusing a fields-hash the caller already has. The
 * fields-hash is commutative (a sum of per-field hashes), so an HSET/HDEL that
 * adds or removes one field can update it in O(1) (hash +/- that field's hash)
 * and pass it here instead of rescanning every field. fields must be pre-sorted. */
static hashTemplate *hashTemplateGetOrCreateWithHash(uint64_t hash, sds *fields,
                                              unsigned long long field_count) {
    /* Fields must be strictly ascending; catch unsorted/dup sets in test builds. */
    debugServerAssert(hashTemplateValidateFields(fields, field_count));

    hashTemplate query = {
        .hash = hash,
        .field_count = field_count,
        .fields = fields
    };

    dictEntry *de = dictFind(htemplates->by_fields, &query);
    if (de) return dictGetKey(de);

    hashTemplate *tmpl = hashTemplateCreateInternal(hash, fields, field_count);
    dictAdd(htemplates->by_fields, tmpl, NULL);
    return tmpl;
}

/* Registry lookup without creating on miss. fields must be pre-sorted. */
static hashTemplate *hashTemplateFindByFields(uint64_t hash, sds *fields,
                                              unsigned long long field_count) {
    debugServerAssert(hashTemplateValidateFields(fields, field_count));

    hashTemplate query = { .hash = hash, .field_count = field_count, .fields = fields };
    dictEntry *de = dictFind(htemplates->by_fields, &query);
    return de ? dictGetKey(de) : NULL;
}

/* Bump key_refcount when a hash key starts using tmpl. */
void hashTemplateIncrKeyRef(hashTemplate *tmpl) {
    tmpl->key_refcount++;
    htemplates->total_key_refs++;
}

/* Bump hold_refcount for a non-key holder (a client's prepared fieldset from
 * HIMPORT PREPARE, or an in-progress RDB load). */
void hashTemplateIncrHoldRef(hashTemplate *tmpl) {
    tmpl->hold_refcount++;
}

/* Free the template when no key and no holder reference it. */
static void hashTemplateFreeIfUnreferenced(hashTemplate *tmpl) {
    if (tmpl->key_refcount == 0 && tmpl->hold_refcount == 0)
        dictDelete(htemplates->by_fields, tmpl);
}

/* Drop one holdref (a client's prepared fieldset, or an in-progress RDB load).
 * When the last holder leaves, free tmpl if no keys reference it either. */
void hashTemplateDecrHoldRef(hashTemplate *tmpl) {
    serverAssert(tmpl->hold_refcount > 0);
    tmpl->hold_refcount--;
    if (tmpl->hold_refcount > 0) return;

    hashTemplateFreeIfUnreferenced(tmpl);
}

/* True if the calling thread is the one allowed to mutate the registry: the
 * main thread or a thread holding the module GIL. Any other thread (a BIO
 * lazyfree job) must defer the drop using hashTemplateRecordPendingDrop() */
static inline int canThreadWriteRegistry(void) {
    return pthread_equal(pthread_self(), server.main_thread_id) || moduleThreadHoldsGIL();
}

/* Decrement key_refcount when freeing a hash key. */
static void hashTemplateDecrKeyRef(hashTemplate *tmpl) {
    serverAssert(canThreadWriteRegistry());
    serverAssert(tmpl);
    serverAssert(tmpl->key_refcount > 0);
    htemplates->total_key_refs--;
    if (--tmpl->key_refcount == 0) hashTemplateFreeIfUnreferenced(tmpl);
}

/* Apply the key ref drops recorded by the BIO lazyfree thread. Processes
 * incrementally with a time limit to avoid blocking the main thread when
 * draining large batches (e.g. after FLUSHALL). */
static void hashTemplateApplyPendingDrops(void) {
    static dict *batch = NULL;
    static dictIterator it;

    /* Acquire new batch if not already draining. */
    if (batch == NULL) {
        uintptr_t cur;
        atomicExchangeAcquire(bio_pending_drops, 0, cur);
        if (cur == 0) return;
        batch = (dict *)cur;
        dictInitSafeIterator(&it, batch);
    }

    /* Time limit: spread work across multiple cron cycles to avoid spikes. */
    long long start = ustime();
    long long timelimit = 1000000 / server.hz / 20;  /* 5% of a hz cycle */
    if (timelimit <= 0) timelimit = 1;

    int i = 0;
    dictEntry *de;
    while ((de = dictNext(&it)) != NULL) {
        hashTemplate *tmpl = hashTemplateGetById((uint64_t)(uintptr_t)dictGetKey(de));
        uint64_t n = dictGetUnsignedIntegerVal(de);
        serverAssert(tmpl != NULL && tmpl->key_refcount >= n);
        htemplates->total_key_refs -= n;
        tmpl->key_refcount -= n;
        if (tmpl->key_refcount == 0) hashTemplateFreeIfUnreferenced(tmpl);
        /* Check the clock every 64 drops. */
        if ((++i & 63) == 0 && ustime() - start > timelimit) return;
    }
    dictResetIterator(&it);
    dictRelease(batch);
    batch = NULL;
}

/* Idle time (ms) after which an unused fields_lp blob is deleted which was
 * generated due to DUMP/RESTORE propagation master->repica or ASM */
#define HASH_TEMPLATE_FIELDS_LP_IDLE_MS 5000

/* Max idle blobs collected per batch. */
#define FIELDS_LP_BATCH 64

typedef struct {
    hashTemplate *tmpls[FIELDS_LP_BATCH];
    int collected; /* idle blobs gathered into tmpls[] */
    int scanned;   /* blobs scanned this window */
} fieldsLpReclaimCtx;

/* dictScan callback: collect templates whose fields_lp is idle. */
static void fieldsLpReclaimCollect(void *privdata, const dictEntry *de, dictEntry **plink) {
    UNUSED(plink);
    fieldsLpReclaimCtx *ctx = privdata;
    if (ctx->collected >= FIELDS_LP_BATCH) return;

    hashTemplate *tmpl = dictGetVal((dictEntry *)de);
    if (tmpl->fields_lp == NULL) return;
    ctx->scanned++;
    if (server.mstime - tmpl->fields_lp_last_used < HASH_TEMPLATE_FIELDS_LP_IDLE_MS) return; /* still in use */
    ctx->tmpls[ctx->collected++] = tmpl;
}

/* Reclaim fields_lp blobs idle longer than HASH_TEMPLATE_FIELDS_LP_IDLE_MS.
 * These blobs are built lazily during DUMP/RESTORE/ASM for O(1) template lookup;
 * once idle they are dead weight (rebuilt on the next DUMP if needed). */
static void hashTemplatesCleanupFieldsLpCron(void) {
    static unsigned long cursor = 0;
    dict *d = htemplates ? htemplates->by_fields_lp : NULL;
    if (!d || dictSize(d) == 0) return;

    long long start = ustime();
    long long timelimit = 1000000 / server.hz / 200;  /* 0.5% of a hz cycle (~0.5ms) */
    if (timelimit <= 0) timelimit = 1;

    while (1) {
        /* Collect a small batch, then free it */
        fieldsLpReclaimCtx ctx = { .collected = 0 };
        int steps = 0;

        do {
            cursor = dictScan(d, cursor, fieldsLpReclaimCollect, &ctx);
        } while (cursor != 0 && ++steps < 16 && ctx.collected < FIELDS_LP_BATCH);

        /* Blobs present but none idle: nothing to reclaim now, stop (e.g. ASM). */
        if (ctx.collected == 0 && ctx.scanned > 0) break;

        for (int i = 0; i < ctx.collected; i++) {
            hashTemplate *tmpl = ctx.tmpls[i];
            hashTemplateDropFieldsLp(tmpl);
        }

        if (cursor == 0) break; /* completed a full sweep */
        if (ustime() - start > timelimit) break;   /* out of time budget */
    }
}

/* Template registry maintenance, once per serverCron cycle. */
void hashTemplatesCron(void) {
    if (!htemplates) return;
    hashTemplateApplyPendingDrops();
    hashTemplatesCleanupFieldsLpCron();
}

/* Get number of templates in the registry. */
size_t hashTemplateRegistrySize(void) {
    if (!htemplates || !htemplates->by_fields) return 0;
    return dictSize(htemplates->by_fields);
}

/* Get total number of template-based keys (sum of all key_refcounts). */
size_t hashTemplateKeyCount(void) {
    if (!htemplates) return 0;
    return htemplates->total_key_refs;
}

/* Memory held by the shared template registry, reported as
 * used_memory_hash_templates (INFO) and hash.templates (MEMORY STATS). */
size_t hashTemplatesMemUsage(void) {
    if (!htemplates) return 0;
    return htemplates->total_mem_size + sizeof(*htemplates) +
           dictMemUsage(htemplates->by_fields) +
           dictMemUsage(htemplates->by_fields_lp) +
           htemplates->by_id_cap * sizeof(tmplIdChunk *) +
           htemplates->by_id_chunks * sizeof(tmplIdChunk);
}

/* Find field index in tmpl using binary search (fields are sorted).
 * Returns the index (>= 0) if found, otherwise -(insert_pos + 1) where
 * insert_pos is the position to splice field into to keep fields sorted. */
long long hashTemplateFieldIndex(hashTemplate *tmpl, sds field) {
    long long lo = 0, hi = (long long)tmpl->field_count - 1;
    while (lo <= hi) {
        long long mid = lo + (hi - lo) / 2;
        int cmp = sdscmplen(field, tmpl->fields[mid]);
        if (cmp == 0) return mid;
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return -(lo + 1);
}

/* Return the template with `field` inserted at `insert_pos` (key-ref taken). */
static hashTemplate *hashTemplateForInsertedField(hashTemplate *tmpl, sds field,
                                                  long long insert_pos) {
    serverAssert(insert_pos >= 0 && (unsigned long long)insert_pos <= tmpl->field_count);
    unsigned long long new_count = tmpl->field_count + 1;
    sds stack_fields[HASH_TMPL_STACK_ENTRIES];
    sds *new_fields = (new_count <= HASH_TMPL_STACK_ENTRIES) ?
                      stack_fields : zmalloc(sizeof(sds) * new_count);
    memcpy(new_fields, tmpl->fields, sizeof(sds) * insert_pos);
    new_fields[insert_pos] = field;
    memcpy(&new_fields[insert_pos + 1], &tmpl->fields[insert_pos],
           sizeof(sds) * (tmpl->field_count - insert_pos));

    /* Incremental hash: add the new field's hash. */
    uint64_t new_hash = tmpl->hash + computeFieldHash(field);
    hashTemplate *new_tmpl = hashTemplateGetOrCreateWithHash(new_hash, new_fields, new_count);
    hashTemplateIncrKeyRef(new_tmpl);
    if (new_fields != stack_fields) zfree(new_fields);
    return new_tmpl;
}

/* Return the template with the field at `idx` deleted (key-ref taken).
 * Precondition: at least one field remains. */
static hashTemplate *hashTemplateForDeletedField(hashTemplate *tmpl, long long idx) {
    serverAssert(tmpl->field_count >= 2); /* at least one field remains after delete */
    serverAssert(idx >= 0 && (unsigned long long)idx < tmpl->field_count);
    unsigned long long new_count = tmpl->field_count - 1;
    sds stack_fields[HASH_TMPL_STACK_ENTRIES];
    sds *new_fields = (new_count <= HASH_TMPL_STACK_ENTRIES) ?
                      stack_fields : zmalloc(sizeof(sds) * new_count);
    unsigned long long j = 0;
    for (unsigned long long i = 0; i < tmpl->field_count; i++)
        if ((long long)i != idx) new_fields[j++] = tmpl->fields[i];

    /* Incremental hash: subtract the removed field's hash. */
    uint64_t new_hash = tmpl->hash - computeFieldHash(tmpl->fields[idx]);
    hashTemplate *new_tmpl = hashTemplateGetOrCreateWithHash(new_hash, new_fields, new_count);
    hashTemplateIncrKeyRef(new_tmpl);
    if (new_fields != stack_fields) zfree(new_fields);
    return new_tmpl;
}

/* Return 1 if fields are strictly ascending (sorted, no duplicates), else 0. */
int hashTemplateValidateFields(sds *fields, unsigned long long field_count) {
    for (unsigned long long i = 1; i < field_count; i++)
        if (sdscmplen(fields[i - 1], fields[i]) >= 0)
            return 0;
    return 1;
}

/*-----------------------------------------------------------------------------
 * HashTemplateLp functions (OBJ_ENCODING_TMPL_LP)
 *
 * Listpack format: [template_id (varint)][value1][value2]...
 * o->ptr points directly to the listpack
 *----------------------------------------------------------------------------*/

/* Read the template ID stored as the first listpack entry. */
static uint64_t hashTemplateLpGetTemplateId(unsigned char *lp) {
    unsigned char *p = lpFirst(lp);
    serverAssert(p != NULL); /* id entry always present in a well-formed TMPL_LP */
    long long id;
    if (!lpGetIntegerValue(p, &id))
        serverPanic("TMPL_LP listpack header is not an integer template ID");
    return (uint64_t)id;
}

static hashTemplate *hashTemplateLpGetTemplate(unsigned char *lp) {
    hashTemplate *tmpl = hashTemplateGetById(hashTemplateLpGetTemplateId(lp));
    if (tmpl == NULL)
        serverPanic("TMPL_LP listpack references unknown template ID");
    return tmpl;
}

/* Return the shared template of a template-encoded hash (TMPL_LP or TMPL_ARRAY). */
hashTemplate *hashTypeGetTemplate(robj *o) {
    serverAssert(o->encoding == OBJ_ENCODING_TMPL_LP ||
                 o->encoding == OBJ_ENCODING_TMPL_ARRAY);
    return (o->encoding == OBJ_ENCODING_TMPL_LP) ?
        hashTemplateLpGetTemplate(o->ptr) :
        hashTemplateArrayGetTemplate(o->ptr);
}

/* Replace template ID in listpack. */
unsigned char *hashTemplateLpSetTemplate(unsigned char *lp, hashTemplate *tmpl) {
    unsigned char *p = lpFirst(lp);
    serverAssert(p != NULL); /* id entry always present in a well-formed TMPL_LP */
    unsigned char *v = lpReplaceInteger(lp, &p, (long long)tmpl->id);
    serverAssert(v != NULL);
    return v;
}

/* Get pointer to first value entry (skip template ID). */
static unsigned char *hashTemplateLpFirstValue(unsigned char *lp) {
    unsigned char *p = lpFirst(lp);  /* template ID entry */
    serverAssert(p != NULL);         /* id entry always present */
    unsigned char *v = lpNext(lp, p);
    serverAssert(v != NULL);         /* a TMPL_LP always holds >= 1 value */
    return v;
}

/* Seek the value entry at template field index 'idx' (entry 0 is the template
 * ID, so value idx lives at idx+1). A well-formed TMPL_LP always has this
 * entry; so assert here rather than hand a NULL cursor. */
static unsigned char *hashTemplateLpSeekValue(unsigned char *lp, long long idx) {
    unsigned char *p = lpSeek(lp, idx + 1);
    serverAssert(p != NULL);
    return p;
}

/* Walk a TMPL_LP listpack once, filling vptrs[i] with a pointer to the entry
 * holding the value of field i (i in [0, field_count)). Lets callers that need
 * many values index them in O(1) instead of an O(field_count) lpSeek per draw
 * (e.g. HRANDFIELD with a large count). */
static void hashTemplateLpCollectValuePtrs(unsigned char *lp, unsigned char **vptrs,
                                           unsigned long long field_count) {
    unsigned char *p = hashTemplateLpFirstValue(lp);
    for (unsigned long long i = 0; i < field_count; i++) {
        serverAssert(p != NULL);
        vptrs[i] = p;
        p = lpNext(lp, p);
    }
}

/* Create listpack with template ID and values. Increments key_refcount. */
static unsigned char *hashTemplateLpCreate(hashTemplate *tmpl, sds *values) {
    hashTemplateIncrKeyRef(tmpl);

    unsigned long long n = tmpl->field_count;
    /* +1 for template ID entry */
    listpackEntry stack_entries[HASH_TMPL_STACK_ENTRIES + 1];
    listpackEntry *entries = (n + 1 <= HASH_TMPL_STACK_ENTRIES + 1) ?
                             stack_entries :
                             zmalloc(sizeof(listpackEntry) * (n + 1));

    /* First entry: template ID */
    entries[0].lval = (long long)tmpl->id;
    entries[0].sval = NULL;

    /* Remaining entries: values */
    for (unsigned long long i = 0; i < n; i++) {
        entries[i + 1].sval = (unsigned char *)values[i];
        entries[i + 1].slen = sdslen(values[i]);
    }

    unsigned char *lp = lpNewWithEntries(entries, n + 1);
    serverAssert(lp != NULL);
    if (entries != stack_entries) zfree(entries);

    return lp;
}

typedef struct {
    sds field;
    unsigned char *vstr;
    size_t vlen;
    long long vll;
} hashTypeFvPair;

/* qsort comparator: order hashTypeFvPair by field name. */
static int hashTypeFvPairCmp(const void *a, const void *b) {
    return sdscmplen(((const hashTypeFvPair *)a)->field,
                     ((const hashTypeFvPair *)b)->field);
}

/* Create TMPL_LP from field-value pairs */
static unsigned char *hashTemplateLpCreateFromPairs(hashTemplate *tmpl,
                                                    hashTypeFvPair *pairs,
                                                    unsigned long long n) {
    hashTemplateIncrKeyRef(tmpl);

    /* +1 for template ID entry */
    listpackEntry stack_entries[HASH_TMPL_STACK_ENTRIES + 1];
    listpackEntry *entries = (n + 1 <= HASH_TMPL_STACK_ENTRIES + 1) ?
                             stack_entries :
                             zmalloc(sizeof(listpackEntry) * (n + 1));

    entries[0].lval = (long long)tmpl->id;  /* template ID */
    entries[0].sval = NULL;
    for (unsigned long long i = 0; i < n; i++) {
        if (pairs[i].vstr) {
            entries[i + 1].sval = pairs[i].vstr;
            entries[i + 1].slen = pairs[i].vlen;
        } else {
            entries[i + 1].sval = NULL;      /* int-encoded value */
            entries[i + 1].lval = pairs[i].vll;
        }
    }

    unsigned char *lp = lpNewWithEntries(entries, n + 1);
    serverAssert(lp != NULL);
    if (entries != stack_entries) zfree(entries);

    return lp;
}

/* Release a template listpack's key ref and free the listpack. May run
 * in a BIO lazyfree thread and it may just record the ref count drop to be 
 * collected by main thread later in hashTemplateApplyPendingDrops() */
void hashTemplateLpFree(unsigned char *lp) {
    uint64_t id = hashTemplateLpGetTemplateId(lp);
    if (!canThreadWriteRegistry()) {
        hashTemplateRecordPendingDrop(id);
    } else {
        hashTemplate *tmpl = hashTemplateGetById(id);
        if (tmpl == NULL)
            serverPanic("TMPL_LP listpack references unknown template ID");
        hashTemplateDecrKeyRef(tmpl);
    }
    lpFree(lp);
}

/*-----------------------------------------------------------------------------
 * hashTemplateArray functions (OBJ_ENCODING_TMPL_ARRAY)
 *----------------------------------------------------------------------------*/

static hashTemplate *hashTemplateArrayGetTemplate(hashTemplateArray *hta) {
    hashTemplate *tmpl = hashTemplateGetById(hta->tmpl_id);
    serverAssert(tmpl != NULL);
    return tmpl;
}

/* Create a new hashTemplateArray. Increments key_refcount.
 * If take is set, takes ownership of the SDS strings in values array.
 * Otherwise copies them with sdsdup. */
static hashTemplateArray *hashTemplateArrayCreate(hashTemplate *tmpl, sds *values, int take) {
    unsigned long long n = tmpl->field_count;
    hashTemplateArray *hta = zmalloc(sizeof(*hta) + sizeof(sds) * n);
    hta->tmpl_id = tmpl->id;
    hta->field_count = n;

    for (unsigned long long i = 0; i < n; i++)
        hta->values[i] = take ? values[i] : sdsdup(values[i]);

    hashTemplateIncrKeyRef(tmpl);
    return hta;
}

/* Free a hashTemplateArray (release key ref and free data). May run in a BIO
 * lazyfree thread: uses the tmpl_id/field_count, never the registry. */
void hashTemplateArrayFree(hashTemplateArray *hta) {
    for (unsigned long long i = 0; i < hta->field_count; i++)
        sdsfree(hta->values[i]);

    if (!canThreadWriteRegistry())
        hashTemplateRecordPendingDrop(hta->tmpl_id);
    else
        hashTemplateDecrKeyRef(hashTemplateGetById(hta->tmpl_id));

    zfree(hta);
}

/* Build a hash object that shares `tmpl` and stores the given values.
 * Two template-backed encodings exist:
 *   TMPL_LP    - values packed in a listpack (compact, small hashes).
 *   TMPL_ARRAY - values stored as an sds array (used when listpack limits
 *                would be exceeded).
 * If `take` is set, ownership of the value sds strings is transferred here. */
robj *createHashObjectFromTemplate(hashTemplate *tmpl, sds *values, int take) {
    robj *o;

    /* TMPL_LP if the values fit a listpack (field names live in the template). */
    if (hashTypeSdsArrayFitsLp(values, tmpl->field_count)) {
        o = createObject(OBJ_HASH, hashTemplateLpCreate(tmpl, values));
        o->encoding = OBJ_ENCODING_TMPL_LP;
        /* The listpack copied the value bytes; if we own them, free them now. */
        if (take)
            for (unsigned long long i = 0; i < tmpl->field_count; i++)
                sdsfree(values[i]);
    } else {
        o = createObject(OBJ_HASH, hashTemplateArrayCreate(tmpl, values, take));
        o->encoding = OBJ_ENCODING_TMPL_ARRAY;
    }
    return o;
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
    OnFieldExpireCtx *ctx = info->ctx;
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

        /* Collect expired field for subkey notification. */
        if (ctx->vexpired) {
            char *fstr = (char *)(fref ? fref : intbuf);
            vecPush(ctx->vexpired, createStringObject(fstr, flen));
        }

        propagateHashFieldDeletion(db, key, (char *)((fref) ? fref : intbuf), flen);
        server.stat_expired_subkeys++;
        if (ctx->activeEx) server.stat_expired_subkeys_active++;

        ptr = lpNext(lpt->lp, ptr);

        info->itemsExpired++;
        expired++;
    }

    if (expired) {
        size_t oldsize = 0;
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(kv);
        lpt->lp = lpDeleteRange(lpt->lp, 0, expired * 3);
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(db, getKeySlot(key), kv, oldsize, kvobjAllocSize(kv));

        /* update keysizes */
        unsigned long l = lpLength(lpt->lp) / 3;
        updateKeysizesHist(db, OBJ_HASH, l + expired, l);
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
        hashTypeDelete(ex->hashObj, field);
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
    if (server.allow_access_expired) return 0;

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
void hashTypeTryConversion(redisDb *db, kvobj *o, robj **argv, int start, int end) {
    int tmpl_lp = (o->encoding == OBJ_ENCODING_TMPL_LP);
    int target_enc = tmpl_lp ? OBJ_ENCODING_TMPL_ARRAY : OBJ_ENCODING_HT;

    /* TMPL_ARRAY and HT don't need conversion checks. */
    if (o->encoding != OBJ_ENCODING_LISTPACK &&
        o->encoding != OBJ_ENCODING_LISTPACK_EX &&
        o->encoding != OBJ_ENCODING_TMPL_LP)
    {
        return;
    }

    /* Determine target encoding for conversion. */
    if (!tmpl_lp) {
        /* Check field count limit for regular listpack. */
        size_t new_fields = (end - start + 1) / 2;
        if (new_fields > server.hash_max_listpack_entries) {
            hashTypeConvert(db, o, OBJ_ENCODING_HT);
            dictExpand(o->ptr, new_fields);
            return;
        }
    }

    /* Check value sizes (and field sizes for non-TMPL_LP). */
    size_t sum = 0;
    for (int i = start; i <= end; i++) {
        if (tmpl_lp && ((i - start) % 2 == 0))
            continue;
        if (!sdsEncodedObject(argv[i]))
            continue;
        size_t len = sdslen(argv[i]->ptr);
        if (len > server.hash_max_listpack_value) {
            hashTypeConvert(db, o, target_enc);
            return;
        }
        sum += len;
    }
    unsigned char *lp = (o->encoding == OBJ_ENCODING_LISTPACK_EX) ?
                        ((listpackEx*)o->ptr)->lp : o->ptr;
    if (!lpSafeToAdd(lp, sum))
        hashTypeConvert(db, o, target_enc);
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

    Entry *entry = dictGetKey(de);
    *expiredAt = entryGetExpiry(entry);
    *value = entryGetValue(entry);
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
    sds key = kvobjGetKey(o);
    GetFieldRes res;
    uint64_t dummy;
    size_t oldsize = 0;
    if (expiredAt == NULL) expiredAt = &dummy;
    if (o->encoding == OBJ_ENCODING_LISTPACK ||
        o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        *vstr = NULL;
        res = hashTypeGetFromListpack(o, field, vstr, vlen, vll, expiredAt);

        if (res == GETF_NOT_FOUND)
            return GETF_NOT_FOUND;

    } else if (o->encoding == OBJ_ENCODING_HT) {
        sds value = NULL;
        if (server.memory_tracking_enabled && !(hfeFlags & HFE_LAZY_NO_UPDATE_ALLOCSIZES))
            oldsize = kvobjAllocSize(o);
        res = hashTypeGetFromHashTable(o, field, &value, expiredAt);
        if (server.memory_tracking_enabled && !(hfeFlags & HFE_LAZY_NO_UPDATE_ALLOCSIZES))
            updateSlotAllocSize(db, getKeySlot(key), o, oldsize, kvobjAllocSize(o));

        if (res == GETF_NOT_FOUND)
            return GETF_NOT_FOUND;

        *vstr = (unsigned char*) value;
        *vlen = sdslen(value);
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP ||
               o->encoding == OBJ_ENCODING_TMPL_ARRAY)
    {
        long long idx = hashTemplateFieldIndex(hashTypeGetTemplate(o), field);
        if (idx < 0) return GETF_NOT_FOUND;

        if (o->encoding == OBJ_ENCODING_TMPL_LP) {
            unsigned char *p = hashTemplateLpSeekValue(o->ptr, idx);
            *vstr = lpGetValue(p, vlen, vll);
        } else {
            sds value = ((hashTemplateArray *)o->ptr)->values[idx];
            *vstr = (unsigned char*) value;
            *vlen = sdslen(value);
        }
        *expiredAt = EB_EXPIRE_TIME_INVALID;
        res = GETF_OK;
    } else {
        serverPanic("Unknown hash encoding");
    }

    if ((server.allow_access_expired) ||
        (*expiredAt >= (uint64_t) commandTimeSnapshot()) ||
        (hfeFlags & HFE_LAZY_ACCESS_EXPIRED))
        return GETF_OK;

    if (server.masterhost || server.cluster_enabled) {
        /* If CLIENT_MASTER, assume valid as long as it didn't get delete.
         *
         * In cluster mode, we also assume valid if we are importing data
         * from the source, to avoid deleting fields that are still in use.
         * We create a fake master client for data import, which can be
         * identified using the CLIENT_MASTER flag. */
        if (server.current_client && (server.current_client->flags & CLIENT_MASTER))
            return GETF_OK;

        /* For replica, if user client, then act as if expired, but don't delete! */
        if (server.masterhost) return GETF_EXPIRED;
    }

    if ((server.loading) ||
        (hfeFlags & HFE_LAZY_AVOID_FIELD_DEL) ||
        (isPausedActionsWithUpdate(PAUSE_ACTION_EXPIRE)))
        return GETF_EXPIRED;

    /* delete the field and propagate the deletion */
    if (server.memory_tracking_enabled && !(hfeFlags & HFE_LAZY_NO_UPDATE_ALLOCSIZES))
        oldsize = kvobjAllocSize(o);
    serverAssert(hashTypeDelete(o, field) == 1);
    if (server.memory_tracking_enabled && !(hfeFlags & HFE_LAZY_NO_UPDATE_ALLOCSIZES))
        updateSlotAllocSize(db, getKeySlot(key), o, oldsize, kvobjAllocSize(o));
    propagateHashFieldDeletion(db, key, field, sdslen(field));
    server.stat_expired_subkeys++;

    if (!(hfeFlags & HFE_LAZY_NO_UPDATE_KEYSIZES)) {
        uint64_t l = hashTypeLength(o, 0);
        updateKeysizesHist(db, OBJ_HASH, l+1, l);
    }

    /* If the field is the last one in the hash, then the hash will be deleted */
    res = GETF_EXPIRED;
    robj *keyObj = createStringObject(key, sdslen(key));
    unsigned long length = hashTypeLength(o, 0);
    if ((length != 0) && !(hfeFlags & HFE_LAZY_NO_NOTIFICATION)) {
        robj fobj, *farr[1] = {&fobj};
        initStaticStringObject(fobj, field);
        notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hexpired", keyObj, db->id, farr, 1);
    }
    if ((length == 0) && (!(hfeFlags & HFE_LAZY_AVOID_HASH_DEL))) {
        if (!(hfeFlags & HFE_LAZY_NO_NOTIFICATION))
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", keyObj, db->id);
        dbDelete(db,keyObj);
        o = NULL;
        res = GETF_EXPIRED_HASH;
    }
    keyModified(NULL, db, keyObj, o, !(hfeFlags & HFE_LAZY_NO_SIGNAL));
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
/* Skip hashTypeSet's auto-conversion to template. A command adding N fields sets
 * this and converts once at the end instead, so the template is created a single
 * time rather than rebuilt on every field added past the threshold. */
#define HASH_SET_NO_TEMPLATE_CONVERT (1<<3)

static_assert(HASH_SET_TAKE_VALUE == ENTRY_TAKE_VALUE, "ENTRY_TAKE_VALUE must match HASH_SET_TAKE_VALUE");

int hashTypeSet(redisDb *db, kvobj *o, sds field, sds value, int flags) {
    int update = 0;

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
            hashTypeConvert(db, o, OBJ_ENCODING_HT);
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
            hashTypeConvert(db, o, OBJ_ENCODING_HT);

    } else if (o->encoding == OBJ_ENCODING_HT) {
        dict *ht = o->ptr;
        /* check if field already exists */
        dictEntryLink bucket, link = dictFindLink(ht, field, &bucket);
        size_t *alloc_size = htGetMetadataSize(ht);

        /* take ownership of value if requested */
        uint32_t newEntryFlags = flags & HASH_SET_TAKE_VALUE;
        flags &= ~HASH_SET_TAKE_VALUE;

        if (link == NULL) {
            /* Create entry and transfer value ownership if possible */
            size_t usable;
            Entry *newEntry = entryCreate(field, value, newEntryFlags, &usable);

            dictSetKeyAtLink(ht, newEntry, &bucket, 1);
            *alloc_size += usable;
        } else {
            /* Existing field - update value in entry */
            Entry *oldEntry = dictGetKey(*link);

            /* Check if old entry has expiration before potentially freeing it */
            uint64_t oldExpireAt = entryGetExpiry(oldEntry);
            uint64_t newExpireAt = EB_EXPIRE_TIME_INVALID;

            /* If attached TTL to the old field, then remove it from hash's
             * private ebuckets. We do this before updating the value because
             * the entry might be reallocated and freed. */
            if (oldExpireAt != EB_EXPIRE_TIME_INVALID) {
                hfieldPersist(o, oldEntry);
                if (flags & HASH_SET_KEEP_TTL) {
                    newExpireAt = oldExpireAt;
                    newEntryFlags |= ENTRY_HAS_EXPIRY;
                }
            }
            
            ssize_t usableDiff;
            Entry *newEntry = entryUpdate(oldEntry, value, newEntryFlags, &usableDiff);

            /* If entry was reallocated, update the dict key */
            if (newEntry != oldEntry) {
                /* entryUpdate already freed the old entry if needed */
                /* Update the dict to point to the new entry using dictSetKeyAtLink (no_value=1) */
                dictSetKeyAtLink(ht, newEntry, &link, 0);
            }

            /* If keeping TTL, add the (potentially new) entry back to ebuckets */
            if (newExpireAt != EB_EXPIRE_TIME_INVALID) {
                dict *d = o->ptr;
                htMetadataEx *dictExpireMeta = htGetMetadataEx(d);
                ebAdd(&dictExpireMeta->hfe, &hashFieldExpireBucketsType, newEntry, newExpireAt);
            }

            *alloc_size += usableDiff;
            update = 1;
        }
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP ||
               o->encoding == OBJ_ENCODING_TMPL_ARRAY)
    {
        hashTemplate *tmpl = hashTypeGetTemplate(o);

        /* Check if field exists in tmpl */
        long long field_idx = hashTemplateFieldIndex(tmpl, field);
        int is_new = field_idx < 0;

        /* Promote TMPL_LP -> TMPL_ARRAY up front if the value or field count no
         * longer fits a listpack. The template (and field_idx) is unchanged. */
        if (o->encoding == OBJ_ENCODING_TMPL_LP &&
            (sdslen(value) > server.hash_max_listpack_value ||
             !lpSafeToAdd(o->ptr, sdslen(value)) ||
             (is_new && tmpl->field_count + 1 > server.hash_max_listpack_entries)))
        {
            hashTypeConvert(db, o, OBJ_ENCODING_TMPL_ARRAY);
        }

        if (!is_new) {
            /* Field exists - update value in place. */
            if (o->encoding == OBJ_ENCODING_TMPL_LP) {
                unsigned char *lp = o->ptr;
                unsigned char *p = hashTemplateLpSeekValue(lp, field_idx);
                o->ptr = lpReplace(lp, &p, (unsigned char *)value, sdslen(value));
                serverAssert(o->ptr != NULL);
            } else {
                hashTemplateArray *hta = o->ptr;
                if (hta->values[field_idx]) sdsfree(hta->values[field_idx]);
                if (flags & HASH_SET_TAKE_VALUE) {
                    hta->values[field_idx] = value;  /* adopt, don't copy */
                    value = NULL;
                } else {
                    hta->values[field_idx] = sdsdup(value);
                }
            }
            update = 1;
            goto cleanup;
        }

        /* Field not in tmpl - switch to the template that adds it at insert_pos. */
        long long insert_pos = -field_idx - 1;
        unsigned long long new_field_count = tmpl->field_count + 1;
        hashTemplate *new_tmpl = hashTemplateForInsertedField(tmpl, field, insert_pos);

        /* Insert value at insert_pos in existing structure. */
        if (o->encoding == OBJ_ENCODING_TMPL_LP) {
            unsigned char *lp = o->ptr;
            /* Update template ID. */
            lp = hashTemplateLpSetTemplate(lp, new_tmpl);
            /* Insert value at position (offset +1 for template ID). */
            if ((unsigned long long)insert_pos == tmpl->field_count) {
                lp = lpAppend(lp, (unsigned char *)value, sdslen(value));
            } else {
                unsigned char *p = hashTemplateLpSeekValue(lp, insert_pos);
                lp = lpInsertString(lp, (unsigned char *)value, sdslen(value), p, LP_BEFORE, NULL);
            }
            serverAssert(lp != NULL);
            hashTemplateDecrKeyRef(tmpl);
            o->ptr = lp;
        } else {
            hashTemplateArray *hta = o->ptr;
            /* Expand struct and shift elements to make room. */
            hta = zrealloc(hta, sizeof(*hta) + sizeof(sds) * new_field_count);
            if ((unsigned long long)insert_pos < tmpl->field_count) {
                memmove(&hta->values[insert_pos + 1], &hta->values[insert_pos],
                        sizeof(sds) * (tmpl->field_count - insert_pos));
            }
            if (flags & HASH_SET_TAKE_VALUE) {
                hta->values[insert_pos] = value;  /* adopt, don't copy */
                value = NULL;
            } else {
                hta->values[insert_pos] = sdsdup(value);
            }
            hashTemplateDecrKeyRef(tmpl);
            hta->tmpl_id = new_tmpl->id;
            hta->field_count = new_tmpl->field_count;
            o->ptr = hta;
        }
    } else {
        serverPanic("Unknown hash encoding");
    }

cleanup:
    /* Free SDS strings we did not referenced elsewhere if the flags
     * want this function to be responsible. */
    if (flags & HASH_SET_TAKE_FIELD && field) sdsfree(field);
    if (flags & HASH_SET_TAKE_VALUE && value) sdsfree(value);

    /* Auto-convert to template if threshold met and not already template.
     * Skipped when HASH_SET_NO_TEMPLATE_CONVERT is set: multi-field callers defer
     * this to a single post-loop conversion to avoid per-field template lookup. */
    if (!(flags & HASH_SET_NO_TEMPLATE_CONVERT) &&
        server.hash_min_template_entries > 0)
    {
        hashTypeTryConvertToTemplate(o, server.hash_min_template_entries,
                                    server.hash_max_template_entries, NULL);
    }

    return update;
}

SetExRes hashTypeSetExpiryHT(HashTypeSetEx *exInfo, sds field, uint64_t expireAt) {
    dict *ht = exInfo->hashObj->ptr;
    dictEntryLink link = NULL;
    Entry *entryNew = NULL;

    link = dictFindLink(ht, field, NULL);
    if (link == NULL)
        return HSETEX_NO_FIELD;

    dictEntry *existingEntry = *link;
    Entry *oldEntry = dictGetKey(existingEntry);
    /* Special value of EXPIRE_TIME_INVALID indicates field should be persisted.*/
    if (expireAt == EB_EXPIRE_TIME_INVALID) {
        /* Return error if already there is no ttl. */
        if (entryGetExpiry(oldEntry) == EB_EXPIRE_TIME_INVALID)
            return HSETEX_NO_CONDITION_MET;

        hfieldPersist(exInfo->hashObj, oldEntry);
        return HSETEX_OK;
    }

    /* If field doesn't have expiry metadata attached */
    if (!entryHasExpiry(oldEntry)) {
        size_t *alloc_size = htGetMetadataSize(ht);

        /* For fields without expiry, LT condition is considered valid */
        if (exInfo->expireSetCond & (HFE_XX | HFE_GT))
            return HSETEX_NO_CONDITION_MET;

        ssize_t usableDiff;
        entryNew = entryUpdate(oldEntry, NULL, ENTRY_HAS_EXPIRY, &usableDiff);
        *alloc_size += usableDiff;
    } else { /* field has ExpireMeta struct attached */
        uint64_t prevExpire = entryGetExpiry(oldEntry);

        /* If field has valid expiration time, then check GT|LT|NX */
        if (prevExpire != EB_EXPIRE_TIME_INVALID) {
            if (((exInfo->expireSetCond == HFE_GT) && (prevExpire >= expireAt)) ||
                ((exInfo->expireSetCond == HFE_LT) && (prevExpire <= expireAt)) ||
                (exInfo->expireSetCond == HFE_NX) )
                return HSETEX_NO_CONDITION_MET;

            /* If expiry time is the same, then nothing to do */
            if (prevExpire == expireAt)
                return HSETEX_OK;

            /* remove old expiry time from hash's private ebuckets */
            htMetadataEx *dm = htGetMetadataEx(ht);
            ebRemove(&dm->hfe, &hashFieldExpireBucketsType, oldEntry);

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
        entryNew = oldEntry;
    }

    dictSetKeyAtLink(ht, entryNew, &link, 0);  /* newItem=0 for updating existing entry */


    /* If expired, then delete the field and propagate the deletion.
     * If replica, continue like the field is valid */
    if (unlikely(checkAlreadyExpired(expireAt))) {
        /* replicas should not initiate deletion of fields */
        propagateHashFieldDeletion(exInfo->db, exInfo->key->ptr, field, sdslen(field));
        hashTypeDelete(exInfo->hashObj, field);
        server.stat_expired_subkeys++;
        return HSETEX_DELETED;
    }

    if (exInfo->minExpireFields > expireAt)
        exInfo->minExpireFields = expireAt;

    htMetadataEx *dm = htGetMetadataEx(ht);
    ebAdd(&dm->hfe, &hashFieldExpireBucketsType, entryNew, expireAt);
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

    htMetadataEx *m = htGetMetadataEx(ht);
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
        hashTypeConvert(c->db, o, OBJ_ENCODING_LISTPACK_EX);
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP ||
               o->encoding == OBJ_ENCODING_TMPL_ARRAY)
    {
        /* Prepare template key: Convert to LISTPACK_EX if it fits, else HT */
        hashTypeConvert(c->db, o, OBJ_ENCODING_LISTPACK_EX);
    } else if (o->encoding == OBJ_ENCODING_HT) {
        /* Take care dict has HFE metadata */
        if (!isDictWithMetaHFE(ht)) {
            /* Realloc (only header of dict) with metadata for hash-field expiration */
            dictTypeAddMeta(&ht, &entryHashDictTypeWithHFE);
            htMetadataEx *m = htGetMetadataEx(ht);
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

    int slot = getKeySlot(ex->key->ptr);
    if (ex->minExpire != EB_EXPIRE_TIME_INVALID) {
        if (newMinExpire != EB_EXPIRE_TIME_INVALID)
            estoreUpdate(ex->db->subexpires, slot, ex->hashObj, newMinExpire);
        else
            estoreRemove(ex->db->subexpires, slot, ex->hashObj);
    } else {
        if (newMinExpire != EB_EXPIRE_TIME_INVALID)
            estoreAdd(ex->db->subexpires, slot, ex->hashObj, newMinExpire);
    }
}

/* Delete an element from a hash.
 *
 * Return 1 on deleted and 0 on not found.
 * field - sds field name to delete */
int hashTypeDelete(robj *o, void *field) {
    int deleted = 0;
    int fieldLen = sdslen((sds)field);

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
        /* dictDelete() will call dictEntryDestructor() */
        if (dictDelete((dict*)o->ptr, field) == C_OK) {
            deleted = 1;
        }
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP ||
               o->encoding == OBJ_ENCODING_TMPL_ARRAY)
    {
        hashTemplate *tmpl = hashTypeGetTemplate(o);
        long long idx = hashTemplateFieldIndex(tmpl, field);
        if (idx >= 0) {
            long long old_count = tmpl->field_count;
            long long new_count = old_count - 1;

            if (new_count == 0) {
                /* Last field deleted - convert to empty listpack. */
                if (o->encoding == OBJ_ENCODING_TMPL_LP)
                    hashTemplateLpFree(o->ptr);
                else
                    hashTemplateArrayFree(o->ptr);
                o->ptr = lpNew(0);
                o->encoding = OBJ_ENCODING_LISTPACK;
            } else {
                hashTemplate *new_tmpl = hashTemplateForDeletedField(tmpl, idx);

                if (o->encoding == OBJ_ENCODING_TMPL_LP) {
                    /* Delete value at idx. */
                    unsigned char *lp = o->ptr;
                    unsigned char *p = hashTemplateLpSeekValue(lp, idx);
                    lp = lpDeleteRangeWithEntry(lp, &p, 1);
                    lp = hashTemplateLpSetTemplate(lp, new_tmpl);
                    o->ptr = lp;
                } else {
                    hashTemplateArray *hta = o->ptr;
                    sdsfree(hta->values[idx]);
                    memmove(&hta->values[idx], &hta->values[idx + 1],
                            sizeof(sds) * (old_count - idx - 1));
                    hta->tmpl_id = new_tmpl->id;
                    hta->field_count = new_count;
                    hta = zrealloc(hta, sizeof(*hta) + sizeof(sds) * new_count);
                    o->ptr = hta;
                }
                hashTemplateDecrKeyRef(tmpl);
            }
            deleted = 1;
        }
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
    /* If expired field access is allowed, don't subtract expired fields from the count.
     * Check subtractExpiredFields first so that populateDeltaHistograms(), 
     * which reaches here with subtractExpiredFields==0 from a BIO thread, never
     * reads server.allow_access_expired (TSAN complains about it). */
    if (subtractExpiredFields && server.allow_access_expired)
        subtractExpiredFields = 0;

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
            htMetadataEx *meta = htGetMetadataEx(d);
            /* If dict registered in global HFE DS */
            if (meta->expireMeta.trash == 0)
                expiredItems = ebExpireDryRun(meta->hfe,
                                              &hashFieldExpireBucketsType,
                                              commandTimeSnapshot());
        }
        length = dictSize(d) - expiredItems;
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP) {
        /* First entry is the template ID, the rest are values. */
        unsigned long n = lpLength(o->ptr);
        serverAssert(n >= 1);
        length = n - 1;
    } else if (o->encoding == OBJ_ENCODING_TMPL_ARRAY) {
        hashTemplateArray *hta = o->ptr;
        length = hta->field_count;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return length;
}

size_t hashTypeAllocSize(const robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_HASH);
    size_t size = 0;
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        size = lpBytes(o->ptr);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        listpackEx *lpt = o->ptr;
        size = sizeof(listpackEx) + lpBytes(lpt->lp);
    } else if (o->encoding == OBJ_ENCODING_HT) {
        dict *d = o->ptr;
        size += sizeof(dict) + dictMemUsage(d) + *htGetMetadataSize(d);
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP) {
        unsigned char *lp = o->ptr;
        size = lpBytes(lp);
    } else if (o->encoding == OBJ_ENCODING_TMPL_ARRAY) {
        hashTemplateArray *hta = o->ptr;
        size = sizeof(hashTemplateArray) + sizeof(sds) * hta->field_count;
        for (unsigned long long i = 0; i < hta->field_count; i++) {
            if (hta->values[i]) size += sdsAllocSize(hta->values[i]);
        }
    } else {
        serverPanic("Unknown hash encoding");
    }
    return size;
}

/* Per-key share of the template's fixed memory usage, split across referencing keys.
 * MEMORY USAGE only, deliberately NOT part of kvobjAllocSize: per-slot/db
 * alloc-size stats add a key's size on insert and subtract it on delete, and
 * assume it only changes when that key itself is touched. This share changes
 * when OTHER keys drop/add the same template (key_refcount moves) with no hook
 * updating this key, so folding it into kvobjAllocSize would drift the slot
 * totals and trigger assert in dbgAssertAllocSizePerSlot. */
size_t hashTemplatePerKeyMemoryShare(const robj *o) {
    hashTemplate *tmpl = hashTypeGetTemplate((robj *)o);
    serverAssert(tmpl->key_refcount > 0);
    return tmpl->mem_size / tmpl->key_refcount;
}

void hashTypeInitIterator(hashTypeIterator *hi, robj *subject) {
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
        dictInitIterator(&hi->di, subject->ptr);
    } else if (hi->encoding == OBJ_ENCODING_TMPL_LP ||
               hi->encoding == OBJ_ENCODING_TMPL_ARRAY)
    {
        hi->field_index = -1;  /* Not started yet. */
        hi->vptr = NULL;
        hi->expire_time = EB_EXPIRE_TIME_INVALID;
        hi->tmpl = hashTypeGetTemplate(subject);
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void hashTypeResetIterator(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_HT)
        dictResetIterator(&hi->di);
}

/* Move to the next entry in the hash. Return C_OK when the next entry
 * could be found and C_ERR when the iterator reaches the end. */
int hashTypeNext(hashTypeIterator *hi, int skipExpiredFields) {
    /* If expired field access is allowed, don't skip expired fields during iteration */
    if (server.allow_access_expired)
        skipExpiredFields = 0;

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

        while ((hi->de = dictNext(&hi->di)) != NULL) {
            Entry *e = dictGetKey(hi->de);
            hi->expire_time = entryGetExpiry(e);
            /* this condition still valid if expire_time equals EB_EXPIRE_TIME_INVALID */
            if (skipExpiredFields && ((mstime_t)hi->expire_time < commandTimeSnapshot()))
                continue;
            return C_OK;
        }
        return C_ERR;
    } else if (hi->encoding == OBJ_ENCODING_TMPL_LP) {
        unsigned char *lp = hi->subject->ptr;

        /* Advance to next field. lpNext returning NULL signals end. */
        hi->field_index++;
        hi->vptr = (hi->field_index == 0) ?
                   hashTemplateLpFirstValue(lp) : lpNext(lp, hi->vptr);

        unsigned long long idx = hi->field_index;
        serverAssert((hi->vptr && idx < hi->tmpl->field_count) ||
                     (!hi->vptr && idx == hi->tmpl->field_count));

        if (!hi->vptr) return C_ERR;
    } else if (hi->encoding == OBJ_ENCODING_TMPL_ARRAY) {
        /* Advance to next field. */
        hi->field_index++;
        if ((unsigned long long)hi->field_index >= hi->tmpl->field_count)
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
                                 size_t *vlen,
                                 long long *vll,
                                 uint64_t *expireTime)
{
    serverAssert(hi->encoding == OBJ_ENCODING_LISTPACK ||
                 hi->encoding == OBJ_ENCODING_LISTPACK_EX);

    unsigned int lplen = 0;
    if (what & OBJ_HASH_KEY) {
        *vstr = lpGetValue(hi->fptr, &lplen, vll);
    } else {
        *vstr = lpGetValue(hi->vptr, &lplen, vll);
    }
    *vlen = lplen;

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
    Entry *e = dictGetKey(hi->de);

    if (what & OBJ_HASH_KEY) {
        sds field = entryGetField(e);
        *str = field;
        *len = sdslen(field);
    } else {
        sds val = entryGetValue(e);
        *str = val;
        *len = sdslen(val);
    }

    if (expireTime)
        *expireTime = hi->expire_time;
}

/* Get the field or value at iterator cursor */
void hashTypeCurrentFromTmplLp(hashTypeIterator *hi, int what,
                               unsigned char **vstr,
                               size_t *vlen,
                               long long *vll,
                               uint64_t *expireTime)
{
    serverAssert(hi->encoding == OBJ_ENCODING_TMPL_LP);

    if (what & OBJ_HASH_KEY) {
        sds field = hi->tmpl->fields[hi->field_index];
        *vstr = (unsigned char*) field;
        *vlen = sdslen(field);
    } else {
        unsigned int lplen = 0;
        *vstr = lpGetValue(hi->vptr, &lplen, vll);
        *vlen = lplen;
    }

    if (expireTime)
        *expireTime = EB_EXPIRE_TIME_INVALID;
}

/* Get the field or value at iterator cursor */
void hashTypeCurrentFromTmplArray(hashTypeIterator *hi, int what,
                                  char **str, size_t *len,
                                  uint64_t *expireTime)
{
    serverAssert(hi->encoding == OBJ_ENCODING_TMPL_ARRAY);

    if (what & OBJ_HASH_KEY) {
        sds field = hi->tmpl->fields[hi->field_index];
        *str = field;
        *len = sdslen(field);
    } else {
        hashTemplateArray *hta = hi->subject->ptr;
        sds val = hta->values[hi->field_index];
        *str = val;
        *len = sdslen(val);
    }

    if (expireTime)
        *expireTime = EB_EXPIRE_TIME_INVALID;
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
                           size_t *vlen,
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
    } else if (hi->encoding == OBJ_ENCODING_TMPL_LP) {
        hashTypeCurrentFromTmplLp(hi, what, vstr, vlen, vll, expireTime);
    } else if (hi->encoding == OBJ_ENCODING_TMPL_ARRAY) {
        char *ele;
        size_t eleLen;
        hashTypeCurrentFromTmplArray(hi, what, &ele, &eleLen, expireTime);
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
    size_t vlen;
    long long vll;

    hashTypeCurrentObject(hi,what,&vstr,&vlen,&vll, NULL);
    if (vstr) return sdsnewlen(vstr,vlen);
    return sdsfromlonglong(vll);
}

/* Return the key at the current iterator position as a new entry. */
Entry *hashTypeCurrentObjectNewEntry(hashTypeIterator *hi, size_t *usable) {
    char fieldBuf[LONG_STR_SIZE], valueBuf[LONG_STR_SIZE];
    unsigned char *fieldStr, *valueStr;
    size_t fieldLen, valueLen;
    long long fieldLl, valueLl;
    Entry *entry;

    /* Get field */
    hashTypeCurrentObject(hi, OBJ_HASH_KEY, &fieldStr, &fieldLen, &fieldLl, NULL);
    if (!fieldStr) {
        fieldLen = ll2string(fieldBuf, sizeof(fieldBuf), fieldLl);
        fieldStr = (unsigned char *) fieldBuf;
    }
    sds field = sdsnewlen(fieldStr, fieldLen);

    /* Get value */
    hashTypeCurrentObject(hi, OBJ_HASH_VALUE, &valueStr, &valueLen, &valueLl, NULL);
    if (!valueStr) {
        valueLen = ll2string(valueBuf, sizeof(valueBuf), valueLl);
        valueStr = (unsigned char *) valueBuf;
    }
    sds value = sdsnewlen(valueStr, valueLen);
    int hasExpiry = (hi->expire_time != EB_EXPIRE_TIME_INVALID);

    /* Create entry with field and value, using iterator's expire_time */
    uint32_t entryFlags = ENTRY_TAKE_VALUE | ((hasExpiry) ? ENTRY_HAS_EXPIRY : 0); 
    entry = entryCreate(field, value, entryFlags, usable);
    sdsfree(field);  /* entryCreate() doesn't take ownership of field */

    return entry;
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

/* Can a TMPL_LP / TMPL_ARRAY hash be re-encoded as a plain listpack? */
static int hashTypeCanConvertTmplToListpack(robj *o) {
    serverAssert(o->encoding == OBJ_ENCODING_TMPL_LP ||
                 o->encoding == OBJ_ENCODING_TMPL_ARRAY);
    hashTemplate *tmpl = hashTypeGetTemplate(o);

    if (tmpl->field_count > server.hash_max_listpack_entries)
        return 0;

    ssize_t fsum = hashTypeSdsArrayLpBytes(tmpl->fields, tmpl->field_count);
    if (fsum < 0) return 0;

    if (o->encoding == OBJ_ENCODING_TMPL_ARRAY) {
        hashTemplateArray *hta = o->ptr;
        ssize_t vsum = hashTypeSdsArrayLpBytes(hta->values, tmpl->field_count);
        if (vsum < 0) return 0;
        return lpSafeToAdd(NULL, (size_t)fsum + (size_t)vsum) ? 1 : 0;
    }

    /* TMPL_LP: values already live in the listpack blob. */
    return lpSafeToAdd(NULL, lpBytes((unsigned char *)o->ptr) + (size_t)fsum) ? 1 : 0;
}

/* Convert a TMPL_LP / TMPL_ARRAY hash to LISTPACK, LISTPACK_EX or HT. Fields
 * and values are read through the generic hash iterator, which already
 * abstracts over both template encodings, so one body covers every
 * source/target pair. 'with_hfe' implies 'HT with hfe'. */
static void hashTypeConvertTmplToListpackOrHT(robj *o, int lp_enc, int with_hfe) {
    serverAssert(o->encoding == OBJ_ENCODING_TMPL_LP ||
                 o->encoding == OBJ_ENCODING_TMPL_ARRAY);
    int target_enc = hashTypeCanConvertTmplToListpack(o) ? lp_enc : OBJ_ENCODING_HT;
    int src_enc = o->encoding;
    void *old_ptr = o->ptr;

    hashTypeIterator hi;
    hashTypeInitIterator(&hi, o);

    void *new_ptr;
    if (target_enc == OBJ_ENCODING_HT) {
        dict *d = dictCreate(with_hfe ? &entryHashDictTypeWithHFE
                                      : &entryHashDictType);
        dictExpand(d, hashTypeLength(o, 0));

        size_t usable, *alloc_size;
        if (with_hfe) {
            htMetadataEx *meta = htGetMetadataEx(d);
            meta->hfe = ebCreate();
            meta->expireMeta.trash = 1;
            alloc_size = &meta->alloc_size;
        } else {
            alloc_size = htGetMetadataSize(d);
        }

        while (hashTypeNext(&hi, 0) != C_ERR) {
            Entry *entry = hashTypeCurrentObjectNewEntry(&hi, &usable);
            int ret = dictAdd(d, entry, NULL);
            serverAssert(ret == DICT_OK);
            *alloc_size += usable;
        }
        new_ptr = d;
    } else {
        /* LISTPACK or LISTPACK_EX: rebuild field/value pairs */
        int ex = (target_enc == OBJ_ENCODING_LISTPACK_EX);
        unsigned char *new_lp = lpNew(0);
        while (hashTypeNext(&hi, 0) != C_ERR) {
            unsigned char *vstr;
            size_t vlen;
            long long vll;

            hashTypeCurrentObject(&hi, OBJ_HASH_KEY, &vstr, &vlen, &vll, NULL);
            new_lp = lpAppend(new_lp, vstr, vlen);

            hashTypeCurrentObject(&hi, OBJ_HASH_VALUE, &vstr, &vlen, &vll, NULL);
            if (vstr)
                new_lp = lpAppend(new_lp, vstr, vlen);
            else
                new_lp = lpAppendInteger(new_lp, vll);

            if (ex)
                new_lp = lpAppendInteger(new_lp, HASH_LP_NO_TTL);
        }
        if (ex) {
            listpackEx *lpt = listpackExCreate();
            lpt->lp = new_lp;
            new_ptr = lpt;
        } else {
            new_ptr = new_lp;
        }
    }

    hashTypeResetIterator(&hi);

    /* Release the source template structure (drops its template ref). */
    if (src_enc == OBJ_ENCODING_TMPL_LP)
        hashTemplateLpFree(old_ptr);
    else
        hashTemplateArrayFree(old_ptr);

    o->ptr = new_ptr;
    o->encoding = target_enc;
}

/* TMPL_LP -> TMPL_ARRAY */
static void hashTypeConvertTmplLpToArray(robj *o) {
    serverAssert(o->encoding == OBJ_ENCODING_TMPL_LP);

    unsigned char *lp = o->ptr;
    hashTemplate *tmpl = hashTemplateLpGetTemplate(lp);
    unsigned long long field_count = tmpl->field_count;

    sds *values = zmalloc(sizeof(sds) * field_count);
    unsigned char *p = hashTemplateLpFirstValue(lp);
    for (unsigned long long i = 0; i < field_count; i++) {
        serverAssert(p != NULL); /* value entry always present per field */
        unsigned int vlen;
        long long vll;
        unsigned char *vstr = lpGetValue(p, &vlen, &vll);
        if (vstr)
            values[i] = sdsnewlen(vstr, vlen);
        else
            values[i] = sdsfromlonglong(vll);
        p = lpNext(lp, p);
    }
    serverAssert(p == NULL); /* no values beyond field_count */

    hashTemplateArray *hta = hashTemplateArrayCreate(tmpl, values, 1);
    zfree(values);
    hashTemplateLpFree(lp);

    o->encoding = OBJ_ENCODING_TMPL_ARRAY;
    o->ptr = hta;
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
        hashTypeIterator hi;
        dict *dict;
        int ret;

        hashTypeInitIterator(&hi, o);
        dict = dictCreate(&entryHashDictType);

        /* Presize the dict to avoid rehashing */
        dictExpand(dict,hashTypeLength(o, 0));

        size_t usable, *alloc_size = htGetMetadataSize(dict);
        while (hashTypeNext(&hi, 0) != C_ERR) {
            Entry *entry = hashTypeCurrentObjectNewEntry(&hi, &usable);
            ret = dictAdd(dict, entry, NULL);
            if (ret != DICT_OK) {
                entryFree(entry, NULL); /* Needed for gcc ASAN */
                hashTypeResetIterator(&hi);  /* Needed for gcc ASAN */
                serverLogHexDump(LL_WARNING,"listpack with dup elements dump",
                    o->ptr,lpBytes(o->ptr));
                serverPanic("Listpack corruption detected");
            }
            *alloc_size += usable;
        }
        hashTypeResetIterator(&hi);
        zfree(o->ptr);
        o->encoding = OBJ_ENCODING_HT;
        o->ptr = dict;
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* db can be NULL to avoid registration in subexpires */
void hashTypeConvertListpackEx(redisDb *db, robj *o, int enc) {
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK_EX);

    if (enc == OBJ_ENCODING_LISTPACK_EX) {
        return;
    } else if (enc == OBJ_ENCODING_HT) {
        uint64_t minExpire = EB_EXPIRE_TIME_INVALID;
        int ret, slot = -1;
        hashTypeIterator hi;
        dict *dict;
        htMetadataEx *dictExpireMeta;
        listpackEx *lpt = o->ptr;

        if (db && lpt->meta.trash != 1) {
            minExpire = hashTypeGetMinExpire(o, 0);
            slot = getKeySlot(kvobjGetKey(o));
            estoreRemove(db->subexpires, slot, o);
        }

        dict = dictCreate(&entryHashDictTypeWithHFE);
        dictExpand(dict,hashTypeLength(o, 0));
        dictExpireMeta = htGetMetadataEx(dict);

        /* Fillup dict HFE metadata */
        dictExpireMeta->hfe = ebCreate();     /* Allocate HFE DS */
        dictExpireMeta->expireMeta.trash = 1; /* mark as trash (as long it wasn't ebAdd()) */

        hashTypeInitIterator(&hi, o);

        size_t usable, *alloc_size = &dictExpireMeta->alloc_size;
        while (hashTypeNext(&hi, 0) != C_ERR) {
            /* Create entry with both field and value */
            Entry *entry = hashTypeCurrentObjectNewEntry(&hi, &usable);
            ret = dictAdd(dict, entry, NULL);
            if (ret != DICT_OK) {
                entryFree(entry, NULL); /* Needed for gcc ASAN */
                hashTypeResetIterator(&hi);  /* Needed for gcc ASAN */
                serverLogHexDump(LL_WARNING,"listpack with dup elements dump",
                                 lpt->lp,lpBytes(lpt->lp));
                serverPanic("Listpack corruption detected");
            }
            *alloc_size += usable;

            if (hi.expire_time != EB_EXPIRE_TIME_INVALID)
                ebAdd(&dictExpireMeta->hfe, &hashFieldExpireBucketsType, entry, hi.expire_time);
        }
        hashTypeResetIterator(&hi);
        listpackExFree(lpt);

        o->encoding = OBJ_ENCODING_HT;
        o->ptr = dict;

        if (minExpire != EB_EXPIRE_TIME_INVALID)
            estoreAdd(db->subexpires, slot, o, minExpire);
    } else {
        serverPanic("Unknown hash encoding: %d", enc);
    }
}

/* Convert TMPL_LP to target encoding. HT is not a valid direct target: a
 * template reaches HT only as the no-fit fallback inside the LISTPACK /
 * LISTPACK_EX path (see hashTypeConvertTmplToListpackOrHT). */
void hashTypeConvertTmplLp(robj *o, int enc) {
    serverAssert(o->encoding == OBJ_ENCODING_TMPL_LP);

    if (enc == OBJ_ENCODING_TMPL_LP) {
        /* Nothing to do. */
    } else if (enc == OBJ_ENCODING_LISTPACK || enc == OBJ_ENCODING_LISTPACK_EX) {
        /* LISTPACK_EX implies HFE, so the HT fallback must keep HFE metadata. */
        hashTypeConvertTmplToListpackOrHT(o, enc, enc == OBJ_ENCODING_LISTPACK_EX);
    } else if (enc == OBJ_ENCODING_TMPL_ARRAY) {
        hashTypeConvertTmplLpToArray(o);
    } else {
        serverPanic("Invalid conversion from TMPL_LP to %d", enc);
    }
}

/* Convert TMPL_ARRAY to target encoding. HT is not a valid direct target: a
 * template reaches HT only as the no-fit fallback inside the LISTPACK /
 * LISTPACK_EX path (see hashTypeConvertTmplToListpackOrHT). */
void hashTypeConvertTmplArray(robj *o, int enc) {
    serverAssert(o->encoding == OBJ_ENCODING_TMPL_ARRAY);

    if (enc == OBJ_ENCODING_TMPL_ARRAY) {
        /* Nothing to do. */
    } else if (enc == OBJ_ENCODING_LISTPACK || enc == OBJ_ENCODING_LISTPACK_EX) {
        /* LISTPACK_EX implies HFE, so the HT fallback must keep HFE metadata. */
        hashTypeConvertTmplToListpackOrHT(o, enc, enc == OBJ_ENCODING_LISTPACK_EX);
    } else {
        serverPanic("Invalid conversion from TMPL_ARRAY to %d", enc);
    }
}

void hashTypeConvert(redisDb *db, robj *o, int enc) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        hashTypeConvertListpack(o, enc);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        hashTypeConvertListpackEx(db, o, enc);
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP) {
        hashTypeConvertTmplLp(o, enc);
    } else if (o->encoding == OBJ_ENCODING_TMPL_ARRAY) {
        hashTypeConvertTmplArray(o, enc);
    } else if (o->encoding == OBJ_ENCODING_HT) {
        serverPanic("Not implemented");
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* ------------------------------------------------------------------------- *
 * RDB-load automatic template conversion
 *
 * Background: a template holds one shared copy of a hash's field names, so many
 * hashes with the same fields can drop their per-key field-name copies and point
 * at the template instead. This only pays off when a template is shared by many
 * keys; a template used by a single key costs more memory than it saves.
 *
 * While loading a whole RDB we can opportunistically turn plain hashes into
 * template-based ones. Three configs control this. They apply to RDB load
 * only (not to RESTORE/DUMP), and all of them are skipped if the RDB already
 * carries a template header:
 *
 *   - hash-rdb-load-min-template-entries: min field count for a hash to be
 *     eligible (0 = feature off, nothing is converted).
 *   - hash-rdb-load-max-template-entries: max field count (0 = no upper bound).
 *   - hash-rdb-load-template-disassembly-threshold: how many keys a template
 *     must have to be worth keeping (0 = this context off; every eligible hash is
 *     converted and kept).
 *
 * Goal: keep templates that are shared by many keys and avoid templates that end
 * up with only a few keys: such a template wastes memory instead of saving it.
 * We call a template "few-key" while its key count is below the threshold.
 *
 * The context does two things during the load:
 *   - Throttle: once few-key templates clearly dominate, stop creating new
 *     templates (hashes whose fields match an existing template still attach).
 *   - Disassemble: at end of load, turn every template still below the threshold
 *     key count back into a plain LISTPACK/HT hash.
 *
 * Why the reverse map (template -> list of the hash kvobjs using it): to undo a
 * few-key template at end of load we need its keys, but a template doesn't know
 * who points at it. Building the list as we go lets us disassemble without
 * scanning the whole keyspace. A template that reaches the threshold "graduates"
 * (leaves the map); only few-key templates stay tracked. */

/* The throttle activates only after this many few-key templates exist, so a
 * small early sample won't start it. See rdbLoadTemplateCtxShouldStopCreating(). */
#define MIN_REVERSE_LOOKUP 1000
struct rdbLoadTemplateCtx {
    dict *reverse_lookup;   /* hashTemplate* -> list of hash kvobj* (few-key
                             * templates only; entries leave on graduation). */
    size_t disassembly_threshold; /* Keep templates with >= this many keys. */
    size_t number_of_templates;   /* Templates created this load (monotonic). */
    int stop_creating;        /* One-way latch: stop creating new templates. */
};

/* A hash to disassemble at end of load, identified by db + key name. */
typedef struct rdbLoadTemplateCtxKvRef {
    redisDb *db;
    sds key;
} rdbLoadTemplateCtxKvRef;

static void rdbLoadTemplateCtxKvRefListFree(void *ref) {
    rdbLoadTemplateCtxKvRef *r = ref;
    sdsfree(r->key);
    zfree(r);
}

static void rdbLoadTemplateCtxListValDestructor(dict *d, void *val) {
    UNUSED(d);
    listRelease(val);
}

/* Keys are template pointers, values are lists of hash kvobjs freed when the 
 * entry is dropped/released. */
static dictType rdbLoadTemplateCtxReverseDictType = {
    .hashFunction = dictPtrHash,
    .valDestructor = rdbLoadTemplateCtxListValDestructor,
};

rdbLoadTemplateCtx *rdbLoadTemplateCtxCreate(size_t disassembly_threshold) {
    rdbLoadTemplateCtx *ctx = zcalloc(sizeof(*ctx));
    ctx->disassembly_threshold = disassembly_threshold;
    if (disassembly_threshold > 0)
        ctx->reverse_lookup = dictCreate(&rdbLoadTemplateCtxReverseDictType);
    return ctx;
}

/* Record 'kv' under its template so few-key templates can be reverted to plain
 * hashes at end of load. Only template hashes below the threshold are tracked:
 * the key that reaches the threshold makes tmpl worth keeping, so it leaves the
 * tracking map. */
void rdbLoadTemplateCtxRecord(rdbLoadTemplateCtx *ctx, robj *kv, redisDb *db) {
    if (ctx == NULL || ctx->reverse_lookup == NULL || kv == NULL) return;
    if (kv->type != OBJ_HASH) return;
    if (kv->encoding != OBJ_ENCODING_TMPL_LP &&
        kv->encoding != OBJ_ENCODING_TMPL_ARRAY) return;

    hashTemplate *tmpl = hashTypeGetTemplate(kv);
    if (tmpl->key_refcount == ctx->disassembly_threshold) {
        /* This key graduates tmpl: drop the tracking list so it survives. */
        dictDelete(ctx->reverse_lookup, tmpl);
        return;
    }
    if (tmpl->key_refcount > ctx->disassembly_threshold) return; /* already graduated */

    dictEntry *de = dictFind(ctx->reverse_lookup, tmpl);
    list *l;
    if (de == NULL) {
        l = listCreate();
        listSetFreeMethod(l, rdbLoadTemplateCtxKvRefListFree);
        dictAdd(ctx->reverse_lookup, tmpl, l);
    } else {
        l = dictGetVal(de);
    }
    rdbLoadTemplateCtxKvRef *ref = zmalloc(sizeof(*ref));
    ref->db = db;
    ref->key = sdsdup(kvobjGetKey(kv));
    listAddNodeTail(l, ref);
}

int rdbLoadTemplateCtxTryConvert(rdbLoadTemplateCtx *ctx, robj *o) {
    /* No context means don't convert: either we are loading a RESTORE/DUMP payload
     * or running redis-check-rdb, or the RDB already carries its own templates (the
     * context is freed the moment a template record is seen). */
    if (ctx == NULL) return 0;
    return hashTypeTryConvertToTemplate(o, server.hash_rdb_load_min_template_entries,
                                        server.hash_rdb_load_max_template_entries, ctx);
}

/* End of rdb load: disassemble every few-key template back to a plain hash.
 * Each disassembly drops a key ref; a template that loses its last key is freed
 * from the registry. */
void rdbLoadTemplateCtxDisassemble(rdbLoadTemplateCtx *ctx) {
    if (ctx == NULL || ctx->reverse_lookup == NULL) return;
    size_t disassembled_templates = 0, disassembled_keys = 0;
    dictIterator *di = dictGetIterator(ctx->reverse_lookup);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        list *l = dictGetVal(de);
        listIter li;
        listNode *ln;
        listRewind(l, &li);
        int disassembled = 0;
        while ((ln = listNext(&li)) != NULL) {
            rdbLoadTemplateCtxKvRef *ref = listNodeValue(ln);
            kvobj *o = dbFind(ref->db, ref->key);
            if (o == NULL) continue;
            size_t oldsize = kvobjAllocSize(o);
            if (o->encoding == OBJ_ENCODING_TMPL_LP)
                hashTypeConvertTmplLp(o, OBJ_ENCODING_LISTPACK);
            else if (o->encoding == OBJ_ENCODING_TMPL_ARRAY)
                hashTypeConvertTmplArray(o, OBJ_ENCODING_LISTPACK);
            else
                continue;
            disassembled_keys++;
            disassembled = 1;
            /* dbAddRDBLoad recorded the compact TMPL size into the per-slot
             * allocation-size histogram; the in-place TMPL->LISTPACK conversion
             * above cannot reach updateSlotAllocSize, so fix the histogram here
             * to keep it consistent (otherwise dbgAssertAllocSizePerSlot panics
             * and INFO reports wrong sizes). Field count is unchanged, so the
             * key-size histogram needs no fixup. */
            if (server.memory_tracking_enabled)
                updateSlotAllocSize(ref->db, getKeySlot(kvobjGetKey(o)), o,
                                    oldsize, kvobjAllocSize(o));

            /* This runs at end of load and can be long (many few-key templates).
             * Yield back to the event loop to reply -LOADING. */
            if ((disassembled_keys & 1023) == 0) {
                if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER)
                    replicationSendNewlineToMaster();
                processEventsWhileBlocked();
            }
        }
        disassembled_templates += disassembled;
    }
    dictReleaseIterator(di);
    if (disassembled_templates) {
        serverLog(LL_NOTICE,
            "Hash template conversion during RDB load: disassembled %zu templates (%zu keys) "
            "back to plain hashes; each held fewer than %zu keys "
            "(hash-rdb-load-template-disassembly-threshold config was set).",
            disassembled_templates, disassembled_keys, ctx->disassembly_threshold);
    }
}

void rdbLoadTemplateCtxFree(rdbLoadTemplateCtx *ctx) {
    if (ctx == NULL) return;
    if (ctx->reverse_lookup) dictRelease(ctx->reverse_lookup);
    zfree(ctx);
}

/* The throttle described in the rdbLoadTemplateCtx doc above: once over half of
 * all templates created so far are still few-key, stop creating new ones for the
 * rest of the load. (See MIN_REVERSE_LOOKUP.) */
static int rdbLoadTemplateCtxShouldStopCreating(rdbLoadTemplateCtx *ctx) {
    if (!ctx->stop_creating) {
        size_t few_key = dictSize(ctx->reverse_lookup);
        if (few_key > MIN_REVERSE_LOOKUP && few_key * 2 > ctx->number_of_templates) {
            ctx->stop_creating = 1;
            serverLog(LL_NOTICE,
                "Hash template creation throttled during RDB load: %zu of the %zu "
                "templates created so far hold fewer than %zu keys "
                "(hash-rdb-load-template-disassembly-threshold); the few-key ones "
                "dominate, so no new templates are created for the rest of the load.",
                few_key, ctx->number_of_templates, ctx->disassembly_threshold);
        }
    }
    return ctx->stop_creating;
}

/* Convert a hash to template encoding when its field count is within
 * [min_fields, max_fields] (min_fields 0 = disabled, max_fields 0 = no upper
 * bound). LP -> TMPL_LP, HT (no HFE) -> TMPL_ARRAY. Returns 1 if converted.
 * LP_EX, HT with HFE are left as-is.
 * 'ctx' is set only on the RDB-load path and drives the throttle/tracking for 
 * disassembly. */
int hashTypeTryConvertToTemplate(robj *o, 
                                 size_t min_fields,
                                 size_t max_fields,
                                 rdbLoadTemplateCtx *ctx) 
{
    /* min_fields == 0 means the feature is disabled (the default). */
    if (min_fields == 0) return 0;

    /* Only LP and HT (without HFE) can be converted. */
    if (o->encoding == OBJ_ENCODING_LISTPACK_EX) return 0;
    if (o->encoding == OBJ_ENCODING_TMPL_LP) return 0;
    if (o->encoding == OBJ_ENCODING_TMPL_ARRAY) return 0;
    if (o->encoding == OBJ_ENCODING_HT && isDictWithMetaHFE(o->ptr)) return 0;

    /* Check field count threshold. */
    size_t num_fields = hashTypeLength(o, 0);
    if (num_fields < min_fields) return 0;

    /* max_fields == 0 means no upper bound. */
    if (max_fields > 0 && num_fields > max_fields) return 0;

    /* Extract field/value pairs so we can sort them by field name
     * before handing them to hashTemplateGetOrCreate (which requires
     * pre-sorted fields). */
    hashTypeFvPair *pairs = zmalloc(sizeof(*pairs) * num_fields);

    hashTypeIterator hi;
    hashTypeInitIterator(&hi, o);
    size_t i = 0;
    while (hashTypeNext(&hi, 0) != C_ERR) {
        serverAssert(i < num_fields);
        pairs[i].field = hashTypeCurrentObjectNewSds(&hi, OBJ_HASH_KEY);
        /* Keep the value as a pointer into the source (no copy); it is written
         * into the new encoding below, while the source is still alive. */
        hashTypeCurrentObject(&hi, OBJ_HASH_VALUE, &pairs[i].vstr,
                              &pairs[i].vlen, &pairs[i].vll, NULL);
        i++;
    }
    hashTypeResetIterator(&hi);
    serverAssert(i == num_fields);

    qsort(pairs, num_fields, sizeof(*pairs), hashTypeFvPairCmp);

    /* Contiguous sorted field array for the template lookup/creation. */
    sds *fields = zmalloc(sizeof(sds) * num_fields);
    for (size_t j = 0; j < num_fields; j++) fields[j] = pairs[j].field;

    int ret = 0;          /* 0 = left plain (not converted) */

    /* Are we converting a plain hash to template encoding during an RDB load with
     * hash-rdb-load-template-disassembly-threshold set? */
    int rdb_load_conversion = ctx && ctx->disassembly_threshold > 0;

    hashTemplate *tmpl;
    if (!rdb_load_conversion) {
        tmpl = hashTemplateGetOrCreate(fields, num_fields);
    } else {
        /* Reuse a matching template, or create one unless the throttle has decided
         * few-key templates dominate; then leave this hash plain (a later hash
         * whose fields match an existing template still attaches to it). */
        uint64_t fhash = computeFieldsHash(fields, num_fields);
        tmpl = hashTemplateFindByFields(fhash, fields, num_fields);
        if (tmpl == NULL) {
            if (rdbLoadTemplateCtxShouldStopCreating(ctx)) goto cleanup;
            tmpl = hashTemplateCreateInternal(fhash, fields, num_fields);
            dictAdd(htemplates->by_fields, tmpl, NULL);
            ctx->number_of_templates++;
        }
    }

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        /* LP → TMPL_LP */
        unsigned char *new_lp = hashTemplateLpCreateFromPairs(tmpl, pairs, num_fields);
        zfree(o->ptr);
        o->ptr = new_lp;
        o->encoding = OBJ_ENCODING_TMPL_LP;
    } else {
        /* HT → TMPL_ARRAY */
        sds *values = zmalloc(sizeof(sds) * num_fields);
        for (size_t j = 0; j < num_fields; j++)
            values[j] = pairs[j].vstr ?
                        sdsnewlen(pairs[j].vstr, pairs[j].vlen) :
                        sdsfromlonglong(pairs[j].vll);
        hashTemplateArray *hta = hashTemplateArrayCreate(tmpl, values, 1);
        zfree(values);  /* array adopted the sds; free only the array */
        dictRelease(o->ptr);
        o->ptr = hta;
        o->encoding = OBJ_ENCODING_TMPL_ARRAY;
    }

    ret = 1;

cleanup:
    for (size_t j = 0; j < num_fields; j++) sdsfree(pairs[j].field);
    zfree(fields);
    zfree(pairs);
    return ret;
}

/* This is a helper function for the COPY command.
 * Duplicate a hash object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *hashTypeDup(kvobj *o, uint64_t *minHashExpire) {
    robj *hobj;
    hashTypeIterator hi;

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
        htMetadataEx *dictExpireMetaSrc, *dictExpireMetaDst = NULL;
        dict *d;

        /* If dict doesn't have HFE metadata, then create a new dict without it */
        if (!isDictWithMetaHFE(o->ptr)) {
            d = dictCreate(&entryHashDictType);
        } else {
            /* Create a new dict with HFE metadata */
            d = dictCreate(&entryHashDictTypeWithHFE);
            dictExpireMetaSrc = htGetMetadataEx((dict *) o->ptr);
            dictExpireMetaDst = htGetMetadataEx(d);
            dictExpireMetaDst->hfe = ebCreate();     /* Allocate HFE DS */
            dictExpireMetaDst->expireMeta.trash = 1; /* mark as trash (as long it wasn't ebAdd()) */

            /* Extract the minimum expire time of the source hash (Will be used by caller
             * to register the new hash in the global subexpires DB) */
            if (dictExpireMetaSrc->expireMeta.trash == 0)
                *minHashExpire = ebGetMetaExpTime(&dictExpireMetaSrc->expireMeta);
        }
        dictExpand(d, dictSize((const dict*)o->ptr));

        size_t usable, *alloc_size = htGetMetadataSize(d);
        hashTypeInitIterator(&hi, o);
        while (hashTypeNext(&hi, 0) != C_ERR) {
            Entry *newEntry;
            uint64_t expireTime;
            /* Extract a field-value pair from an original hash object.*/
            char *field, *value;
            size_t fieldLen, valueLen;
            hashTypeCurrentFromHashTable(&hi, OBJ_HASH_KEY, &field, &fieldLen, &expireTime);
            hashTypeCurrentFromHashTable(&hi, OBJ_HASH_VALUE, &value, &valueLen, NULL);

            /* Create new entry with field and value */
            sds newFieldSds = sdsnewlen(field, fieldLen);
            sds newValueSds = sdsnewlen(value, valueLen);
            /* Create new entry with field and value, optional expiry. */
            if (expireTime == EB_EXPIRE_TIME_INVALID) {
                newEntry = entryCreate(newFieldSds, newValueSds, 
                                       ENTRY_TAKE_VALUE, &usable);
            } else {
                newEntry = entryCreate(newFieldSds, newValueSds, 
                                       ENTRY_TAKE_VALUE | ENTRY_HAS_EXPIRY, &usable);
                ebAdd(&dictExpireMetaDst->hfe, &hashFieldExpireBucketsType, newEntry, expireTime);
            }
            sdsfree(newFieldSds); /* (Only value ownership transferred to entry) */

            /* Add entry to new hash object. */
            dictAdd(d, newEntry, NULL);  /* no_value=1, so value is NULL */
            *alloc_size += usable;
        }
        hashTypeResetIterator(&hi);

        hobj = createObject(OBJ_HASH, d);
        hobj->encoding = OBJ_ENCODING_HT;
    } else if (o->encoding == OBJ_ENCODING_TMPL_LP) {
        unsigned char *old_lp = o->ptr;
        hashTemplate *tmpl = hashTemplateLpGetTemplate(old_lp);

        /* Create new listpack copy. */
        size_t sz = lpBytes(old_lp);
        unsigned char *new_lp = zmalloc(sz);
        memcpy(new_lp, old_lp, sz);

        /* Increment refcount for new key. */
        hashTemplateIncrKeyRef(tmpl);

        hobj = createObject(OBJ_HASH, new_lp);
        hobj->encoding = OBJ_ENCODING_TMPL_LP;
    } else if (o->encoding == OBJ_ENCODING_TMPL_ARRAY) {
        hashTemplateArray *hta = o->ptr;
        unsigned long long n = hta->field_count;

        /* Create new array structure with duplicated values. */
        hashTemplateArray *new_hta = zmalloc(sizeof(*new_hta) + sizeof(sds) * n);
        new_hta->tmpl_id = hta->tmpl_id;
        new_hta->field_count = n;
        hashTemplateIncrKeyRef(hashTemplateGetById(new_hta->tmpl_id));
        for (unsigned long long i = 0; i < n; i++) {
            new_hta->values[i] = sdsdup(hta->values[i]);
        }

        hobj = createObject(OBJ_HASH, new_hta);
        hobj->encoding = OBJ_ENCODING_TMPL_ARRAY;
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
        Entry *entry = dictGetKey(de);
        sds field = entryGetField(entry);
        key->sval = (unsigned char*) field;
        key->slen = sdslen(field);
        if (val) {
            sds s = entryGetValue(entry);
            val->sval = (unsigned char*)s;
            val->slen = sdslen(s);
        }
    } else if (hashobj->encoding == OBJ_ENCODING_LISTPACK) {
        lpRandomPair(hashobj->ptr, hashsize, (listpackEntry *) key, (listpackEntry *) val, 2);
    } else if (hashobj->encoding == OBJ_ENCODING_LISTPACK_EX) {
        lpRandomPair(hashTypeListpackGetLp(hashobj), hashsize, (listpackEntry *) key,
                     (listpackEntry *) val, 3);
    } else if (hashobj->encoding == OBJ_ENCODING_TMPL_LP) {
        unsigned char *lp = hashobj->ptr;
        hashTemplate *tmpl = hashTemplateLpGetTemplate(lp);
        unsigned long long idx = randomULong() % tmpl->field_count;

        /* Get field from tmpl. */
        sds field = tmpl->fields[idx];
        key->sval = (unsigned char *)field;
        key->slen = sdslen(field);

        if (val) {
            unsigned char *p = hashTemplateLpSeekValue(lp, idx);
            unsigned int vlen;
            long long vll;
            val->sval = lpGetValue(p, &vlen, &vll);
            if (val->sval) {
                val->slen = vlen;
            } else {
                val->lval = vll;
            }
        }
    } else if (hashobj->encoding == OBJ_ENCODING_TMPL_ARRAY) {
        hashTemplateArray *hta = hashobj->ptr;
        hashTemplate *tmpl = hashTemplateArrayGetTemplate(hta);
        unsigned long long idx = randomULong() % tmpl->field_count;

        /* Get field from tmpl. */
        sds field = tmpl->fields[idx];
        key->sval = (unsigned char *)field;
        key->slen = sdslen(field);

        if (val) {
            /* Get value from array at index. */
            sds v = hta->values[idx];
            val->sval = (unsigned char *)v;
            val->slen = sdslen(v);
        }
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* Delete all expired fields from the hash and delete the hash if left empty.
 *
 * updateSubexpires - If the hash should be updated in the subexpires DB with new
 *                   expiration time in case expired fields were deleted.
 *
 * Return next Expire time of the hash
 * - 0 if hash got deleted
 * - EB_EXPIRE_TIME_INVALID if no more fields to expire
 */
uint64_t hashTypeExpire(redisDb *db, kvobj *o, uint32_t *quota, int updateSubexpires, int activeEx) {
    uint64_t noExpireLeftRes = EB_EXPIRE_TIME_INVALID;

    /* Collect expired field names for batched subkey notification.
     * Skip allocation entirely when subkey notifications are disabled. */
    fieldvec fvexpired;
    vec *vexpired = isSubkeyNotifyEnabled(NOTIFY_HASH) ?
                        fieldvecInit(&fvexpired, FIELDS_STACK_SIZE) : NULL;

    OnFieldExpireCtx onFieldExpireCtx = { .hashObj = o, .db = db, .activeEx = activeEx, .vexpired = vexpired };
    ExpireInfo info = (ExpireInfo) {
                .maxToExpire = *quota,
                .now = commandTimeSnapshot(),
                .ctx = &onFieldExpireCtx,
                .itemsExpired = 0};

    if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        listpackExExpire(db, o, &info);
    } else {
        serverAssert(o->encoding == OBJ_ENCODING_HT);

        dict *d = o->ptr;
        htMetadataEx *dictExpireMeta = htGetMetadataEx(d);

        info.onExpireItem = onFieldExpire;
        ebExpire(&dictExpireMeta->hfe, &hashFieldExpireBucketsType, &info);
    }

    /* Update quota left */
    *quota -= info.itemsExpired;

    /* In some cases, a field might have been deleted without updating the global DS.
     * As a result, active-expire might not expire any fields, in such cases,
     * we don't need to send notifications or perform other operations for this key. */
    if (info.itemsExpired) {
        sds keystr = kvobjGetKey(o);
        robj *key = createStringObject(keystr, sdslen(keystr));

        /* Send subkey notification with all expired fields */
        notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hexpired", key, db->id,
            vexpired ? (robj**)vecData(vexpired) : NULL, vexpired ? vecSize(vexpired) : 0);

        int slot;
        int deleted = 0;

        if (updateSubexpires) {
            slot = getKeySlot(keystr);
            estoreRemove(db->subexpires, slot, o);
        }

        if (hashTypeLength(o, 0) == 0) {
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, db->id);
            dbDelete(db, key);
            noExpireLeftRes = 0;
            deleted = 1;
        } else {
            if ((updateSubexpires) && (info.nextExpireTime != EB_EXPIRE_TIME_INVALID))
                estoreAdd(db->subexpires, slot, o, info.nextExpireTime);
        }

        keyModified(NULL, db, key, deleted ? NULL : o, 1);
        decrRefCount(key);
    }

    /* Free collected expired fields */
    if (vexpired) {
        for (size_t i = 0; i < vecSize(vexpired); i++) {
            decrRefCount(vecGet(vexpired, i));
        }
        vecRelease(vexpired);
    }

    /* return 0 if hash got deleted, EB_EXPIRE_TIME_INVALID if no more fields
     * with expiration. Else return next expiration time */
    return (info.nextExpireTime == EB_EXPIRE_TIME_INVALID) ? noExpireLeftRes : info.nextExpireTime;
}

/* Delete all expired fields in hash if needed (Currently used only by HRANDFIELD)
 *
 * NOTICE: If we call this function in other places, we should consider the slot
 * migration scenario, where we don't want to delete expired fields. See also
 * expireIfNeeded().
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
    uint32_t quota = UINT32_MAX;
    nextExpireTime = hashTypeExpire(db, o, &quota, 1, 0);
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

    /* TMPL_* encodings don't support hash field expiration. */
    if (o->encoding == OBJ_ENCODING_TMPL_LP ||
        o->encoding == OBJ_ENCODING_TMPL_ARRAY) {
        return EB_EXPIRE_TIME_INVALID;
    }

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

            expireMeta = &htGetMetadataEx(d)->expireMeta;
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

        htMetadataEx *expireMeta = htGetMetadataEx(d);
        return ebGetNextTimeToExpire(expireMeta->hfe, &hashFieldExpireBucketsType);
    }
}

int hashTypeIsFieldsWithExpire(robj *o) {
    /* TMPL_* and plain LISTPACK encodings never carry field expiration (HFE
     * forces a conversion away from these before any TTL is set). */
    if (o->encoding == OBJ_ENCODING_TMPL_LP ||
        o->encoding == OBJ_ENCODING_TMPL_ARRAY ||
        o->encoding == OBJ_ENCODING_LISTPACK) {
        return 0;
    } else if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
        return EB_EXPIRE_TIME_INVALID != listpackExGetMinExpire(o);
    } else { /* o->encoding == OBJ_ENCODING_HT */
        dict *d = o->ptr;
        /* If dict doesn't holds HFE metadata */
        if (!isDictWithMetaHFE(d))
            return 0;
        htMetadataEx *meta = htGetMetadataEx(d);
        return ebGetTotalItems(meta->hfe, &hashFieldExpireBucketsType) != 0;
    }
}

void hashTypeFree(robj *o) {
    switch (o->encoding) {
        case OBJ_ENCODING_HT:
            /* Verify hash is not registered in global HFE ds */
            if (isDictWithMetaHFE((dict*)o->ptr)) {
                htMetadataEx *m = htGetMetadataEx((dict*)o->ptr);
                serverAssert(m->expireMeta.trash == 1);
            }
#ifdef DEBUG_ASSERTIONS
            dictEmpty(o->ptr, NULL);
            debugServerAssert(*htGetMetadataSize(o->ptr) == 0);
#endif
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
        case OBJ_ENCODING_TMPL_LP:
            hashTemplateLpFree(o->ptr);
            break;
        case OBJ_ENCODING_TMPL_ARRAY:
            hashTemplateArrayFree(o->ptr);
            break;
        default:
            serverPanic("Unknown hash encoding type");
            break;
    }
}

ebuckets *hashTypeGetDictMetaHFE(dict *d) {
    htMetadataEx *dictExpireMeta = htGetMetadataEx(d);
    return &dictExpireMeta->hfe;
}

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

void hsetnxCommand(client *c) {
    unsigned long hlen;
    int isHashDeleted;
    size_t oldsize = 0;
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

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(kv);
    hashTypeTryConversion(c->db, kv, c->argv, 2, 3);
    hashTypeSet(c->db, kv, c->argv[2]->ptr, c->argv[3]->ptr, HASH_SET_COPY);
    addReply(c, shared.cone);
    keyModified(c,c->db,c->argv[1], kv, 1);
    hlen = hashTypeLength(kv, 0);
    updateKeysizesHist(c->db, OBJ_HASH, hlen - 1, hlen);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), kv, oldsize, kvobjAllocSize(kv));
    notifyKeyspaceEventWithSubkeys(NOTIFY_HASH,"hset",c->argv[1],c->db->id,&c->argv[2],1);
    KSN_INVALIDATE_KVOBJ(kv);
    server.dirty++;
}

/* Fast path for building a fresh (empty) listpack hash from a wide HSET/HMSET:
 * fills the hash from the numfields (field,value) pairs in argv in O(n), instead
 * of the per-field hashTypeSet() loop whose repeated lpFind() rescans make a wide
 * fresh build O(n^2). In-command duplicate fields resolve last-wins and field
 * order matches the per-field path (first-occurrence position), so the result is
 * byte-identical to building the hash field by field.
 *
 * Caller guarantees: o is an empty OBJ_ENCODING_LISTPACK hash and
 * hashTypeTryConversion() has already run for this command, so the result is
 * guaranteed to fit a listpack (no per-field length checks and no post-build
 * conversion needed).
 * Returns the number of fields created (unique fields, <= numfields). */
#define HSET_LP_STACK_PAIRS 128
static int hashTypeBuildFreshListpack(kvobj *o, robj **argv, int numfields) {
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK && lpLength(o->ptr) == 0);

    /* Single pass over argv: a transient dict maps each field sds -> its slot
     * index in pairs[]. The first occurrence of a field appends a new slot
     * (preserving argv order); a later duplicate reuses that slot and overwrites
     * its value (last-wins). sdsReplyDictType borrows the argv sds (no copy, no
     * free); the slot index is stored in the entry value (no allocation). The
     * unique pairs are then written with a single lpBatchAppend(). pairs[] is on
     * the stack for the common small case and heap-allocated for wider commands
     * -- matching lpBatchInsert(), which heap-allocates its own scratch for
     * large batches. */
    listpackEntry stackpairs[2 * HSET_LP_STACK_PAIRS];
    listpackEntry *pairs = (numfields <= HSET_LP_STACK_PAIRS) ? stackpairs :
                           zmalloc(sizeof(listpackEntry) * 2 * (size_t)numfields);

    /* dict: field sds -> slot index in pairs[]. First occurrence appends a slot
     * (argv order); a duplicate reuses it (last value wins). */
    dict *slots = dictCreate(&sdsReplyDictType);
    dictExpand(slots, numfields);
    int n = 0;
    for (int j = 0; j < numfields; j++) {
        sds field = argv[j*2]->ptr, value = argv[j*2+1]->ptr;
        dictEntry *existing, *de = dictAddRaw(slots, field, &existing);
        int slot;
        if (de != NULL) {                       /* new field -> new slot */
            slot = n++;
            dictSetUnsignedIntegerVal(de, slot); /* slot >= 0; the unsigned accessor is declared in dict.h */
            pairs[slot*2].sval = (unsigned char*)field;
            pairs[slot*2].slen = sdslen(field);
        } else {                                /* duplicate -> first-seen slot */
            slot = (int)dictGetUnsignedIntegerVal(existing);
        }
        pairs[slot*2+1].sval = (unsigned char*)value; /* last value wins */
        pairs[slot*2+1].slen = sdslen(value);
    }
    dictRelease(slots);

    o->ptr = lpBatchAppend(o->ptr, pairs, (unsigned long)n * 2);
    if (pairs != stackpairs) zfree(pairs);
    return n;
}

void hsetCommand(client *c) {
    int i, created = 0;
    size_t oldsize = 0;
    kvobj *kv;

    if ((c->argc % 2) == 1) {
        addReplyErrorArity(c);
        return;
    }

    if ((kv = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(kv);
    hashTypeTryConversion(c->db, kv, c->argv, 2, c->argc-1);

    int numfields = (c->argc - 2) / 2;
    /* The fresh-build fast path pays for a transient dedup dict, which only pays
     * off once the field count is large enough; 5 fields is a reasonable
     * threshold, below it the per-field loop is cheaper. */
    if (kv->encoding == OBJ_ENCODING_LISTPACK && numfields >= 5 && lpLength(kv->ptr) == 0) {
        /* Fresh wide build: single dict pass (last-wins) + one batch append. */
        created = hashTypeBuildFreshListpack(kv, c->argv + 2, numfields);
    } else {
        for (i = 2; i < c->argc; i += 2)
            created += !hashTypeSet(c->db, kv, c->argv[i]->ptr, c->argv[i+1]->ptr, HASH_SET_COPY | HASH_SET_NO_TEMPLATE_CONVERT);
    }

    /* Convert to template once after all fields are set, rather than per-field,
     * to avoid a template lookup for each field set. */
    hashTypeTryConvertToTemplate(kv, server.hash_min_template_entries,
                                 server.hash_max_template_entries, NULL);


    /* HMSET (deprecated) and HSET return value is different. */
    char *cmdname = c->argv[0]->ptr;
    if (cmdname[1] == 's' || cmdname[1] == 'S') {
        /* HSET */
        addReplyLongLong(c, created);
    } else {
        /* HMSET */
        addReply(c, shared.ok);
    }
    keyModified(c,c->db,c->argv[1],kv,1);
    unsigned long l = hashTypeLength(kv, 0);
    updateKeysizesHist(c->db, OBJ_HASH, l - created, l);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), kv, oldsize, kvobjAllocSize(kv));

    /* Collect field pointers for subkey notification. Fields are at argv[2,4,6...]. */
    fieldvec fvset;
    vec *vset = fieldvecInit(&fvset, numfields);
    for (i = 0; i < numfields; i++) {
        vecPush(vset, c->argv[2 + i * 2]);
    }
    notifyKeyspaceEventWithSubkeys(NOTIFY_HASH,"hset",c->argv[1],c->db->id,(robj**)vecData(vset),numfields);
    vecRelease(vset);
    KSN_INVALIDATE_KVOBJ(kv);
    server.dirty += (c->argc - 2)/2;
}

/* Per-client fieldset: a session-local binding from a user-chosen name to a
 * shared hashTemplate, populated by HIMPORT PREPARE and consumed by HIMPORT
 * SET. Exists to make HIMPORT SET cheap on the hot write path.
 *
 * With HIMPORT PREPARE the user hints that many keys with this same field set
 * are coming, so the server resolves them to one shared template up front and
 * stores every such key in template encoding automatically.
 *
 * HIMPORT PREPARE does the expensive work once:
 *   - sorts the field names
 *   - computes value_order (user argv index for each sorted field position)
 *   - looks up the global template registry and creates the hashTemplate if
 *     the layout is new, then takes a hold reference on it
 *   - stores name + tmpl + value_order as a himportFieldset on the client
 *
 * HIMPORT SET then just looks up the fieldset by name and writes the key
 * using the cached tmpl/value_order: no registry lookup, no field sorting,
 * no per-call allocation for the layout.
 *
 * The binding lives until HIMPORT DISCARD, HIMPORT DISCARDALL, or client
 * disconnect; the underlying hashTemplate stays in the registry while any
 * client or hash key still references it. */
typedef struct himportFieldset {
    sds name;           /* Fieldset name. */
    uint64_t tmpl_id;   /* Template that matches the fieldset. */
    int *value_order;   /* Maps tmpl index -> user argv field index (pre-computed). */
} himportFieldset;

/* Per-client HIMPORT fieldsets: name->fieldset dict + last-used cache. */
typedef struct himportFieldsets {
    dict *dict;              /* fieldset name -> himportFieldset. */
    himportFieldset *last;   /* Last-used fieldset cache, or NULL. */
    size_t mem_usage;        /* Total mem_usage of fieldsets. */
} himportFieldsets;

/* dict value destructor. */
static void himportFsDictValDestructor(dict *d, void *val) {
    UNUSED(d);
    himportFieldset *fs = val;
    sdsfree(fs->name);
    hashTemplate *fs_tmpl = hashTemplateGetById(fs->tmpl_id);
    if (fs_tmpl) hashTemplateDecrHoldRef(fs_tmpl);
    zfree(fs->value_order);
    zfree(fs);
}
static dictType himportFsDictType = {
    .hashFunction = dictSdsHash,
    .keyCompare = dictSdsKeyCompare,
    .valDestructor = himportFsDictValDestructor,
};

/* Memory usage for one fieldset: the fieldset + the full template it pins. */
static size_t himportFieldsetMemUsage(himportFieldset *fs) {
    hashTemplate *tmpl = hashTemplateGetById(fs->tmpl_id);
    serverAssert(tmpl != NULL);
    return sizeof(*fs) + sdsZmallocSize(fs->name) +
           zmalloc_size(fs->value_order) + tmpl->mem_size;
}

/* Get fieldset by name. Checks the last-used cache first. */
static himportFieldset *himportFieldsetsGet(client *c, sds name) {
    if (!c->himport_fieldsets) return NULL;
    
    himportFieldsets *fieldsets = c->himport_fieldsets;
    if (fieldsets->last && sdscmplen(fieldsets->last->name, name) == 0)
        return fieldsets->last;

    dictEntry *de = dictFind(fieldsets->dict, name);
    if (!de) return NULL;
    
    /* Cache last used and return */
    fieldsets->last = dictGetVal(de);
    return fieldsets->last;
}

/* Add or replace a fieldset. */
static void himportFieldsetsAdd(client *c, sds name, hashTemplate *tmpl, int *value_order) {
    serverAssert(value_order != NULL);

    himportFieldsets *fieldsets = c->himport_fieldsets;
    if (!fieldsets) {
        fieldsets = zcalloc(sizeof(*fieldsets));
        fieldsets->dict = dictCreate(&himportFsDictType);
        c->himport_fieldsets = fieldsets;
    }

    /* Replace any existing binding: drop its memory and cache. */
    dictEntry *old = dictUnlink(fieldsets->dict, name);
    if (old) {
        himportFieldset *ofs = dictGetVal(old);
        if (fieldsets->last == ofs) fieldsets->last = NULL;
        fieldsets->mem_usage -= himportFieldsetMemUsage(ofs);
        dictFreeUnlinkedEntry(fieldsets->dict, old);
    }

    himportFieldset *fs = zmalloc(sizeof(*fs));
    fs->name = name;
    fs->tmpl_id = tmpl->id;
    fs->value_order = value_order;
    dictAdd(fieldsets->dict, fs->name, fs);
    fieldsets->mem_usage += himportFieldsetMemUsage(fs);
}

/* Remove a fieldset by name. Returns 1 if removed, 0 if not found. */
static int himportFieldsetsRemove(client *c, sds name) {
    if (!c->himport_fieldsets) return 0;
    himportFieldsets *fieldsets = c->himport_fieldsets;

    dictEntry *de = dictUnlink(fieldsets->dict, name); 
    if (!de) return 0;

    himportFieldset *fs = dictGetVal(de);
    if (fieldsets->last == fs) fieldsets->last = NULL;
    fieldsets->mem_usage -= himportFieldsetMemUsage(fs);
    dictFreeUnlinkedEntry(fieldsets->dict, de);

    /* Free the container if this was the last fieldset. */
    if (dictSize(fieldsets->dict) == 0)
        himportFieldsetsFree(c);
    return 1;
}

/* Free client's fieldsets. Called from freeClient(). Returns the count freed. */
int64_t himportFieldsetsFree(client *c) {
    if (!c->himport_fieldsets) return 0;
    
    himportFieldsets *fieldsets = c->himport_fieldsets;
    int64_t removed = dictSize(fieldsets->dict);
    dictRelease(fieldsets->dict);
    zfree(fieldsets);
    c->himport_fieldsets = NULL;
    return removed;
}

/* Memory used by this client's HIMPORT fieldset bindings, for maxmemory-clients.
 * O(1): the per-fieldset sum is tracked incrementally in fieldsets->mem_usage. */
size_t himportFieldsetsMemOverhead(client *c) {
    himportFieldsets *fieldsets = c->himport_fieldsets;
    if (!fieldsets) return 0;
    return sizeof(*fieldsets) + dictMemUsage(fieldsets->dict) + fieldsets->mem_usage;
}

/* Context for himportCmpFieldIdx, set before qsort call. */
static robj **himport_cmp_argv = NULL;

/* Compare function for sorting field indexes by field name. */
static int himportCmpFieldIdx(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return sdscmplen(himport_cmp_argv[ia]->ptr,
                     himport_cmp_argv[ib]->ptr);
}

/* HIMPORT PREPARE <fieldset_name> <field1> [field2 ...]
 *
 * Register a named fieldset on this client so subsequent HIMPORT SET calls
 * only have to pass values (not field names). Sorts the fields in sdscmplen
 * order (by length, then bytes) and looks up / creates the matching template
 * in the registry, taking a hold reference on it. So that on HIMPORT SET, no
 * lookup will be needed. The original user-provided field order is preserved 
 * as field_order[], so HIMPORT SET can map its positional values back into 
 * template-sorted order. Rejects duplicate field names in the fieldset. */
void himportPrepareCommand(client *c) {
    sds fieldset_name = c->argv[2]->ptr;
    int field_count = c->argc - 3;
    robj **field_argv = &c->argv[3];  /* Fields start at argv[3]. */

    /* field_order maps template field index -> user argv field position. 
     * Template fields are kept sorted (sdscmplen: length first, then bytes),
     * so we sort the user's indexes by field name.
     * e.g. PREPARE fs f1 f3 f2 -> template order f1 f2 f3. */
    int *field_order = zmalloc(sizeof(int) * field_count);
    for (int i = 0; i < field_count; i++)
        field_order[i] = i;

    himport_cmp_argv = field_argv;
    qsort(field_order, field_count, sizeof(int), himportCmpFieldIdx);

    /* Build sorted fields array for template lookup. */
    sds *sorted_fields = zmalloc(sizeof(sds) * field_count);
    for (int i = 0; i < field_count; i++)
        sorted_fields[i] = field_argv[field_order[i]]->ptr;

    /* Reject duplicate field names. After sort, duplicates are adjacent. */
    for (int i = 1; i < field_count; i++) {
        if (sdscmplen(sorted_fields[i - 1], sorted_fields[i]) == 0) {
            zfree(sorted_fields);
            zfree(field_order);
            addReplyError(c, "duplicate field name in fieldset");
            return;
        }
    }

    /* Get or create template with sorted fields and hold a reference. */
    hashTemplate *tmpl = hashTemplateGetOrCreate(sorted_fields, field_count);
    hashTemplateIncrHoldRef(tmpl);
    zfree(sorted_fields);

    himportFieldsetsAdd(c, sdsdup(fieldset_name), tmpl, field_order);
    addReply(c, shared.ok);
}

/* HIMPORT SET <key> <fieldset> <value1> [value2 ...]
 *
 * Create a hash from a fieldset prepared earlier (HIMPORT PREPARE): the values
 * map positionally onto the fieldset's fields, and the key is stored directly in
 * template encoding using the fieldset's shared template. */
void himportSetCommand(client *c) {
    /* Lookup existing key. */
    dictEntryLink link;
    kvobj *existing = lookupKeyWriteWithLink(c->db, c->argv[2], &link);
    if (checkType(c, existing, OBJ_HASH)) return;

    /* Lookup fieldset. */
    sds fieldset_name = c->argv[3]->ptr;
    himportFieldset *fs = himportFieldsetsGet(c, fieldset_name);
    if (!fs) {
        addReplyError(c, "no such fieldset");
        return;
    }

    hashTemplate *tmpl = hashTemplateGetById(fs->tmpl_id);
    unsigned long long field_count = tmpl->field_count;
    int valuec = c->argc - 4;

    /* Number of values must match fieldset field count. */
    if ((unsigned long long)valuec != field_count) {
        addReplyError(c, "value count does not match fieldset field count");
        return;
    }

    /* Build values array using a stack buffer for small fieldsets.
     * Field order in template (ordered by sdscmplen()) may not match fieldset's
     * given by the user. value_order[i] maps fieldset index i to user's argv index.  */
    int *value_order = fs->value_order;
    sds stack_values[HASH_TMPL_STACK_ENTRIES];
    sds *values = (field_count <= HASH_TMPL_STACK_ENTRIES) ?
                  stack_values : zmalloc(sizeof(sds) * field_count);

    size_t total_values_length = 0;
    for (unsigned long long i = 0; i < field_count; i++) {
        values[i] = c->argv[4 + value_order[i]]->ptr;
        total_values_length += sdslen(values[i]);
    }

    robj *o = createHashObjectFromTemplate(tmpl, values, 0);
    if (values != stack_values) zfree(values);

    setKeyByLink(c, c->db, c->argv[2], &o,
                 existing ? SETKEY_ALREADY_EXIST : SETKEY_DOESNT_EXIST, &link);

    /* How to propagate this write to replicas/AOF/ASM?. Ideally we'd mirror how
     * the client does it (a HIMPORT PREPARE for the fields, then a SET), but
     * propagating the fields separately gets complex once replicas, sub-replicas
     * and atomic slot migration are in the picture. So we send the fields
     * together with the values in one command. A plain HSET-like command would
     * work, but serializing/deserializing all the fields is slow and a
     * replica may fall behind the master. Instead we send a RESTORE: it is more
     * compact (field names usually travel as one listpack blob, unless they
     * don't fit one, then they go as individual strings) and fast on the
     * serialize path, and the receiver resolves it with a
     * fields_listpack_blob -> template lookup without parsing the fields at all.
     *
     * In the future this can be improved to send the fields only once instead of
     * on every SET, lowering the propagation cost further. */
    if (shouldPropagate(PROPAGATE_AOF | PROPAGATE_REPL)) {
        /* Presize the payload: field-name footprint + value bytes (over-estimate ok). */
        sds payload = createRawDumpPayload(o, c->argv[2], c->db->id, 0,
                                           tmpl->mem_size + total_values_length);
        robj *restore_pl = createObject(OBJ_STRING, payload);
        robj *rargv[5] = {
                shared.restore,
                c->argv[2],
                shared.integers[0],
                restore_pl,
                shared.replace
        };
        alsoPropagate(c->db->id, rargv, 5, PROPAGATE_AOF | PROPAGATE_REPL);
        decrRefCount(restore_pl);
    }
    preventCommandPropagation(c);

    /* Notify keyspace listeners (HSET semantics). Build the field-name subkeys
     * only when a subkey consumer exists; the "hset" event fires either way. */
    robj stack_fobjs[HASH_TMPL_STACK_ENTRIES];
    robj *stack_fptrs[HASH_TMPL_STACK_ENTRIES];
    robj *fobjs = NULL;
    robj **fields_robj = NULL;
    int heap = 0;
    if (isSubkeyNotifyEnabled(NOTIFY_HASH)) {
        heap = field_count > HASH_TMPL_STACK_ENTRIES;
        fobjs = heap ? zmalloc(sizeof(robj) * field_count) : stack_fobjs;
        fields_robj = heap ? zmalloc(sizeof(robj *) * field_count) : stack_fptrs;
        for (unsigned long long i = 0; i < field_count; i++) {
            initStaticStringObject(fobjs[i], tmpl->fields[i]);
            fields_robj[i] = &fobjs[i];
        }
    }
    notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hset", c->argv[2], c->db->id,
                                   fields_robj, fields_robj ? (int)field_count : 0);
    if (heap) { zfree(fobjs); zfree(fields_robj); }

    server.dirty++;
    addReply(c, shared.ok);
}

/* HIMPORT DISCARD <fieldset> 
 * Remove the fieldset from this client's fieldset list. */
void himportDiscardCommand(client *c) {
    addReplyLongLong(c, himportFieldsetsRemove(c, c->argv[2]->ptr));
}

/* HIMPORT DISCARDALL 
 * Remove all fieldsets from this client. */
void himportDiscardallCommand(client *c) {
    addReplyLongLong(c, himportFieldsetsFree(c));
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

/* Command types for unified hash argument parser */
#define HASH_CMD_HGETEX 0
#define HASH_CMD_HSETEX 1

/* Parse hash field expiration command arguments for both HGETEX and HSETEX.
 * HGETEX <key> [EX seconds|PX milliseconds|EXAT unix-time-seconds|PXAT unix-time-milliseconds|PERSIST]
 *              FIELDS <numfields> field [field ...]
 * HSETEX <key> [EX seconds|PX milliseconds|EXAT unix-time-seconds|PXAT unix-time-milliseconds|KEEPTTL]
 *              [FXX|FNX] FIELDS <numfields> field value [field value ...]
 */
static int parseHashFieldExpireArgs(client *c, int *flags,
                                    long long *expire_time, int *expire_time_pos,
                                    int *first_field_pos, int *field_count,
                                    int command_type) {
    *flags = 0;
    *first_field_pos = -1;
    *field_count = -1;
    *expire_time_pos = -1;

    for (int i = 2; i < c->argc; i++) {
        if (!strcasecmp(c->argv[i]->ptr, "fields")) {
            /* Ensure only one FIELDS argument is provided */
            if (*first_field_pos != -1) {
                addReplyError(c, "FIELDS keyword specified multiple times");
                return C_ERR;
            }

            int args_per_field = (command_type == HASH_CMD_HSETEX) ? 2 : 1;
            long val;
            /* Ensure we have at least the numfields argument */
            if (i + 1 >= c->argc) {
                addReplyErrorArity(c);
                return C_ERR;
            }

            if (getRangeLongFromObjectOrReply(c, c->argv[i + 1], 1, INT_MAX, &val,
                                              "invalid number of fields") != C_OK)
                return C_ERR;

            *first_field_pos = i + 2;
            *field_count = (int) val;

            /* Validate field count based on command type */
            long long required_args = *first_field_pos + ((long long)*field_count * args_per_field);
            if (required_args > c->argc) {
                addReplyError(c, "wrong number of arguments");
                return C_ERR;
            }

            /* Skip over numfields and all field-value pairs
             * Set i to the last position of the FIELDS block, loop will increment past it */
            i = *first_field_pos + (*field_count * args_per_field) - 1;
            continue;
        } else if (!strcasecmp(c->argv[i]->ptr, "EX")) {
            if (*flags & (HFE_EX | HFE_EXAT | HFE_PX | HFE_PXAT | HFE_KEEPTTL | HFE_PERSIST))
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
            if (*flags & (HFE_EX | HFE_EXAT | HFE_PX | HFE_PXAT | HFE_KEEPTTL | HFE_PERSIST))
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
            if (*flags & (HFE_EX | HFE_EXAT | HFE_PX | HFE_PXAT | HFE_KEEPTTL | HFE_PERSIST))
                goto err_expiration;

            if (i >= c->argc - 1)
                goto err_missing_expire;

            *flags |= HFE_EXAT;
            i++;
            if (parseExpireTime(c, c->argv[i], UNIT_SECONDS, 0, expire_time) != C_OK)
                return C_ERR;

            *expire_time_pos = i;
        } else if (!strcasecmp(c->argv[i]->ptr, "PXAT")) {
            if (*flags & (HFE_EX | HFE_EXAT | HFE_PX | HFE_PXAT | HFE_KEEPTTL | HFE_PERSIST))
                goto err_expiration;

            if (i >= c->argc - 1)
                goto err_missing_expire;

            *flags |= HFE_PXAT;
            i++;
            if (parseExpireTime(c, c->argv[i], UNIT_MILLISECONDS, 0,
                                expire_time) != C_OK)
                return C_ERR;

            *expire_time_pos = i;
        } else if (command_type == HASH_CMD_HGETEX && !strcasecmp(c->argv[i]->ptr, "PERSIST")) {
            if (*flags & (HFE_EX | HFE_EXAT | HFE_PX | HFE_PXAT | HFE_PERSIST))
                goto err_expiration;
            *flags |= HFE_PERSIST;
        } else if (command_type == HASH_CMD_HSETEX && !strcasecmp(c->argv[i]->ptr, "KEEPTTL")) {
            if (*flags & (HFE_EX | HFE_EXAT | HFE_PX | HFE_PXAT | HFE_KEEPTTL))
                goto err_expiration;
            *flags |= HFE_KEEPTTL;
        } else if (command_type == HASH_CMD_HSETEX && !strcasecmp(c->argv[i]->ptr, "FXX")) {
            if (*flags & (HFE_FXX | HFE_FNX))
                goto err_condition;
            *flags |= HFE_FXX;
        } else if (command_type == HASH_CMD_HSETEX && !strcasecmp(c->argv[i]->ptr, "FNX")) {
            if (*flags & (HFE_FXX | HFE_FNX))
                goto err_condition;
            *flags |= HFE_FNX;
        } else {
            addReplyErrorFormat(c, "unknown argument: %s", (char*) c->argv[i]->ptr);
            return C_ERR;
        }
    }

    /* Ensure FIELDS is specified */
    if (*first_field_pos == -1) {
        addReplyError(c, "missing FIELDS argument");
        return C_ERR;
    }

    return C_OK;

err_missing_expire:
    addReplyError(c, "missing expire time");
    return C_ERR;
err_condition:
    addReplyError(c, "Only one of FXX or FNX arguments can be specified");
    return C_ERR;
err_expiration:
    if (command_type == HASH_CMD_HSETEX) {
        addReplyError(c, "Only one of EX, PX, EXAT, PXAT or KEEPTTL arguments can be specified");
    } else {
        addReplyError(c, "Only one of EX, PX, EXAT, PXAT or PERSIST arguments can be specified");
    }
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
    int set_expiry;
    long long expire_time = EB_EXPIRE_TIME_INVALID;
    int64_t oldlen, newlen;
    HashTypeSetEx setex;
    dictEntryLink link;
    size_t oldsize = 0;

    if (parseHashFieldExpireArgs(c, &flags, &expire_time, &expire_time_pos,
                                 &first_field_pos, &field_count, HASH_CMD_HSETEX) != C_OK)
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
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(o);

    /* Track fields for subkey notifications by event type. */
    fieldvec fvexpired, fvset, fvdeleted, fvupdated;
    vec *vexpired = fieldvecInit(&fvexpired, field_count);
    vec *vset = fieldvecInit(&fvset, field_count);
    vec *vdeleted = fieldvecInit(&fvdeleted, field_count);
    vec *vupdated = fieldvecInit(&fvupdated, field_count);

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
                            HFE_LAZY_NO_UPDATE_KEYSIZES |
                            HFE_LAZY_NO_UPDATE_ALLOCSIZES;

            GetFieldRes res = hashTypeGetValue(c->db, o, field, &vstr, &vlen, &vll, opt, NULL);
            int exists = (res == GETF_OK);
            if (res == GETF_EXPIRED) {
                vecPush(vexpired, c->argv[first_field_pos + (i * 2)]);
            }
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

        int opt = HASH_SET_COPY | HASH_SET_NO_TEMPLATE_CONVERT;
        /* If we are going to set the expiration time later, no need to discard
         * it as part of set operation now. */
        if (flags & (HFE_EX | HFE_PX | HFE_EXAT | HFE_PXAT | HFE_KEEPTTL))
            opt |= HASH_SET_KEEP_TTL;

        hashTypeSet(c->db, o, field, value, opt);
        vecPush(vset, c->argv[first_field_pos + (i * 2)]);
        /* Update the expiration time. */
        if (set_expiry) {
            int ret = hashTypeSetEx(o, field, expire_time, &setex);
            if (ret == HSETEX_OK) {
                vecPush(vupdated, c->argv[first_field_pos + (i * 2)]);
            } else if (ret == HSETEX_DELETED) {
                vecPush(vdeleted, c->argv[first_field_pos + (i * 2)]);
            }
        }
    }

    if (set_expiry)
        hashTypeSetExDone(&setex);
    
    hashTypeTryConvertToTemplate(o, server.hash_min_template_entries,
                                server.hash_max_template_entries, NULL);
    server.dirty += field_count;

    if (vecSize(vdeleted)) {
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
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldsize, kvobjAllocSize(o));
    /* Emit keyspace notifications based on field expiry, mutation, or key deletion */
    if (vecSize(vset) || vecSize(vexpired)) {
        newlen = (int64_t) hashTypeLength(o, 0); 
        keyModified(c, c->db, c->argv[1], o, 1);
        if (vecSize(vexpired)) {
            notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hexpired", c->argv[1],
                                           c->db->id, (robj**)vecData(vexpired), vecSize(vexpired));
        }
        if (vecSize(vset)) {
            notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hset", c->argv[1],
                                           c->db->id, (robj**)vecData(vset), vecSize(vset));
            if (vecSize(vdeleted)) {
                notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hdel", c->argv[1],
                                               c->db->id, (robj**)vecData(vdeleted), vecSize(vdeleted));
            } else if (vecSize(vupdated)) {
                notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hexpire", c->argv[1],
                                               c->db->id, (robj**)vecData(vupdated), vecSize(vupdated));
            }
        }
        
        KSN_INVALIDATE_KVOBJ(o);
        
        /* Key may become empty due to lazy expiry in hashTypeGetValue()
         * or the new expiration time is in the past.*/
        if (newlen == 0) {
            newlen = -1;
            /* Del key but don't update KEYSIZES. else it will decr wrong bin in histogram */
            dbDeleteSkipKeysizesUpdate(c->db, c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        }
        if (oldlen != newlen)
            updateKeysizesHist(c->db, OBJ_HASH, oldlen, newlen);
    }

    vecRelease(vexpired);
    vecRelease(vset);
    vecRelease(vdeleted);
    vecRelease(vupdated);
}

void hincrbyCommand(client *c) {
    long long value, incr, oldvalue;
    kvobj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;
    size_t oldsize = 0;

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
        updateKeysizesHist(c->db, OBJ_HASH, l, l + 1);
    } else {
        /* Field expired and in turn hash deleted. Create new one! */
        o = createHashObject();
        dbAdd(c->db,c->argv[1],&o);
        value = 0;
        updateKeysizesHist(c->db, OBJ_HASH, 0, 1);
    }

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr;
    new = sdsfromlonglong(value);
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(o);

    robj obj, *argv[2] = {c->argv[2], &obj};
    initStaticStringObject(obj, new);
    hashTypeTryConversion(c->db, o, argv, 0, 1);

    hashTypeSet(c->db, o,c->argv[2]->ptr,new,HASH_SET_TAKE_VALUE | HASH_SET_KEEP_TTL);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldsize, kvobjAllocSize(o));
    addReplyLongLong(c,value);
    keyModified(c,c->db,c->argv[1], o, 1);
    notifyKeyspaceEventWithSubkeys(NOTIFY_HASH,"hincrby",c->argv[1],c->db->id,&c->argv[2],1);
    KSN_INVALIDATE_KVOBJ(o);
    server.dirty++;
}

void hincrbyfloatCommand(client *c) {
    long double value, incr;
    long long ll;
    kvobj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;
    size_t oldsize = 0;

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
        updateKeysizesHist(c->db, OBJ_HASH, l, l + 1);
    } else {
        /* Field expired and in turn hash deleted. Create new one! */
        o = createHashObject();
        dbAdd(c->db, c->argv[1], &o);
        value = 0;
        updateKeysizesHist(c->db, OBJ_HASH, 0, 1);
    }

    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }

    char buf[MAX_LONG_DOUBLE_CHARS];
    int len = ld2string(buf,sizeof(buf),value,LD_STR_HUMAN);
    new = sdsnewlen(buf,len);
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(o);

    robj obj, *argv[2] = {c->argv[2], &obj};
    initStaticStringObject(obj, new);
    hashTypeTryConversion(c->db, o, argv, 0, 1);

    hashTypeSet(c->db, o,c->argv[2]->ptr,new,HASH_SET_TAKE_VALUE | HASH_SET_KEEP_TTL);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldsize, kvobjAllocSize(o));
    addReplyBulkCBuffer(c,buf,len);
    keyModified(c,c->db,c->argv[1],o,1);
    notifyKeyspaceEventWithSubkeys(NOTIFY_HASH,"hincrbyfloat",c->argv[1],c->db->id,&c->argv[2],1);
    KSN_INVALIDATE_KVOBJ(o);
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
    int i, deleted = 0;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HMGET should respond with a series of null bulks. */
    kvobj *o = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c,o,OBJ_HASH)) return;

    /* Track expired fields for subkey notification. */
    fieldvec fvexpired;
    vec *vexpired = fieldvecInit(&fvexpired, c->argc-2);

    addReplyArrayLen(c, c->argc-2);
    for (i = 2; i < c->argc ; i++) {
        if (!deleted) {
            res = addHashFieldToReply(c, o, c->argv[i]->ptr, HFE_LAZY_NO_NOTIFICATION);
            if (res == GETF_EXPIRED) {
                vecPush(vexpired, c->argv[i]);
            }
            deleted += (res == GETF_EXPIRED_HASH);
        } else {
            /* If hash got lazy expired since all fields are expired (o is invalid),
             * then fill the rest with trivial nulls and return. */
            addReplyNull(c);
        }
    }

    if (vecSize(vexpired)) {
        notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hexpired", c->argv[1],
                                       c->db->id, (robj**)vecData(vexpired), vecSize(vexpired));
    }
    if (deleted)
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);

    vecRelease(vexpired);
}

/* Get and delete the value of one or more fields of a given hash key.
 * HGETDEL <key> FIELDS <numfields> field1 field2 ...
 * Reply: list of the value associated with each field or nil if the field
 *        doesn’t exist.
 */
void hgetdelCommand(client *c) {
    int res = 0, hfe = 0;
    int64_t oldlen = -1; /* not exists as long as it is not set */
    long num_fields = 0;
    size_t oldsize = 0;

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
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(o);
    }

    /* Track fields for subkey notifications. */
    fieldvec fvexpired, fvdeleted;
    vec *vexpired = fieldvecInit(&fvexpired, num_fields);
    vec *vdeleted = fieldvecInit(&fvdeleted, num_fields);

    addReplyArrayLen(c, num_fields);
    for (int i = 4; i < c->argc; i++) {
        const int flags = HFE_LAZY_NO_NOTIFICATION |
                          HFE_LAZY_NO_SIGNAL |
                          HFE_LAZY_AVOID_HASH_DEL |
                          HFE_LAZY_NO_UPDATE_KEYSIZES |
                          HFE_LAZY_NO_UPDATE_ALLOCSIZES;
        res = addHashFieldToReply(c, o, c->argv[i]->ptr, flags);
        if (res == GETF_EXPIRED) {
            vecPush(vexpired, c->argv[i]);
        }
        /* Try to delete only if it's found and not expired lazily. */
        if (res == GETF_OK) {
            vecPush(vdeleted, c->argv[i]);
            serverAssert(hashTypeDelete(o, c->argv[i]->ptr) == 1);
        }
    }

    /* Return if no modification has been made. */
    if (vecSize(vexpired) == 0 && vecSize(vdeleted) == 0) {
        vecRelease(vexpired);
        vecRelease(vdeleted);
        return;
    }

    int64_t newlen = (int64_t) hashTypeLength(o, 0);
    /* del key if become empty */
    int delete_key = (newlen == 0);
    /* update new len for keysizes histogram */
    int64_t hist_newlen = delete_key ? -1 : newlen;
    if (oldlen != hist_newlen)
        updateKeysizesHist(c->db, OBJ_HASH, oldlen, hist_newlen);
    /* update memory tracking */
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldsize, kvobjAllocSize(o));
    /* is it last HFE */
    if (!delete_key && hfe && (hashTypeIsFieldsWithExpire(o) == 0))
        estoreRemove(c->db->subexpires, getKeySlot(c->argv[1]->ptr), o);
    
    keyModified(c, c->db, c->argv[1], o, 1);

    if (vecSize(vexpired)) {
        notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hexpired", c->argv[1],
                                       c->db->id, (robj**)vecData(vexpired), vecSize(vexpired));
    }
    if (vecSize(vdeleted)) {
        notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hdel", c->argv[1],
                                       c->db->id, (robj**)vecData(vdeleted), vecSize(vdeleted));
        server.dirty += vecSize(vdeleted);

        /* Propagate as HDEL command.
         * Orig: HGETDEL <key> FIELDS <numfields> field1 field2 ...
         * Repl: HDEL <key> field1 field2 ... */
        rewriteClientCommandArgument(c, 0, shared.hdel);
        rewriteClientCommandArgument(c, 2, NULL);  /* Delete FIELDS arg */
        rewriteClientCommandArgument(c, 2, NULL);  /* Delete <numfields> arg */
    }

    vecRelease(vexpired);
    vecRelease(vdeleted);
    KSN_INVALIDATE_KVOBJ(o);

    /* Key may have become empty because of deleting fields or lazy expire. */
    if (delete_key) {
        /* Del key but don't update KEYSIZES. else it will decr wrong bin in histogram */
        dbDeleteSkipKeysizesUpdate(c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    }
}

/* Get the value of one or more fields of a given hash key and optionally set 
 * their expiration.
 *
 * HGETEX <key>
 *   [EX seconds | PX milliseconds | EXAT unix-time-seconds | PXAT unix-time-milliseconds | PERSIST]
 *   FIELDS <numfields> field1 field2 ...
 *
 * Reply: list of the value associated with each field or nil if the field
 *        doesn’t exist.
 */
void hgetexCommand(client *c) {
    int parse_flags = 0, expire_time_pos = -1, first_field_pos = -1, num_fields = -1;
    long long expire_time = 0;
    int64_t oldlen = 0, newlen = -1;
    HashTypeSetEx setex;
    size_t oldsize = 0;

    kvobj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_HASH))
        return;

    /* Parse arguments using flexible parser */
    if (parseHashFieldExpireArgs(c, &parse_flags, &expire_time, &expire_time_pos, &first_field_pos, &num_fields, HASH_CMD_HGETEX) != C_OK)
        return;

    /* Non-existing keys and empty hashes are the same thing. Reply null if the
     * key does not exist.*/
    if (!o) {
        addReplyArrayLen(c, num_fields);
        for (int i = 0; i < num_fields; i++)
            addReplyNull(c);
        return;
    }

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(o);
    oldlen = hashTypeLength(o, 0);
    if (parse_flags)
        hashTypeSetExInit(c->argv[1], o, c, c->db, 0, &setex);

    /* Track fields for subkey notifications by event type. */
    fieldvec fvexpired, fvdeleted, fvupdated;
    vec *vexpired = fieldvecInit(&fvexpired, num_fields);
    vec *vdeleted = fieldvecInit(&fvdeleted, num_fields);
    vec *vupdated = fieldvecInit(&fvupdated, num_fields);

    addReplyArrayLen(c, num_fields);
    for (int i = first_field_pos; i < first_field_pos + num_fields; i++) {
        const int flags = HFE_LAZY_NO_NOTIFICATION |
                          HFE_LAZY_NO_SIGNAL |
                          HFE_LAZY_AVOID_HASH_DEL |
                          HFE_LAZY_NO_UPDATE_KEYSIZES |
                          HFE_LAZY_NO_UPDATE_ALLOCSIZES;
        sds field = c->argv[i]->ptr;
        int res = addHashFieldToReply(c, o, c->argv[i]->ptr, flags);
        if (res == GETF_EXPIRED) {
            vecPush(vexpired, c->argv[i]);
        }

        /* Set expiration only if the field exists and not expired lazily. */
        if (res == GETF_OK && parse_flags) {
            if (parse_flags & HFE_PERSIST)
                expire_time = EB_EXPIRE_TIME_INVALID;

            res = hashTypeSetEx(o, field, expire_time, &setex);
            if (res == HSETEX_DELETED) {
                vecPush(vdeleted, c->argv[i]);
            } else if (res == HSETEX_OK) {
                vecPush(vupdated, c->argv[i]);
            }
        }
    }

    if (parse_flags)
        hashTypeSetExDone(&setex);

    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldsize, kvobjAllocSize(o));

    /* Exit early if no modification has been made. */
    if (vecSize(vexpired) == 0 && vecSize(vdeleted) == 0 && vecSize(vupdated) == 0) {
        vecRelease(vexpired);
        vecRelease(vdeleted);
        vecRelease(vupdated);
        return;
    }

    server.dirty += vecSize(vdeleted) + vecSize(vupdated);
    keyModified(c, c->db, c->argv[1], o, 1);

    /* This command will never be propagated as it is. It will be propagated as
     * HDELs when fields are lazily expired or deleted, if the new timestamp is
     * in the past. HDEL's will be emitted as part of addHashFieldToReply()
     * or hashTypeSetEx() in this case.
     *
     * If PERSIST flags is used, it will be propagated as HPERSIST command.
     * IF EX/EXAT/PX/PXAT flags are used, it will be replicated as HPEXPRITEAT.
     */
    if (vecSize(vexpired)) {
        notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hexpired", c->argv[1],
                                       c->db->id, (robj**)vecData(vexpired), vecSize(vexpired));
    }
    if (vecSize(vupdated)) {
        /* Build canonical command for propagation */
        int canonical_argc;
        robj **canonical_argv;
        int idx = 0;

        if (parse_flags & HFE_PERSIST) {
            notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hpersist", c->argv[1],
                                           c->db->id, (robj**)vecData(vupdated), vecSize(vupdated));
            /* Build canonical HPERSIST command: HPERSIST key FIELDS numfields field1 field2 ... */
            canonical_argc = 4 + num_fields;
            canonical_argv = zmalloc(sizeof(robj*) * canonical_argc);
            canonical_argv[idx++] = shared.hpersist;
            incrRefCount(shared.hpersist);
            canonical_argv[idx++] = c->argv[1]; /* key */
            incrRefCount(c->argv[1]);
        } else {
            notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hexpire", c->argv[1],
                                           c->db->id, (robj**)vecData(vupdated), vecSize(vupdated));
            /* Build canonical HPEXPIREAT command: HPEXPIREAT key timestamp FIELDS numfields field1 field2 ... */
            canonical_argc = 5 + num_fields;
            canonical_argv = zmalloc(sizeof(robj*) * canonical_argc);
            canonical_argv[idx++] = shared.hpexpireat;
            incrRefCount(shared.hpexpireat);
            canonical_argv[idx++] = c->argv[1]; /* key */
            incrRefCount(c->argv[1]);
            canonical_argv[idx++] = createStringObjectFromLongLong(expire_time); /* timestamp */
        }

        canonical_argv[idx++] = shared.fields;
        incrRefCount(shared.fields);
        canonical_argv[idx++] = createStringObjectFromLongLong(num_fields);
        for (int i = 0; i < num_fields; i++) {
            canonical_argv[idx++] = c->argv[first_field_pos + i];
            incrRefCount(c->argv[first_field_pos + i]);
        }

        replaceClientCommandVector(c, canonical_argc, canonical_argv);
    } else if (vecSize(vdeleted)) {
        /* If we are here, fields are deleted because new timestamp was in the
         * past. HDELs are already propagated as part of hashTypeSetEx(). */
        notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hdel", c->argv[1],
                                       c->db->id, (robj**)vecData(vdeleted), vecSize(vdeleted));
        preventCommandPropagation(c);
    }

    vecRelease(vexpired);
    vecRelease(vdeleted);
    vecRelease(vupdated);

    /* Key may become empty due to lazy expiry in addHashFieldToReply()
     * or the new expiration time is in the past.*/
    newlen = hashTypeLength(o, 0);

    updateKeysizesHist(c->db, OBJ_HASH, oldlen, newlen);
    if (newlen == 0) {
        dbDelete(c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    }
}

void hdelCommand(client *c) {
    kvobj *o;
    int j, keyremoved = 0;
    size_t oldsize = 0;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    int64_t oldLen = (int64_t) hashTypeLength(o, 0);
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(o);

    /* Hash field expiration is optimized to avoid frequent update global HFE DS for
     * each field deletion. Eventually active-expiration will run and update or remove
     * the hash from global HFE DS gracefully. Nevertheless, statistic "subexpiry"
     * might reflect wrong number of hashes with HFE to the user if it is the last
     * field with expiration. The following logic checks if this is indeed the last
     * field with expiration and removes it from global HFE DS. */
    int isHFE = hashTypeIsFieldsWithExpire(o);

    /* Track which fields were actually deleted for subkey notification. */
    fieldvec fvdeleted;
    vec *vdeleted = fieldvecInit(&fvdeleted, c->argc - 2);

    if (o->encoding == OBJ_ENCODING_HT)
        dictPauseAutoResize((dict*)o->ptr);
    for (j = 2; j < c->argc; j++) {
        if (hashTypeDelete(o,c->argv[j]->ptr)) {
            vecPush(vdeleted, c->argv[j]);
            if (hashTypeLength(o, 0) == 0) {
                keyremoved = 1;
                break;
            }
        }
    }
    
    if (!keyremoved && o->encoding == OBJ_ENCODING_HT) {
        dictResumeAutoResize((dict*)o->ptr);
        dictShrinkIfNeeded((dict*)o->ptr);
    }
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldsize, kvobjAllocSize(o));
    if (vecSize(vdeleted)) {
        /* Update keysizes histogram */
        int64_t newLen = (int64_t) hashTypeLength(o, 0);
        updateKeysizesHist(c->db, OBJ_HASH, oldLen, keyremoved ? -1 : newLen);
        
        if (keyremoved) {
            /* del key but don't update KEYSIZES. Else it will decr wrong bin in histogram */
            dbDeleteSkipKeysizesUpdate(c->db, c->argv[1]);
        } else {
            /* is it last HFE */
            if (isHFE && (hashTypeIsFieldsWithExpire(o) == 0))
                estoreRemove(c->db->subexpires, getKeySlot(c->argv[1]->ptr), o);
        }

        /* Signal key modification */
        keyModified(c, c->db, c->argv[1], keyremoved ? NULL : o, 1);
        notifyKeyspaceEventWithSubkeys(NOTIFY_HASH,"hdel",c->argv[1],c->db->id,(robj**)vecData(vdeleted),vecSize(vdeleted));
        
        KSN_INVALIDATE_KVOBJ(o); /* Invalidate local kvobj pointer */
        
        /* Notify del event if key was deleted */
        if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty += vecSize(vdeleted);
    }
    addReplyLongLong(c,vecSize(vdeleted));
    vecRelease(vdeleted);
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
    unsigned char *vstr = NULL;
    size_t vlen = 0;
    long long vll = LLONG_MAX;

    hashTypeCurrentObject(hi, what, &vstr, &vlen, &vll, NULL);
    if (vstr)
        addReplyBulkCBuffer(c, vstr, vlen);
    else
        addReplyBulkLongLong(c, vll);
}

void genericHgetallCommand(client *c, int flags) {
    kvobj *o;
    hashTypeIterator hi;
    int length, count = 0;
    size_t oldsize = 0;

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

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(o);

    /* Fast path: batched prefetch for hashtable-encoded HGETALL.
     * Collect a batch of dict entries, prefetch their Entry structs and
     * value SDS data, then emit replies while the data is cache-warm.
     * This hides the latency of pointer chasing through scattered
     * heap allocations (dictEntry → Entry → value SDS). */
#define HGETALL_BATCH 16
    if (o->encoding == OBJ_ENCODING_HT) {
        int skip_expired = !server.allow_access_expired;
        dict *d = o->ptr;
        dictIterator di;
        dictInitSafeIterator(&di, d);
        Entry *batch_entry[HGETALL_BATCH];
        sds batch_val[HGETALL_BATCH];

        while (1) {
            /* Phase 1: pull a batch of entries from the dict iterator and
             * prefetch their Entry structs. Pure pointer-fetch — we don't
             * dereference Entry here so the prefetch is effective. */
            int batch_count = 0;
            while (batch_count < HGETALL_BATCH) {
                dictEntry *de = dictNext(&di);
                if (!de) break;
                Entry *e = dictGetKey(de);
                batch_entry[batch_count++] = e;
                redis_prefetch_read(e);
            }
            if (batch_count == 0) break;

            /* Phase 2: Entry structs are warm — check expiry, extract value,
             * and prefetch the value SDS. Expired entries are dropped from
             * the batch by compacting in place. */
            int valid_count = 0;
            for (int i = 0; i < batch_count; i++) {
                Entry *e = batch_entry[i];
                if (skip_expired) {
                    uint64_t expire_time = entryGetExpiry(e);
                    if (expire_time != EB_EXPIRE_TIME_INVALID && (mstime_t)expire_time < commandTimeSnapshot())
                        continue;
                }
                batch_entry[valid_count] = e;
                if (flags & OBJ_HASH_VALUE) {
                    sds val = entryGetValue(e);
                    batch_val[valid_count] = val;
                    redis_prefetch_read(val);
                }
                valid_count++;
            }

            /* Phase 3: emit replies — field + value data is cache-warm. */
            for (int i = 0; i < valid_count; i++) {
                if (flags & OBJ_HASH_KEY) {
                    sds field = entryGetField(batch_entry[i]);
                    addReplyBulkCBuffer(c, field, sdslen(field));
                    count++;
                }
                if (flags & OBJ_HASH_VALUE) {
                    sds val = batch_val[i];
                    addReplyBulkCBuffer(c, val, sdslen(val));
                    count++;
                }
            }
        }
        dictResetIterator(&di);
        goto done;
    }

    hashTypeInitIterator(&hi, o);

    while (hashTypeNext(&hi, 1 /*skipExpiredFields*/) != C_ERR) {
        if (flags & OBJ_HASH_KEY) {
            addHashIteratorCursorToReply(c, &hi, OBJ_HASH_KEY);
            count++;
        }
        if (flags & OBJ_HASH_VALUE) {
            addHashIteratorCursorToReply(c, &hi, OBJ_HASH_VALUE);
            count++;
        }
    }

    hashTypeResetIterator(&hi);

done:
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldsize, kvobjAllocSize(o));

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
    size_t oldsize = 0;

    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(o);
    scanGenericCommand(c,o,cursor);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldsize, kvobjAllocSize(o));
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

/* HRANDFIELD: reply with the template-hash field at index fi (and its value if
 * withvalues). vptrs holds pre-collected value pointers for TMPL_LP, NULL otherwise. */
static void hrandfieldAddTmplReply(client *c, robj *hash, hashTemplate *tmpl,
                                   unsigned char **vptrs, unsigned long long field_idx,
                                   int withvalues)
{
    if (withvalues && c->resp > 2)
        addReplyArrayLen(c, 2);
    addReplyBulkCBuffer(c, tmpl->fields[field_idx], sdslen(tmpl->fields[field_idx]));
    if (!withvalues) return;
    if (hash->encoding == OBJ_ENCODING_TMPL_LP) {
        unsigned int vlen;
        long long vll;
        unsigned char *vstr = lpGetValue(vptrs[field_idx], &vlen, &vll);
        if (vstr)
            addReplyBulkCBuffer(c, vstr, vlen);
        else
            addReplyBulkLongLong(c, vll);
    } else {
        hashTemplateArray *hta = hash->ptr;
        addReplyBulkCBuffer(c, hta->values[field_idx], sdslen(hta->values[field_idx]));
    }
}

void hrandfieldWithCountCommand(client *c, long l, int withvalues) {
    unsigned long count, size;
    int uniq = 1;
    kvobj *hash;
    size_t oldsize = 0;

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

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(hash);

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
                Entry *entry = dictGetKey(de);
                sds fieldStr = entryGetField(entry);
                if (withvalues && c->resp > 2)
                    addReplyArrayLen(c,2);
                addReplyBulkCBuffer(c, fieldStr, sdslen(fieldStr));
                if (withvalues) {
                    sds value = entryGetValue(entry);
                    addReplyBulkCBuffer(c, value, sdslen(value));
                }
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
        } else if (hash->encoding == OBJ_ENCODING_TMPL_LP ||
                   hash->encoding == OBJ_ENCODING_TMPL_ARRAY) {

            /* Sample with replacement by random index. For TMPL_LP, collect
             * value-entry pointers in a single pass so each of the (unbounded)
             * draws is an O(1) index instead of an O(field_count) lpSeek. */
            hashTemplate *tmpl = hashTypeGetTemplate(hash);
            unsigned long long field_count = tmpl->field_count;
            unsigned char *stack_vptrs[HASH_TMPL_STACK_ENTRIES];
            unsigned char **vptrs = NULL;
            if (withvalues && hash->encoding == OBJ_ENCODING_TMPL_LP) {
                vptrs = (field_count <= HASH_TMPL_STACK_ENTRIES) ?
                            stack_vptrs : zmalloc(sizeof(*vptrs) * field_count);
                hashTemplateLpCollectValuePtrs(hash->ptr, vptrs, field_count);
            }
            while (count--) {
                unsigned long long fi = randomULong() % field_count;
                hrandfieldAddTmplReply(c, hash, tmpl, vptrs, fi, withvalues);
                if (c->flags & CLIENT_CLOSE_ASAP)
                    break;
            }
            if (vptrs && vptrs != stack_vptrs) zfree(vptrs);
        }
        goto out;
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
        hashTypeIterator hi;
        hashTypeInitIterator(&hi, hash);
        while (hashTypeNext(&hi, 0) != C_ERR) {
            if (withvalues && c->resp > 2)
                addReplyArrayLen(c,2);
            addHashIteratorCursorToReply(c, &hi, OBJ_HASH_KEY);
            if (withvalues)
                addHashIteratorCursorToReply(c, &hi, OBJ_HASH_VALUE);
        }
        hashTypeResetIterator(&hi);
        goto out;
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
        goto out;
    }

    /* CASE 2.5b Template-based hashes. Pick unique random indexes. */
    if (hash->encoding == OBJ_ENCODING_TMPL_LP ||
        hash->encoding == OBJ_ENCODING_TMPL_ARRAY)
    {
        hashTemplate *tmpl = hashTypeGetTemplate(hash);
        unsigned long long field_count = tmpl->field_count;
        if (count > field_count) count = field_count;

        /* Pick unique random indexes using Fisher-Yates partial shuffle. */
        unsigned long long stack_idx[HASH_TMPL_STACK_ENTRIES];
        unsigned long long *idx = (field_count <= HASH_TMPL_STACK_ENTRIES) ?
                   stack_idx : zmalloc(sizeof(*idx) * field_count);
        for (unsigned long long i = 0; i < field_count; i++) idx[i] = i;
        for (unsigned long long i = 0; i < count; i++) {
            unsigned long long j = i + (randomULong() % (field_count - i));
            unsigned long long tmp = idx[i];
            idx[i] = idx[j];
            idx[j] = tmp;
        }

        /* For TMPL_LP, collect value-entry pointers in a single pass so each
         * lookup below is O(1) instead of an O(field_count) lpSeek per draw. */
        unsigned char *stack_vptrs[HASH_TMPL_STACK_ENTRIES];
        unsigned char **vptrs = NULL;
        if (withvalues && hash->encoding == OBJ_ENCODING_TMPL_LP) {
            vptrs = (field_count <= HASH_TMPL_STACK_ENTRIES) ?
                        stack_vptrs : zmalloc(sizeof(*vptrs) * field_count);
            hashTemplateLpCollectValuePtrs(hash->ptr, vptrs, field_count);
        }

        for (unsigned long long i = 0; i < count; i++)
            hrandfieldAddTmplReply(c, hash, tmpl, vptrs, idx[i], withvalues);
        if (idx != stack_idx) zfree(idx);
        if (vptrs && vptrs != stack_vptrs) zfree(vptrs);
        goto out;
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
            sds field;
            sds value;
        } *pairs = zmalloc(sizeof(struct FieldValPair) * size);

        /* Add all the elements into the temporary array. */
        dictInitIterator(&di, ht);
        while((de = dictNext(&di)) != NULL) {
            Entry *e = dictGetKey(de);
            pairs[idx++] = (struct FieldValPair) {entryGetField(e), entryGetValue(e)};
        }
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
            addReplyBulkCBuffer(c, pairs[idx].field, sdslen(pairs[idx].field));
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
            Entry *e = dictGetKey(de);
            sds field = entryGetField(e);
            sds value = entryGetValue(e);

            /* Try to add the object to the dictionary. If it already exists
            * free it, otherwise increment the number of objects we have
            * in the result dictionary. */
            if (dictAdd(dictUnique, field, value) != DICT_OK)
                continue;

            added++;

            /* We can reply right away, so that we don't need to store the value in the dict. */
            if (withvalues && c->resp > 2)
                addReplyArrayLen(c,2);

            addReplyBulkCBuffer(c, field, sdslen(field));
            if (withvalues)
                addReplyBulkCBuffer(c, value, sdslen(value));
        }

        /* Release memory */
        dictRelease(dictUnique);
    }
out:
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), hash, oldsize, kvobjAllocSize(hash));
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
    size_t oldsize = 0;

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

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(hash);
    hashTypeRandomElement(hash,hashTypeLength(hash, 0),&ele,NULL);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), hash, oldsize, kvobjAllocSize(hash));

    if (ele.sval)
        addReplyBulkCBuffer(c, ele.sval, ele.slen);
    else
        addReplyBulkLongLong(c, ele.lval);
}

/*-----------------------------------------------------------------------------
 * Hash Field with optional expiry (based on entry)
 *----------------------------------------------------------------------------*/

static ExpireMeta* hentryGetExpireMeta(const eItem e) {
    /* extract the expireMeta from the field (entry) */
    return entryRefExpiryMeta((Entry *)e);
}

/* Remove TTL from the field. Assumed ExpireMeta is attached and has valid value */
static void hfieldPersist(robj *hashObj, Entry *entry) {
    uint64_t fieldExpireTime = entryGetExpiry(entry);
    if (fieldExpireTime == EB_EXPIRE_TIME_INVALID)
        return;

    /* if field is set with expire, then dict must has HFE metadata attached */
    dict *d = hashObj->ptr;
    htMetadataEx *dictExpireMeta = htGetMetadataEx(d);

    /* Remove field from private HFE DS */
    ebRemove(&dictExpireMeta->hfe, &hashFieldExpireBucketsType, entry);

    /* Don't have to update global HFE DS. It's unnecessary. Implementing this
     * would introduce significant complexity and overhead for an operation that
     * isn't critical. In the worst case scenario, the hash will be efficiently
     * updated later by an active-expire operation, or it will be removed by the
     * hash's dbGenericDelete() function. */
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
    Entry *e = item;
    kvobj *kv = expCtx->hashObj;
    size_t oldsize = 0;
    sds key = kvobjGetKey(kv);

    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(kv);
    sds field = entryGetField(e);

    /* Collect expired field for subkey notification (before deletion) */
    if (expCtx->vexpired)
        vecPush(expCtx->vexpired, createStringObject(field, sdslen(field)));

    propagateHashFieldDeletion(expCtx->db, key, field, sdslen(field));

    /* update keysizes */
    unsigned long l = hashTypeLength(expCtx->hashObj, 0);
    updateKeysizesHist(expCtx->db, OBJ_HASH, l, l - 1);

    serverAssert(hashTypeDelete(expCtx->hashObj, field) == 1);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(expCtx->db, getKeySlot(key), kv, oldsize, kvobjAllocSize(kv));
    server.stat_expired_subkeys++;
    if (expCtx->activeEx)
        server.stat_expired_subkeys_active++;
    return ACT_REMOVE_EXP_ITEM;
}

/* Retrieve the ExpireMeta associated with the hash.
 * The caller is responsible for ensuring that it is indeed attached. */
ExpireMeta *hashGetExpireMeta(const eItem hash) {
    robj *hashObj = (robj *)hash;
    if (hashObj->encoding == OBJ_ENCODING_LISTPACK_EX) {
        listpackEx *lpt = hashObj->ptr;
        return &lpt->meta;
    } else if (hashObj->encoding == OBJ_ENCODING_HT) {
        dict *d = hashObj->ptr;
        htMetadataEx *dictExpireMeta = htGetMetadataEx(d);
        return &dictExpireMeta->expireMeta;
    } else {
        serverPanic("Unknown encoding: %d", hashObj->encoding);
    }
}

/* Generic structure to hold parsed arguments for all hash field commands */
typedef struct {
    /* FIELDS arguments */
    int fieldsPos;          /* Position of FIELDS keyword (-1 if not found) */
    int numFieldsPos;       /* Position of numfields argument */
    int firstFieldPos;      /* Position of first field */
    int fieldCount;         /* Number of fields */

    /* HEXPIRE family arguments */
    int expireTimePos;      /* Position of expire time argument */
    long long expireTime;   /* Parsed expire time */
    int expireCondition;    /* HFE_NX, HFE_XX, HFE_GT, HFE_LT */
} HashCommandArgs;

/* Parser for HEXPIRE family commands with flexible keyword ordering.
 * Returns C_OK on success, C_ERR on error (with reply sent). */
static int parseHashCommandArgs(client *c, HashCommandArgs *args,
                                long long basetime, int unit)
{
    memset(args, 0, sizeof(*args));
    args->fieldsPos = -1;
    args->expireTimePos = 2;

    if (parseExpireTime(c, c->argv[2], unit, basetime, &args->expireTime) != C_OK) {
        return C_ERR;
    }

    /* Parse remaining arguments starting from position 3 */
    for (int i = 3; i < c->argc; i++) {
        char *arg = c->argv[i]->ptr;

        /* FIELDS keyword - supported by ALL hash field commands */
        if (!strcasecmp(arg, "FIELDS")) {
            if (args->fieldsPos != -1) {
                addReplyError(c, "FIELDS keyword specified multiple times");
                return C_ERR;
            }

            if (i >= c->argc - 2) {
                addReplyError(c, "FIELDS requires at least numfields and one field argument");
                return C_ERR;
            }

            args->fieldsPos = i;
            args->numFieldsPos = i + 1;
            long numFields;
            if (getRangeLongFromObjectOrReply(c, c->argv[args->numFieldsPos], 1, INT_MAX,
                                              &numFields, "Parameter `numFields` should be greater than 0") != C_OK)
                return C_ERR;

            args->firstFieldPos = i + 2;

            /* Check bounds - we must have exactly the right number of fields */
            if (numFields > c->argc - args->firstFieldPos) {
                addReplyError(c, "wrong number of arguments");
                return C_ERR;
            }

            args->fieldCount = (int)numFields;

            /* Skip over the field arguments */
            i = args->firstFieldPos + args->fieldCount - 1;
            continue;
        }

        /* Expiration condition keywords - validation moved outside loop for performance */
        if (!strcasecmp(arg, "NX")) {
            args->expireCondition |= HFE_NX;
            continue;
        } else if (!strcasecmp(arg, "XX")) {
            args->expireCondition |= HFE_XX;
            continue;
        } else if (!strcasecmp(arg, "GT")) {
            args->expireCondition |= HFE_GT;
            continue;
        } else if (!strcasecmp(arg, "LT")) {
            args->expireCondition |= HFE_LT;
            continue;
        }

        addReplyErrorFormat(c, "unknown argument: %s", (char*) c->argv[i]->ptr);
        return C_ERR;
    }

    /* Ensure FIELDS is specified */
    if (args->fieldsPos == -1) {
        addReplyError(c, "missing FIELDS argument");
        return C_ERR;
    }

    if (__builtin_popcount(args->expireCondition & (HFE_NX|HFE_XX|HFE_GT|HFE_LT)) > 1) {
        addReplyError(c, "Multiple condition flags specified");
        return C_ERR;
    }

    return C_OK;
}

/* HTTL key <FIELDS count field [field ...]>  */
static void httlGenericCommand(client *c, const char *cmd, long long basetime, int unit){
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

    /* Template encodings don't support HFE. */
    if (hashObj->encoding == OBJ_ENCODING_TMPL_LP ||
        hashObj->encoding == OBJ_ENCODING_TMPL_ARRAY)
    {
        hashTemplate *tmpl = hashTypeGetTemplate(hashObj);

        addReplyArrayLen(c, numFields);
        for (int i = 0; i < numFields; i++) {
            sds field = c->argv[numFieldsAt+1+i]->ptr;
            if (hashTemplateFieldIndex(tmpl, field) >= 0)
                addReplyLongLong(c, HFE_GET_NO_TTL);
            else
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
        size_t oldsize = 0;
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(hashObj);

        addReplyArrayLen(c, numFields);
        for (int i = 0 ; i < numFields ; i++) {
            sds field = c->argv[numFieldsAt+1+i]->ptr;
            dictEntry *de = dictFind(d, field);
            if (de == NULL) {
                addReplyLongLong(c, HFE_GET_NO_FIELD);
                continue;
            }

            Entry *entry = dictGetKey(de);
            uint64_t expire = entryGetExpiry(entry);
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
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), hashObj, oldsize, kvobjAllocSize(hashObj));
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
    HashCommandArgs args;
    int fieldsNotSet = 0;
    int64_t oldlen, newlen;
    robj *keyArg = c->argv[1];
    size_t oldsize = 0;

    /* Read the hash object */
    kvobj *hashObj = lookupKeyWrite(c->db, keyArg);
    if (checkType(c, hashObj, OBJ_HASH))
        return;

    /* Parse arguments using flexible keyword-based parsing */
    if (parseHashCommandArgs(c, &args, basetime, unit) != C_OK)
        return;

    /* Non-existing keys and empty hashes are the same thing. It also means
     * fields in the command don't exist in the hash key. */
    if (!hashObj) {
        addReplyArrayLen(c, args.fieldCount);
        for (int i = 0; i < args.fieldCount; i++) {
            addReplyLongLong(c, HSETEX_NO_FIELD);
        }
        return;
    }

    oldlen = hashTypeLength(hashObj, 0);
    if (server.memory_tracking_enabled)
        oldsize = kvobjAllocSize(hashObj);

    HashTypeSetEx exCtx;
    hashTypeSetExInit(keyArg, hashObj, c, c->db, args.expireCondition, &exCtx);
    addReplyArrayLen(c, args.fieldCount);

    /* Lazy allocation of fieldsToRemove - only allocate when failures occur */
    int *fieldsToRemove = NULL;
    int removeCount = 0;

    /* Track fields for subkey notifications. */
    fieldvec fvupdated, fvdeleted;
    vec *vupdated = fieldvecInit(&fvupdated, args.fieldCount);
    vec *vdeleted = fieldvecInit(&fvdeleted, args.fieldCount);

    for (int i = 0; i < args.fieldCount; i++) {
        int fieldPos = args.firstFieldPos + i;
        sds field = c->argv[fieldPos]->ptr;
        SetExRes res = hashTypeSetEx(hashObj, field, args.expireTime, &exCtx);
        if (res == HSETEX_OK) {
            vecPush(vupdated, c->argv[fieldPos]);
        } else if (res == HSETEX_DELETED) {
            vecPush(vdeleted, c->argv[fieldPos]);
        }

        if (unlikely(res != HSETEX_OK)) {
            if (fieldsToRemove == NULL) {
                fieldsToRemove = zmalloc(sizeof(int) * (args.fieldCount > 0 ? args.fieldCount : 1));
            }
            /* Remember this field position for later removal from propagation */
            fieldsToRemove[removeCount++] = fieldPos;
            fieldsNotSet = 1;
        }

        addReplyLongLong(c, res);
    }

    hashTypeSetExDone(&exCtx);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(keyArg->ptr), hashObj, oldsize, kvobjAllocSize(hashObj));

    if (vecSize(vdeleted) + vecSize(vupdated) > 0) {
        server.dirty += vecSize(vdeleted) + vecSize(vupdated);
        keyModified(c, c->db, keyArg, hashObj, 1);
        if (vecSize(vdeleted)) notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hdel",
                                keyArg, c->db->id, (robj**)vecData(vdeleted), vecSize(vdeleted));
        if (vecSize(vupdated)) notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hexpire",
                                keyArg, c->db->id, (robj**)vecData(vupdated), vecSize(vupdated));
    }

    newlen = (int64_t) hashTypeLength(hashObj, 0);
    if (newlen == 0) {
        newlen = -1;
        /* Del key but don't update KEYSIZES. Else it will decr wrong bin in histogram */
        dbDeleteSkipKeysizesUpdate(c->db, keyArg);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", keyArg, c->db->id);
    }

    if (oldlen != newlen)
        updateKeysizesHist(c->db, OBJ_HASH, oldlen, newlen);

    /* Avoid propagating command if not even one field was updated (Either because
     * the time is in the past, and corresponding HDELs were sent, or conditions
     * not met) then it is useless and invalid to propagate command with no fields */
    if (vecSize(vupdated) == 0) {
        vecRelease(vupdated);
        vecRelease(vdeleted);
        preventCommandPropagation(c);
        zfree(fieldsToRemove);
        return;
    }

    /* Handle propagation using command rewriting
     * Rewrite to canonical HPEXPIREAT command */
    if (c->cmd->proc != hpexpireatCommand) {
        rewriteClientCommandArgument(c, 0, shared.hpexpireat);

        robj *expireTimeObj = createStringObjectFromLongLong(args.expireTime);
        rewriteClientCommandArgument(c, args.expireTimePos, expireTimeObj);
        decrRefCount(expireTimeObj);
    }

    /* For partial failures, remove failed fields from the original command */
    if (fieldsNotSet) {
        for (int i = removeCount - 1; i >= 0; i--) {
            rewriteClientCommandArgument(c, fieldsToRemove[i], NULL);
        }
        robj *newFieldCount = createStringObjectFromLongLong(vecSize(vupdated));
        rewriteClientCommandArgument(c, args.fieldsPos + 1, newFieldCount);
        decrRefCount(newFieldCount);
    }

    if (fieldsToRemove)
        zfree(fieldsToRemove);

    vecRelease(vupdated);
    vecRelease(vdeleted);
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

    /* Template encodings don't support HFE. */
    if (hashObj->encoding == OBJ_ENCODING_TMPL_LP ||
        hashObj->encoding == OBJ_ENCODING_TMPL_ARRAY)
    {
        hashTemplate *tmpl = hashTypeGetTemplate(hashObj);

        addReplyArrayLen(c, numFields);
        for (int i = 0; i < numFields; i++) {
            sds field = c->argv[numFieldsAt+1+i]->ptr;
            if (hashTemplateFieldIndex(tmpl, field) >= 0)
                addReplyLongLong(c, HFE_PERSIST_NO_TTL);
            else
                addReplyLongLong(c, HFE_PERSIST_NO_FIELD);
        }
        return;
    }

    /* Track which fields were successfully persisted for subkey notification. */
    fieldvec fvpersisted;
    vec *vpersisted = fieldvecInit(&fvpersisted, numFields);

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
        vecRelease(vpersisted);
        return;
    } else if (hashObj->encoding == OBJ_ENCODING_LISTPACK_EX) {
        long long prevExpire;
        unsigned char *fptr, *vptr, *tptr;
        listpackEx *lpt = hashObj->ptr;
        size_t oldsize = 0;

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

            if (server.memory_tracking_enabled)
                oldsize = kvobjAllocSize(hashObj);
            listpackExUpdateExpiry(hashObj, field, fptr, vptr, HASH_LP_NO_TTL);
            if (server.memory_tracking_enabled)
                updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), hashObj, oldsize, kvobjAllocSize(hashObj));
            addReplyLongLong(c, HFE_PERSIST_OK);
            vecPush(vpersisted, c->argv[numFieldsAt + 1 + i]);
        }
    } else if (hashObj->encoding == OBJ_ENCODING_HT) {
        dict *d = hashObj->ptr;
        size_t oldsize = 0;
        if (server.memory_tracking_enabled)
            oldsize = kvobjAllocSize(hashObj);

        addReplyArrayLen(c, numFields);
        for (int i = 0 ; i < numFields ; i++) {
            sds field = c->argv[numFieldsAt + 1 + i]->ptr;
            dictEntry *de = dictFind(d, field);
            if (de == NULL) {
                addReplyLongLong(c, HFE_PERSIST_NO_FIELD);
                continue;
            }

            Entry *entry = dictGetKey(de);
            uint64_t expire = entryGetExpiry(entry);
            if (expire == EB_EXPIRE_TIME_INVALID) {
                addReplyLongLong(c, HFE_PERSIST_NO_TTL);
                continue;
            }

            /* Already expired. Pretend there is no such field */
            if ( (long long) expire < commandTimeSnapshot()) {
                addReplyLongLong(c, HFE_PERSIST_NO_FIELD);
                continue;
            }

            hfieldPersist(hashObj, entry);
            addReplyLongLong(c, HFE_PERSIST_OK);
            vecPush(vpersisted, c->argv[numFieldsAt + 1 + i]);
        }
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), hashObj, oldsize, kvobjAllocSize(hashObj));
    } else {
        serverPanic("Unknown encoding: %d", hashObj->encoding);
    }

    /* Generates a hpersist event if the expiry time associated with any field
     * has been successfully deleted. */
    if (vecSize(vpersisted)) {
        notifyKeyspaceEventWithSubkeys(NOTIFY_HASH, "hpersist", c->argv[1],
                                       c->db->id, (robj**)vecData(vpersisted), vecSize(vpersisted));
        keyModified(c, c->db, c->argv[1], hashObj, 1);
        server.dirty++;
    }
    vecRelease(vpersisted);
}
