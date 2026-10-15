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
