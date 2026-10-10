/* Native Bloom commands and object lifecycle.
 * Copyright (c) 2026-Present, Redis Ltd.
 * Licensed under RSALv2, SSPLv1, or AGPLv3.
 * The filter algorithm and serialization helpers live in bloom.c/bloom.h. */
#include "server.h"
#include "bloom.h"
#include <math.h>
#include <inttypes.h>

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

typedef struct {
    long long capacity, expansion;
    double error;
    int autocreate, nonscaling;
} bloomInsertOptions;

static bloomInsertOptions bloomDefaults(void) {
    bloomInsertOptions options = {
        .capacity = server.bloom_capacity,
        .expansion = server.bloom_expansion,
        .autocreate = 1,
        .nonscaling = server.bloom_expansion == 0,
    };
    string2d(server.bloom_error_rate, sdslen(server.bloom_error_rate), &options.error);
    if (options.error > 0.25) options.error = 0.25;
    return options;
}

static int bloomArgEquals(robj *arg, const char *name) {
    return sdslen(arg->ptr) == strlen(name) && !strcasecmp(arg->ptr, name);
}

static void bloomInsert(client *c, int first, int multi, const bloomInsertOptions *options) {
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (o && checkType(c, o, OBJ_BLOOM)) return;
    int created = o == NULL;
    if (!o) {
        if (!options->autocreate) { addReplyError(c, "not found"); return; }
        unsigned flags = BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND;
        if (options->nonscaling) flags |= BLOOM_OPT_NO_SCALING;
        int rc;
        SBChain *chain = SB_NewChain(options->capacity, options->error, flags, options->expansion, &rc);
        if (!chain) {
            addReplyError(c, rc == SB_OOM ? "Insufficient memory to create filter" : "could not create filter");
            return;
        }
        o = createBloomObject(chain);
        dbAdd(c->db, c->argv[1], &o);
    }
    size_t oldsize = server.memory_tracking_enabled ? kvobjAllocSize(o) : 0;
    size_t oldcount = bloomObjectLength(o);
    void *arraylen = multi ? addReplyDeferredLen(c) : NULL;
    int replies = 0, modified = created;
    for (int i = first; i < c->argc; i++) {
        int result = SBChain_Add(o->ptr, c->argv[i]->ptr, sdslen(c->argv[i]->ptr));
        replies++;
        if (result < 0) {
            addReplyError(c, result == SB_FULL ? "non scaling filter is full" : "problem inserting into filter");
            /* RedisBloom stops at the first full-filter error, retaining the prefix. */
            if (result == SB_FULL) break;
        } else {
            modified |= result;
            bloomReplyBool(c, result);
        }
    }
    if (multi) setDeferredArrayLen(c, arraylen, replies);
    if (modified) bloomModified(c, o, c->cmd->fullname, oldsize, oldcount);
}

void bfAddCommand(client *c) {
    bloomInsertOptions options = bloomDefaults();
    bloomInsert(c, 2, 0, &options);
}

void bfMAddCommand(client *c) {
    bloomInsertOptions options = bloomDefaults();
    bloomInsert(c, 2, 1, &options);
}

void bfInsertCommand(client *c) {
    bloomInsertOptions options = bloomDefaults();
    int i;
    for (i = 2; i < c->argc; i++) {
        if (bloomArgEquals(c->argv[i], "ITEMS")) { i++; break; }
        if (bloomArgEquals(c->argv[i], "NOCREATE")) options.autocreate = 0;
        else if (bloomArgEquals(c->argv[i], "NONSCALING")) options.nonscaling = 1;
        else if (bloomArgEquals(c->argv[i], "ERROR")) {
            if (++i == c->argc) { addReplyErrorArity(c); return; }
            if (getDoubleFromObjectOrReply(c, c->argv[i], &options.error, "Bad error rate") != C_OK) return;
            if (!isfinite(options.error) || options.error <= 0 || options.error >= 1) {
                addReplyError(c, "Bad error rate"); return;
            }
            if (options.error > 0.25) options.error = 0.25;
        } else if (bloomArgEquals(c->argv[i], "CAPACITY")) {
            if (++i == c->argc) { addReplyErrorArity(c); return; }
            if (getLongLongFromObjectOrReply(c, c->argv[i], &options.capacity, "Bad capacity") != C_OK) return;
            if (options.capacity < 1 || options.capacity > (1LL << 30)) {
                addReplyError(c, "Bad capacity"); return;
            }
        } else if (bloomArgEquals(c->argv[i], "EXPANSION")) {
            if (++i == c->argc) { addReplyErrorArity(c); return; }
            if (getLongLongFromObjectOrReply(c, c->argv[i], &options.expansion, "Bad expansion") != C_OK) return;
            if (options.expansion < 0 || options.expansion > 32768) {
                addReplyError(c, "Bad expansion"); return;
            }
        } else {
            addReplyError(c, "Unknown argument received"); return;
        }
    }
    if (i >= c->argc) { addReplyErrorArity(c); return; }
    if (!options.expansion) options.nonscaling = 1;
    bloomInsert(c, i, 1, &options);
}

void bfExistsCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    /* Preserve RedisBloom's false reply for keys of other types. */
    int found = o && o->type == OBJ_BLOOM && SBChain_Check(o->ptr, c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
    bloomReplyBool(c, found);
}

void bfMExistsCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    addReplyArrayLen(c, c->argc - 2);
    for (int i = 2; i < c->argc; i++)
        bloomReplyBool(c, o && o->type == OBJ_BLOOM &&
            SBChain_Check(o->ptr, c->argv[i]->ptr, sdslen(c->argv[i]->ptr)));
}

void bfCardCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (!o) { addReplyLongLong(c, 0); return; }
    if (checkType(c, o, OBJ_BLOOM)) return;
    addReplyLongLong(c, bloomObjectLength(o));
}

void bfInfoCommand(client *c) {
    if (c->argc > 3) { addReplyErrorArity(c); return; }
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (!o) { addReplyError(c, "not found"); return; }
    if (checkType(c, o, OBJ_BLOOM)) return;
    SBChain *chain = o->ptr;
    const char *options[] = {"CAPACITY", "SIZE", "FILTERS", "ITEMS", "EXPANSION"};
    const char *labels[] = {"Capacity", "Size", "Number of filters", "Number of items inserted", "Expansion rate"};
    uint64_t capacity = 0, bytes = sizeof(*chain) + sizeof(SBLink) * chain->nfilters;
    for (size_t i = 0; i < chain->nfilters; i++) {
        capacity += chain->filters[i].inner.entries;
        bytes += chain->filters[i].inner.bytes;
    }
    uint64_t values[] = {capacity, bytes, chain->nfilters, chain->size, chain->growth};
    int first = 0, end = 5;
    if (c->argc == 3) {
        while (first < 5 && !bloomArgEquals(c->argv[2], options[first])) first++;
        if (first == 5) { addReplyError(c, "Invalid information value"); return; }
        end = first + 1;
    }
    if (c->resp == 3) addReplyMapLen(c, end - first);
    else addReplyArrayLen(c, c->argc == 3 ? 1 : 10);
    for (int i = first; i < end; i++) {
        if (c->resp == 3 || c->argc == 2) addReplyStatus(c, labels[i]);
        if (i == 4 && (chain->options & BLOOM_OPT_NO_SCALING)) addReplyNull(c);
        else addReplyLongLong(c, values[i]);
    }
}

void bfDebugCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (!o) { addReplyError(c, "not found"); return; }
    if (checkType(c, o, OBJ_BLOOM)) return;
    SBChain *chain = o->ptr;
    addReplyArrayLen(c, chain->nfilters + 1);
    char info[256];
    int len = snprintf(info, sizeof(info), "size:%zu", chain->size);
    addReplyBulkCBuffer(c, info, len);
    for (size_t i = 0; i < chain->nfilters; i++) {
        SBLink *link = &chain->filters[i];
        len = snprintf(info, sizeof(info),
            "bytes:%" PRIu64 " bits:%" PRIu64 " hashes:%u hashwidth:%u capacity:%" PRIu64
            " size:%zu ratio:%g", link->inner.bytes,
            link->inner.bits ? link->inner.bits : UINT64_C(1) << link->inner.n2,
            link->inner.hashes, chain->options & BLOOM_OPT_FORCE64 ? 64 : 32,
            link->inner.entries, link->size, link->inner.error);
        addReplyBulkCBuffer(c, info, len);
    }
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
        if (!chain) { addReplyError(c, !strncmp(error, "ERR ", 4) ? error + 4 : error); return; }
        o = createBloomObject(chain);
        dbAdd(c->db, c->argv[1], &o);
    } else {
        if (iter == 1) { addReplyError(c, "item exists"); return; }
        if (SBChain_LoadEncodedChunk(o->ptr, iter, c->argv[3]->ptr, sdslen(c->argv[3]->ptr), &error)) {
            addReplyError(c, !strncmp(error, "ERR ", 4) ? error + 4 : error); return;
        }
    }
    bloomModified(c, o, "bf.loadchunk", server.memory_tracking_enabled ? kvobjAllocSize(o) : 0, bloomObjectLength(o));
    addReply(c, shared.ok);
}
