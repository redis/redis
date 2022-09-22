#include "redismodule.h"

RedisModuleUser *user = NULL;

int call_without_user(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    const char *cmd = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModuleCallReply *rep = RedisModule_Call(ctx, cmd, "Ev", argv + 2, argc - 2);
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }
    return REDISMODULE_OK;
}

int call_with_user_flag(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModule_SetContextUser(ctx, user);

    /* Append Ev to the provided flags. */
    RedisModuleString *flags = RedisModule_CreateStringFromString(ctx, argv[1]);
    RedisModule_StringAppendBuffer(ctx, flags, "Ev", 2);

    const char* flg = RedisModule_StringPtrLen(flags, NULL);
    const char* cmd = RedisModule_StringPtrLen(argv[2], NULL);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, flg, argv + 3, argc - 3);
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }
    RedisModule_FreeString(ctx, flags);

    return REDISMODULE_OK;
}

int add_to_acl(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    size_t acl_len;
    const char *acl = RedisModule_StringPtrLen(argv[1], &acl_len);

    RedisModuleString *error;
    int ret = RedisModule_SetModuleUserACLString(ctx, user, acl, &error);
    if (ret) {
        size_t len;
        const char * e = RedisModule_StringPtrLen(error, &len);
        RedisModule_ReplyWithError(ctx, e);
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");

    return REDISMODULE_OK;
}

int get_acl(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);

    if (argc != 1) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModule_Assert(user != NULL);

    RedisModuleString *acl = RedisModule_GetModuleUserACLString(user);

    RedisModule_ReplyWithString(ctx, acl);

    RedisModule_FreeString(NULL, acl);

    return REDISMODULE_OK;
}

int reset_user(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);

    if (argc != 1) {
        return RedisModule_WrongArity(ctx);
    }

    if (user != NULL) {
        RedisModule_FreeModuleUser(user);
    }

    user = RedisModule_CreateModuleUser("module_user");

    RedisModule_ReplyWithSimpleString(ctx, "OK");

    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"usercall",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"usercall.call_without_user", call_without_user,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"usercall.call_with_user_flag", call_with_user_flag,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "usercall.add_to_acl", add_to_acl, "write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"usercall.reset_user", reset_user,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"usercall.get_acl", get_acl,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
