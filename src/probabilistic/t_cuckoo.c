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
#include <ctype.h>

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

int cuckooValidateBucketSize(long long value, const char **err) {
    if (value > server.cf_initial_size / 2) {
        *err = "cf-bucket-size must not exceed half of cf-initial-size";
        return 0;
    }
    return 1;
}

int cuckooValidateInitialSize(long long value, const char **err) {
    if (value < server.cf_bucket_size * 2) {
        *err = "cf-initial-size must be at least twice cf-bucket-size";
        return 0;
    }
    return 1;
}

/* Preserve the module's first-match option lookup, including duplicate options. */
static int cuckooArgIndex(client *c, const char *arg) {
    size_t len = strlen(arg);
    for (int ii = 0; ii < c->argc; ++ii) {
        sds value = c->argv[ii]->ptr;
        if (sdslen(value) == len && !strncasecmp(value, arg, len)) return ii;
    }
    return -1;
}

void cfReserveCommand(client *c) {
    if ((c->argc % 2) == 0) {
        addReplyErrorArity(c);
        return;
    }
    long long capacity;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &capacity, "Bad capacity") != C_OK) return;
    long long maxIterations = server.cf_max_iterations;
    int mi_loc = cuckooArgIndex(c, "MAXITERATIONS");
    if (mi_loc != -1) {
        if (mi_loc + 1 == c->argc) { addReplyErrorArity(c); return; }
        if (getLongLongFromObjectOrReply(c, c->argv[mi_loc + 1], &maxIterations,
                                       "Couldn't parse MAXITERATIONS") != C_OK) return;
        if (maxIterations < 1 || maxIterations > UINT16_MAX) {
            addReplyError(c, "MAXITERATIONS: value must be in the range [1, 65535]");
            return;
        }
    }
    long long bucketSize = server.cf_bucket_size;
    long long maxBucketSize = min(CF_MAX_BUCKET_SIZE, server.cf_initial_size / 2);
    int bs_loc = cuckooArgIndex(c, "BUCKETSIZE");
    if (bs_loc != -1) {
        if (bs_loc + 1 == c->argc) { addReplyErrorArity(c); return; }
        if (getLongLongFromObjectOrReply(c, c->argv[bs_loc + 1], &bucketSize,
                                       "Couldn't parse BUCKETSIZE") != C_OK) return;
        if (bucketSize < 1 || bucketSize > maxBucketSize) {
            addReplyErrorFormat(c, "BUCKETSIZE: value must be in the range [1, %lld]", maxBucketSize);
            return;
        }
    }
    long long expansion = server.cf_expansion_factor;
    int ex_loc = cuckooArgIndex(c, "EXPANSION");
    if (ex_loc != -1) {
        if (ex_loc + 1 == c->argc) { addReplyErrorArity(c); return; }
        if (getLongLongFromObjectOrReply(c, c->argv[ex_loc + 1], &expansion,
                                       "Couldn't parse EXPANSION") != C_OK) return;
        if (expansion < 0 || expansion > CF_MAX_EXPANSION) {
            addReplyError(c, "EXPANSION: value must be in the range [0, 32768]");
            return;
        }
    }
    if (bucketSize * 2 > capacity || capacity > (1LL << 30)) {
        addReplyError(c, "Capacity must be in the range [2 * BUCKETSIZE, 1073741824]");
        return;
    }
    dictEntryLink link;
    kvobj *kv = lookupKeyWriteWithLink(c->db, c->argv[1], &link);
    if (checkType(c, kv, OBJ_CUCKOO)) return;
    if (kv) { addReplyError(c, "item exists"); return; }
    CuckooFilter *cf = ztrycalloc(sizeof(*cf));
    if (!cf || CuckooFilter_Init(cf, capacity, bucketSize, maxIterations, expansion) != 0) {
        CuckooFilter_Free(cf);
        zfree(cf);
        addReplyError(c, "Insufficient memory to create filter");
        return;
    }
    robj *o = createCuckooObject(cf);
    kv = dbAddByLink(c->db, c->argv[1], &o, &link);
    keyModified(c, c->db, c->argv[1], kv, 1);
    server.dirty++;
    addReply(c, shared.ok);
}

typedef struct {
    int is_nx;
    int autocreate;
    int is_multi;
    long long capacity;
} CFInsertOptions;

static void cfInsertCommon(client *c, int first, const CFInsertOptions *options) {
    dictEntryLink link;
    kvobj *kv = lookupKeyWriteWithLink(c->db, c->argv[1], &link);
    if (checkType(c, kv, OBJ_CUCKOO)) return;
    int autocreated = kv == NULL;
    if (autocreated) {
        if (!options->autocreate) { addReplyError(c, "not found"); return; }
        if (options->capacity < server.cf_bucket_size * 2) {
            addReplyError(c, "Could not create filter");
            return;
        }
        CuckooFilter *cf = ztrycalloc(sizeof(*cf));
        if (!cf || CuckooFilter_Init(cf, options->capacity, server.cf_bucket_size,
                                    server.cf_max_iterations, server.cf_expansion_factor) != 0) {
            CuckooFilter_Free(cf);
            zfree(cf);
            addReplyError(c, "Insufficient memory to create filter");
            return;
        }
        robj *o = createCuckooObject(cf);
        kv = dbAddByLink(c->db, c->argv[1], &o, &link);
        keyModified(c, c->db, c->argv[1], kv, 1);
        server.dirty++;
    }
    CuckooFilter *cf = kv->ptr;
    if (cf->numFilters >= server.cf_max_expansions) {
        if (autocreated) {
            /* Preserve the empty key without replaying a command that errors. */
            robj *args[] = {createStringObject("CF.RESERVE",10), c->argv[1],
                createStringObjectFromLongLong(options->capacity),
                createStringObject("BUCKETSIZE",10), createStringObjectFromLongLong(cf->bucketSize),
                createStringObject("MAXITERATIONS",13), createStringObjectFromLongLong(cf->maxIterations),
                createStringObject("EXPANSION",9), createStringObjectFromLongLong(cf->expansion)};
            rewriteClientCommandVector(c,9,args[0],args[1],args[2],args[3],args[4],args[5],args[6],args[7],args[8]);
            for (int i = 0; i < 9; i++) if (i != 1) decrRefCount(args[i]);
        }
        addReplyError(c, "Maximum expansions reached");
        return;
    }
    uint64_t oldlen = cf->numItems;
    size_t oldsize = server.memory_tracking_enabled ? kvobjAllocSize(kv) : 0;
    int propagate = shouldPropagate(server.allowed_propagate_targets);
    robj **argv = NULL;
    int argc = 0;
    if (options->is_multi) addReplyArrayLen(c, c->argc - first);
    for (int ii = first; ii < c->argc; ++ii) {
        CuckooHash hash = CUCKOO_GEN_HASH(c->argv[ii]->ptr, sdslen(c->argv[ii]->ptr));
        CuckooInsertStatus insStatus = options->is_nx ? CuckooFilter_InsertUnique(cf, hash) :
                                                       CuckooFilter_Insert(cf, hash);
        if (insStatus < 0 && propagate && !argv) {
            argv = zmalloc(sizeof(*argv) * c->argc);
            argc = ii;
            memcpy(argv, c->argv, sizeof(*argv) * argc);
        }
        if (insStatus >= 0) {
            if (argv) argv[argc++] = c->argv[ii];
            if (options->is_nx && options->is_multi) addReplyLongLong(c, insStatus);
            else addReplyBool(c, insStatus == CuckooInsert_Inserted);
        } else if (!options->is_multi) {
            addReplyError(c, insStatus == CuckooInsert_NoSpace ? "Filter is full" : "Insufficient memory");
        } else if (c->resp == 3 && !options->is_nx) {
            addReplyBool(c, 0);
        } else {
            addReplyLongLong(c, -1);
        }
    }
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), kv, oldsize, kvobjAllocSize(kv));
    if (cf->numItems != oldlen) {
        updateKeysizesHist(c->db, OBJ_CUCKOO, oldlen, cf->numItems);
        keyModified(c, c->db, c->argv[1], kv, 1);
        server.dirty += cf->numItems - oldlen;
    }
    if (argv) {
        if (argc > first) {
            for (int i = 0; i < argc; i++) incrRefCount(argv[i]);
            replaceClientCommandVector(c, argc, argv);
        } else {
            zfree(argv);
            preventCommandPropagation(c);
        }
    }
}

static void cuckooAdd(client *c, int nx) {
    CFInsertOptions options = {.autocreate = 1, .capacity = server.cf_initial_size, .is_nx = nx};
    cfInsertCommon(c, 2, &options);
}
void cfAddCommand(client *c) { cuckooAdd(c, 0); }
void cfAddNxCommand(client *c) { cuckooAdd(c, 1); }

static void cuckooInsert(client *c, int nx) {
    CFInsertOptions options = {.autocreate = 1, .capacity = server.cf_initial_size,
                               .is_multi = 1, .is_nx = nx};
    int cur_pos = 2, items_pos = -1;
    while (cur_pos < c->argc && items_pos < 0) {
        const char *argstr = c->argv[cur_pos]->ptr;
        /* Preserve the module's first-character option matching. */
        switch (tolower((unsigned char)*argstr)) {
        case 'c':
            if (++cur_pos == c->argc) { addReplyErrorArity(c); return; }
            if (getLongLongFromObjectOrReply(c, c->argv[cur_pos++], &options.capacity, "Bad capacity") != C_OK) return;
            if (options.capacity < server.cf_bucket_size * 2 || options.capacity > (1LL << 30)) {
                addReplyError(c, "Capacity must be in the range [cf-bucket-size * 2, 1073741824]");
                return;
            }
            break;
        case 'i': items_pos = ++cur_pos; break;
        case 'n': options.autocreate = 0; cur_pos++; break;
        default: addReplyError(c, "Unknown argument received"); return;
        }
    }
    if (items_pos < 0 || items_pos == c->argc) { addReplyErrorArity(c); return; }
    cfInsertCommon(c, items_pos, &options);
}
void cfInsertCommand(client *c) { cuckooInsert(c, 0); }
void cfInsertNxCommand(client *c) { cuckooInsert(c, 1); }

#define CUCKOO_MAX_SCANDUMP_SIZE (16 * 1024 * 1024)

typedef struct __attribute__((packed)) {
    uint64_t numItems;
    uint64_t numBuckets;
    uint64_t numDeletes;
    uint64_t numFilters;
    uint16_t bucketSize;
    uint16_t maxIterations;
    uint16_t expansion;
} CFHeader;


static const char *CF_GetEncodedChunk(const CuckooFilter *cf, long long *pos, size_t *buflen,
                               size_t bytelimit) {
    // First find filter
    long long offset = *pos - 1;
    long long currentSize = 0;
    int filterIx = 0;
    SubCF *filter;
    for (; filterIx < cf->numFilters; ++filterIx) {
        filter = cf->filters + filterIx;
        currentSize = filter->bucketSize * filter->numBuckets;
        if (offset < currentSize) {
            break;
        }
        offset -= currentSize;
    }

    // all filters already returned
    if (filterIx == cf->numFilters) {
        return NULL;
    }

    if (currentSize <= bytelimit) {
        // filter size is smaller than the limit. Add it all
        *buflen = currentSize;
        *pos += currentSize;
        return (const char *)filter->data;
    } else {
        size_t remaining = currentSize - offset;
        if (remaining > bytelimit) {
            // return another piece from the filter
            *buflen = bytelimit;
            *pos += bytelimit;
        } else {
            // return the rest of the filter
            *buflen = remaining;
            *pos += remaining;
        }
        return (const char *)filter->data + offset;
    }
}

static int CF_LoadEncodedChunk(const CuckooFilter *cf, long long pos, const char *data, size_t datalen) {
    if (datalen == 0 || pos <= 0 || (size_t)(pos - 1) < datalen) {
        return C_ERR;
    }

    // calculate offset
    long long offset = pos - datalen - 1;
    long long currentSize;
    int filterIx = 0;
    SubCF *filter = NULL;
    for (; filterIx < cf->numFilters; ++filterIx) {
        filter = cf->filters + filterIx;
        currentSize = filter->bucketSize * filter->numBuckets;
        if (offset < currentSize) {
            break;
        }
        offset -= currentSize;
    }

    // Boundary check before memcpy()
    if (filterIx == cf->numFilters || !filter || ((size_t)offset > SIZE_MAX - datalen) ||
        filter->bucketSize * filter->numBuckets < offset + datalen) {
        return C_ERR;
    }

    // copy data to filter
    memcpy(filter->data + offset, data, datalen);
    return C_OK;
}

static CuckooFilter *CFHeader_Load(const CFHeader *header) {
    /* Validate wide header fields before narrowing or allocating. */
    if (header->numFilters == 0 || header->numFilters > UINT16_MAX) return NULL;
    CuckooFilter *filter = ztrycalloc(sizeof(*filter));
    if (!filter) return NULL;
    filter->numBuckets = header->numBuckets;
    filter->numFilters = header->numFilters;
    filter->numItems = header->numItems;
    filter->numDeletes = header->numDeletes;
    filter->bucketSize = header->bucketSize;
    filter->maxIterations = header->maxIterations;
    filter->expansion = header->expansion;
    if (CuckooFilter_ValidateIntegrity(filter) != 0) goto error;
    filter->filters = ztrycalloc(filter->numFilters * sizeof(*filter->filters));
    if (!filter->filters) goto error;
    uint64_t buckets = filter->numBuckets;
    uint64_t total = 0;
    for (size_t ii = 0; ii < filter->numFilters; ++ii) {
        SubCF *cur = filter->filters + ii;
        if (ii) {
            if (!filter->expansion || buckets > CF_MAX_NUM_BUCKETS / filter->expansion) goto error;
            buckets *= filter->expansion;
        }
        if (buckets > CF_MAX_NUM_BUCKETS || buckets > SIZE_MAX / filter->bucketSize) goto error;
        size_t bytes = (size_t)buckets * filter->bucketSize;
        if (bytes > (uint64_t)LLONG_MAX - 1 - total) goto error;
        total += bytes;
        cur->bucketSize = filter->bucketSize;
        cur->numBuckets = buckets;
        cur->data = ztrycalloc(bytes);
        if (!cur->data) goto error;
    }
    return filter;
error:
    CuckooFilter_Free(filter);
    zfree(filter);
    return NULL;
}

static CFHeader fillCFHeader(const CuckooFilter *cf) {
    return (CFHeader){
        .numItems = cf->numItems,
        .numBuckets = cf->numBuckets,
        .numDeletes = cf->numDeletes,
        .numFilters = cf->numFilters,
        .bucketSize = cf->bucketSize,
        .maxIterations = cf->maxIterations,
        .expansion = cf->expansion,
    };
}

void cfScanDumpCommand(client *c) {
    long long pos;
    if (getLongLongFromObject(c->argv[2], &pos) != C_OK || pos < 0) {
        addReplyError(c, "Invalid position");
        return;
    }
    CuckooFilter *cf = cuckooLookupRead(c);
    if (!cf) return;
    addReplyArrayLen(c, 2);
    if (!cf->numItems) {
        addReplyLongLong(c, 0);
        addReplyNull(c);
    } else if (pos == 0) {
        CFHeader header = fillCFHeader(cf);
        addReplyLongLong(c, 1);
        addReplyBulkCBuffer(c, &header, sizeof(header));
    } else {
        size_t chunkLen = 0;
        const char *chunk = CF_GetEncodedChunk(cf, &pos, &chunkLen, CUCKOO_MAX_SCANDUMP_SIZE);
        addReplyLongLong(c, chunk ? pos : 0);
        if (chunk) addReplyBulkCBuffer(c, chunk, chunkLen);
        else addReplyNull(c);
    }
}

void cfLoadChunkCommand(client *c) {
    dictEntryLink link;
    kvobj *kv = lookupKeyWriteWithLink(c->db, c->argv[1], &link);
    long long pos;
    if (getLongLongFromObject(c->argv[2], &pos) != C_OK || pos == 0) {
        addReplyError(c, "Invalid position");
        return;
    }
    sds blob = c->argv[3]->ptr;
    if (pos == 1) {
        if (checkType(c, kv, OBJ_CUCKOO)) return;
        if (kv) { addReplyError(c, "item exists"); return; }
        if (sdslen(blob) != sizeof(CFHeader)) { addReplyError(c, "Invalid header"); return; }
        CFHeader header;
        memcpy(&header, blob, sizeof(header));
        CuckooFilter *cf = CFHeader_Load(&header);
        if (!cf) { addReplyError(c, "Couldn't create filter!"); return; }
        robj *o = createCuckooObject(cf);
        kv = dbAddByLink(c->db, c->argv[1], &o, &link);
    } else {
        if (!kv) { addReplyError(c, "not found"); return; }
        if (checkType(c, kv, OBJ_CUCKOO)) return;
        if (CF_LoadEncodedChunk(kv->ptr, pos, blob, sdslen(blob)) != C_OK) {
            addReplyError(c, "Couldn't load chunk!");
            return;
        }
    }
    keyModified(c, c->db, c->argv[1], kv, 1);
    server.dirty++;
    addReply(c, shared.ok);
}
