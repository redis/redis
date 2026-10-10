/* Native Cuckoo filter commands and persistence.
 * Copyright (c) 2026-Present, Redis Ltd.; RSALv2/SSPLv1/AGPLv3. */
#include "server.h"
#include "cf.h"
#include "probabilistic.h"
#include <inttypes.h>

robj *createCuckooObject(void *p) { return createObject(OBJ_CUCKOO, p); }
void freeCuckooObject(robj *o) { CuckooFilter_Free(o->ptr); zfree(o->ptr); }
size_t cuckooObjectLength(robj *o) {
    uint64_t n = ((CuckooFilter *)o->ptr)->numItems;
    return n > SIZE_MAX ? SIZE_MAX : n;
}
size_t cuckooFreeEffort(robj *o) { return ((CuckooFilter *)o->ptr)->numFilters + 2; }
size_t cuckooAllocSize(robj *o) {
    CuckooFilter *cf = o->ptr;
    size_t size = zmalloc_size(cf) + zmalloc_size(cf->filters);
    for (unsigned i = 0; i < cf->numFilters; i++) size += zmalloc_size(cf->filters[i].data);
    return size;
}
void cuckooDismiss(robj *o) {
    CuckooFilter *cf = o->ptr;
    for (unsigned i = 0; i < cf->numFilters; i++)
        dismissMemory(cf->filters[i].data, cf->filters[i].numBuckets * cf->bucketSize);
}
robj *cuckooDup(robj *o) {
    CuckooFilter *cf = o->ptr, *copy = zmalloc(sizeof(*copy));
    *copy = *cf;
    copy->filters = zmalloc(cf->numFilters * sizeof(SubCF));
    memcpy(copy->filters, cf->filters, cf->numFilters * sizeof(SubCF));
    for (unsigned i = 0; i < cf->numFilters; i++) {
        size_t size = cf->filters[i].numBuckets * cf->bucketSize;
        copy->filters[i].data = zmalloc(size);
        memcpy(copy->filters[i].data, cf->filters[i].data, size);
    }
    return createCuckooObject(copy);
}
void cuckooDefrag(robj *o, void *(*defrag)(void *)) {
    void *p = defrag(o->ptr);
    if (p) o->ptr = p;
    CuckooFilter *cf = o->ptr;
    p = defrag(cf->filters);
    if (p) cf->filters = p;
    for (unsigned i = 0; i < cf->numFilters; i++) {
        p = defrag(cf->filters[i].data);
        if (p) cf->filters[i].data = p;
    }
}
void cuckooDigest(unsigned char *digest, robj *o) {
    CuckooFilter *cf = o->ptr;
    CFHeader header = fillCFHeader(cf);
    mixDigest(digest, &header, sizeof(header));
    for (unsigned i = 0; i < cf->numFilters; i++)
        mixDigest(digest, cf->filters[i].data, cf->filters[i].numBuckets * cf->bucketSize);
}
static void cfModified(client *c, robj *o, size_t oldsize, size_t oldcount) {
    updateKeysizesHist(c->db, OBJ_CUCKOO, oldcount, cuckooObjectLength(o));
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldsize, kvobjAllocSize(o));
    keyModified(c, c->db, c->argv[1], o, 1);
    notifyKeyspaceEvent(NOTIFY_CUCKOO, c->cmd->fullname, c->argv[1], c->db->id);
    server.dirty++;
}
static int cfToken(robj *o, const char *s) {
    return sdslen(o->ptr) == strlen(s) && !strcasecmp(o->ptr, s);
}
static void cfBool(client *c, int n) {
    if (c->resp == 3) addReplyBool(c, n != 0); else addReplyLongLong(c, n);
}
static robj *cfLookup(client *c, int write, const char *missing) {
    robj *o = write ? lookupKeyWrite(c->db, c->argv[1]) : lookupKeyRead(c->db, c->argv[1]);
    if (!o) { addReplyError(c, missing); return NULL; }
    if (checkType(c, o, OBJ_CUCKOO)) return NULL;
    return o;
}
static robj *cfCreate(client *c, long long capacity, long long bucket, long long iterations, long long expansion) {
    if (capacity < bucket * 2) { addReplyError(c, "Could not create filter"); return NULL; }
    CuckooFilter *cf = zcalloc(sizeof(*cf));
    if (CuckooFilter_Init(cf, capacity, bucket, iterations, expansion)) {
        CuckooFilter_Free(cf); zfree(cf);
        addReplyError(c, "Insufficient memory to create filter"); return NULL;
    }
    robj *o = createCuckooObject(cf);
    dbAdd(c->db, c->argv[1], &o);
    return o;
}
void cfReserveCommand(client *c) {
    if (!(c->argc % 2)) { addReplyErrorArity(c); return; }
    long long cap, bucket = server.cf_bucket, iterations = server.cf_iterations, expansion = server.cf_expansion;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &cap, "Bad capacity") != C_OK) return;
    for (int i = 3; i < c->argc; i += 2) {
        long long *target, max, min;
        const char *name;
        if (cfToken(c->argv[i], "BUCKETSIZE")) { target=&bucket; min=1; max=255; name="BUCKETSIZE"; }
        else if (cfToken(c->argv[i], "MAXITERATIONS")) { target=&iterations; min=1; max=65535; name="MAXITERATIONS"; }
        else if (cfToken(c->argv[i], "EXPANSION")) { target=&expansion; min=0; max=32768; name="EXPANSION"; }
        else { addReplyError(c, "Unknown argument received"); return; }
        if (getLongLongFromObject(c->argv[i+1], target) != C_OK) {
            addReplyErrorFormat(c, "Couldn't parse %s", name); return;
        }
        if (*target < min || *target > max) {
            addReplyErrorFormat(c, "%s: value must be in the range [%lld, %lld]", name, min, max); return;
        }
    }
    if (cap < bucket * 2 || cap > (1LL<<30)) {
        addReplyError(c, "Capacity must be in the range [2 * BUCKETSIZE, 1073741824]"); return;
    }
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (o) { if (!checkType(c, o, OBJ_CUCKOO)) addReplyError(c, "item exists"); return; }
    o = cfCreate(c, cap, bucket, iterations, expansion);
    if (!o) return;
    cfModified(c, o, server.memory_tracking_enabled ? kvobjAllocSize(o) : 0, 0);
    addReply(c, shared.ok);
}
void cfInsertCommand(client *c) {
    int multi = !strncmp(c->cmd->fullname, "cf.insert", 9);
    int nx = c->cmd->fullname[sdslen(c->cmd->fullname)-1] == 'x';
    int first = 2, create = 1;
    long long cap = server.cf_capacity;
    if (multi) {
        for (; first < c->argc; first++) {
            if (cfToken(c->argv[first], "ITEMS")) { first++; break; }
            if (cfToken(c->argv[first], "NOCREATE")) { create=0; continue; }
            if (!cfToken(c->argv[first], "CAPACITY")) { addReplyError(c, "Unknown argument received"); return; }
            if (++first == c->argc) { addReplyErrorArity(c); return; }
            if (getLongLongFromObjectOrReply(c, c->argv[first], &cap, "Bad capacity") != C_OK) return;
            if (cap < server.cf_bucket * 2 || cap > (1LL<<30)) {
                addReplyError(c, "Capacity must be in the range [cf-bucket-size * 2, 1073741824]"); return;
            }
        }
        if (first == c->argc) { addReplyErrorArity(c); return; }
    }
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (o && checkType(c, o, OBJ_CUCKOO)) return;
    int created = !o;
    if (!o && !create) { addReplyError(c, "not found"); return; }
    if (!o) o = cfCreate(c, cap, server.cf_bucket, server.cf_iterations, server.cf_expansion);
    if (!o) return;
    size_t oldsize = server.memory_tracking_enabled ? kvobjAllocSize(o) : 0;
    size_t oldcount = created ? 0 : cuckooObjectLength(o);
    CuckooFilter *cf = o->ptr;
    if (cf->numFilters >= server.cf_max_expansions) {
        if (created) {
            cfModified(c, o, oldsize, oldcount);
            /* Only an empty filter was created. Replicating CF.ADD would insert
             * an item on a replica with different local expansion settings. */
            robj *reserve = createStringObject("CF.RESERVE", 10);
            robj *capacity = createStringObjectFromLongLong(cap);
            robj *bucket = createStringObject("BUCKETSIZE", 10);
            robj *bucketval = createStringObjectFromLongLong(cf->bucketSize);
            robj *iterations = createStringObject("MAXITERATIONS", 13);
            robj *iterationval = createStringObjectFromLongLong(cf->maxIterations);
            robj *expansion = createStringObject("EXPANSION", 9);
            robj *expansionval = createStringObjectFromLongLong(cf->expansion);
            rewriteClientCommandVector(c, 9, reserve, c->argv[1], capacity, bucket,
                                       bucketval, iterations, iterationval, expansion, expansionval);
            decrRefCount(reserve); decrRefCount(capacity); decrRefCount(bucket);
            decrRefCount(bucketval); decrRefCount(iterations); decrRefCount(iterationval);
            decrRefCount(expansion); decrRefCount(expansionval);
        }
        addReplyError(c, "Maximum expansions reached"); return;
    }
    if (multi) addReplyArrayLen(c, c->argc - first);
    int changed = created;
    for (int i = first; i < c->argc; i++) {
        sds item = c->argv[i]->ptr;
        CuckooHash hash = CUCKOO_GEN_HASH(item, sdslen(item));
        int status = nx ? CuckooFilter_InsertUnique(cf, hash) : CuckooFilter_Insert(cf, hash);
        if (status == CuckooInsert_Inserted) changed = 1;
        if (status < 0 && !multi)
            addReplyError(c, status == CuckooInsert_NoSpace ? "Filter is full" : "Insufficient memory");
        else if (c->resp == 3 && (!multi || !nx)) cfBool(c, status > 0);
        else addReplyLongLong(c, status < 0 ? -1 : status);
    }
    if (changed) cfModified(c, o, oldsize, oldcount);
}
void cfCheckCommand(client *c) {
    int multi = !strcasecmp(c->cmd->fullname, "cf.mexists");
    int count = !strcasecmp(c->cmd->fullname, "cf.count");
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (multi) addReplyArrayLen(c, c->argc-2);
    for (int i = 2; i < c->argc; i++) {
        long long result = 0;
        if (o && o->type == OBJ_CUCKOO) {
            sds item = c->argv[i]->ptr;
            CuckooHash hash = CUCKOO_GEN_HASH(item, sdslen(item));
            result = count ? CuckooFilter_Count(o->ptr, hash) : (uint64_t)CuckooFilter_Check(o->ptr, hash);
        }
        if (count) addReplyLongLong(c, result); else cfBool(c, result);
    }
}
void cfDelCommand(client *c) {
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (!o || o->type != OBJ_CUCKOO) { addReplyError(c, "Not found"); return; }
    size_t oldsize = server.memory_tracking_enabled ? kvobjAllocSize(o) : 0, oldcount = cuckooObjectLength(o);
    sds item = c->argv[2]->ptr;
    int result = CuckooFilter_Delete(o->ptr, CUCKOO_GEN_HASH(item, sdslen(item)));
    if (result) cfModified(c, o, oldsize, oldcount);
    cfBool(c, result);
}
void cfCompactCommand(client *c) {
    robj *o = cfLookup(c, 1, "Cuckoo filter was not found");
    if (!o) return;
    size_t oldsize = server.memory_tracking_enabled ? kvobjAllocSize(o) : 0, oldcount = cuckooObjectLength(o);
    CuckooFilter_Compact(o->ptr, true);
    cfModified(c, o, oldsize, oldcount);
    addReply(c, shared.ok);
}
void cfInfoCommand(client *c) {
    robj *o = cfLookup(c, 0, "not found");
    if (!o) return;
    CuckooFilter *cf = o->ptr;
    size_t size = sizeof(*cf) + cf->numFilters * sizeof(SubCF);
    for (unsigned i = 0; i < cf->numFilters; i++) size += cf->filters[i].numBuckets * cf->bucketSize;
    if (c->resp == 3) addReplyMapLen(c, 8); else addReplyArrayLen(c, 16);
    const char *names[] = {"Size", "Number of buckets", "Number of filters", "Number of items inserted",
                          "Number of items deleted", "Bucket size", "Expansion rate", "Max iterations"};
    uint64_t values[] = {size, cf->numBuckets, cf->numFilters, cf->numItems, cf->numDeletes,
                         cf->bucketSize, cf->expansion, cf->maxIterations};
    for (int i = 0; i < 8; i++) { addReplyStatus(c, names[i]); addReplyLongLong(c, values[i]); }
}
void cfDebugCommand(client *c) {
    robj *o = cfLookup(c, 0, "not found");
    if (!o) return;
    CuckooFilter *cf = o->ptr;
    sds s = sdscatprintf(sdsempty(), "bktsize:%u buckets:%" PRIu64 " items:%" PRIu64
                        " deletes:%" PRIu64 " filters:%u max_iterations:%u expansion:%u",
                        cf->bucketSize, cf->numBuckets, cf->numItems, cf->numDeletes,
                        cf->numFilters, cf->maxIterations, cf->expansion);
    addReplyBulkSds(c, s);
}
void cfScandumpCommand(client *c) {
    long long pos;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &pos, "Invalid position") != C_OK) return;
    if (pos < 0) { addReplyError(c, "Invalid position"); return; }
    robj *o = cfLookup(c, 0, "not found");
    if (!o) return;
    CuckooFilter *cf = o->ptr;
    addReplyArrayLen(c, 2);
    if (cf->numItems && pos == 0) {
        CFHeader header = fillCFHeader(cf);
        addReplyLongLong(c, 1); addReplyBulkCBuffer(c, &header, sizeof(header)); return;
    }
    size_t len = 0;
    const char *chunk = cf->numItems ? CF_GetEncodedChunk(cf, &pos, &len, 16*1024*1024) : NULL;
    if (chunk) { addReplyLongLong(c, pos); addReplyBulkCBuffer(c, chunk, len); }
    else { addReplyLongLong(c, 0); addReplyNull(c); }
}
void cfLoadchunkCommand(client *c) {
    long long pos;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &pos, "Invalid position") != C_OK) return;
    if (pos <= 0) { addReplyError(c, "Invalid position"); return; }
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (o && checkType(c, o, OBJ_CUCKOO)) return;
    sds blob = c->argv[3]->ptr;
    if (pos == 1) {
        if (o) { addReplyError(c, "item exists"); return; }
        if (sdslen(blob) != sizeof(CFHeader)) { addReplyError(c, "Invalid header"); return; }
        CFHeader header;
        memcpy(&header, blob, sizeof(header));
        CuckooFilter *cf = CFHeader_Load(&header);
        if (!cf) { addReplyError(c, "Couldn't create filter!"); return; }
        o = createCuckooObject(cf);
        dbAdd(c->db, c->argv[1], &o);
        cfModified(c, o, server.memory_tracking_enabled ? kvobjAllocSize(o) : 0, 0);
    } else {
        if (!o) { addReplyError(c, "not found"); return; }
        if (CF_LoadEncodedChunk(o->ptr, pos, blob, sdslen(blob))) {
            addReplyError(c, "Couldn't load chunk!"); return;
        }
        cfModified(c, o, server.memory_tracking_enabled ? kvobjAllocSize(o) : 0, cuckooObjectLength(o));
    }
    addReply(c, shared.ok);
}
uint64_t cuckooRdbId(void) { return probRdbId("MBbloomCF", 4); }
ssize_t cuckooRdbSave(rio *rdb, robj *o) {
    CuckooFilter *cf = o->ptr;
    probIO io = {.rdb=rdb};
    addSaved(&io, rdbSaveLen(rdb, cuckooRdbId()));
    saveUnsigned(&io, cf->numFilters); saveUnsigned(&io, cf->numBuckets);
    saveUnsigned(&io, cf->numItems); saveUnsigned(&io, cf->numDeletes);
    saveUnsigned(&io, cf->bucketSize); saveUnsigned(&io, cf->maxIterations); saveUnsigned(&io, cf->expansion);
    for (unsigned i = 0; i < cf->numFilters; i++) {
        saveUnsigned(&io, cf->filters[i].numBuckets);
        saveString(&io, (char *)cf->filters[i].data, cf->filters[i].numBuckets * cf->bucketSize);
    }
    addSaved(&io, rdbSaveLen(rdb, RDB_MODULE_OPCODE_EOF));
    return io.error ? -1 : io.bytes;
}
robj *cuckooRdbLoad(rio *rdb, int version) {
    if (version > 4) return NULL;
    probIO io = {.rdb=rdb};
    uint64_t n = loadUnsigned(&io), buckets = loadUnsigned(&io);
    if (io.error || !n || n > UINT16_MAX || !buckets || buckets > CF_MAX_NUM_BUCKETS) return NULL;
    CuckooFilter *cf = zcalloc(sizeof(*cf));
    cf->numBuckets = buckets;
    cf->numItems = loadUnsigned(&io);
    uint64_t bucket = server.cf_bucket, iterations = server.cf_iterations, expansion = server.cf_expansion;
    if (version >= 4) {
        cf->numDeletes = loadUnsigned(&io);
        bucket=loadUnsigned(&io); iterations=loadUnsigned(&io); expansion=loadUnsigned(&io);
    }
    if (io.error || bucket > 255 || iterations > UINT16_MAX || expansion > 32768) goto error;
    cf->bucketSize=bucket; cf->maxIterations=iterations; cf->expansion=expansion;
    cf->numFilters=n;
    if (CuckooFilter_ValidateIntegrity(cf)) goto error;
    cf->filters = zcalloc_num(n, sizeof(SubCF));
    for (unsigned i=0; i<n; i++) {
        uint64_t sub = version >= 4 ? loadUnsigned(&io) : buckets;
        if (!sub || sub > CF_MAX_NUM_BUCKETS || (sub & (sub-1)) || sub > SIZE_MAX / bucket) goto error;
        cf->filters[i].numBuckets=sub; cf->filters[i].bucketSize=bucket;
        size_t len=0;
        cf->filters[i].data=loadString(&io, &len);
        if (io.error || !cf->filters[i].data || len != sub * bucket) goto error;
    }
    if (!loadOpcode(&io, RDB_MODULE_OPCODE_EOF)) goto error;
    return createCuckooObject(cf);
error:
    CuckooFilter_Free(cf); zfree(cf); return NULL;
}
int cuckooRewriteAof(rio *rdb, robj *key, robj *o, int dbid) {
    return probRewriteAof(rdb, key, o, dbid);
}
