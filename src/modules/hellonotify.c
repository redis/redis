/* Helloworld module -- A few examples of the Redis Modules API in the form
 * of commands showing how to accomplish common tasks.
 *
 * This module does not do anything useful, if not for a few commands. The
 * examples are designed in order to show the API.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2016, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../redismodule.h"

/* HELLO.SIMPLE is among the simplest commands you can implement.
 * It just returns the currently selected DB id, a functionality which is
 * missing in Redis. The command uses two important API calls: one to
 * fetch the currently selected DB, the other in order to send the client
 * an integer reply as response. */
int HelloSimple_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv,
                             int argc) {
  REDISMODULE_NOT_USED(argv);
  REDISMODULE_NOT_USED(argc);
  RedisModule_ReplyWithLongLong(ctx, RedisModule_GetSelectedDb(ctx));
  return REDISMODULE_OK;
}

int HelloNotify_Callback(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
  REDISMODULE_NOT_USED(ctx);
  REDISMODULE_NOT_USED(ctx);
  RedisModule_Log(ctx,
                  "notice",
                  "Received notification! Event type: %d, event: %s, key: %s",
                  type, event, RedisModule_StringPtrLen(key, NULL));
  RedisModule_Call(ctx, "SET", "cc", "foo", "bar");
  return REDISMODULE_OK;
}
/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv,
                       int argc) {
  if (RedisModule_Init(ctx, "notify", 1, REDISMODULE_APIVER_1) ==
      REDISMODULE_ERR)
    return REDISMODULE_ERR;
  REDISMODULE_NOT_USED(argv);
  REDISMODULE_NOT_USED(argc);
  RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_ALL, HelloNotify_Callback);

  return REDISMODULE_OK;
}