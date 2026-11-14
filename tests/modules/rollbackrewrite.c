#include "redismodule.h"

static int enabled;
static int rewrite_pending;

static int getBoolConfig(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(privdata);
    return enabled;
}

static int setBoolConfig(const char *name, int value, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(privdata);
    REDISMODULE_NOT_USED(err);
    enabled = value;
    /* Keep the request pending when rollback restores the old value. */
    if (value) rewrite_pending = 1;
    return REDISMODULE_OK;
}

static int applyConfig(RedisModuleCtx *ctx, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(privdata);
    if (!rewrite_pending) return REDISMODULE_OK;
    rewrite_pending = 0;

    /* Apply callbacks have no client; use a detached context for commands. */
    RedisModuleCtx *callctx = RedisModule_GetDetachedThreadSafeContext(ctx);
    RedisModuleCallReply *reply = RedisModule_Call(callctx, "CONFIG", "c", "REWRITE");
    int success = reply && RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_STRING;
    if (reply) RedisModule_FreeCallReply(reply);
    RedisModule_FreeThreadSafeContext(callctx);
    if (!success) {
        *err = RedisModule_CreateString(ctx, "CONFIG REWRITE failed", 21);
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx, "rollbackrewrite", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_RegisterBoolConfig(ctx, "enabled", 0, REDISMODULE_CONFIG_DEFAULT,
                                      getBoolConfig, setBoolConfig, applyConfig, NULL) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    return RedisModule_LoadConfigs(ctx);
}
