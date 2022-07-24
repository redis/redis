#include "../../src/redismodule.h"
#include <stdio.h>
#include <strings.h>

RedisModuleUser *user = NULL;

int call_without_acl(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t cmd_len;
    const char * cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);

    printf("cmd = %.*s\n", (int) cmd_len, cmd);

    RedisModuleCallReply *reply = RedisModule_Call(ctx, cmd, "Ev", argv + 2, argc - 2);
    if (reply == NULL) {
        RedisModule_ReplyWithError(ctx, "Failed to Execute");
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithCallReply(ctx, reply);
    RedisModule_FreeCallReply(reply);
    return REDISMODULE_OK;
}

int call_with_acl(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t cmd_len;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);
    RedisModuleCallReply *reply = RedisModule_CallWithUser(ctx, user, cmd, "Ev", argv + 2, argc - 2);
    if (reply == NULL) {
        RedisModule_ReplyWithError(ctx, "Failed to Execute");
        return REDISMODULE_OK;
    }

    int type = RedisModule_CallReplyType(reply);

    size_t str_len;
    const char * str = RedisModule_CallReplyStringPtr(reply, &str_len);

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

    RedisModuleString * error = RedisModule_SetModuleUserACLString(ctx, user, acl);
    if (error) {
        size_t len;
        const char * e = RedisModule_StringPtrLen(error, &len);
        printf("error = %p, e = %.*s\n", error, (int) len, e);
        RedisModule_ReplyWithError(ctx, e);
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithSimpleString(ctx, "OK");

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

    if (RedisModule_CreateCommand(ctx,"usercall.call_without_acl", call_without_acl,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"usercall.call_with_acl", call_with_acl,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "usercall.add_to_acl", add_to_acl, "write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"usercall.reset_user", reset_user,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}