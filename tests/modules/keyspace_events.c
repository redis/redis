/* This module is used to test the server keyspace events API.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2020, Meir Shpilraien <meir at redislabs dot com>
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
#include <stdio.h>
#include <string.h>

/** stores all the keys on which we got 'loaded' keyspace notification **/
RedisModuleDict *loaded_event_log = NULL;
/** stores all the keys on which we got 'module' keyspace notification **/
RedisModuleDict *module_event_log = NULL;

static int KeySpace_NotificationLoaded(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(type);

    if(strcmp(event, "loaded") == 0){
        const char* keyName = RedisModule_StringPtrLen(key, NULL);
        int nokey;
        RedisModule_DictGetC(loaded_event_log, (void*)keyName, strlen(keyName), &nokey);
        if(nokey){
            RedisModule_DictSetC(loaded_event_log, (void*)keyName, strlen(keyName), RedisModule_HoldString(ctx, key));
        }
    }

    return REDISMODULE_OK;
}

static int KeySpace_NotificationGeneric(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(type);

    if (strcmp(event, "del") == 0) {
        RedisModuleString *copykey = RedisModule_CreateStringPrintf(ctx, "%s_copy", RedisModule_StringPtrLen(key, NULL));
        RedisModuleCallReply* rep = RedisModule_Call(ctx, "DEL", "s!", copykey);
        RedisModule_FreeString(ctx, copykey);
        RedisModule_FreeCallReply(rep);

        int ctx_flags = RedisModule_GetContextFlags(ctx);
        if (ctx_flags & REDISMODULE_CTX_FLAGS_LUA) {
            RedisModuleCallReply* rep = RedisModule_Call(ctx, "INCR", "c", "lua");
            RedisModule_FreeCallReply(rep);
        }
        if (ctx_flags & REDISMODULE_CTX_FLAGS_MULTI) {
            RedisModuleCallReply* rep = RedisModule_Call(ctx, "INCR", "c", "multi");
            RedisModule_FreeCallReply(rep);
        }
    }

    return REDISMODULE_OK;
}

static int KeySpace_NotificationModule(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char* keyName = RedisModule_StringPtrLen(key, NULL);
    int nokey;
    RedisModule_DictGetC(module_event_log, (void*)keyName, strlen(keyName), &nokey);
    if(nokey){
        RedisModule_DictSetC(module_event_log, (void*)keyName, strlen(keyName), RedisModule_HoldString(ctx, key));
    }
    return REDISMODULE_OK;
}

static int cmdNotify(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(argc != 2){
        return RedisModule_WrongArity(ctx);
    }

    RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, "notify", argv[1]);
    RedisModule_ReplyWithNull(ctx);
    return REDISMODULE_OK;
}

static int cmdIsModuleKeyNotified(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(argc != 2){
        return RedisModule_WrongArity(ctx);
    }

    const char* key  = RedisModule_StringPtrLen(argv[1], NULL);

    int nokey;
    RedisModuleString* keyStr = RedisModule_DictGetC(module_event_log, (void*)key, strlen(key), &nokey);

    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithLongLong(ctx, !nokey);
    if(nokey){
        RedisModule_ReplyWithNull(ctx);
    }else{
        RedisModule_ReplyWithString(ctx, keyStr);
    }
    return REDISMODULE_OK;
}

static int cmdIsKeyLoaded(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(argc != 2){
        return RedisModule_WrongArity(ctx);
    }

    const char* key  = RedisModule_StringPtrLen(argv[1], NULL);

    int nokey;
    RedisModuleString* keyStr = RedisModule_DictGetC(loaded_event_log, (void*)key, strlen(key), &nokey);

    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithLongLong(ctx, !nokey);
    if(nokey){
        RedisModule_ReplyWithNull(ctx);
    }else{
        RedisModule_ReplyWithString(ctx, keyStr);
    }
    return REDISMODULE_OK;
}

static int cmdDelKeyCopy(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2)
        return RedisModule_WrongArity(ctx);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, "DEL", "s!", argv[1]);
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }
    return REDISMODULE_OK;
}

/* Call INCR and propagate using RM_Call with `!`. */
static int cmdIncrCase1(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2)
        return RedisModule_WrongArity(ctx);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, "INCR", "s!", argv[1]);
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }
    return REDISMODULE_OK;
}

/* Call INCR and propagate using RM_Replicate. */
static int cmdIncrCase2(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2)
        return RedisModule_WrongArity(ctx);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, "INCR", "s", argv[1]);
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }
    RedisModule_Replicate(ctx, "INCR", "s", argv[1]);
    return REDISMODULE_OK;
}

/* Call INCR and propagate using RM_ReplicateVerbatim. */
static int cmdIncrCase3(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2)
        return RedisModule_WrongArity(ctx);

    RedisModuleCallReply* rep = RedisModule_Call(ctx, "INCR", "s", argv[1]);
    if (!rep) {
        RedisModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        RedisModule_ReplyWithCallReply(ctx, rep);
        RedisModule_FreeCallReply(rep);
    }
    RedisModule_ReplicateVerbatim(ctx);
    return REDISMODULE_OK;
}


/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"testkeyspace",1,REDISMODULE_APIVER_1) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    loaded_event_log = RedisModule_CreateDict(ctx);
    module_event_log = RedisModule_CreateDict(ctx);

    int keySpaceAll = RedisModule_GetKeyspaceNotificationFlagsAll();

    if (!(keySpaceAll & REDISMODULE_NOTIFY_LOADED)) {
        // REDISMODULE_NOTIFY_LOADED event are not supported we can not start
        return REDISMODULE_ERR;
    }

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_LOADED, KeySpace_NotificationLoaded) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_GENERIC, KeySpace_NotificationGeneric) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_MODULE, KeySpace_NotificationModule) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx,"keyspace.notify", cmdNotify,"",0,0,0) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx,"keyspace.is_module_key_notified", cmdIsModuleKeyNotified,"",0,0,0) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx,"keyspace.is_key_loaded", cmdIsKeyLoaded,"",0,0,0) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx,"keyspace.del_key_copy", cmdDelKeyCopy,"",0,0,0) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }
    
    if (RedisModule_CreateCommand(ctx,"keyspace.incr_case1", cmdIncrCase1,"",0,0,0) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }
    
    if (RedisModule_CreateCommand(ctx,"keyspace.incr_case2", cmdIncrCase2,"",0,0,0) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }
    
    if (RedisModule_CreateCommand(ctx,"keyspace.incr_case3", cmdIncrCase3,"",0,0,0) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    RedisModuleDictIter *iter = RedisModule_DictIteratorStartC(loaded_event_log, "^", NULL, 0);
    char* key;
    size_t keyLen;
    RedisModuleString* val;
    while((key = RedisModule_DictNextC(iter, &keyLen, (void**)&val))){
        RedisModule_FreeString(ctx, val);
    }
    RedisModule_FreeDict(ctx, loaded_event_log);
    RedisModule_DictIteratorStop(iter);
    loaded_event_log = NULL;

    iter = RedisModule_DictIteratorStartC(module_event_log, "^", NULL, 0);
    while((key = RedisModule_DictNextC(iter, &keyLen, (void**)&val))){
        RedisModule_FreeString(ctx, val);
    }
    RedisModule_FreeDict(ctx, module_event_log);
    RedisModule_DictIteratorStop(iter);
    module_event_log = NULL;

    return REDISMODULE_OK;
}
