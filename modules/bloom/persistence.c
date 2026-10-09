/* RedisBloom-compatible persistence. Included by bf.c.
 * Copyright (c) 2006-Present, Redis Ltd.
 * Licensed under RSALv2, SSPLv1, or AGPLv3. */

static void bloomRdbSave(RedisModuleIO *io, void *value) {
    const SBChain *chain = value;
    RedisModule_SaveUnsigned(io, chain->size);
    RedisModule_SaveUnsigned(io, chain->nfilters);
    RedisModule_SaveUnsigned(io, chain->options);
    RedisModule_SaveUnsigned(io, chain->growth);
    for (size_t i = 0; i < chain->nfilters; i++) {
        const SBLink *link = &chain->filters[i];
        const struct bloom *b = &link->inner;
        RedisModule_SaveUnsigned(io, b->entries);
        RedisModule_SaveDouble(io, b->error);
        RedisModule_SaveUnsigned(io, b->hashes);
        RedisModule_SaveDouble(io, b->bpe);
        RedisModule_SaveUnsigned(io, b->bits);
        RedisModule_SaveUnsigned(io, b->n2);
        RedisModule_SaveStringBuffer(io, (const char *)b->bf, b->bytes);
        RedisModule_SaveUnsigned(io, link->size);
    }
}

/* Retain the module type ID and encoding versions 0..4. Explicit cleanup
 * replaces the upstream Blocks-based error-defer macros. */
static void *bloomRdbLoad(RedisModuleIO *io, int encver) {
    if (encver < 0 || encver > 4) return NULL;
    SBChain *chain = RedisModule_Calloc(1, sizeof(*chain));
    uint64_t size = RedisModule_LoadUnsigned(io);
    uint64_t count = RedisModule_LoadUnsigned(io);
    uint64_t options = encver >= 2 ? RedisModule_LoadUnsigned(io) : 0;
    uint64_t growth = encver >= 4 ? RedisModule_LoadUnsigned(io) : 2;
    if (RedisModule_IsIOError(io) || size > SIZE_MAX || count == 0 || count > INT_MAX ||
        count > SIZE_MAX / sizeof(SBLink) || options > UINT_MAX || growth > UINT_MAX)
        goto error;
    chain->size = size;
    chain->options = options;
    chain->growth = growth;
    unsigned valid = BLOOM_OPT_NOROUND | BLOOM_OPT_ENTS_IS_BITS |
                     BLOOM_OPT_FORCE64 | BLOOM_OPT_NO_SCALING;
    if ((options & ~valid) || (!(options & BLOOM_OPT_NO_SCALING) && !growth)) goto error;
    chain->filters = RedisModule_TryCalloc(count, sizeof(SBLink));
    if (!chain->filters) goto error;
    chain->nfilters = count;
    for (size_t i = 0; i < count; i++) {
        SBLink *link = &chain->filters[i];
        struct bloom *b = &link->inner;
        b->entries = RedisModule_LoadUnsigned(io);
        b->error = RedisModule_LoadDouble(io);
        uint64_t hashes = RedisModule_LoadUnsigned(io);
        b->bpe = RedisModule_LoadDouble(io);
        if (RedisModule_IsIOError(io) || hashes > UINT32_MAX ||
            !isfinite(b->error) || !isfinite(b->bpe)) goto error;
        b->hashes = hashes;
        if (encver == 0) {
            double bits = (double)b->entries * b->bpe;
            if (!isfinite(bits) || bits <= 0 || bits >= (double)UINT64_MAX) goto error;
            b->bits = bits;
        } else {
            b->bits = RedisModule_LoadUnsigned(io);
            uint64_t n2 = RedisModule_LoadUnsigned(io);
            if (n2 > 63) goto error;
            b->n2 = n2;
        }
        size_t bytes = 0;
        b->bf = (unsigned char *)RedisModule_LoadStringBuffer(io, &bytes);
        b->bytes = bytes;
        b->force64 = !!(options & BLOOM_OPT_FORCE64);
        uint64_t items = RedisModule_LoadUnsigned(io);
        if (RedisModule_IsIOError(io) || !b->bf || items > SIZE_MAX || items > b->entries ||
            bloom_validate_integrity(b)) goto error;
        link->size = items;
    }
    if (SB_ValidateIntegrity(chain)) goto error;
    return chain;
error:
    SBChain_Free(chain);
    return NULL;
}

static int scanDumpCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 3) return RedisModule_WrongArity(ctx);
    long long iter;
    if (RedisModule_StringToLongLong(argv[2], &iter) != REDISMODULE_OK || iter < 0)
        return RedisModule_ReplyWithError(ctx, "ERR invalid iterator");
    SBChain *chain;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int status = getBloom(key, &chain);
    if (status <= 0) return RedisModule_ReplyWithError(ctx,
        status < 0 ? REDISMODULE_ERRORMSG_WRONGTYPE : "ERR not found");
    size_t len;
    RedisModule_ReplyWithArray(ctx, 2);
    if (iter == 0) {
        char *header = SBChain_GetEncodedHeader(chain, &len);
        RedisModule_ReplyWithLongLong(ctx, SB_CHUNKITER_INIT);
        RedisModule_ReplyWithStringBuffer(ctx, header, len);
        SB_FreeEncodedHeader(header);
    } else {
        const char *chunk = SBChain_GetEncodedChunk(chain, &iter, &len, 16 * 1024 * 1024);
        RedisModule_ReplyWithLongLong(ctx, iter);
        if (chunk) RedisModule_ReplyWithStringBuffer(ctx, chunk, len);
        else RedisModule_ReplyWithStringBuffer(ctx, "", 0);
    }
    return REDISMODULE_OK;
}

static int loadChunkCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 4) return RedisModule_WrongArity(ctx);
    long long iter;
    if (RedisModule_StringToLongLong(argv[2], &iter) != REDISMODULE_OK || iter <= 0)
        return RedisModule_ReplyWithError(ctx, "ERR invalid iterator");
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    SBChain *chain;
    int status = getBloom(key, &chain);
    if (status < 0) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    size_t len;
    const char *data = RedisModule_StringPtrLen(argv[3], &len);
    const char *error = NULL;
    if (iter == 1) {
        if (status) return RedisModule_ReplyWithError(ctx, "ERR item exists");
        chain = SB_NewChainFromHeader(data, len, &error);
        if (!chain) return RedisModule_ReplyWithError(ctx, error);
        RedisModule_ModuleTypeSetValue(key, BloomType, chain);
    } else {
        if (!status) return RedisModule_ReplyWithError(ctx, "ERR not found");
        if (SBChain_LoadEncodedChunk(chain, iter, data, len, &error))
            return RedisModule_ReplyWithError(ctx, error);
    }
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static void bloomAofRewrite(RedisModuleIO *io, RedisModuleString *key, void *value) {
    SBChain *chain = value;
    size_t len;
    char *header = SBChain_GetEncodedHeader(chain, &len);
    RedisModule_EmitAOF(io, "BF.LOADCHUNK", "slb", key, 1, header, len);
    SB_FreeEncodedHeader(header);
    long long iter = SB_CHUNKITER_INIT;
    const char *chunk;
    while ((chunk = SBChain_GetEncodedChunk(chain, &iter, &len, 16 * 1024 * 1024)))
        RedisModule_EmitAOF(io, "BF.LOADCHUNK", "slb", key, iter, chunk, len);
}
