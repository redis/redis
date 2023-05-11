
#include "redismodule.h"

RedisModuleClient *client = NULL;

int mc_create(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (client == NULL) {
        client = RedisModule_CreateModuleClient(ctx);
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int mc_delete(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (client != NULL) {
        RedisModule_FreeModuleClient(ctx, client);
        client = NULL;
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int mc_exec(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (client == NULL) {
        RedisModule_ReplyWithError(ctx, "Client not already allocated");
        return REDISMODULE_OK;
    }

    if (argc <= 1) {
        RedisModule_ReplyWithError(ctx, "not enough arguments");
        return REDISMODULE_OK;
    }

    size_t cmdstr_len;
    const char *cmdstr = RedisModule_StringPtrLen(argv[1], &cmdstr_len);

    RedisModule_SetContextClient(ctx, client);

    RedisModuleCallReply *reply = RedisModule_Call(ctx, cmdstr, "v", argv+2, argc-2);

    RedisModule_SetContextClient(ctx, NULL);

    RedisModule_ReplyWithCallReply(ctx, reply);
    RedisModule_FreeCallReply(reply);

    return REDISMODULE_OK;
}

int mc_getflags(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (client == NULL) {
        RedisModule_ReplyWithError(ctx, "Client not already allocated");
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithLongLong(ctx, RedisModule_GetClientFlags(client));
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"moduleclient",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"mc.create", mc_create,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"mc.delete", mc_delete,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"mc.exec", mc_exec,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"mc.getflags", mc_getflags,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}