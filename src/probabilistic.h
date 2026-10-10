/* Shared native probabilistic-type persistence helpers.
 * Copyright (c) 2026-Present, Redis Ltd.; RSALv2/SSPLv1/AGPLv3.
 * The module wire encoding is retained for upgrades and rollbacks. */
#ifndef REDIS_PROBABILISTIC_H
#define REDIS_PROBABILISTIC_H
#include "server.h"
typedef struct { rio *rdb; ssize_t bytes; int error; } probIO;


static inline void addSaved(probIO *io, ssize_t count) {
    if (count < 0) io->error = 1;
    else io->bytes += count;
}
static inline void saveUnsigned(probIO *io, uint64_t value) {
    if (io->error) return;
    addSaved(io, rdbSaveLen(io->rdb, RDB_MODULE_OPCODE_UINT));
    addSaved(io, rdbSaveLen(io->rdb, value));
}
static inline void saveDouble(probIO *io, double value) {
    if (io->error) return;
    addSaved(io, rdbSaveLen(io->rdb, RDB_MODULE_OPCODE_DOUBLE));
    addSaved(io, rdbSaveBinaryDoubleValue(io->rdb, value));
}
static inline void saveString(probIO *io, const char *value, size_t len) {
    if (io->error) return;
    addSaved(io, rdbSaveLen(io->rdb, RDB_MODULE_OPCODE_STRING));
    addSaved(io, rdbSaveRawString(io->rdb, (unsigned char *)value, len));
}
static inline int loadOpcode(probIO *io, uint64_t expected) {
    if (io->error) return 0;
    if (rdbLoadLen(io->rdb, NULL) != expected || rioGetReadError(io->rdb)) {
        io->error = 1;
        return 0;
    }
    return 1;
}
static inline uint64_t loadUnsigned(probIO *io) {
    if (!loadOpcode(io, RDB_MODULE_OPCODE_UINT)) return 0;
    uint64_t value = rdbLoadLen(io->rdb, NULL);
    if (rioGetReadError(io->rdb)) io->error = 1;
    return value;
}
static inline double loadDouble(probIO *io) {
    double value = 0;
    if (!loadOpcode(io, RDB_MODULE_OPCODE_DOUBLE)) return 0;
    if (rdbLoadBinaryDoubleValue(io->rdb, &value) == -1) io->error = 1;
    return value;
}
static inline void *loadString(probIO *io, size_t *len) {
    if (!loadOpcode(io, RDB_MODULE_OPCODE_STRING)) return NULL;
    void *value = rdbGenericLoadStringObject(io->rdb, RDB_LOAD_PLAIN, len);
    if (!value) io->error = 1;
    return value;
}

static inline uint64_t probRdbId(const char *name, unsigned version) {
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    uint64_t id = 0;
    for (int i = 0; i < 9; i++) id = (id << 6) | (strchr(alphabet, name[i]) - alphabet);
    return (id << 10) | version;
}

static inline int probRewriteAof(rio *rdb, robj *key, robj *o, int dbid) {
    rio payload;
    createDumpPayload(&payload, o, key, dbid, DUMP_PAYLOAD_SKIP_KEY_META, 0);
    int ok = rioWriteBulkCount(rdb, '*', 4) &&
        rioWriteBulkString(rdb, "RESTORE", 7) && rioWriteBulkObject(rdb, key) &&
        rioWriteBulkLongLong(rdb, 0) &&
        rioWriteBulkString(rdb, payload.io.buffer.ptr, sdslen(payload.io.buffer.ptr));
    sdsfree(payload.io.buffer.ptr);
    return ok;
}
#endif
