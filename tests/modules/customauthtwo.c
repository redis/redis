#include "redismodule.h"

#include <string.h>

/* This is a second sample module to validate that custom authentication callbacks can be registered
 * from multiple modules. */

/* Non Blocking Custom Auth callback / implementation. */
int auth_cb(RedisModuleCtx *ctx, RedisModuleString *username, RedisModuleString *password, RedisModuleString **err) {
    const char* user = RedisModule_StringPtrLen(username, NULL);
    const char* pwd = RedisModule_StringPtrLen(password, NULL);
    if (!strcmp(user,"foo") && !strcmp(pwd,"allow_two")) {
        RedisModule_AuthenticateClientWithACLUser(ctx, "foo", 3, NULL, NULL, NULL);
        return REDISMODULE_AUTH_HANDLED;
    }
    else if (!strcmp(user,"foo") && !strcmp(pwd,"deny_two")) {
        RedisModuleUser *user = RedisModule_GetModuleUserFromUserName(username);
        if (user) {
            RedisModule_ACLAddLogEntry(ctx, user, NULL, REDISMODULE_ACL_LOG_AUTH);
            RedisModule_FreeModuleUser(user);
        }
        const char *err_msg = "Auth denied by Misc Module.";
        *err = RedisModule_CreateString(ctx, err_msg, strlen(err_msg));
        return REDISMODULE_AUTH_HANDLED;
    }
    return REDISMODULE_AUTH_NOT_HANDLED;
}

int test_rm_register_auth_cb(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_RegisterCustomAuthCallback(ctx, auth_cb);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx,"customauthtwo",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"testmoduletwo.rm_register_auth_cb", test_rm_register_auth_cb,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}