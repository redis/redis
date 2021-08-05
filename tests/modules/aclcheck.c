#define REDISMODULE_EXPERIMENTAL_API

#include "redismodule.h"
#include <errno.h>
#include <assert.h>

/* A wrap for SET command with ACL check on the key. */
int set_aclcheck_key(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    /* Check that the key can be accessed */
    RedisModuleUserID * userid = RedisModule_CreateModuleUserID(ctx);
    int ret = RedisModule_ACLCheckKeyPermissions(userid, argv[1]);
    if (ret != 0) {
        RedisModule_ReplyWithError(ctx, "DENIED KEY");
        RedisModule_FreeModuleUserID(userid);
        return REDISMODULE_OK;
    }

    RedisModuleCallReply *rep = RedisModule_Call(ctx, "SET", "v", argv + 1, argc - 1);
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    RedisModule_FreeModuleUserID(userid);
    return REDISMODULE_OK;
}

/* A dummy command that takes a key and do acl check with deleted user. */
int test_aclcheck_key_user_deleted(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleUser *testuser = RedisModule_CreateModuleUser("testuser1");
    RedisModule_SetModuleUserACL(testuser, "allcommands");
    RedisModule_SetModuleUserACL(testuser, "allkeys");
    RedisModule_SetModuleUserACL(testuser, "on");
    RedisModule_AuthenticateClientWithUser(ctx, testuser, NULL, NULL, NULL);

    /* Check that the key can be accessed */
    RedisModuleUserID * testuserid = RedisModule_CreateModuleUserID(ctx);

    /* authenticated back to "default" user (so once we free testuser1 we will not disconnected */
    RedisModule_AuthenticateClientWithACLUser(ctx, "default", 7, NULL, NULL, NULL);
    RedisModule_FreeModuleUser(testuser);

    /* verify that using the UserID of deleted user, fails*/
    int ret = RedisModule_ACLCheckKeyPermissions(testuserid, argv[1]);
    assert(ret != REDISMODULE_OK);
    assert(errno == ENOTSUP);
    RedisModule_ReplyWithError(ctx, "NO ACL USER");
    RedisModule_FreeModuleUserID(testuserid);
    return REDISMODULE_OK;
}

/* A wrap for PUBLISH command with ACL check on the channel. */
int publish_aclcheck_channel(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    /* Check that the key can be accessed */
    RedisModuleUserID * userid = RedisModule_CreateModuleUserID(ctx);
    int ret = RedisModule_ACLCheckChannelPermissions(userid, argv[1], 1);
    if (ret != 0) {
        RedisModule_ReplyWithError(ctx, "DENIED CHANNEL");
        RedisModule_FreeModuleUserID(userid);
        return REDISMODULE_OK;
    }

    RedisModuleCallReply *rep = RedisModule_Call(ctx, "PUBLISH", "v", argv + 1, argc - 1);
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }

    RedisModule_FreeModuleUserID(userid);
    return REDISMODULE_OK;
}

/* A wrap for RM_Call that check first that the command can be executed */
int rm_call_aclcheck_cmd(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    /* Check that the command can be executed */
    RedisModuleUserID * userid = RedisModule_CreateModuleUserID(ctx);
    int ret = RedisModule_ACLCheckCommandPermissions(userid, argv + 1, argc - 1);
    if (ret != 0) {
        RedisModule_ReplyWithError(ctx, "DENIED CMD");
        RedisModule_FreeModuleUserID(userid);
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

    RedisModule_FreeModuleUserID(userid);
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

    if (RedisModule_CreateCommand(ctx,"set.aclcheck.key", set_aclcheck_key,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.aclcheck.key.user.deleted", test_aclcheck_key_user_deleted,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"publish.aclcheck.channel", publish_aclcheck_channel,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"rm_call.aclcheck.cmd", rm_call_aclcheck_cmd,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"rm_call.aclcheck", rm_call_aclcheck,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
