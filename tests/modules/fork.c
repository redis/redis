#define REDISMODULE_EXPERIMENTAL_API

/* define macros for having usleep */
#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include "redismodule.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define UNUSED(V) ((void) V)

int child_pid = -1;
int exitted_with_code = -1;

void done_handler(int exitcode, int bysignal, void *user_data) {
    child_pid = -1;
    exitted_with_code = exitcode;
    assert(user_data==(void*)0xdeadbeef);
    UNUSED(bysignal);
}

int fork_create(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    long long code_to_exit_with;
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if(!RMAPI_FUNC_SUPPORTED(RedisModule_Fork)){
        RedisModule_ReplyWithError(ctx, "Fork api is not supported in the current redis version");
        return REDISMODULE_OK;
    }

    RedisModule_StringToLongLong(argv[1], &code_to_exit_with);
    exitted_with_code = -1;
    child_pid = RedisModule_Fork(done_handler, (void*)0xdeadbeef);
    if (child_pid < 0) {
        RedisModule_ReplyWithError(ctx, "Fork failed");
        return REDISMODULE_OK;
    } else if (child_pid > 0) {
        /* parent */
        RedisModule_ReplyWithLongLong(ctx, child_pid);
        return REDISMODULE_OK;
    }

    /* child */
    RedisModule_Log(ctx, "notice", "fork child started");
    usleep(500000);
    RedisModule_Log(ctx, "notice", "fork child exiting");
    RedisModule_ExitFromChild(code_to_exit_with);
    /* unreachable */
    return 0;
}

int fork_exitcode(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);
    RedisModule_ReplyWithLongLong(ctx, exitted_with_code);
    return REDISMODULE_OK;
}

int fork_kill(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);
    if (RedisModule_KillForkChild(child_pid) != REDISMODULE_OK)
        RedisModule_ReplyWithError(ctx, "KillForkChild failed");
    else
        RedisModule_ReplyWithLongLong(ctx, 1);
    child_pid = -1;
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (RedisModule_Init(ctx,"fork",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fork.create", fork_create,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fork.exitcode", fork_exitcode,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"fork.kill", fork_kill,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
