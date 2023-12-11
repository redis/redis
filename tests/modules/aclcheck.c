
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

int module_test_acl_category(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int commandBlockCheck(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    int response_ok = 0;
    int result = RedisModule_CreateCommand(ctx,"command.that.should.fail", module_test_acl_category, "", 0, 0, 0);
    response_ok |= (result == REDISMODULE_OK);

    result = RedisModule_AddACLCategory(ctx,"blockedcategory");
    response_ok |= (result == REDISMODULE_OK);
    
    RedisModuleCommand *parent = RedisModule_GetCommand(ctx,"block.commands.outside.onload");
    result = RedisModule_SetCommandACLCategories(parent, "write");
    response_ok |= (result == REDISMODULE_OK);

    result = RedisModule_CreateSubcommand(parent,"subcommand.that.should.fail",module_test_acl_category,"",0,0,0);
    response_ok |= (result == REDISMODULE_OK);
    
    /* This validates that it's not possible to create commands or add
     * a new ACL Category outside OnLoad function.
     * thus returns an error if they succeed. */
    if (response_ok) {
        RedisModule_ReplyWithError(ctx, "UNEXPECTEDOK");
    } else {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

    if (RedisModule_Init(ctx,"aclcheck",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (argc > 1) return RedisModule_WrongArity(ctx);
    
    /* When that flag is passed, we try to create too many categories,
     * and the test expects this to fail. In this case redis returns REDISMODULE_ERR
     * and set errno to ENOMEM*/
    if (argc == 1) {
        long long fail_flag = 0;
        RedisModule_StringToLongLong(argv[0], &fail_flag);
        if (fail_flag) {
            for (size_t j = 0; j < 45; j++) {
                RedisModuleString* name =  RedisModule_CreateStringPrintf(ctx, "customcategory%zu", j);
                if (RedisModule_AddACLCategory(ctx, RedisModule_StringPtrLen(name, NULL)) == REDISMODULE_ERR) {
                    RedisModule_Assert(errno == ENOMEM);
                    RedisModule_FreeString(ctx, name);
                    return REDISMODULE_ERR;
                }
                RedisModule_FreeString(ctx, name);
            }
        }
    }

    if (RedisModule_CreateCommand(ctx,"aclcheck.set.check.key", set_aclcheck_key,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"block.commands.outside.onload", commandBlockCheck,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"aclcheck.module.command.aclcategories.write", module_test_acl_category,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommand *aclcategories_write = RedisModule_GetCommand(ctx,"aclcheck.module.command.aclcategories.write");

    if (RedisModule_SetCommandACLCategories(aclcategories_write, "write") == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"aclcheck.module.command.aclcategories.write.function.read.category", module_test_acl_category,"write",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommand *read_category = RedisModule_GetCommand(ctx,"aclcheck.module.command.aclcategories.write.function.read.category");

    if (RedisModule_SetCommandACLCategories(read_category, "read") == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"aclcheck.module.command.aclcategories.read.only.category", module_test_acl_category,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommand *read_only_category = RedisModule_GetCommand(ctx,"aclcheck.module.command.aclcategories.read.only.category");

    if (RedisModule_SetCommandACLCategories(read_only_category, "read") == REDISMODULE_ERR)
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

    /* This validates that, when module tries to add a category with invalid characters,
     * redis returns REDISMODULE_ERR and set errno to `EINVAL` */
    if (RedisModule_AddACLCategory(ctx,"!nval!dch@r@cter$") == REDISMODULE_ERR)
        RedisModule_Assert(errno == EINVAL);
    else 
        return REDISMODULE_ERR;
    
    /* This validates that, when module tries to add a category that already exists,
     * redis returns REDISMODULE_ERR and set errno to `EBUSY` */
    if (RedisModule_AddACLCategory(ctx,"write") == REDISMODULE_ERR)
        RedisModule_Assert(errno == EBUSY);
    else 
        return REDISMODULE_ERR;
    
    if (RedisModule_AddACLCategory(ctx,"foocategory") == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    
    if (RedisModule_CreateCommand(ctx,"aclcheck.module.command.test.add.new.aclcategories", module_test_acl_category,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommand *test_add_new_aclcategories = RedisModule_GetCommand(ctx,"aclcheck.module.command.test.add.new.aclcategories");

    if (RedisModule_SetCommandACLCategories(test_add_new_aclcategories, "foocategory") == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    
    return REDISMODULE_OK;
}
