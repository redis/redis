/* Native RDB/AOF integration retaining the RedisBloom wire representation.
 * Copyright (c) 2026-Present, Redis Ltd.; RSALv2/SSPLv1/AGPLv3. */
#include "server.h"
#include "bloom.h"
#include <math.h>

typedef struct { rio *rdb; ssize_t bytes; int error; } bloomIO;

/* MBbloom--, encoding version 4. Keep this ID for bidirectional migration. */
uint64_t bloomRdbId(void) { return UINT64_C(3465209449566631940); }

static void addSaved(bloomIO *io, ssize_t count) {
    if (count < 0) io->error = 1;
    else io->bytes += count;
}
static void saveUnsigned(bloomIO *io, uint64_t value) {
    if (io->error) return;
    addSaved(io, rdbSaveLen(io->rdb, RDB_MODULE_OPCODE_UINT));
    addSaved(io, rdbSaveLen(io->rdb, value));
}
static void saveDouble(bloomIO *io, double value) {
    if (io->error) return;
    addSaved(io, rdbSaveLen(io->rdb, RDB_MODULE_OPCODE_DOUBLE));
    addSaved(io, rdbSaveBinaryDoubleValue(io->rdb, value));
}
static void saveString(bloomIO *io, const char *value, size_t len) {
    if (io->error) return;
    addSaved(io, rdbSaveLen(io->rdb, RDB_MODULE_OPCODE_STRING));
    addSaved(io, rdbSaveRawString(io->rdb, (unsigned char *)value, len));
}
static int loadOpcode(bloomIO *io, uint64_t expected) {
    if (io->error) return 0;
    if (rdbLoadLen(io->rdb, NULL) != expected || rioGetReadError(io->rdb)) {
        io->error = 1;
        return 0;
    }
    return 1;
}
static uint64_t loadUnsigned(bloomIO *io) {
    if (!loadOpcode(io, RDB_MODULE_OPCODE_UINT)) return 0;
    uint64_t value = rdbLoadLen(io->rdb, NULL);
    if (rioGetReadError(io->rdb)) io->error = 1;
    return value;
}
static double loadDouble(bloomIO *io) {
    double value = 0;
    if (!loadOpcode(io, RDB_MODULE_OPCODE_DOUBLE)) return 0;
    if (rdbLoadBinaryDoubleValue(io->rdb, &value) == -1) io->error = 1;
    return value;
}
static void *loadString(bloomIO *io, size_t *len) {
    if (!loadOpcode(io, RDB_MODULE_OPCODE_STRING)) return NULL;
    void *value = rdbGenericLoadStringObject(io->rdb, RDB_LOAD_PLAIN, len);
    if (!value) io->error = 1;
    return value;
}

static void saveChain(bloomIO *io, void *value) {
    const SBChain *chain = value;
    saveUnsigned(io, chain->size);
    saveUnsigned(io, chain->nfilters);
    saveUnsigned(io, chain->options);
    saveUnsigned(io, chain->growth);
    for (size_t i = 0; i < chain->nfilters; i++) {
        const SBLink *link = &chain->filters[i];
        const struct bloom *b = &link->inner;
        saveUnsigned(io, b->entries);
        saveDouble(io, b->error);
        saveUnsigned(io, b->hashes);
        saveDouble(io, b->bpe);
        saveUnsigned(io, b->bits);
        saveUnsigned(io, b->n2);
        saveString(io, (const char *)b->bf, b->bytes);
        saveUnsigned(io, link->size);
    }
}

/* Retain the module type ID and encoding versions 0..4. Explicit cleanup
 * replaces the upstream Blocks-based error-defer macros. */
static void *loadChain(bloomIO *io, int encver) {
    if (encver < 0 || encver > 4) return NULL;
    SBChain *chain = zcalloc_num(1, sizeof(*chain));
    uint64_t size = loadUnsigned(io);
    uint64_t count = loadUnsigned(io);
    uint64_t options = encver >= 2 ? loadUnsigned(io) : 0;
    uint64_t growth = encver >= 4 ? loadUnsigned(io) : 2;
    if (io->error || size > SIZE_MAX || count == 0 || count > INT_MAX ||
        count > SIZE_MAX / sizeof(SBLink) || options > UINT_MAX || growth > UINT_MAX)
        goto error;
    chain->size = size;
    chain->options = options;
    chain->growth = growth;
    unsigned valid = BLOOM_OPT_NOROUND | BLOOM_OPT_ENTS_IS_BITS |
                     BLOOM_OPT_FORCE64 | BLOOM_OPT_NO_SCALING;
    if ((options & ~valid) || (!(options & BLOOM_OPT_NO_SCALING) && !growth)) goto error;
    chain->filters = ztrycalloc(count * sizeof(SBLink));
    if (!chain->filters) goto error;
    chain->nfilters = count;
    for (size_t i = 0; i < count; i++) {
        SBLink *link = &chain->filters[i];
        struct bloom *b = &link->inner;
        b->entries = loadUnsigned(io);
        b->error = loadDouble(io);
        uint64_t hashes = loadUnsigned(io);
        b->bpe = loadDouble(io);
        if (io->error || hashes > UINT32_MAX ||
            !isfinite(b->error) || !isfinite(b->bpe)) goto error;
        b->hashes = hashes;
        if (encver == 0) {
            double bits = (double)b->entries * b->bpe;
            if (!isfinite(bits) || bits <= 0 || bits >= (double)UINT64_MAX) goto error;
            b->bits = bits;
        } else {
            b->bits = loadUnsigned(io);
            uint64_t n2 = loadUnsigned(io);
            if (n2 > 63) goto error;
            b->n2 = n2;
        }
        size_t bytes = 0;
        b->bf = (unsigned char *)loadString(io, &bytes);
        b->bytes = bytes;
        b->force64 = !!(options & BLOOM_OPT_FORCE64);
        uint64_t items = loadUnsigned(io);
        if (io->error || !b->bf || items > SIZE_MAX || items > b->entries ||
            bloom_validate_integrity(b)) goto error;
        link->size = items;
    }
    if (SB_ValidateIntegrity(chain)) goto error;
    return chain;
error:
    SBChain_Free(chain);
    return NULL;
}


ssize_t bloomRdbSave(rio *rdb, robj *o) {
    bloomIO io = {.rdb = rdb};
    addSaved(&io, rdbSaveLen(rdb, bloomRdbId()));
    saveChain(&io, o->ptr);
    addSaved(&io, rdbSaveLen(rdb, RDB_MODULE_OPCODE_EOF));
    return io.error ? -1 : io.bytes;
}

robj *bloomRdbLoad(rio *rdb, int version) {
    bloomIO io = {.rdb = rdb};
    SBChain *chain = loadChain(&io, version);
    if (!chain) return NULL;
    if (!loadOpcode(&io, RDB_MODULE_OPCODE_EOF)) {
        SBChain_Free(chain);
        return NULL;
    }
    return createBloomObject(chain);
}

static int writeChunk(rio *rdb, robj *key, long long iter, const char *data, size_t len) {
    return rioWriteBulkCount(rdb, '*', 4) &&
           rioWriteBulkString(rdb, "BF.LOADCHUNK", 12) &&
           rioWriteBulkObject(rdb, key) &&
           rioWriteBulkLongLong(rdb, iter) &&
           rioWriteBulkString(rdb, data, len);
}

int bloomRewriteAof(rio *rdb, robj *key, robj *o) {
    size_t len;
    char *header = SBChain_GetEncodedHeader(o->ptr, &len);
    int ok = writeChunk(rdb, key, 1, header, len);
    SB_FreeEncodedHeader(header);
    if (!ok) return 0;
    long long iter = SB_CHUNKITER_INIT;
    const char *chunk;
    while ((chunk = SBChain_GetEncodedChunk(o->ptr, &iter, &len, 16 * 1024 * 1024)))
        if (!writeChunk(rdb, key, iter, chunk, len)) return 0;
    return 1;
}
