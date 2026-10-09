/* Built-in Bloom filter data type. Copyright Redis Ltd.; RSALv2/SSPLv1/AGPLv3. */
#include "../../src/redismodule.h"
#include "sb.h"
#include <math.h>
#include <string.h>
#include <strings.h>
#include <limits.h>

#include "config.c"
static RedisModuleType *BloomType;

/* Match RedisBloom's option lookup, including case-insensitive tokens. */
static int argumentIndex(const char *name, RedisModuleString **argv, int argc) {
    size_t expected = strlen(name);
    for (int i = 0; i < argc; i++) {
        size_t len;
        const char *arg = RedisModule_StringPtrLen(argv[i], &len);
        if (len == expected && !strncasecmp(arg, name, len)) return i;
    }
    return -1;
}

static int replyBool(RedisModuleCtx *ctx, int value) {
    if (RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_RESP3)
        return RedisModule_ReplyWithBool(ctx, value != 0);
    return RedisModule_ReplyWithLongLong(ctx, value != 0);
}

static int getBloom(RedisModuleKey *key, SBChain **chain) {
    *chain = NULL;
    int type = RedisModule_KeyType(key);
    if (type == REDISMODULE_KEYTYPE_EMPTY) return 0;
    if (type != REDISMODULE_KEYTYPE_MODULE || RedisModule_ModuleTypeGetType(key) != BloomType)
        return -1;
    *chain = RedisModule_ModuleTypeGetValue(key);
    return 1;
}

static SBChain *createBloom(RedisModuleKey *key, uint64_t capacity, double error,
                            unsigned expansion, unsigned options) {
    int rc;
    SBChain *chain = SB_NewChain(capacity, error,
        options | BLOOM_OPT_FORCE64 | BLOOM_OPT_NOROUND, expansion, &rc);
    if (chain) RedisModule_ModuleTypeSetValue(key, BloomType, chain);
    return chain;
}

static int reserveCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc < 4 || argc > 7) return RedisModule_WrongArity(ctx);
    double error;
    long long capacity;
    if (RedisModule_StringToDouble(argv[2], &error) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR bad error rate");
    if (!isfinite(error) || error <= 0 || error >= 1)
        return RedisModule_ReplyWithError(ctx, "ERR error rate must be in the range (0.000000, 1.000000)");
    if (error > 0.25) error = 0.25;
    if (RedisModule_StringToLongLong(argv[3], &capacity) != REDISMODULE_OK)
        return RedisModule_ReplyWithError(ctx, "ERR bad capacity");
    if (capacity < 1 || capacity > (1LL << 30))
        return RedisModule_ReplyWithError(ctx, "ERR capacity must be in the range [1, 1073741824]");
    unsigned options = bloomExpansion == 0 ? BLOOM_OPT_NO_SCALING : 0;
    unsigned expansion = bloomExpansion;
    if (argumentIndex("NONSCALING", argv, argc) != -1) options = BLOOM_OPT_NO_SCALING;
    int index = argumentIndex("EXPANSION", argv, argc);
    if (index + 1 == argc) return RedisModule_ReplyWithError(ctx, "ERR no expansion");
    if (index != -1) {
        long long parsed;
        if (RedisModule_StringToLongLong(argv[index + 1], &parsed) != REDISMODULE_OK)
            return RedisModule_ReplyWithError(ctx, "ERR bad expansion");
        if (!parsed) options = BLOOM_OPT_NO_SCALING;
        else if (options) return RedisModule_ReplyWithError(ctx, "Nonscaling filters cannot expand");
        if (parsed < 0 || parsed > 32768)
            return RedisModule_ReplyWithError(ctx, "ERR expansion must be in the range [0, 32768]");
        expansion = parsed;
    }
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    SBChain *chain;
    int status = getBloom(key, &chain);
    if (status != 0) return RedisModule_ReplyWithError(ctx,
        status < 0 ? REDISMODULE_ERRORMSG_WRONGTYPE : "ERR item exists");
    if (!createBloom(key, capacity, error, expansion, options))
        return RedisModule_ReplyWithError(ctx, "ERR could not create filter");
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int addCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 3) return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    SBChain *chain;
    int status = getBloom(key, &chain);
    if (status < 0) return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
    if (status == 0 && !(chain = createBloom(key, bloomCapacity, bloomErrorRate, bloomExpansion,
                                           bloomExpansion == 0 ? BLOOM_OPT_NO_SCALING : 0)))
        return RedisModule_ReplyWithError(ctx, "ERR could not create filter");
    size_t len;
    const char *item = RedisModule_StringPtrLen(argv[2], &len);
    int result = SBChain_Add(chain, item, len);
    if (result == SB_FULL) return RedisModule_ReplyWithError(ctx, "ERR non scaling filter is full");
    if (result < 0) return RedisModule_ReplyWithError(ctx, "ERR problem inserting into filter");
    RedisModule_ReplicateVerbatim(ctx);
    return replyBool(ctx, result);
}

static int existsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 3) return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    SBChain *chain;
    int status = getBloom(key, &chain);
    /* RedisBloom returns false for both missing keys and other key types. */
    if (status <= 0) return replyBool(ctx, 0);
    size_t len;
    const char *item = RedisModule_StringPtrLen(argv[2], &len);
    return replyBool(ctx, SBChain_Check(chain, item, len));
}

#include "persistence.c"

static void bloomFree(void *value) { SBChain_Free(value); }

static size_t bloomMemUsage(const void *value) {
    const SBChain *chain = value;
    size_t bytes = sizeof(*chain) + chain->nfilters * sizeof(*chain->filters);
    for (size_t i = 0; i < chain->nfilters; i++) bytes += chain->filters[i].inner.bytes;
    return bytes;
}

static int registerCommand(RedisModuleCtx *ctx, const char *name, RedisModuleCmdFunc callback,
                           const char *flags, const char *categories) {
    if (RedisModule_CreateCommand(ctx, name, callback, flags, 1, 1, 1) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    RedisModuleCommand *command = RedisModule_GetCommand(ctx, name);
    if (!command || RedisModule_SetCommandACLCategories(command, categories) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    RedisModuleCommandInfo info = {.version = REDISMODULE_COMMAND_INFO_VERSION};
    if (callback == reserveCommand) {
        info.arity = -4;
        info.summary = "Create a Bloom filter with the specified error rate and capacity.";
        info.complexity = "O(1)";
    } else if (callback == addCommand) {
        info.arity = 3;
        info.summary = "Add an item to a Bloom filter, creating it if needed.";
        info.complexity = "O(k), where k is the number of hash functions across subfilters.";
    } else if (callback == existsCommand) {
        info.arity = 3;
        info.summary = "Test whether an item may be present in a Bloom filter.";
        info.complexity = "O(k), where k is the number of hash functions across subfilters.";
    } else {
        info.arity = callback == scanDumpCommand ? 3 : 4;
        info.summary = callback == scanDumpCommand ? "Serialize a Bloom filter incrementally."
                                                  : "Restore a serialized Bloom filter chunk.";
        info.complexity = "O(n), where n is the chunk size.";
    }
    return RedisModule_SetCommandInfo(command, &info);
}

int Bloom_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "bf", 1, REDISMODULE_APIVER_1) != REDISMODULE_OK)
        return REDISMODULE_ERR;
    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS);
    if (registerBloomConfigs(ctx) != REDISMODULE_OK) return REDISMODULE_ERR;
    if (RedisModule_AddACLCategory(ctx, "bloom") != REDISMODULE_OK) return REDISMODULE_ERR;
    if (registerCommand(ctx, "BF.RESERVE", reserveCommand, "write deny-oom", "write fast bloom") ||
        registerCommand(ctx, "BF.ADD", addCommand, "write deny-oom", "write bloom") ||
        registerCommand(ctx, "BF.EXISTS", existsCommand, "readonly fast", "read fast bloom") ||
        registerCommand(ctx, "BF.SCANDUMP", scanDumpCommand, "readonly", "read bloom") ||
        registerCommand(ctx, "BF.LOADCHUNK", loadChunkCommand, "write deny-oom", "write bloom"))
        return REDISMODULE_ERR;
    RedisModuleTypeMethods methods = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_save = bloomRdbSave,
        .rdb_load = bloomRdbLoad,
        .aof_rewrite = bloomAofRewrite,
        .mem_usage = bloomMemUsage,
        .free = bloomFree,
    };
    BloomType = RedisModule_CreateDataType(ctx, "MBbloom--", 4, &methods);
    return BloomType ? REDISMODULE_OK : REDISMODULE_ERR;
}
