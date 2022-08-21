
#include "redismodule.h"
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <strings.h>

/* A wrap for SET command with ACL check on the key. */
int dryrun(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    size_t cmd_len;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);

    RedisModuleCallReply *rep = RedisModule_Call(ctx, cmd, "CMDv", argv + 2, argc - 2);

    if (rep) {
        RedisModule_ReplyWithCallReply(ctx, rep);
    } else {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    }

    if (rep) {
        RedisModule_FreeCallReply(rep);
    }

    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"dryrun",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"dryrun", dryrun,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
