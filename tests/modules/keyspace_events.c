/* This module is used to test the server keyspace events API.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2019, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "redismodule.h"
#include <stdio.h>
#include <string.h>

/** strores all the keys on which we got 'loaded' keyspace notification **/
RedisModuleDict *loaded_event_log = NULL;

static int KeySpace_Notification(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(type);

    if(strcmp(event, "loaded") == 0){
        const char* keyName = RedisModule_StringPtrLen(key, NULL);
        int nokey;
        RedisModule_DictGetC(loaded_event_log, (void*)keyName, strlen(keyName), &nokey);
        if(nokey){
            RedisModule_DictSetC(loaded_event_log, (void*)keyName, strlen(keyName), NULL);
        }
    }

    return REDISMODULE_OK;
}

static int cmdLoadedEvents(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    long long size = RedisModule_DictSize(loaded_event_log);
    RedisModule_ReplyWithLongLong(ctx, size);
    return REDISMODULE_OK;
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"testkeyspace",1,REDISMODULE_APIVER_1) == REDISMODULE_ERR) return REDISMODULE_ERR;

    loaded_event_log = RedisModule_CreateDict(ctx);

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_ALL, KeySpace_Notification) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx,"keyspace.loaded_events", cmdLoadedEvents,"",0,0,0) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    RedisModule_FreeDict(ctx, loaded_event_log);
    loaded_event_log = NULL;
    return REDISMODULE_OK;
}
