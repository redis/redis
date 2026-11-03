/* Native Redis Bloom objects.
 *
 * Copyright (c) 2026-present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */
#include "server.h"
#include "t_bloom.h"
#include "endianconv.h"
#include <math.h>

robj *createBloomObject(SBChain *chain) {
    robj *o = createObject(OBJ_BLOOM, chain);
    o->encoding = OBJ_ENCODING_BLOOM;
    return o;
}

robj *bloomDup(const robj *o) {
    const SBChain *src = o->ptr;
    SBChain *dst = zmalloc(sizeof(*dst));
    *dst = *src;
    dst->filters = zmalloc(src->nfilters * sizeof(*dst->filters));
    memcpy(dst->filters, src->filters, src->nfilters * sizeof(*dst->filters));
    for (size_t i = 0; i < src->nfilters; i++) {
        struct bloom *b = &dst->filters[i].inner;
        b->bf = zmalloc(b->bytes);
        memcpy(b->bf, src->filters[i].inner.bf, b->bytes);
    }
    return createBloomObject(dst);
}

size_t bloomAllocSize(const robj *o) {
    const SBChain *sb = o->ptr;
    size_t size = zmalloc_size((void *)sb) + zmalloc_size(sb->filters);
    for (size_t i = 0; i < sb->nfilters; i++) size += zmalloc_size(sb->filters[i].inner.bf);
    return size;
}

void bloomDismiss(robj *o) {
    SBChain *sb = o->ptr;
    for (size_t i = 0; i < sb->nfilters; i++)
        dismissMemory(sb->filters[i].inner.bf, sb->filters[i].inner.bytes);
    dismissMemory(sb->filters, sb->nfilters * sizeof(*sb->filters));
    dismissMemory(sb, sizeof(*sb));
}

/* Cursor is the next filter to visit. Metadata is relocated first, including
 * on incremental calls, because the chain may grow between defrag steps. */
unsigned long bloomDefrag(robj *o, unsigned long cursor, size_t steps,
                         void *(*defrag)(void *)) {
    SBChain *sb = o->ptr;
    void *moved = defrag(sb);
    if (moved) o->ptr = sb = moved;
    if ((moved = defrag(sb->filters))) sb->filters = moved;
    while (cursor < sb->nfilters && steps--) {
        struct bloom *b = &sb->filters[cursor++].inner;
        if ((moved = defrag(b->bf))) b->bf = moved;
    }
    return cursor < sb->nfilters ? cursor : 0;
}

void bloomMixDigest(unsigned char *digest, const robj *o) {
    const SBChain *sb = o->ptr;
    uint64_t meta[] = {sb->size, sb->nfilters, sb->options, sb->growth};
    for (size_t i = 0; i < sizeof(meta) / sizeof(*meta); i++) memrev64ifbe(&meta[i]);
    mixDigest(digest, meta, sizeof(meta));
    for (size_t i = 0; i < sb->nfilters; i++) {
        const SBLink *link = &sb->filters[i];
        const struct bloom *b = &link->inner;
        uint64_t fields[] = {link->size, b->entries, b->hashes, b->bits, b->n2, 0, 0};
        memcpy(&fields[5], &b->error, sizeof(double));
        memcpy(&fields[6], &b->bpe, sizeof(double));
        for (size_t j = 0; j < sizeof(fields) / sizeof(*fields); j++) memrev64ifbe(&fields[j]);
        mixDigest(digest, fields, sizeof(fields));
        mixDigest(digest, b->bf, b->bytes);
    }
}

int bloomRewriteAof(rio *r, robj *key, robj *o, int dbid) {
    rio payload;
    /* KeyMeta is emitted below through its AOF callbacks, so the RESTORE
     * payload must not contain another copy. */
    createDumpPayload(&payload, o, key, dbid, DUMP_PAYLOAD_SKIP_KEY_META, 0);

    int ok = rioWriteBulkCount(r,'*',5) &&
             rioWriteBulkString(r,"RESTORE",7) &&
             rioWriteBulkObject(r,key) &&
             rioWriteBulkString(r,"0",1) &&
             rioWriteBulkString(r,payload.io.buffer.ptr,sdslen(payload.io.buffer.ptr)) &&
             rioWriteBulkString(r,"REPLACE",7);
    sdsfree(payload.io.buffer.ptr);
    return ok;
}

#define BLOOM_LEGACY_RDB_TYPE_NAME "MBbloom--"
/* Module type IDs reserve their low 10 bits for the encoding version. */
#define BLOOM_LEGACY_RDB_VERSION_MASK UINT64_C(0x3FF)
#define BLOOM_LEGACY_RDB_VERSION_INITIAL 0
#define BLOOM_LEGACY_RDB_VERSION_BITS 1
#define BLOOM_LEGACY_RDB_VERSION_OPTIONS 2
#define BLOOM_LEGACY_RDB_VERSION_GROWTH 4
#define BLOOM_LEGACY_RDB_VERSION_MAX BLOOM_LEGACY_RDB_VERSION_GROWTH
#define BLOOM_LEGACY_RDB_DEFAULT_OPTIONS 0
#define BLOOM_LEGACY_RDB_DEFAULT_GROWTH 2
#define BLOOM_RDB_NATIVE_VERSION (-1)
#define BLOOM_RDB_MAX_N2 (sizeof(uint64_t) * CHAR_BIT - 1)
#define BLOOM_RDB_VALID_OPTIONS (BLOOM_OPT_NOROUND | BLOOM_OPT_ENTS_IS_BITS | \
                                 BLOOM_OPT_FORCE64 | BLOOM_OPT_NO_SCALING)

int bloomIsLegacyRdbType(uint64_t moduleid) {
    char name[sizeof(BLOOM_LEGACY_RDB_TYPE_NAME)];
    moduleTypeNameByID(name,moduleid);
    return !strcmp(name,BLOOM_LEGACY_RDB_TYPE_NAME);
}

robj *rdbLoadBloomObject(rio *rdb, uint64_t moduleid, int *is_corrupt) {
    int encver = moduleid == BLOOM_RDB_NATIVE_ID ? BLOOM_RDB_NATIVE_VERSION :
                 (int)(moduleid & BLOOM_LEGACY_RDB_VERSION_MASK);
    *is_corrupt = 0;
    SBChain *sb = NULL;
    uint64_t size, nfilters, options = BLOOM_LEGACY_RDB_DEFAULT_OPTIONS;
    uint64_t growth = BLOOM_LEGACY_RDB_DEFAULT_GROWTH;
    int encoded;
#define LOAD_BLOOM_RAW_LEN(dest) do { \
    if (rdbLoadLenByRef(rdb, &encoded, &(dest)) == -1) goto error; \
    if (encoded) goto corrupt; \
} while (0)
#define LOAD_BLOOM_OPCODE(expected) do { \
    if (encver != BLOOM_RDB_NATIVE_VERSION) { \
        uint64_t opcode; \
        LOAD_BLOOM_RAW_LEN(opcode); \
        if (opcode != (expected)) goto corrupt; \
    } \
} while (0)
#define LOAD_BLOOM_LEN(dest) do { \
    LOAD_BLOOM_OPCODE(RDB_MODULE_OPCODE_UINT); \
    LOAD_BLOOM_RAW_LEN(dest); \
} while (0)
    if (encver > BLOOM_LEGACY_RDB_VERSION_MAX) goto corrupt;
    LOAD_BLOOM_LEN(size);
    LOAD_BLOOM_LEN(nfilters);
    if (encver == BLOOM_RDB_NATIVE_VERSION || encver >= BLOOM_LEGACY_RDB_VERSION_OPTIONS)
        LOAD_BLOOM_LEN(options);
    if (encver == BLOOM_RDB_NATIVE_VERSION || encver >= BLOOM_LEGACY_RDB_VERSION_GROWTH)
        LOAD_BLOOM_LEN(growth);
    if (size > SIZE_MAX || nfilters == 0 || nfilters > INT_MAX ||
        nfilters > SIZE_MAX / sizeof(SBLink) || options > UINT_MAX || growth > UINT_MAX ||
        (options & ~BLOOM_RDB_VALID_OPTIONS) ||
        (!(options & BLOOM_OPT_NO_SCALING) && growth == 0)) goto corrupt;
    sb = ztrycalloc(sizeof(*sb));
    if (!sb) goto error;
    sb->filters = ztrycalloc(nfilters * sizeof(*sb->filters));
    if (!sb->filters) goto error;
    sb->size = size;
    sb->options = options;
    sb->growth = growth;
    for (size_t i = 0; i < nfilters; i++) {
        /* Cleanup visits only initialized links after a short read. */
        sb->nfilters = i + 1;
        SBLink *link = &sb->filters[i];
        struct bloom *b = &link->inner;
        uint64_t hashes, n2 = 0, count;
        size_t bytes;
        LOAD_BLOOM_LEN(b->entries);
        LOAD_BLOOM_OPCODE(RDB_MODULE_OPCODE_DOUBLE);
        if (rdbLoadBinaryDoubleValue(rdb, &b->error) == -1) goto error;
        LOAD_BLOOM_LEN(hashes);
        LOAD_BLOOM_OPCODE(RDB_MODULE_OPCODE_DOUBLE);
        if (rdbLoadBinaryDoubleValue(rdb, &b->bpe) == -1) goto error;
        if (encver == BLOOM_LEGACY_RDB_VERSION_INITIAL) {
            double bits = (double)b->entries * b->bpe;
            if (!isfinite(bits) || bits < 1 || bits >= (double)UINT64_MAX) goto corrupt;
            b->bits = (uint64_t)bits;
        } else {
            LOAD_BLOOM_LEN(b->bits);
            LOAD_BLOOM_LEN(n2);
        }
        if (hashes > UINT32_MAX || n2 > BLOOM_RDB_MAX_N2) goto corrupt;
        b->hashes = hashes;
        b->n2 = n2;
        b->force64 = !!(sb->options & BLOOM_OPT_FORCE64);
        LOAD_BLOOM_OPCODE(RDB_MODULE_OPCODE_STRING);
        b->bf = rdbGenericLoadStringObject(rdb, RDB_LOAD_PLAIN, &bytes);
        if (!b->bf) goto error;
        b->bytes = bytes;
        if (bloom_validate_integrity(b)) goto corrupt;
        if (encver >= BLOOM_LEGACY_RDB_VERSION_BITS && b->bits != b->bytes * CHAR_BIT) goto corrupt;
        LOAD_BLOOM_LEN(count);
        if (count > SIZE_MAX || count > b->entries) goto corrupt;
        link->size = count;
    }
    LOAD_BLOOM_OPCODE(RDB_MODULE_OPCODE_EOF);
#undef LOAD_BLOOM_LEN
#undef LOAD_BLOOM_OPCODE
#undef LOAD_BLOOM_RAW_LEN
    if (SB_ValidateIntegrity(sb)) goto corrupt;
    if (!(sb->options & BLOOM_OPT_NO_SCALING) &&
        sb->filters[sb->nfilters - 1].inner.entries > UINT64_MAX / sb->growth) goto corrupt;
    return createBloomObject(sb);

corrupt:
    SBChain_Free(sb);
    *is_corrupt = 1;
    return NULL;
error:
    SBChain_Free(sb);
    return NULL;
}

/* Propagate the empty filter's exact geometry, independently of local defaults. */
static void bloomPropagateCreation(client *c, SBChain *sb) {
    if (!shouldPropagate(server.allowed_propagate_targets)) return;
    size_t len;
    char *header = SBChain_GetEncodedHeader(sb, &len);
    robj *argv[] = {createStringObject("BF.LOADCHUNK",12), c->argv[1],
                    createStringObjectFromLongLong(SB_CHUNKITER_INIT),
                    createStringObject(header,len)};
    alsoPropagate(c->db->id,argv,4,PROPAGATE_AOF|PROPAGATE_REPL);
    decrRefCount(argv[0]);
    decrRefCount(argv[2]);
    decrRefCount(argv[3]);
    SB_FreeEncodedHeader(header);
}

static SBChain *bloomCreateChain(client *c, long long capacity, double error,
                                long long expansion, int nonscaling) {
    int err;
    unsigned options = BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND;
    if (nonscaling || expansion == 0) options |= BLOOM_OPT_NO_SCALING;
    SBChain *sb = SB_NewChain(capacity,error,options,expansion,&err);
    if (!sb) addReplyError(c,err == SB_OOM ? "Insufficient memory to create filter" :
                                                         "could not create filter");
    return sb;
}

void bfReserveCommand(client *c) {
    if (c->argc > 7) {
        addReplyErrorArity(c);
        return;
    }
    double error;
    long long capacity, expansion = server.bf_expansion_factor;
    int nonscaling = expansion == 0, explicit_expansion = 0;
    if (getDoubleFromObjectOrReply(c,c->argv[2],&error,"bad error rate") != C_OK) return;
    if (!(error > 0 && error < 1)) {
        addReplyError(c,"error rate must be in the range (0.000000, 1.000000)");
        return;
    }
    if (error > 0.25) error = 0.25;
    if (getLongLongFromObjectOrReply(c,c->argv[3],&capacity,"bad capacity") != C_OK) return;
    if (capacity < 1 || capacity > (1LL<<30)) {
        addReplyError(c,"capacity must be in the range [1, 1073741824]");
        return;
    }
    for (int i = 4; i < c->argc; i++) {
        if (!strcasecmp(c->argv[i]->ptr,"NONSCALING")) {
            nonscaling = 1;
        } else if (!strcasecmp(c->argv[i]->ptr,"EXPANSION")) {
            if (++i == c->argc) {
                addReplyError(c,"no expansion");
                return;
            }
            if (getLongLongFromObjectOrReply(c,c->argv[i],&expansion,"bad expansion") != C_OK) return;
            explicit_expansion = 1;
            if (expansion < 0 || expansion > 32768) {
                addReplyError(c,"expansion must be in the range [0, 32768]");
                return;
            }
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    if (nonscaling && explicit_expansion && expansion != 0) {
        addReplyError(c,"-Nonscaling filters cannot expand");
        return;
    }
    dictEntryLink link;
    kvobj *kv = lookupKeyWriteWithLink(c->db,c->argv[1],&link);
    if (checkType(c,kv,OBJ_BLOOM)) return;
    if (kv) {
        addReplyError(c,"item exists");
        return;
    }
    SBChain *sb = bloomCreateChain(c,capacity,error,expansion,nonscaling);
    if (!sb) return;
    robj *o = createBloomObject(sb);
    kv = dbAddByLink(c->db,c->argv[1],&o,&link);
    keyModified(c,c->db,c->argv[1],kv,1);
    server.dirty++;
    bloomPropagateCreation(c,sb);
    preventCommandPropagation(c);
    addReply(c,shared.ok);
}

static void bloomInsert(client *c, int first, int multi, long long capacity,
                        double error, long long expansion, int nonscaling, int nocreate) {
    dictEntryLink link;
    kvobj *kv = lookupKeyWriteWithLink(c->db,c->argv[1],&link);
    if (checkType(c,kv,OBJ_BLOOM)) return;
    int created = kv == NULL;
    if (created) {
        if (nocreate) {
            addReplyError(c,"not found");
            return;
        }
        SBChain *sb = bloomCreateChain(c,capacity,error,expansion,nonscaling);
        if (!sb) return;
        robj *o = createBloomObject(sb);
        kv = dbAddByLink(c->db,c->argv[1],&o,&link);
        bloomPropagateCreation(c,sb);
    }
    SBChain *sb = kv->ptr;
    size_t oldlen = sb->size;
    size_t oldsize = server.memory_tracking_enabled ? kvobjAllocSize(kv) : 0;
    void *replylen = multi ? addReplyDeferredLen(c) : NULL;
    /* Build replay arguments only when a successful insert needs propagation. */
    int propagate = shouldPropagate(server.allowed_propagate_targets);
    robj **argv = NULL;
    int argc = 4, replies = 0;
    for (int i = first; i < c->argc; i++) {
        int rv = SBChain_Add(sb,c->argv[i]->ptr,sdslen(c->argv[i]->ptr));
        replies++;
        if (rv == SB_FULL) {
            addReplyError(c,"non scaling filter is full");
            break;
        } else if (rv < 0) {
            addReplyError(c,"problem inserting into filter");
        } else {
            addReplyBool(c,rv);
            if (rv && propagate) {
                /* Failed items must not be replayed after a partial failure. */
                if (!argv) {
                    argv = zmalloc(sizeof(*argv) * (c->argc - first + 4));
                    argv[0] = createStringObject("BF.INSERT",9);
                    argv[1] = c->argv[1];
                    argv[2] = createStringObject("NOCREATE",8);
                    argv[3] = createStringObject("ITEMS",5);
                }
                argv[argc++] = c->argv[i];
            }
        }
    }
    if (multi) setDeferredArrayLen(c,replylen,replies);
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db,getKeySlot(c->argv[1]->ptr),kv,oldsize,kvobjAllocSize(kv));
    if (created || sb->size != oldlen) {
        updateKeysizesHist(c->db,OBJ_BLOOM,oldlen,sb->size);
        keyModified(c,c->db,c->argv[1],kv,1);
        server.dirty += created + sb->size - oldlen;
    }
    if (argv) {
        alsoPropagate(c->db->id,argv,argc,PROPAGATE_AOF|PROPAGATE_REPL);
        decrRefCount(argv[0]);
        decrRefCount(argv[2]);
        decrRefCount(argv[3]);
        zfree(argv);
    }
    preventCommandPropagation(c);
}

void bfAddCommand(client *c) {
    bloomInsert(c,2,0,server.bf_initial_size,server.bf_error_rate,
                server.bf_expansion_factor,0,0);
}

void bfMAddCommand(client *c) {
    bloomInsert(c,2,1,server.bf_initial_size,server.bf_error_rate,
                server.bf_expansion_factor,0,0);
}

void bfInsertCommand(client *c) {
    long long capacity = server.bf_initial_size, expansion = server.bf_expansion_factor;
    double error = server.bf_error_rate;
    int nonscaling = expansion == 0, nocreate = 0, i;
    for (i = 2; i < c->argc; i++) {
        char *arg = c->argv[i]->ptr;
        if (!strcasecmp(arg,"ITEMS")) {
            i++;
            break;
        } else if (!strcasecmp(arg,"NONSCALING")) {
            nonscaling = 1;
        } else if (!strcasecmp(arg,"NOCREATE")) {
            nocreate = 1;
        } else if (!strcasecmp(arg,"ERROR") || !strcasecmp(arg,"CAPACITY") ||
                   !strcasecmp(arg,"EXPANSION")) {
            if (++i == c->argc) break;
            if (!strcasecmp(arg,"ERROR")) {
                if (getDoubleFromObject(c->argv[i],&error) != C_OK || !(error > 0 && error < 1)) {
                    addReplyError(c,"-Bad error rate");
                    return;
                }
                if (error > 0.25) error = 0.25;
            } else if (!strcasecmp(arg,"CAPACITY")) {
                if (getLongLongFromObject(c->argv[i],&capacity) != C_OK ||
                    capacity < 1 || capacity > (1LL<<30)) {
                    addReplyError(c,"-Bad capacity");
                    return;
                }
            } else if (getLongLongFromObject(c->argv[i],&expansion) != C_OK ||
                       expansion < 0 || expansion > 32768) {
                addReplyError(c,"-Bad expansion");
                return;
            }
        } else {
            addReplyError(c,"-Unknown argument received");
            return;
        }
    }
    if (i >= c->argc) {
        addReplyErrorArity(c);
        return;
    }
    bloomInsert(c,i,1,capacity,error,expansion,nonscaling,nocreate);
}

static void bloomExists(client *c, int multi) {
    kvobj *kv = lookupKeyRead(c->db,c->argv[1]);
    /* Preserve RedisBloom's false reply for missing keys and other data types. */
    SBChain *sb = kv && kv->type == OBJ_BLOOM ? kv->ptr : NULL;
    if (multi) addReplyArrayLen(c,c->argc - 2);
    for (int i = 2; i < c->argc; i++)
        addReplyBool(c,sb && SBChain_Check(sb,c->argv[i]->ptr,sdslen(c->argv[i]->ptr)));
}

void bfExistsCommand(client *c) { bloomExists(c,0); }
void bfMExistsCommand(client *c) { bloomExists(c,1); }

void bfCardCommand(client *c) {
    kvobj *kv = lookupKeyRead(c->db,c->argv[1]);
    if (checkType(c,kv,OBJ_BLOOM)) return;
    addReplyLongLong(c,kv ? ((SBChain *)kv->ptr)->size : 0);
}

static SBChain *bloomLookupRead(client *c) {
    kvobj *kv = lookupKeyRead(c->db,c->argv[1]);
    if (!kv) {
        addReplyError(c,"not found");
        return NULL;
    }
    if (checkType(c,kv,OBJ_BLOOM)) return NULL;
    return kv->ptr;
}

void bfInfoCommand(client *c) {
    if (c->argc > 3) {
        addReplyErrorArity(c);
        return;
    }
    SBChain *sb = bloomLookupRead(c);
    if (!sb) return;
    const char *options[] = {"capacity","size","filters","items","expansion"};
    const char *labels[] = {"Capacity","Size","Number of filters",
                           "Number of items inserted","Expansion rate"};
    int first = 0, end = 5;
    if (c->argc == 3) {
        for (; first < 5; first++)
            if (!strcasecmp(c->argv[2]->ptr,options[first])) break;
        if (first == 5) {
            addReplyError(c,"-Invalid information value");
            return;
        }
        end = first + 1;
    }
    uint64_t values[] = {0,sizeof(*sb) + sb->nfilters * sizeof(*sb->filters),
                         sb->nfilters,sb->size,sb->growth};
    for (size_t i = 0; i < sb->nfilters; i++) {
        values[0] += sb->filters[i].inner.entries;
        values[1] += sb->filters[i].inner.bytes;
    }
    if (c->argc == 3 && c->resp == 2) addReplyArrayLen(c,1);
    else addReplyMapLen(c,end - first);
    for (int i = first; i < end; i++) {
        if (c->argc == 2 || c->resp > 2) addReplyStatus(c,labels[i]);
        if (i == 4 && (sb->options & BLOOM_OPT_NO_SCALING)) addReplyNull(c);
        else addReplyLongLong(c,values[i]);
    }
}

void bfDebugCommand(client *c) {
    SBChain *sb = bloomLookupRead(c);
    if (!sb) return;
    addReplyArrayLen(c,1 + sb->nfilters);
    addReplyBulkSds(c,sdscatprintf(sdsempty(),"size:%zu",sb->size));
    for (size_t i = 0; i < sb->nfilters; i++) {
        SBLink *link = &sb->filters[i];
        struct bloom *b = &link->inner;
        addReplyBulkSds(c,sdscatprintf(sdsempty(),
            "bytes:%" PRIu64 " bits:%" PRIu64 " hashes:%u hashwidth:%u capacity:%" PRIu64
            " size:%zu ratio:%g", b->bytes,b->bits ? b->bits : UINT64_C(1)<<b->n2,
            b->hashes,sb->options & BLOOM_OPT_FORCE64 ? 64 : 32,b->entries,link->size,b->error));
    }
}

void bfScanDumpCommand(client *c) {
    SBChain *sb = bloomLookupRead(c);
    if (!sb) return;
    long long iter;
    if (getLongLongFromObjectOrReply(c,c->argv[2],&iter,"-Second argument must be numeric") != C_OK) return;
    size_t len = 0;
    if (iter == 0) {
        char *header = SBChain_GetEncodedHeader(sb,&len);
        addReplyArrayLen(c,2);
        addReplyLongLong(c,SB_CHUNKITER_INIT);
        addReplyBulkCBuffer(c,header,len);
        SB_FreeEncodedHeader(header);
    } else {
        const char *buf = SBChain_GetEncodedChunk(sb,&iter,&len,16*1024*1024);
        addReplyArrayLen(c,2);
        addReplyLongLong(c,iter);
        addReplyBulkCBuffer(c,buf ? buf : "",len);
    }
}

void bfLoadChunkCommand(client *c) {
    long long iter;
    if (getLongLongFromObjectOrReply(c,c->argv[2],&iter,"Second argument must be numeric") != C_OK) return;
    dictEntryLink link;
    kvobj *kv = lookupKeyWriteWithLink(c->db,c->argv[1],&link);
    if (checkType(c,kv,OBJ_BLOOM)) return;
    sds buf = c->argv[3]->ptr;
    const char *err;
    if (!kv) {
        if (iter != SB_CHUNKITER_INIT) {
            addReplyError(c,"not found");
            return;
        }
        SBChain *sb = SB_NewChainFromHeader(buf,sdslen(buf),&err);
        if (!sb) {
            addReplyErrorFormat(c,"-%s",err);
            return;
        }
        robj *o = createBloomObject(sb);
        kv = dbAddByLink(c->db,c->argv[1],&o,&link);
    } else if (SBChain_LoadEncodedChunk(kv->ptr,iter,buf,sdslen(buf),&err) != 0) {
        addReplyErrorFormat(c,"-%s",err);
        return;
    }
    keyModified(c,c->db,c->argv[1],kv,1);
    server.dirty++;
    addReply(c,shared.ok);
}
