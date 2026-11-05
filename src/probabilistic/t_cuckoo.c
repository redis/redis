/* Native Redis Cuckoo objects.
 *
 * Copyright (c) 2026-present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */
#include "server.h"
#include "t_cuckoo.h"
#include "endianconv.h"

robj *createCuckooObject(CuckooFilter *filter) {
    robj *o = createObject(OBJ_CUCKOO, filter);
    o->encoding = OBJ_ENCODING_CUCKOO;
    return o;
}

void freeCuckooObject(robj *o) {
    CuckooFilter_Free(o->ptr);
    zfree(o->ptr);
}

robj *cuckooDup(const robj *o) {
    const CuckooFilter *src = o->ptr;
    CuckooFilter *dst = zmalloc(sizeof(*dst));
    *dst = *src;
    dst->filters = zmalloc(src->numFilters * sizeof(*dst->filters));
    memcpy(dst->filters, src->filters, src->numFilters * sizeof(*dst->filters));
    for (size_t i = 0; i < src->numFilters; i++) {
        SubCF *filter = &dst->filters[i];
        size_t bytes = (size_t)filter->numBuckets * filter->bucketSize;
        filter->data = zmalloc(bytes);
        memcpy(filter->data, src->filters[i].data, bytes);
    }
    return createCuckooObject(dst);
}

size_t cuckooAllocSize(const robj *o) {
    const CuckooFilter *cf = o->ptr;
    size_t size = zmalloc_size((void *)cf) + zmalloc_size(cf->filters);
    for (size_t i = 0; i < cf->numFilters; i++) size += zmalloc_size(cf->filters[i].data);
    return size;
}

size_t cuckooLength(const robj *o) {
    return ((const CuckooFilter *)o->ptr)->numItems;
}

size_t cuckooFreeEffort(const robj *o) {
    return ((const CuckooFilter *)o->ptr)->numFilters + 2;
}

int cuckooDefragShouldDefer(const robj *o) {
    return ((const CuckooFilter *)o->ptr)->numFilters > server.active_defrag_max_scan_fields;
}

void cuckooLogDebugInfo(const robj *o) {
    const CuckooFilter *cf = o->ptr;
    serverLog(LL_WARNING,"Cuckoo items: %llu, filters: %u",
              (unsigned long long)cf->numItems, (unsigned)cf->numFilters);
}

void cuckooDismiss(robj *o) {
    CuckooFilter *cf = o->ptr;
    for (size_t i = 0; i < cf->numFilters; i++) {
        SubCF *filter = &cf->filters[i];
        dismissMemory(filter->data, (size_t)filter->numBuckets * filter->bucketSize);
    }
    dismissMemory(cf->filters, cf->numFilters * sizeof(*cf->filters));
    dismissMemory(cf, sizeof(*cf));
}

/* Revisit metadata on every call: insertions and compaction can change the
 * filter array between incremental defrag steps. */
unsigned long cuckooDefrag(robj *o, unsigned long cursor, size_t steps,
                          void *(*defrag)(void *)) {
    CuckooFilter *cf = o->ptr;
    void *moved = defrag(cf);
    if (moved) o->ptr = cf = moved;
    if ((moved = defrag(cf->filters))) cf->filters = moved;
    while (cursor < cf->numFilters && steps--) {
        SubCF *filter = &cf->filters[cursor++];
        if ((moved = defrag(filter->data))) filter->data = moved;
    }
    return cursor < cf->numFilters ? cursor : 0;
}

void cuckooMixDigest(unsigned char *digest, const robj *o) {
    const CuckooFilter *cf = o->ptr;
    uint64_t meta[] = {cf->numBuckets, cf->numItems, cf->numDeletes, cf->numFilters,
                       cf->bucketSize, cf->maxIterations, cf->expansion};
    for (size_t i = 0; i < sizeof(meta) / sizeof(*meta); i++) memrev64ifbe(&meta[i]);
    mixDigest(digest, meta, sizeof(meta));
    for (size_t i = 0; i < cf->numFilters; i++) {
        const SubCF *filter = &cf->filters[i];
        uint64_t fields[] = {filter->numBuckets, filter->bucketSize};
        for (size_t j = 0; j < sizeof(fields) / sizeof(*fields); j++) memrev64ifbe(&fields[j]);
        mixDigest(digest, fields, sizeof(fields));
        mixDigest(digest, filter->data, (size_t)filter->numBuckets * filter->bucketSize);
    }
}

#define CUCKOO_LEGACY_RDB_TYPE_NAME "MBbloomCF"
#define CUCKOO_LEGACY_RDB_VERSION_MASK UINT64_C(0x3FF)
#define CUCKOO_LEGACY_RDB_VERSION_EXPANSION 4
#define CUCKOO_RDB_NATIVE_VERSION (-1)

int cuckooIsLegacyRdbType(uint64_t moduleid) {
    char name[sizeof(CUCKOO_LEGACY_RDB_TYPE_NAME)];
    moduleTypeNameByID(name, moduleid);
    return !strcmp(name, CUCKOO_LEGACY_RDB_TYPE_NAME);
}

/* Keep the module field order, using native RDB integers and strings. */
ssize_t rdbSaveCuckooObject(rio *rdb, const robj *o) {
    const CuckooFilter *cf = o->ptr;
    ssize_t n, total = 0;
#define SAVE_CUCKOO(expr) do { if ((n = (expr)) == -1) return -1; total += n; } while (0)
    SAVE_CUCKOO(rdbSaveLen(rdb, cf->numFilters));
    SAVE_CUCKOO(rdbSaveLen(rdb, cf->numBuckets));
    SAVE_CUCKOO(rdbSaveLen(rdb, cf->numItems));
    SAVE_CUCKOO(rdbSaveLen(rdb, cf->numDeletes));
    SAVE_CUCKOO(rdbSaveLen(rdb, cf->bucketSize));
    SAVE_CUCKOO(rdbSaveLen(rdb, cf->maxIterations));
    SAVE_CUCKOO(rdbSaveLen(rdb, cf->expansion));
    for (size_t i = 0; i < cf->numFilters; i++) {
        const SubCF *filter = &cf->filters[i];
        SAVE_CUCKOO(rdbSaveLen(rdb, filter->numBuckets));
        SAVE_CUCKOO(rdbSaveRawString(rdb, filter->data,
                                    (size_t)filter->numBuckets * filter->bucketSize));
    }
#undef SAVE_CUCKOO
    return total;
}

robj *rdbLoadCuckooObject(rio *rdb, uint64_t moduleid, int *is_corrupt) {
    int encver = moduleid == CUCKOO_RDB_NATIVE_ID ? CUCKOO_RDB_NATIVE_VERSION :
                 (int)(moduleid & CUCKOO_LEGACY_RDB_VERSION_MASK);
    int legacy = encver != CUCKOO_RDB_NATIVE_VERSION;
    int encoded;
    uint64_t numFilters, numBuckets, numItems, numDeletes = 0;
    uint64_t bucketSize = server.cf_bucket_size;
    uint64_t maxIterations = server.cf_max_iterations;
    uint64_t expansion = server.cf_expansion_factor;
    CuckooFilter *cf = NULL;
    *is_corrupt = 0;
#define LOAD_CUCKOO_RAW_LEN(dest) do { \
    if (rdbLoadLenByRef(rdb, &encoded, &(dest)) == -1) goto error; \
    if (encoded) goto corrupt; \
} while (0)
#define LOAD_CUCKOO_OPCODE(expected) do { \
    if (legacy) { \
        uint64_t opcode; \
        LOAD_CUCKOO_RAW_LEN(opcode); \
        if (opcode != (expected)) goto corrupt; \
    } \
} while (0)
#define LOAD_CUCKOO_LEN(dest) do { \
    LOAD_CUCKOO_OPCODE(RDB_MODULE_OPCODE_UINT); \
    LOAD_CUCKOO_RAW_LEN(dest); \
} while (0)
    if (encver > CUCKOO_LEGACY_RDB_VERSION_EXPANSION) goto corrupt;
    LOAD_CUCKOO_LEN(numFilters);
    LOAD_CUCKOO_LEN(numBuckets);
    LOAD_CUCKOO_LEN(numItems);
    if (!legacy || encver >= CUCKOO_LEGACY_RDB_VERSION_EXPANSION) {
        LOAD_CUCKOO_LEN(numDeletes);
        LOAD_CUCKOO_LEN(bucketSize);
        LOAD_CUCKOO_LEN(maxIterations);
        LOAD_CUCKOO_LEN(expansion);
    }
    if (numFilters == 0 || numFilters > UINT16_MAX ||
        numBuckets == 0 || numBuckets > CF_MAX_NUM_BUCKETS ||
        bucketSize > UINT16_MAX || maxIterations > UINT16_MAX || expansion > UINT16_MAX)
        goto corrupt;
    cf = ztrycalloc(sizeof(*cf));
    if (!cf) goto error;
    cf->numBuckets = numBuckets;
    cf->numItems = numItems;
    cf->numDeletes = numDeletes;
    cf->bucketSize = bucketSize;
    cf->maxIterations = maxIterations;
    cf->expansion = expansion;
    cf->numFilters = numFilters;
    if (CuckooFilter_ValidateIntegrity(cf)) goto corrupt;
    cf->filters = ztrycalloc(numFilters * sizeof(*cf->filters));
    if (!cf->filters) goto error;
    for (size_t i = 0; i < numFilters; i++) {
        uint64_t buckets = numBuckets;
        size_t bytes;
        SubCF *filter = &cf->filters[i];
        if (!legacy || encver >= CUCKOO_LEGACY_RDB_VERSION_EXPANSION)
            LOAD_CUCKOO_LEN(buckets);
        if (buckets == 0 || buckets > CF_MAX_NUM_BUCKETS ||
            (buckets & (buckets - 1)) || buckets > SIZE_MAX / bucketSize) goto corrupt;
        filter->numBuckets = buckets;
        filter->bucketSize = bucketSize;
        LOAD_CUCKOO_OPCODE(RDB_MODULE_OPCODE_STRING);
        filter->data = rdbGenericLoadStringObject(rdb, RDB_LOAD_PLAIN, &bytes);
        if (!filter->data) goto error;
        if (bytes != (size_t)buckets * bucketSize) goto corrupt;
    }
    LOAD_CUCKOO_OPCODE(RDB_MODULE_OPCODE_EOF);
#undef LOAD_CUCKOO_LEN
#undef LOAD_CUCKOO_OPCODE
#undef LOAD_CUCKOO_RAW_LEN
    return createCuckooObject(cf);

corrupt:
    *is_corrupt = 1;
error:
    CuckooFilter_Free(cf);
    zfree(cf);
    return NULL;
}

int cuckooRewriteAof(rio *r, robj *key, robj *o, int dbid) {
    rio payload;
    /* Key metadata is emitted separately by the AOF callbacks. */
    createDumpPayload(&payload, o, key, dbid, DUMP_PAYLOAD_SKIP_KEY_META, 0);
    int ok = rioWriteBulkCount(r, '*', 5) &&
             rioWriteBulkString(r, "RESTORE", 7) &&
             rioWriteBulkObject(r, key) &&
             rioWriteBulkString(r, "0", 1) &&
             rioWriteBulkString(r, payload.io.buffer.ptr, sdslen(payload.io.buffer.ptr)) &&
             rioWriteBulkString(r, "REPLACE", 7);
    sdsfree(payload.io.buffer.ptr);
    return ok;
}

static void cuckooCheck(client *c, int multi, int count) {
    kvobj *kv = lookupKeyRead(c->db, c->argv[1]);
    /* Preserve the module's zero reply for missing keys and other types. */
    CuckooFilter *cf = kv && kv->type == OBJ_CUCKOO ? kv->ptr : NULL;
    if (multi) addReplyArrayLen(c, c->argc - 2);
    for (int ii = 2; ii < c->argc; ++ii) {
        uint64_t rv = 0;
        if (cf) {
            CuckooHash hash = CUCKOO_GEN_HASH(c->argv[ii]->ptr, sdslen(c->argv[ii]->ptr));
            rv = count ? CuckooFilter_Count(cf, hash) : CuckooFilter_Check(cf, hash);
        }
        if (count) addReplyLongLong(c, rv);
        else addReplyBool(c, !!rv);
    }
}

void cfExistsCommand(client *c) { cuckooCheck(c, 0, 0); }
void cfMExistsCommand(client *c) { cuckooCheck(c, 1, 0); }
void cfCountCommand(client *c) { cuckooCheck(c, 0, 1); }

static CuckooFilter *cuckooLookupRead(client *c) {
    kvobj *kv = lookupKeyRead(c->db, c->argv[1]);
    if (!kv) {
        addReplyError(c, "not found");
        return NULL;
    }
    if (checkType(c, kv, OBJ_CUCKOO)) return NULL;
    return kv->ptr;
}

void cfInfoCommand(client *c) {
    CuckooFilter *cf = cuckooLookupRead(c);
    if (!cf) return;
    /* CF.INFO reports logical size, as in the module; MEMORY USAGE reports
     * allocator size through cuckooAllocSize(). */
    size_t size = sizeof(*cf) + sizeof(*cf->filters) * cf->numFilters;
    for (const SubCF *filter = cf->filters; filter < cf->filters + cf->numFilters; ++filter)
        size += sizeof(*filter->data) * filter->bucketSize * filter->numBuckets;
    addReplyMapLen(c, 8);
    addReplyStatus(c, "Size");
    addReplyLongLong(c, size);
    addReplyStatus(c, "Number of buckets");
    addReplyLongLong(c, cf->numBuckets);
    addReplyStatus(c, "Number of filters");
    addReplyLongLong(c, cf->numFilters);
    addReplyStatus(c, "Number of items inserted");
    addReplyLongLong(c, cf->numItems);
    addReplyStatus(c, "Number of items deleted");
    addReplyLongLong(c, cf->numDeletes);
    addReplyStatus(c, "Bucket size");
    addReplyLongLong(c, cf->bucketSize);
    addReplyStatus(c, "Expansion rate");
    addReplyLongLong(c, cf->expansion);
    addReplyStatus(c, "Max iterations");
    addReplyLongLong(c, cf->maxIterations);
}

void cfDebugCommand(client *c) {
    CuckooFilter *cf = cuckooLookupRead(c);
    if (!cf) return;
    addReplyBulkSds(c, sdscatprintf(sdsempty(),
        "bktsize:%u buckets:%" PRIu64 " items:%" PRIu64 " deletes:%" PRIu64
        " filters:%u max_iterations:%u expansion:%u",
        cf->bucketSize, cf->numBuckets, cf->numItems, cf->numDeletes, cf->numFilters,
        cf->maxIterations, cf->expansion));
}

void cfDelCommand(client *c) {
    kvobj *kv = lookupKeyWrite(c->db, c->argv[1]);
    if (!kv || kv->type != OBJ_CUCKOO) {
        addReplyError(c, "Not found");
        return;
    }
    CuckooFilter *cf = kv->ptr;
    uint64_t oldlen = cf->numItems;
    size_t oldsize = server.memory_tracking_enabled ? kvobjAllocSize(kv) : 0;
    CuckooHash hash = CUCKOO_GEN_HASH(c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
    int rv = CuckooFilter_Delete(cf, hash);
    if (rv) {
        if (server.memory_tracking_enabled)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), kv, oldsize, kvobjAllocSize(kv));
        updateKeysizesHist(c->db, OBJ_CUCKOO, oldlen, cf->numItems);
        keyModified(c, c->db, c->argv[1], kv, 1);
        server.dirty++;
    }
    addReplyBool(c, !!rv);
}

void cfCompactCommand(client *c) {
    kvobj *kv = lookupKeyWrite(c->db, c->argv[1]);
    if (!kv || kv->type != OBJ_CUCKOO) {
        addReplyError(c, "Cuckoo filter was not found");
        return;
    }
    size_t oldsize = server.memory_tracking_enabled ? kvobjAllocSize(kv) : 0;
    CuckooFilter_Compact(kv->ptr, true);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), kv, oldsize, kvobjAllocSize(kv));
    /* Compaction can relocate fingerprints even without removing a subfilter. */
    keyModified(c, c->db, c->argv[1], kv, 1);
    server.dirty++;
    addReply(c, shared.ok);
}
