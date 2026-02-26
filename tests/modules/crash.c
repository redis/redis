#include "redismodule.h"

#include <strings.h>
#include <sys/mman.h>

#define UNUSED(V) ((void) V)

void assertCrash(RedisModuleInfoCtx *ctx, int for_crash_report) {
    UNUSED(ctx);
    UNUSED(for_crash_report);
    RedisModule_Assert(0);
}

void segfaultCrash(RedisModuleInfoCtx *ctx, int for_crash_report) {
    UNUSED(ctx);
    UNUSED(for_crash_report);
    /* Compiler gives warnings about writing to a random address
     * e.g "*((char*)-1) = 'x';". As a workaround, we map a read-only area
     * and try to write there to trigger segmentation fault. */
    char *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *p = 'x';
}

int cmd_crash(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(ctx);
    UNUSED(argv);
    UNUSED(argc);

    RedisModule_Assert(0);
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx,"modulecrash",1,REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (argc >= 1) {
        if (!strcasecmp(RedisModule_StringPtrLen(argv[0], NULL), "segfault")) {
            if (RedisModule_RegisterInfoFunc(ctx, segfaultCrash) == REDISMODULE_ERR) return REDISMODULE_ERR;
        } else if (!strcasecmp(RedisModule_StringPtrLen(argv[0], NULL),"assert")) {
            if (RedisModule_RegisterInfoFunc(ctx, assertCrash) == REDISMODULE_ERR) return REDISMODULE_ERR;
        }
    }

    /* Create modulecrash.xadd command which is similar to xadd command.
     * It will crash in the command handler to verify we print command tokens
     * when hide-user-data-from-log config is enabled */
    RedisModuleCommandInfo info = {
            .version = REDISMODULE_COMMAND_INFO_VERSION,
            .arity = -5,
            .key_specs = (RedisModuleCommandKeySpec[]){
                    {
                            .notes = "UPDATE instead of INSERT because of the optional trimming feature",
                            .flags = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_UPDATE,
                            .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                            .bs.index.pos = 1,
                            .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
                            .fk.range = {0,1,0}
                    },
                    {0}
            },
            .args = (RedisModuleCommandArg[]){
                    {
                            .name = "key",
                            .type = REDISMODULE_ARG_TYPE_KEY,
                            .key_spec_index = 0
                    },
                    {
                            .name = "nomkstream",
                            .type = REDISMODULE_ARG_TYPE_PURE_TOKEN,
                            .token = "NOMKSTREAM",
                            .since = "6.2.0",
                            .flags = REDISMODULE_CMD_ARG_OPTIONAL
                    },
                    {
                            .name = "trim",
                            .type = REDISMODULE_ARG_TYPE_BLOCK,
                            .flags = REDISMODULE_CMD_ARG_OPTIONAL,
                            .subargs = (RedisModuleCommandArg[]){
                                    {
                                            .name = "strategy",
                                            .type = REDISMODULE_ARG_TYPE_ONEOF,
                                            .subargs = (RedisModuleCommandArg[]){
                                                    {
                                                            .name = "maxlen",
                                                            .type = REDISMODULE_ARG_TYPE_PURE_TOKEN,
                                                            .token = "MAXLEN",
                                                    },
                                                    {
                                                            .name = "minid",
                                                            .type = REDISMODULE_ARG_TYPE_PURE_TOKEN,
                                                            .token = "MINID",
                                                            .since = "6.2.0",
                                                    },
                                                    {0}
                                            }
                                    },
                                    {
                                            .name = "operator",
                                            .type = REDISMODULE_ARG_TYPE_ONEOF,
                                            .flags = REDISMODULE_CMD_ARG_OPTIONAL,
                                            .subargs = (RedisModuleCommandArg[]){
                                                    {
                                                            .name = "equal",
                                                            .type = REDISMODULE_ARG_TYPE_PURE_TOKEN,
                                                            .token = "="
                                                    },
                                                    {
                                                            .name = "approximately",
                                                            .type = REDISMODULE_ARG_TYPE_PURE_TOKEN,
                                                            .token = "~"
                                                    },
                                                    {0}
                                            }
                                    },
                                    {
                                            .name = "threshold",
                                            .type = REDISMODULE_ARG_TYPE_STRING,
                                            .display_text = "threshold" /* Just for coverage, doesn't have a visible effect */
                                    },
                                    {
                                            .name = "count",
                                            .type = REDISMODULE_ARG_TYPE_INTEGER,
                                            .token = "LIMIT",
                                            .since = "6.2.0",
                                            .flags = REDISMODULE_CMD_ARG_OPTIONAL
                                    },
                                    {0}
                            }
                    },
                    {
                            .name = "id-selector",
                            .type = REDISMODULE_ARG_TYPE_ONEOF,
                            .subargs = (RedisModuleCommandArg[]){
                                    {
                                            .name = "auto-id",
                                            .type = REDISMODULE_ARG_TYPE_PURE_TOKEN,
                                            .token = "*"
                                    },
                                    {
                                            .name = "id",
                                            .type = REDISMODULE_ARG_TYPE_STRING,
                                    },
                                    {0}
                            }
                    },
                    {
                            .name = "data",
                            .type = REDISMODULE_ARG_TYPE_BLOCK,
                            .flags = REDISMODULE_CMD_ARG_MULTIPLE,
                            .subargs = (RedisModuleCommandArg[]){
                                    {
                                            .name = "field",
                                            .type = REDISMODULE_ARG_TYPE_STRING,
                                    },
                                    {
                                            .name = "value",
                                            .type = REDISMODULE_ARG_TYPE_STRING,
                                    },
                                    {0}
                            }
                    },
                    {0}
            }
    };

    RedisModuleCommand *cmd;

    if (RedisModule_CreateCommand(ctx,"modulecrash.xadd", cmd_crash,"write deny-oom random fast",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    cmd = RedisModule_GetCommand(ctx,"modulecrash.xadd");
    if (RedisModule_SetCommandInfo(cmd, &info) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Create a subcommand: modulecrash.parent sub
     * It will crash in the command handler to verify we print subcommand name
     * when hide-user-data-from-log config is enabled */
    RedisModuleCommandInfo subcommand_info = {
            .version = REDISMODULE_COMMAND_INFO_VERSION,
            .arity = -5,
            .key_specs = (RedisModuleCommandKeySpec[]){
                    {
                            .flags = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_UPDATE,
                            .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                            .bs.index.pos = 1,
                            .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
                            .fk.range = {0,1,0}
                    },
                    {0}
            },
            .args = (RedisModuleCommandArg[]){
                    {
                            .name = "key",
                            .type = REDISMODULE_ARG_TYPE_KEY,
                            .key_spec_index = 0
                    },
                    {
                            .name = "token",
                            .type = REDISMODULE_ARG_TYPE_PURE_TOKEN,
                            .token = "TOKEN",
                            .flags = REDISMODULE_CMD_ARG_OPTIONAL
                    },
                    {
                            .name = "data",
                            .type = REDISMODULE_ARG_TYPE_BLOCK,
                            .subargs = (RedisModuleCommandArg[]){
                                    {
                                            .name = "field",
                                            .type = REDISMODULE_ARG_TYPE_STRING,
                                    },
                                    {
                                            .name = "value",
                                            .type = REDISMODULE_ARG_TYPE_STRING,
                                    },
                                    {0}
                            }
                    },
                    {0}
            }
    };

    if (RedisModule_CreateCommand(ctx,"modulecrash.parent",NULL,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleCommand *parent = RedisModule_GetCommand(ctx,"modulecrash.parent");

    if (RedisModule_CreateSubcommand(parent,"subcmd",cmd_crash,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    cmd = RedisModule_GetCommand(ctx,"modulecrash.parent|subcmd");
    if (RedisModule_SetCommandInfo(cmd, &subcommand_info) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    /* Create modulecrash.zunion command which is similar to zunion command.
    * It will crash in the command handler to verify we print command tokens
    * when hide-user-data-from-log config is enabled */
    RedisModuleCommandInfo zunioninfo = {
            .version = REDISMODULE_COMMAND_INFO_VERSION,
            .arity = -5,
            .key_specs = (RedisModuleCommandKeySpec[]){
                    {
                            .flags = REDISMODULE_CMD_KEY_RO,
                            .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                            .bs.index.pos = 1,
                            .find_keys_type = REDISMODULE_KSPEC_FK_KEYNUM,
                            .fk.keynum = {0,1,1}
                    },
                    {0}
            },
            .args = (RedisModuleCommandArg[]){
                    {
                            .name = "numkeys",
                            .type = REDISMODULE_ARG_TYPE_INTEGER,
                    },
                    {
                            .name = "key",
                            .type = REDISMODULE_ARG_TYPE_KEY,
                            .key_spec_index = 0,
                            .flags = REDISMODULE_CMD_ARG_MULTIPLE
                    },
                    {
                            .name = "weights",
                            .type = REDISMODULE_ARG_TYPE_INTEGER,
                            .token = "WEIGHTS",
                            .flags = REDISMODULE_CMD_ARG_OPTIONAL | REDISMODULE_CMD_ARG_MULTIPLE
                    },
                    {
                            .name = "aggregate",
                            .type = REDISMODULE_ARG_TYPE_ONEOF,
                            .token = "AGGREGATE",
                            .flags = REDISMODULE_CMD_ARG_OPTIONAL,
                            .subargs = (RedisModuleCommandArg[]){
                                    {
                                            .name = "sum",
                                            .type = REDISMODULE_ARG_TYPE_PURE_TOKEN,
                                            .token = "sum"
                                    },
                                    {
                                            .name = "min",
                                            .type = REDISMODULE_ARG_TYPE_PURE_TOKEN,
                                            .token = "min"
                                    },
                                    {
                                            .name = "max",
                                            .type = REDISMODULE_ARG_TYPE_PURE_TOKEN,
                                            .token = "max"
                                    },
                                    {0}
                            }
                    },
                    {
                            .name = "withscores",
                            .type = REDISMODULE_ARG_TYPE_PURE_TOKEN,
                            .token = "WITHSCORES",
                            .flags = REDISMODULE_CMD_ARG_OPTIONAL
                    },
                    {0}
            }
    };

    if (RedisModule_CreateCommand(ctx,"modulecrash.zunion", cmd_crash,"readonly",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    cmd = RedisModule_GetCommand(ctx,"modulecrash.zunion");
    if (RedisModule_SetCommandInfo(cmd, &zunioninfo) == REDISMODULE_ERR)
        return REDISMODULE_ERR;


    return REDISMODULE_OK;
}
