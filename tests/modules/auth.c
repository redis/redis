#define REDISMODULE_EXPERIMENTAL_API
#include "redismodule.h"

// A simple global user
static RedisModuleUser *global = NULL;
static long long client_change_delta = 0;

void UserChangedCallback(uint64_t client_id, void *privdata) {
    REDISMODULE_NOT_USED(privdata);
    REDISMODULE_NOT_USED(client_id);
    client_change_delta++;
}

int Auth_CreateModuleUser(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (global) {
        RedisModule_FreeModuleUser(global);
    }

    global = RedisModule_CreateModuleUser("global");
    RedisModule_SetModuleUserACL(global, "allcommands");
    RedisModule_SetModuleUserACL(global, "allkeys");
    RedisModule_SetModuleUserACL(global, "on");

    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int Auth_AuthModuleUser(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    uint64_t client_id;
    RedisModule_AuthenticateClientWithUser(ctx, global, UserChangedCallback, NULL, &client_id);

    return RedisModule_ReplyWithLongLong(ctx, (uint64_t) client_id);
}

int Auth_AuthRealUser(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);

    size_t length;
    uint64_t client_id;

    RedisModuleString *user_string = argv[1];
    const char *name = RedisModule_StringPtrLen(user_string, &length);

    if (RedisModule_AuthenticateClientWithACLUser(ctx, name, length, 
            UserChangedCallback, NULL, &client_id) == REDISMODULE_ERR) {
        return RedisModule_ReplyWithError(ctx, "Invalid user");   
    }

    return RedisModule_ReplyWithLongLong(ctx, (uint64_t) client_id);
}

int Auth_ChangeCount(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    long long result = client_change_delta;
    client_change_delta = 0;
    return RedisModule_ReplyWithLongLong(ctx, result);
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"testacl",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"auth.authrealuser",
        Auth_AuthRealUser,"no-auth",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"auth.createmoduleuser",
        Auth_CreateModuleUser,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"auth.authmoduleuser",
        Auth_AuthModuleUser,"no-auth",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"auth.changecount",
        Auth_ChangeCount,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
