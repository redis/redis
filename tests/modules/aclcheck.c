
#include "redismodule.h"
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <strings.h>

/* A wrap for SET command with ACL check on the key. */
int set_aclcheck_key(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 4) {
        return RedisModule_WrongArity(ctx);
    }

    int permissions;
    const char *flags = RedisModule_StringPtrLen(argv[1], NULL);

    if (!strcasecmp(flags, "W")) {
        permissions = REDISMODULE_CMD_KEY_UPDATE;
    } else if (!strcasecmp(flags, "R")) {
        permissions = REDISMODULE_CMD_KEY_ACCESS;
    } else if (!strcasecmp(flags, "*")) {
        permissions = REDISMODULE_CMD_KEY_UPDATE | REDISMODULE_CMD_KEY_ACCESS;
    } else if (!strcasecmp(flags, "~")) {
        permissions = 0; /* Requires either read or write */
    } else {
        RedisModule_ReplyWithError(ctx, "INVALID FLAGS");
        return REDISMODULE_OK;
    }

    /* Check that the key can be accessed */
    RedisModuleString *user_name = RedisModule_GetCurrentUserName(ctx);
    RedisModuleUser *user = RedisModule_GetModuleUserFromUserName(user_name);
    int ret = RedisModule_ACLCheckKeyPermissions(user, argv[2], permissions);
    if (ret != 0) {
        RedisModule_ReplyWithError(ctx, "DENIED KEY");
        RedisModule_FreeModuleUser(user);
        RedisModule_FreeString(ctx, user_name);
        return REDISMODULE_OK;
    }

    RedisModuleCallReply *rep = RedisModule_Call(ctx, "SET", "v", argv + 2, argc - 2);
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    RedisModule_FreeModuleUser(user);
    RedisModule_FreeString(ctx, user_name);
    return REDISMODULE_OK;
}

/* A wrap for PUBLISH command with ACL check on the channel. */
int publish_aclcheck_channel(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    /* Check that the pubsub channel can be accessed */
    RedisModuleString *user_name = RedisModule_GetCurrentUserName(ctx);
    RedisModuleUser *user = RedisModule_GetModuleUserFromUserName(user_name);
    int ret = RedisModule_ACLCheckChannelPermissions(user, argv[1], REDISMODULE_CMD_CHANNEL_SUBSCRIBE);
    if (ret != 0) {
        RedisModule_ReplyWithError(ctx, "DENIED CHANNEL");
        RedisModule_FreeModuleUser(user);
        RedisModule_FreeString(ctx, user_name);
        return REDISMODULE_OK;
    }

    RedisModuleCallReply *rep = RedisModule_Call(ctx, "PUBLISH", "v", argv + 1, argc - 1);
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    RedisModule_FreeModuleUser(user);
    RedisModule_FreeString(ctx, user_name);
    return REDISMODULE_OK;
}

/* A wrap for RM_Call that check first that the command can be executed */
int rm_call_aclcheck_cmd(RedisModuleCtx *ctx, RedisModuleUser *user, RedisModuleString **argv, int argc) {
    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    /* Check that the command can be executed */
    int ret = RedisModule_ACLCheckCommandPermissions(user, argv + 1, argc - 1);
    if (ret != 0) {
        RedisModule_ReplyWithError(ctx, "DENIED CMD");
        /* Add entry to ACL log */
        RedisModule_ACLAddLogEntry(ctx, user, argv[1], REDISMODULE_ACL_LOG_CMD);
        return REDISMODULE_OK;
    }

    const char* cmd = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, "v", argv + 2, argc - 2);
    if(!rep){
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    }else{
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    return REDISMODULE_OK;
}

int rm_call_aclcheck_cmd_default_user(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleString *user_name = RedisModule_GetCurrentUserName(ctx);
    RedisModuleUser *user = RedisModule_GetModuleUserFromUserName(user_name);

    int res = rm_call_aclcheck_cmd(ctx, user, argv, argc);

    RedisModule_FreeModuleUser(user);
    RedisModule_FreeString(ctx, user_name);
    return res;
}

int rm_call_aclcheck_cmd_module_user(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    /* Create a user and authenticate */
    RedisModuleUser *user = RedisModule_CreateModuleUser("testuser1");
    RedisModule_SetModuleUserACL(user, "allcommands");
    RedisModule_SetModuleUserACL(user, "allkeys");
    RedisModule_SetModuleUserACL(user, "on");
    RedisModule_AuthenticateClientWithUser(ctx, user, NULL, NULL, NULL);

    int res = rm_call_aclcheck_cmd(ctx, user, argv, argc);

    /* authenticated back to "default" user (so once we free testuser1 we will not disconnected */
    RedisModule_AuthenticateClientWithACLUser(ctx, "default", 7, NULL, NULL, NULL);
    RedisModule_FreeModuleUser(user);
    return res;
}

int rm_call_aclcheck_with_errors(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if(argc < 2){
        return RedisModule_WrongArity(ctx);
    }

    const char* cmd = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, "vEC", argv + 2, argc - 2);
    RedisModule_ReplyWithCallReply(ctx, rep);
    RedisModule_FreeCallReply(rep);
    return REDISMODULE_OK;
}

/* A wrap for RM_Call that pass the 'C' flag to do ACL check on the command. */
int rm_call_aclcheck(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if(argc < 2){
        return RedisModule_WrongArity(ctx);
    }

    const char* cmd = RedisModule_StringPtrLen(argv[1], NULL);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, cmd, "vC", argv + 2, argc - 2);
    if(!rep) {
        char err[100];
        switch (errno) {
            case EACCES:
                RedisModule_ReplyWithError(ctx, "ERR NOPERM");
                break;
            default:
                snprintf(err, sizeof(err) - 1, "ERR errno=%d", errno);
                RedisModule_ReplyWithError(ctx, err);
                break;
        }
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"aclcheck",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"aclcheck.set.check.key", set_aclcheck_key,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"aclcheck.publish.check.channel", publish_aclcheck_channel,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"aclcheck.rm_call.check.cmd", rm_call_aclcheck_cmd_default_user,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"aclcheck.rm_call.check.cmd.module.user", rm_call_aclcheck_cmd_module_user,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"aclcheck.rm_call", rm_call_aclcheck,
                                  "write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"aclcheck.rm_call_with_errors", rm_call_aclcheck_with_errors,
                                      "write",0,0,0) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
