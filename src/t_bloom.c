/* Native Bloom commands and object lifecycle.
 * Copyright (c) 2026-Present, Redis Ltd.
 * Licensed under RSALv2, SSPLv1, or AGPLv3.
 * The filter algorithm and serialization helpers live in bloom.c/bloom.h. */
#include "server.h"
#include "bloom.h"
#include <math.h>

int bloomValidateErrorRate(char *value, const char **err) {
    double rate;
    if (!string2d(value, strlen(value), &rate) || !isfinite(rate) || rate <= 0 || rate >= 1) {
        *err = "error rate must be between 0 and 1";
        return 0;
    }
    return 1;
}

int bloomUpdateErrorRate(const char **err) {
    UNUSED(err);
    double rate;
    if (string2d(server.bloom_error_rate, sdslen(server.bloom_error_rate), &rate) && rate > 0.25) {
        sdsfree(server.bloom_error_rate);
        server.bloom_error_rate = sdsnew("0.25");
    }
    return 1;
}

static int bloomArgumentIndex(client *c, const char *name) {
    for (int i = 4; i < c->argc; i++)
        if (sdslen(c->argv[i]->ptr) == strlen(name) && !strcasecmp(c->argv[i]->ptr, name)) return i;
    return -1;
}

static void bloomReplyBool(client *c, int value) {
    if (c->resp == 3) addReplyBool(c, value != 0);
    else addReplyLongLong(c, value != 0);
}

robj *createBloomObject(void *chain) {
    return createObject(OBJ_BLOOM, chain);
}

void freeBloomObject(robj *o) { SBChain_Free(o->ptr); }
size_t bloomObjectLength(robj *o) { return ((SBChain *)o->ptr)->size; }
size_t bloomFreeEffort(robj *o) { return ((SBChain *)o->ptr)->nfilters + 2; }

size_t bloomAllocSize(robj *o) {
    SBChain *chain = o->ptr;
    size_t size = zmalloc_size(chain) + zmalloc_size(chain->filters);
    for (size_t i = 0; i < chain->nfilters; i++) size += zmalloc_size(chain->filters[i].inner.bf);
    return size;
}

void bloomDismiss(robj *o) {
    SBChain *chain = o->ptr;
    for (size_t i = 0; i < chain->nfilters; i++)
        dismissMemory(chain->filters[i].inner.bf, chain->filters[i].inner.bytes);
}

robj *bloomDup(robj *o) {
    SBChain *source = o->ptr;
    SBChain *copy = zmalloc(sizeof(*copy));
    *copy = *source;
    copy->filters = zmalloc(source->nfilters * sizeof(SBLink));
    memcpy(copy->filters, source->filters, source->nfilters * sizeof(SBLink));
    for (size_t i = 0; i < source->nfilters; i++) {
        copy->filters[i].inner.bf = zmalloc(source->filters[i].inner.bytes);
        memcpy(copy->filters[i].inner.bf, source->filters[i].inner.bf, source->filters[i].inner.bytes);
    }
    return createBloomObject(copy);
}

void bloomDefrag(robj *o, void *(*defrag)(void *)) {
    void *moved = defrag(o->ptr);
    if (moved) o->ptr = moved;
    SBChain *chain = o->ptr;
    moved = defrag(chain->filters);
    if (moved) chain->filters = moved;
    for (size_t i = 0; i < chain->nfilters; i++) {
        moved = defrag(chain->filters[i].inner.bf);
        if (moved) chain->filters[i].inner.bf = moved;
    }
}

void bloomDigest(unsigned char *digest, robj *o) {
    SBChain *chain = o->ptr;
    size_t len;
    char *header = SBChain_GetEncodedHeader(chain, &len);
    mixDigest(digest, header, len);
    SB_FreeEncodedHeader(header);
    for (size_t i = 0; i < chain->nfilters; i++)
        mixDigest(digest, chain->filters[i].inner.bf, chain->filters[i].inner.bytes);
}

static void bloomModified(client *c, robj *o, const char *event, size_t oldsize, size_t oldcount) {
    updateKeysizesHist(c->db, OBJ_BLOOM, oldcount, bloomObjectLength(o));
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, oldsize, kvobjAllocSize(o));
    keyModified(c, c->db, c->argv[1], o, 1);
    notifyKeyspaceEvent(NOTIFY_BLOOM, (char *)event, c->argv[1], c->db->id);
    server.dirty++;
}

void bfReserveCommand(client *c) {
    if (c->argc > 7) { addReplyErrorArity(c); return; }
    double error;
    long long capacity, expansion = server.bloom_expansion;
    if (getDoubleFromObjectOrReply(c, c->argv[2], &error, "bad error rate") != C_OK) return;
    if (!isfinite(error) || error <= 0 || error >= 1) {
        addReplyError(c, "error rate must be in the range (0.000000, 1.000000)"); return;
    }
    if (error > 0.25) error = 0.25;
    if (getLongLongFromObjectOrReply(c, c->argv[3], &capacity, "bad capacity") != C_OK) return;
    if (capacity < 1 || capacity > (1LL << 30)) {
        addReplyError(c, "capacity must be in the range [1, 1073741824]"); return;
    }
    unsigned options = !expansion || bloomArgumentIndex(c, "NONSCALING") != -1 ? BLOOM_OPT_NO_SCALING : 0;
    int index = bloomArgumentIndex(c, "EXPANSION");
    if (index + 1 == c->argc) { addReplyError(c, "no expansion"); return; }
    if (index != -1) {
        if (getLongLongFromObjectOrReply(c, c->argv[index+1], &expansion, "bad expansion") != C_OK) return;
        if (!expansion) options = BLOOM_OPT_NO_SCALING;
        else if (options) { addReplyError(c, "Nonscaling filters cannot expand"); return; }
        if (expansion < 0 || expansion > 32768) {
            addReplyError(c, "expansion must be in the range [0, 32768]"); return;
        }
    }
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (o) {
        if (!checkType(c, o, OBJ_BLOOM)) addReplyError(c, "item exists");
        return;
    }
    int rc;
    SBChain *chain = SB_NewChain(capacity, error, options | BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND, expansion, &rc);
    if (!chain) { addReplyError(c, "could not create filter"); return; }
    o = createBloomObject(chain);
    dbAdd(c->db, c->argv[1], &o);
    bloomModified(c, o, "bf.reserve", server.memory_tracking_enabled ? kvobjAllocSize(o) : 0, 0);
    addReply(c, shared.ok);
}

void bfAddCommand(client *c) {
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (o && checkType(c, o, OBJ_BLOOM)) return;
    int created = o == NULL;
    if (!o) {
        double error = 0.01;
        string2d(server.bloom_error_rate, sdslen(server.bloom_error_rate), &error);
        if (error > 0.25) error = 0.25;
        unsigned options = BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND;
        if (!server.bloom_expansion) options |= BLOOM_OPT_NO_SCALING;
        int rc;
        SBChain *chain = SB_NewChain(server.bloom_capacity, error, options, server.bloom_expansion, &rc);
        if (!chain) { addReplyError(c, "could not create filter"); return; }
        o = createBloomObject(chain);
    }
    size_t oldsize = server.memory_tracking_enabled ? kvobjAllocSize(o) : 0;
    size_t oldcount = bloomObjectLength(o);
    int result = SBChain_Add(o->ptr, c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
    if (result < 0) {
        if (created) decrRefCount(o);
        addReplyError(c, result == SB_FULL ? "non scaling filter is full" : "problem inserting into filter");
        return;
    }
    if (created) {
        dbAdd(c->db, c->argv[1], &o);
        oldsize = server.memory_tracking_enabled ? kvobjAllocSize(o) : 0;
        oldcount = bloomObjectLength(o);
    }
    if (result) bloomModified(c, o, "bf.add", oldsize, oldcount);
    bloomReplyBool(c, result);
}

void bfExistsCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    /* Preserve RedisBloom's false reply for keys of other types. */
    int found = o && o->type == OBJ_BLOOM && SBChain_Check(o->ptr, c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
    bloomReplyBool(c, found);
}

void bfScanDumpCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (!o) { addReplyError(c, "not found"); return; }
    if (checkType(c, o, OBJ_BLOOM)) return;
    long long iter;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &iter, "invalid iterator") != C_OK) return;
    if (iter < 0) { addReplyError(c, "invalid iterator"); return; }
    size_t len = 0;
    addReplyArrayLen(c, 2);
    if (!iter) {
        char *header = SBChain_GetEncodedHeader(o->ptr, &len);
        addReplyLongLong(c, SB_CHUNKITER_INIT);
        addReplyBulkCBuffer(c, header, len);
        SB_FreeEncodedHeader(header);
    } else {
        const char *chunk = SBChain_GetEncodedChunk(o->ptr, &iter, &len, 16 * 1024 * 1024);
        addReplyLongLong(c, iter);
        addReplyBulkCBuffer(c, chunk ? chunk : "", chunk ? len : 0);
    }
}

void bfLoadChunkCommand(client *c) {
    long long iter;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &iter, "invalid iterator") != C_OK) return;
    if (iter <= 0) { addReplyError(c, "invalid iterator"); return; }
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (o && checkType(c, o, OBJ_BLOOM)) return;
    const char *error = NULL;
    if (!o) {
        if (iter != 1) { addReplyError(c, "not found"); return; }
        SBChain *chain = SB_NewChainFromHeader(c->argv[3]->ptr, sdslen(c->argv[3]->ptr), &error);
        if (!chain) { addReplyError(c, error); return; }
        o = createBloomObject(chain);
        dbAdd(c->db, c->argv[1], &o);
    } else {
        if (iter == 1) { addReplyError(c, "item exists"); return; }
        if (SBChain_LoadEncodedChunk(o->ptr, iter, c->argv[3]->ptr, sdslen(c->argv[3]->ptr), &error)) {
            addReplyError(c, error); return;
        }
    }
    bloomModified(c, o, "bf.loadchunk", server.memory_tracking_enabled ? kvobjAllocSize(o) : 0, bloomObjectLength(o));
    addReply(c, shared.ok);
}
