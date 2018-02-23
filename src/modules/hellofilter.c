#define REDISMODULE_EXPERIMENTAL_API
#include "../redismodule.h"

static RedisModuleString *log_key_name;

static const char log_command_name[] = "hellofilter.log";

int HelloFilter_LogCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisModuleString *s = RedisModule_CreateStringFromString(ctx, argv[0]);

    int i;
    for (i = 1; i < argc; i++) {
        size_t arglen;
        const char *arg = RedisModule_StringPtrLen(argv[i], &arglen);

        RedisModule_StringAppendBuffer(ctx, s, " ", 1);
        RedisModule_StringAppendBuffer(ctx, s, arg, arglen);
    }

    RedisModuleKey *log = RedisModule_OpenKey(ctx, log_key_name, REDISMODULE_WRITE|REDISMODULE_READ);
    RedisModule_ListPush(log, REDISMODULE_LIST_HEAD, s);
    RedisModule_CloseKey(log);
    RedisModule_FreeString(ctx, s);

    size_t cmdlen;
    const char *cmdname = RedisModule_StringPtrLen(argv[1], &cmdlen);
    RedisModuleCallReply *reply = RedisModule_Call(ctx, cmdname, "v", &argv[2], argc - 2);
    if (reply) {
        RedisModule_ReplyWithCallReply(ctx, reply);
        RedisModule_FreeCallReply(reply);
    } else {
        RedisModule_ReplyWithSimpleString(ctx, "Unknown command or invalid arguments");
    }
    return REDISMODULE_OK;
}

void HelloFilter_CommandFilter(RedisModuleCtx *ctx, RedisModuleFilteredCommand *cmd)
{
    cmd->argv = RedisModule_Realloc(cmd->argv, (cmd->argc+1)*sizeof(RedisModuleString *));
    int i;

    for (i = cmd->argc; i > 0; i--) {
        cmd->argv[i] = cmd->argv[i-1];
    }
    cmd->argv[0] = RedisModule_CreateString(ctx, log_command_name, sizeof(log_command_name)-1);
    cmd->argc++;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx,"hellofilter",1,REDISMODULE_APIVER_1)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (argc != 1) {
        RedisModule_Log(ctx, "warning", "Log key name not specified");
        return REDISMODULE_ERR;
    }

    log_key_name = RedisModule_CreateStringFromString(ctx, argv[0]);

    if (RedisModule_CreateCommand(ctx,log_command_name,
                HelloFilter_LogCommand,"write deny-oom",1,1,1) == REDISMODULE_ERR)
            return REDISMODULE_ERR;

    if (RedisModule_RegisterCommandFilter(ctx, HelloFilter_CommandFilter)
            == REDISMODULE_ERR) return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
