#include "../redismodule.h"

static long currentClients = 0;
static long totalClients = 0;
static long lastConnectionId = -1;
static long lastDisconnectionId = -1;

int Commands_Current(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 1) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModule_ReplyWithLongLong(ctx, currentClients);
    return REDISMODULE_OK;
}

int Commands_Total(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 1) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModule_ReplyWithLongLong(ctx, totalClients);
    return REDISMODULE_OK;
}

int Commands_LastConnected(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 1) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModule_ReplyWithLongLong(ctx, lastConnectionId);
    return REDISMODULE_OK;
}

int Commands_LastDisconnected(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 1) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModule_ReplyWithLongLong(ctx, lastDisconnectionId);
    return REDISMODULE_OK;
}

void OnConnection(RedisModuleCtx *ctx) {
    currentClients++;
    totalClients++;
    lastConnectionId = RedisModule_GetClientId(ctx);
}

void OnDisconnection(RedisModuleCtx *ctx) {
    currentClients--;
    lastDisconnectionId = RedisModule_GetClientId(ctx);
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "hooks", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    RedisModule_HookToConnection(ctx, OnConnection);
    RedisModule_HookToDisconnection(ctx, OnDisconnection);

    if (RedisModule_CreateCommand(ctx, "hooks.current", Commands_Current, "readonly", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "hooks.total", Commands_Total, "readonly", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "hooks.lastConnected", Commands_LastConnected, "readonly", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "hooks.lastDisconnected", Commands_LastDisconnected, "readonly", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}
