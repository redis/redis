#include "redismodule.h"

RedisModuleUser *user = NULL;

int call_without_user(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t cmd_len;
    const char * cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);

    RedisModuleCallReply *reply = RedisModule_Call(ctx, cmd, "Ev", argv + 2, argc - 2);
    if (reply == NULL) {
        RedisModule_ReplyWithError(ctx, "Failed to Execute");
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithCallReply(ctx, reply);
    RedisModule_FreeCallReply(reply);
    return REDISMODULE_OK;
}

int call_with_user(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t cmd_len;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);
    RedisModule_SetContextModuleUser(ctx, user);
    RedisModuleCallReply *reply = RedisModule_Call(ctx, cmd, "Ev", argv + 2, argc - 2);
    if (reply == NULL) {
        RedisModule_ReplyWithError(ctx, "Failed to Execute");
        return REDISMODULE_OK;
    }

    int type = RedisModule_CallReplyType(reply);

    size_t str_len;
    const char *str = RedisModule_CallReplyStringPtr(reply, &str_len);

    if (type != REDISMODULE_REPLY_ERROR) {
        RedisModule_ReplyWithCallReply(ctx, reply);
    } else {
        RedisModule_ReplyWithError(ctx, str);
    }
    RedisModule_FreeCallReply(reply);

    return REDISMODULE_OK;
}

int call_with_user_and_acl(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t cmd_len;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);
    RedisModule_SetContextModuleUser(ctx, user);
    RedisModuleCallReply *reply = RedisModule_Call(ctx, cmd, "CEv", argv + 2, argc - 2);
    if (reply == NULL) {
        RedisModule_ReplyWithError(ctx, "Failed to Execute");
        return REDISMODULE_OK;
    }

    int type = RedisModule_CallReplyType(reply);

    size_t str_len;
    const char *str = RedisModule_CallReplyStringPtr(reply, &str_len);

    if (type != REDISMODULE_REPLY_ERROR) {
        RedisModule_ReplyWithCallReply(ctx, reply);
    } else {
        RedisModule_ReplyWithError(ctx, str);
    }
    RedisModule_FreeCallReply(reply);

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

    if (RedisModule_CreateCommand(ctx,"usercall.call_with_user", call_with_user,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"usercall.call_with_user_and_acl", call_with_user_and_acl,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "usercall.add_to_acl", add_to_acl, "write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"usercall.reset_user", reset_user,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"usercall.get_acl", get_acl,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
