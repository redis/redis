/* Copyright (c) 2019, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define REDISMODULE_EXPERIMENTAL_API
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
    usleep(200000);
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
