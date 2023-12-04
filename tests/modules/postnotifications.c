/* This module is used to test the server post keyspace jobs API.
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

/* This module allow to verify 'RedisModule_AddPostNotificationJob' by registering to 3
 * key space event:
 * * STRINGS - the module register to all strings notifications and set post notification job
 *             that increase a counter indicating how many times the string key was changed.
 *             In addition, it increase another counter that counts the total changes that
 *             was made on all strings keys.
 * * EXPIRED - the module register to expired event and set post notification job that that
 *             counts the total number of expired events.
 * * EVICTED - the module register to evicted event and set post notification job that that
 *             counts the total number of evicted events.
 *
 * In addition, the module register a new command, 'postnotification.async_set', that performs a set
 * command from a background thread. This allows to check the 'RedisModule_AddPostNotificationJob' on
 * notifications that was triggered on a background thread. */

#define _BSD_SOURCE
#define _DEFAULT_SOURCE /* For usleep */

#include "redismodule.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

static void KeySpace_PostNotificationStringFreePD(void *pd) {
    RedisModule_FreeString(NULL, pd);
}

static void KeySpace_PostNotificationReadKey(RedisModuleCtx *ctx, void *pd) {
    RedisModuleCallReply* rep = RedisModule_Call(ctx, "get", "!s", pd);
    RedisModule_FreeCallReply(rep);
}

static void KeySpace_PostNotificationString(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(ctx);
    RedisModuleCallReply* rep = RedisModule_Call(ctx, "incr", "!s", pd);
    RedisModule_FreeCallReply(rep);
}

static int KeySpace_NotificationExpired(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);
    REDISMODULE_NOT_USED(key);

    RedisModuleString *new_key = RedisModule_CreateString(NULL, "expired", 7);
    RedisModule_AddPostNotificationJob(ctx, KeySpace_PostNotificationString, new_key, KeySpace_PostNotificationStringFreePD);
    return REDISMODULE_OK;
}

static int KeySpace_NotificationEvicted(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);
    REDISMODULE_NOT_USED(key);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "evicted", 7) == 0) {
        return REDISMODULE_OK; /* do not count the evicted key */
    }

    if (strncmp(key_str, "before_evicted", 14) == 0) {
        return REDISMODULE_OK; /* do not count the before_evicted key */
    }

    RedisModuleString *new_key = RedisModule_CreateString(NULL, "evicted", 7);
    RedisModule_AddPostNotificationJob(ctx, KeySpace_PostNotificationString, new_key, KeySpace_PostNotificationStringFreePD);
    return REDISMODULE_OK;
}

static int KeySpace_NotificationString(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "string_", 7) != 0) {
        return REDISMODULE_OK;
    }

    if (strcmp(key_str, "string_total") == 0) {
        return REDISMODULE_OK;
    }

    RedisModuleString *new_key;
    if (strncmp(key_str, "string_changed{", 15) == 0) {
        new_key = RedisModule_CreateString(NULL, "string_total", 12);
    } else {
        new_key = RedisModule_CreateStringPrintf(NULL, "string_changed{%s}", key_str);
    }

    RedisModule_AddPostNotificationJob(ctx, KeySpace_PostNotificationString, new_key, KeySpace_PostNotificationStringFreePD);
    return REDISMODULE_OK;
}

static int KeySpace_LazyExpireInsidePostNotificationJob(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "read_", 5) != 0) {
        return REDISMODULE_OK;
    }

    RedisModuleString *new_key = RedisModule_CreateString(NULL, key_str + 5, strlen(key_str) - 5);;
    RedisModule_AddPostNotificationJob(ctx, KeySpace_PostNotificationReadKey, new_key, KeySpace_PostNotificationStringFreePD);
    return REDISMODULE_OK;
}

static int KeySpace_NestedNotification(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key){
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(type);
    REDISMODULE_NOT_USED(event);

    const char *key_str = RedisModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "write_sync_", 11) != 0) {
        return REDISMODULE_OK;
    }

    /* This test was only meant to check REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS.
     * In general it is wrong and discourage to perform any writes inside a notification callback.  */
    RedisModuleString *new_key = RedisModule_CreateString(NULL, key_str + 11, strlen(key_str) - 11);;
    RedisModuleCallReply* rep = RedisModule_Call(ctx, "set", "!sc", new_key, "1");
    RedisModule_FreeCallReply(rep);
    RedisModule_FreeString(NULL, new_key);
    return REDISMODULE_OK;
}

static void *KeySpace_PostNotificationsAsyncSetInner(void *arg) {
    RedisModuleBlockedClient *bc = arg;
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(bc);
    RedisModule_ThreadSafeContextLock(ctx);
    RedisModuleCallReply* rep = RedisModule_Call(ctx, "set", "!cc", "string_x", "1");
    RedisModule_ThreadSafeContextUnlock(ctx);
    RedisModule_ReplyWithCallReply(ctx, rep);
    RedisModule_FreeCallReply(rep);

    RedisModule_UnblockClient(bc, NULL);
    RedisModule_FreeThreadSafeContext(ctx);
    return NULL;
}

static int KeySpace_PostNotificationsAsyncSet(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1)
        return RedisModule_WrongArity(ctx);

    pthread_t tid;
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx,NULL,NULL,NULL,0);

    if (pthread_create(&tid,NULL,KeySpace_PostNotificationsAsyncSetInner,bc) != 0) {
        RedisModule_AbortBlock(bc);
        return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return REDISMODULE_OK;
}

typedef struct KeySpace_EventPostNotificationCtx {
    RedisModuleString *triggered_on;
    RedisModuleString *new_key;
} KeySpace_EventPostNotificationCtx;

static void KeySpace_ServerEventPostNotificationFree(void *pd) {
    KeySpace_EventPostNotificationCtx *pn_ctx = pd;
    RedisModule_FreeString(NULL, pn_ctx->new_key);
    RedisModule_FreeString(NULL, pn_ctx->triggered_on);
    RedisModule_Free(pn_ctx);
}

static void KeySpace_ServerEventPostNotification(RedisModuleCtx *ctx, void *pd) {
    REDISMODULE_NOT_USED(ctx);
    KeySpace_EventPostNotificationCtx *pn_ctx = pd;
    RedisModuleCallReply* rep = RedisModule_Call(ctx, "lpush", "!ss", pn_ctx->new_key, pn_ctx->triggered_on);
    RedisModule_FreeCallReply(rep);
}

static void KeySpace_ServerEventCallback(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data) {
    REDISMODULE_NOT_USED(eid);
    REDISMODULE_NOT_USED(data);
    if (subevent > 3) {
        RedisModule_Log(ctx, "warning", "Got an unexpected subevent '%llu'", (unsigned long long)subevent);
        return;
    }
    static const char* events[] = {
            "before_deleted",
            "before_expired",
            "before_evicted",
            "before_overwritten",
    };

    const RedisModuleString *key_name = RedisModule_GetKeyNameFromModuleKey(((RedisModuleKeyInfo*)data)->key);
    const char *key_str = RedisModule_StringPtrLen(key_name, NULL);

    for (int i = 0 ; i < 4 ; ++i) {
        const char *event = events[i];
        if (strncmp(key_str, event , strlen(event)) == 0) {
            return; /* don't log any event on our tracking keys */
        }
    }

    KeySpace_EventPostNotificationCtx *pn_ctx = RedisModule_Alloc(sizeof(*pn_ctx));
    pn_ctx->triggered_on = RedisModule_HoldString(NULL, (RedisModuleString*)key_name);
    pn_ctx->new_key = RedisModule_CreateString(NULL, events[subevent], strlen(events[subevent]));
    RedisModule_AddPostNotificationJob(ctx, KeySpace_ServerEventPostNotification, pn_ctx, KeySpace_ServerEventPostNotificationFree);
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"postnotifications",1,REDISMODULE_APIVER_1) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    if (!(RedisModule_GetModuleOptionsAll() & REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS)) {
        return REDISMODULE_ERR;
    }

    int with_key_events = 0;
    if (argc >= 1) {
        const char *arg = RedisModule_StringPtrLen(argv[0], 0);
        if (strcmp(arg, "with_key_events") == 0) {
            with_key_events = 1;
        }
    }

    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS);

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NotificationString) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_LazyExpireInsidePostNotificationJob) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_STRING, KeySpace_NestedNotification) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_EXPIRED, KeySpace_NotificationExpired) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if(RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_EVICTED, KeySpace_NotificationEvicted) != REDISMODULE_OK){
        return REDISMODULE_ERR;
    }

    if (with_key_events) {
        if(RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Key, KeySpace_ServerEventCallback) != REDISMODULE_OK){
            return REDISMODULE_ERR;
        }
    }

    if (RedisModule_CreateCommand(ctx, "postnotification.async_set", KeySpace_PostNotificationsAsyncSet,
                                      "write", 0, 0, 0) == REDISMODULE_ERR){
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    REDISMODULE_NOT_USED(ctx);
    return REDISMODULE_OK;
}
