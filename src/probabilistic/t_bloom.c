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

/* Propagate the empty filter's exact geometry, independently of local defaults. */
static void bloomPropagateCreation(client *c, SBChain *sb) {
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
    /* Only successful inserts may be replayed after a partial allocation failure. */
    robj **argv = zmalloc(sizeof(*argv) * (c->argc - first + 4));
    argv[0] = createStringObject("BF.INSERT",9);
    argv[1] = c->argv[1];
    argv[2] = createStringObject("NOCREATE",8);
    argv[3] = createStringObject("ITEMS",5);
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
            if (rv) argv[argc++] = c->argv[i];
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
    if (argc > 4) alsoPropagate(c->db->id,argv,argc,PROPAGATE_AOF|PROPAGATE_REPL);
    preventCommandPropagation(c);
    decrRefCount(argv[0]);
    decrRefCount(argv[2]);
    decrRefCount(argv[3]);
    zfree(argv);
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

#ifdef REDIS_TEST
#include "testhelp.h"
#include "rdb.h"

size_t lazyfreeGetFreeEffort(robj *key, robj *obj, int dbid);
size_t kvobjComputeSize(robj *key, kvobj *o, size_t sample_size, int dbid);

/* Move every allocation, even if the allocator would normally leave it alone. */
static void *bloomTestRelocate(void *ptr) {
    size_t len = zmalloc_size(ptr);
    void *moved = zmalloc(len);
    memcpy(moved, ptr, len);
    zfree(ptr);
    return moved;
}

int bloomObjectTest(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;
    int before_failed = __failed_tests;
    size_t before_memory = zmalloc_used_memory();
    int err;
    SBChain *sb = SB_NewChain(2, 0.0001, BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND, 2, &err);
    test_cond("Create native Bloom object", sb != NULL);
    if (!sb) return 1;
    robj *o = createBloomObject(sb);
    for (unsigned i = 0; i < 100; i++) SBChain_Add(sb, &i, sizeof(i));
    test_cond("Bloom object type, encoding, length and free effort",
              o->type == OBJ_BLOOM && o->encoding == OBJ_ENCODING_BLOOM &&
              getObjectLength(o) == sb->size && lazyfreeGetFreeEffort(NULL, o, 0) == sb->nfilters + 2);
    unsigned char original[20] = {0}, copied[20] = {0};
    bloomMixDigest(original, o);
    robj *copy = bloomDup(o);
    bloomMixDigest(copied, copy);
    test_cond("Bloom copy preserves data and metadata", memcmp(original, copied, 20) == 0);
    SBChain *other = copy->ptr;
    int independent = other != sb && other->filters != sb->filters;
    for (size_t i = 0; i < sb->nfilters; i++)
        independent &= other->filters[i].inner.bf != sb->filters[i].inner.bf;
    other->filters[0].inner.bf[0] ^= 1;
    memset(copied, 0, sizeof(copied));
    bloomMixDigest(copied, copy);
    test_cond("Bloom copy has independent allocations and digest detects bit changes",
              independent && memcmp(original, copied, 20) != 0);
    decrRefCount(copy);

    for (int compression = 0; compression <= 1; compression++) {
        int old_compression = server.rdb_compression;
        server.rdb_compression = compression;
        rio out;
        rioInitWithBuffer(&out, sdsempty());
        test_cond("Save native Bloom RDB type and value",
                  rdbSaveObjectType(&out, o) > 0 && rdbSaveObject(&out, o, NULL, 0) > 0);
        rio in;
        rioInitWithBuffer(&in, out.io.buffer.ptr);
        int type = rdbLoadObjectType(&in);
        copy = rdbLoadObject(type, &in, NULL, 0, &err);
        test_cond("Load native Bloom RDB value", type == RDB_TYPE_BLOOM && copy != NULL);
        if (copy) {
            memset(copied, 0, sizeof(copied));
            bloomMixDigest(copied, copy);
            test_cond("RDB round trip preserves Bloom membership and digest",
                      memcmp(original, copied, 20) == 0 && getObjectLength(copy) == sb->size);
            decrRefCount(copy);
        }
        sdsfree(out.io.buffer.ptr);
        server.rdb_compression = old_compression;
    }

    size_t alloc_size = bloomAllocSize(o);
    sds key = sdsnew("bloom");
    copy = bloomDup(o);
    kvobj *kv = kvobjSet(key, copy, 0);
    test_cond("Bloom database memory accounting includes filter allocations",
              kvobjAllocSize(kv) >= bloomAllocSize(kv) + sizeof(kvobj) &&
              kvobjComputeSize(NULL, kv, 0, 0) == kvobjAllocSize(kv));
    decrRefCount(kv);
    sdsfree(key);

    unsigned long cursor = 0;
    do {
        cursor = bloomDefrag(o, cursor, 1, bloomTestRelocate);
    } while (cursor);
    memset(copied, 0, sizeof(copied));
    bloomMixDigest(copied, o);
    test_cond("Incremental defrag preserves all pointers and filter contents",
              memcmp(original, copied, 20) == 0 && bloomAllocSize(o) >= alloc_size);
    bloomDefrag(o, 0, SIZE_MAX, bloomTestRelocate);
    memset(copied, 0, sizeof(copied));
    bloomMixDigest(copied, o);
    test_cond("Full defrag preserves Bloom contents", memcmp(original, copied, 20) == 0);
    decrRefCount(o);
    test_cond("Bloom object operations release all allocations", zmalloc_used_memory() == before_memory);
    return __failed_tests != before_failed;
}
#endif
