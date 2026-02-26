#include "redismodule.h"
#include <errno.h>

int InternalAuth_GetInternalSecret(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    /* NOTE: The internal secret SHOULD NOT be exposed by any module. This is
    done for testing purposes only. */
    size_t len;
    const char *secret = RedisModule_GetInternalSecret(ctx, &len);
    if(secret) {
        RedisModule_ReplyWithStringBuffer(ctx, secret, len);
    } else {
        RedisModule_ReplyWithError(ctx, "ERR no internal secret available");
    }
    return REDISMODULE_OK;
}

int InternalAuth_InternalCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

typedef enum {
    RM_CALL_REGULAR = 0,
    RM_CALL_WITHUSER = 1,
    RM_CALL_WITHDETACHEDCLIENT = 2,
    RM_CALL_REPLICATED = 3
} RMCallMode;

int call_rm_call(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, RMCallMode mode) {
    if(argc < 2){
        return RedisModule_WrongArity(ctx);
    }
    RedisModuleCallReply *rep = NULL;
    RedisModuleCtx *detached_ctx = NULL;
    const char* cmd = RedisModule_StringPtrLen(argv[1], NULL);

    switch (mode) {
        case RM_CALL_REGULAR:
            // Regular call, with the unrestricted user.
            rep = RedisModule_Call(ctx, cmd, "vE", argv + 2, (size_t)argc - 2);
            break;
        case RM_CALL_WITHUSER:
            // Simply call the command with the current client.
            rep = RedisModule_Call(ctx, cmd, "vCE", argv + 2, (size_t)argc - 2);
            break;
        case RM_CALL_WITHDETACHEDCLIENT:
            // Use a context created with the thread-safe-context API
            detached_ctx = RedisModule_GetThreadSafeContext(NULL);
            if(!detached_ctx){
                RedisModule_ReplyWithError(ctx, "ERR failed to create detached context");
                return REDISMODULE_ERR;
            }
            // Dispatch the command with the detached context
            rep = RedisModule_Call(detached_ctx, cmd, "vCE", argv + 2, (size_t)argc - 2);
            break;
        case RM_CALL_REPLICATED:
            rep = RedisModule_Call(ctx, cmd, "vE", argv + 2, (size_t)argc - 2);
    }

    if(!rep) {
        char err[100];
        switch (errno) {
            case EACCES:
                RedisModule_ReplyWithError(ctx, "ERR NOPERM");
                break;
            case ENOENT:
                RedisModule_ReplyWithError(ctx, "ERR unknown command");
                break;
            default:
                snprintf(err, sizeof(err) - 1, "ERR errno=%d", errno);
                RedisModule_ReplyWithError(ctx, err);
                break;
        }
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
        if (mode == RM_CALL_REPLICATED)
            RedisModule_ReplicateVerbatim(ctx);
    }

    if (mode == RM_CALL_WITHDETACHEDCLIENT) {
        RedisModule_FreeThreadSafeContext(detached_ctx);
    }

    return REDISMODULE_OK;
}

int internal_rmcall(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return call_rm_call(ctx, argv, argc, RM_CALL_REGULAR);
}

int noninternal_rmcall(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return call_rm_call(ctx, argv, argc, RM_CALL_REGULAR);
}

int noninternal_rmcall_withuser(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return call_rm_call(ctx, argv, argc, RM_CALL_WITHUSER);
}

int noninternal_rmcall_detachedcontext_withuser(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return call_rm_call(ctx, argv, argc, RM_CALL_WITHDETACHEDCLIENT);
}

int internal_rmcall_replicated(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return call_rm_call(ctx, argv, argc, RM_CALL_REPLICATED);
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"testinternalsecret",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    /* WARNING: A module should NEVER expose the internal secret - this is for
     * testing purposes only. */
    if (RedisModule_CreateCommand(ctx,"internalauth.getinternalsecret",
        InternalAuth_GetInternalSecret,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"internalauth.internalcommand",
        InternalAuth_InternalCommand,"internal",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"internalauth.internal_rmcall",
        internal_rmcall,"write internal",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"internalauth.noninternal_rmcall",
        noninternal_rmcall,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"internalauth.noninternal_rmcall_withuser",
        noninternal_rmcall_withuser,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"internalauth.noninternal_rmcall_detachedcontext_withuser",
        noninternal_rmcall_detachedcontext_withuser,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"internalauth.internal_rmcall_replicated",
        internal_rmcall_replicated,"write internal",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
