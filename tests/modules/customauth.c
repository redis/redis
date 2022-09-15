#include "redismodule.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>

/* This is a sample module to validate that custom authentication callbacks can be registered to
 * implement both non-blocking and blocking module based authentication. */

/* Non Blocking Custom Auth callback / implementation. */
int auth_cb(RedisModuleCtx *ctx, RedisModuleString *username, RedisModuleString *password, const char **err) {
    const char* user = RedisModule_StringPtrLen(username, NULL);
    const char* pwd = RedisModule_StringPtrLen(password, NULL);
    if (!strcmp(user,"foo") && !strcmp(pwd,"allow")) {
        RedisModule_AuthenticateClientWithACLUser(ctx, "foo", 3, NULL, NULL, NULL);
        return REDISMODULE_AUTH_SUCCEEDED;
    }
    else if (!strcmp(user,"foo") && !strcmp(pwd,"deny")) {
        RedisModuleUser *user = RedisModule_GetModuleUserFromUserName(username);
        if (user) {
            RedisModule_ACLAddLogEntry(ctx, user, NULL, REDISMODULE_ACL_LOG_AUTH);
            RedisModule_FreeModuleUser(user);
        }
        *err = "Auth denied by Misc Module.";
        return REDISMODULE_AUTH_DENIED;
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

/*
 * The thread entry point that actually executes the blocking part of the AUTH command.
 * This function sleeps for 0.5 seconds and then unblocks the client which will later call
 * `AuthBlock_Reply`.
 * `arg` is expected to contain the RedisModuleBlockedClient, username, and password.
 */
void *AuthBlock_ThreadMain(void *arg) {
    usleep(500000);
    void **targ = arg;
    RedisModuleBlockedClient *bc = targ[0];
    int result = 2;
    const char* user = RedisModule_StringPtrLen(targ[1], NULL);
    const char* pwd = RedisModule_StringPtrLen(targ[2], NULL);
    if (!strcmp(user,"foo") && !strcmp(pwd,"block_allow")) {
        result = 1;
    }
    else if (!strcmp(user,"foo") && !strcmp(pwd,"block_deny")) {
        result = 0;
    }
    else if (!strcmp(user,"foo") && !strcmp(pwd,"block_abort")) {
        RedisModule_BlockedClientMeasureTimeEnd(bc);
        RedisModule_AbortBlock(bc);
        goto cleanup;
    }
    /* Provide the result to the blocking reply cb. */
    void **replyarg = RedisModule_Alloc(sizeof(void*));
    replyarg[0] = (void *) (uintptr_t) result;
    RedisModule_BlockedClientMeasureTimeEnd(bc);
    RedisModule_UnblockClient(bc, replyarg);
cleanup:
    /* Free the username and password and thread / arg data. */
    RedisModule_FreeString(NULL, targ[1]);
    RedisModule_FreeString(NULL, targ[2]);
    RedisModule_Free(targ);
    return NULL;
}

/*
 * Reply callback for a blocking AUTH command. This is called when the client is unblocked.
 */
int AuthBlock_Reply(RedisModuleCtx *ctx, RedisModuleString *username, RedisModuleString *password, const char **err) {
    REDISMODULE_NOT_USED(password);
    void **targ = RedisModule_GetBlockedClientPrivateData(ctx);
    int result = (uintptr_t) targ[0];
    size_t userlen = 0;
    const char *user = RedisModule_StringPtrLen(username, &userlen);
    /* Handle the success case by authenticating. */
    if (result == 1) {
        RedisModule_AuthenticateClientWithACLUser(ctx, user, userlen, NULL, NULL, NULL);
        return REDISMODULE_AUTH_SUCCEEDED;
    }
    /* Handle the Error case by denying auth */
    else if (result == 0) {
        RedisModuleUser *user = RedisModule_GetModuleUserFromUserName(username);
        if (user) {
            RedisModule_ACLAddLogEntry(ctx, user, NULL, REDISMODULE_ACL_LOG_AUTH);
            RedisModule_FreeModuleUser(user);
        }
        *err = "Auth denied by Misc Module.";
        return REDISMODULE_AUTH_DENIED;
    }
    /* "Skip" Authentication */
    return REDISMODULE_AUTH_NOT_HANDLED;
}

/* Private data freeing callback for custom auths. */
void AuthBlock_FreeData(RedisModuleCtx *ctx, void *privdata) {
    REDISMODULE_NOT_USED(ctx);
    RedisModule_Free(privdata);
}

/* Callback triggered when the engine attempts custom auth
 * Return code here is one of the following: Auth succeeded, Auth denied,
 * Auth not handled, Auth blocked.
 * The Module can have auth succeed / denied here itself, but this is an example
 * of blocking custom auth.
 */
int blocking_auth_cb(RedisModuleCtx *ctx, RedisModuleString *username, RedisModuleString *password, const char **err) {
    REDISMODULE_NOT_USED(username);
    REDISMODULE_NOT_USED(password);
    REDISMODULE_NOT_USED(err);
    /* Block the client from the Module. */
    RedisModuleBlockedClient *bc = RedisModule_BlockClientOnAuth(ctx, AuthBlock_Reply, AuthBlock_FreeData);
    int ctx_flags = RedisModule_GetContextFlags(ctx);
    if (ctx_flags & REDISMODULE_CTX_FLAGS_MULTI || ctx_flags & REDISMODULE_CTX_FLAGS_LUA) {
        /* Clean up by using RedisModule_UnblockClient since we attempted blocking the client. */
        RedisModule_UnblockClient(bc, NULL);
        return REDISMODULE_AUTH_DENIED;
    }
    RedisModule_BlockedClientMeasureTimeStart(bc);
    pthread_t tid;
    /* Allocate memory for information needed. */
    void **targ = RedisModule_Alloc(sizeof(void*)*3);
    targ[0] = bc;
    targ[1] = RedisModule_CreateStringFromString(NULL, username);
    targ[2] = RedisModule_CreateStringFromString(NULL, password);
    /* Create bg thread and pass the blockedclient, username and password to it. */
    if (pthread_create(&tid, NULL, AuthBlock_ThreadMain, targ) != 0) {
        RedisModule_AbortBlock(bc);
    }
    return REDISMODULE_AUTH_BLOCKED;
}

int test_rm_register_blocking_auth_cb(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_RegisterCustomAuthCallback(ctx, blocking_auth_cb);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int test_rm_unregister_auth_cbs(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_UnregisterCustomAuthCallbacks(ctx) == REDISMODULE_OK) {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
        return REDISMODULE_OK;
    }
    else if (errno == ENOENT) {
        RedisModule_ReplyWithError(ctx, "ERR - no custom auth cbs registered by this module");
    }
    else if (errno == EINPROGRESS) {
        RedisModule_ReplyWithError(ctx, "ERR - cannot unregister cbs as custom auth is in progress");
    }
    return REDISMODULE_ERR;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx,"customauth",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"testmoduleone.rm_register_auth_cb", test_rm_register_auth_cb,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"testmoduleone.rm_register_blocking_auth_cb", test_rm_register_blocking_auth_cb,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"testmoduleone.rm_unregister_auth_cbs", test_rm_unregister_auth_cbs,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}