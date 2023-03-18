#include "redismodule.h"
#include <pthread.h>
#include <assert.h>

#define UNUSED(V) ((void) V)

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

typedef struct {
    RedisModuleString **argv;
    int argc;
    RedisModuleBlockedClient *bc;
} bg_call_data;

void *bg_call_worker(void *arg) {
    bg_call_data *bg = arg;

    // Get Redis module context
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(bg->bc);

    // Acquire GIL
    RedisModule_ThreadSafeContextLock(ctx);

    // Set user
    RedisModule_SetContextUser(ctx, user);

    // Call the command
    size_t format_len;
    RedisModuleString *format_redis_str = RedisModule_CreateString(NULL, "v", 1);
    const char *format = RedisModule_StringPtrLen(bg->argv[1], &format_len);
    RedisModule_StringAppendBuffer(NULL, format_redis_str, format, format_len);
    RedisModule_StringAppendBuffer(NULL, format_redis_str, "E", 1);
    format = RedisModule_StringPtrLen(format_redis_str, NULL);
    const char *cmd = RedisModule_StringPtrLen(bg->argv[2], NULL);
    RedisModuleCallReply *rep = RedisModule_Call(ctx, cmd, format, bg->argv + 3, bg->argc - 3);
    RedisModule_FreeString(NULL, format_redis_str);

    // Release GIL
    RedisModule_ThreadSafeContextUnlock(ctx);

    // Reply to client
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    // Unblock client
    RedisModule_UnblockClient(bg->bc, NULL);

    /* Free the arguments */
    for (int i=0; i<bg->argc; i++)
        RedisModule_FreeString(ctx, bg->argv[i]);
    RedisModule_Free(bg->argv);
    RedisModule_Free(bg);

    // Free the Redis module context
    RedisModule_FreeThreadSafeContext(ctx);

    return NULL;
}

int call_with_user_bg(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);

    /* Make sure we're not trying to block a client when we shouldn't */
    int flags = RedisModule_GetContextFlags(ctx);
    int allFlags = RedisModule_GetContextFlagsAll();
    if ((allFlags & REDISMODULE_CTX_FLAGS_MULTI) &&
        (flags & REDISMODULE_CTX_FLAGS_MULTI)) {
        RedisModule_ReplyWithSimpleString(ctx, "Blocked client is not supported inside multi");
        return REDISMODULE_OK;
    }
    if ((allFlags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING) &&
        (flags & REDISMODULE_CTX_FLAGS_DENY_BLOCKING)) {
        RedisModule_ReplyWithSimpleString(ctx, "Blocked client is not allowed");
        return REDISMODULE_OK;
    }

    /* Make a copy of the arguments and pass them to the thread. */
    bg_call_data *bg = RedisModule_Alloc(sizeof(bg_call_data));
    bg->argv = RedisModule_Alloc(sizeof(RedisModuleString*)*argc);
    bg->argc = argc;
    for (int i=0; i<argc; i++)
        bg->argv[i] = RedisModule_HoldString(ctx, argv[i]);

    /* Block the client */
    bg->bc = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);

    /* Start a thread to handle the request */
    pthread_t tid;
    int res = pthread_create(&tid, NULL, bg_call_worker, bg);
    assert(res == 0);

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

    if (RedisModule_CreateCommand(ctx, "usercall.call_with_user_bg", call_with_user_bg, "write", 0, 0, 0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx, "usercall.add_to_acl", add_to_acl, "write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"usercall.reset_user", reset_user,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"usercall.get_acl", get_acl,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
