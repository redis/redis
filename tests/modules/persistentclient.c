
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

static void rm_call_async_send_reply(RedisModuleCtx *ctx, RedisModuleCallReply *reply) {
    RedisModule_ReplyWithCallReply(ctx, reply);
    RedisModule_FreeCallReply(reply);
}

static void rm_call_async_on_unblocked(RedisModuleCtx *ctx, RedisModuleCallReply *reply, void *private_data) {
    REDISMODULE_NOT_USED(ctx);

    RedisModuleBlockedClient *bc = private_data;
    RedisModuleCtx *bctx = RedisModule_GetThreadSafeContext(bc);
    rm_call_async_send_reply(bctx, reply);
    RedisModule_FreeThreadSafeContext(bctx);
    RedisModule_UnblockClient(bc, RedisModule_BlockClientGetPrivateData(bc));
}

static void do_rm_call_async_free_pd(RedisModuleCtx * ctx, void *pd) {
    REDISMODULE_NOT_USED(ctx);

    RedisModule_FreeCallReply(pd);
}

int mc_async(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    const char *cmd = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModule_SetContextClient(ctx, client);
    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, "KEv", argv + 2, argc - 2);
    RedisModule_SetContextClient(ctx, NULL);

    if(RedisModule_CallReplyType(rep) != REDISMODULE_REPLY_PROMISE) {
        rm_call_async_send_reply(ctx, rep);
    } else {
        RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx, NULL, NULL, do_rm_call_async_free_pd, 0);
        RedisModule_BlockClientSetPrivateData(bc, rep);
        RedisModule_CallReplyPromiseSetUnblockHandler(rep, rm_call_async_on_unblocked, bc);
    }

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

    if (RedisModule_CreateCommand(ctx, "mc.exec_async", mc_async, "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}