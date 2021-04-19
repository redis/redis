/* Helloblock module -- An example of blocking command implementation
 * with threads.
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
#include "../redismodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

/* Reply callback for blocking command HELLO.BLOCK */
int HelloBlock_Reply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    int *myint = RedisModule_GetBlockedClientPrivateData(ctx);
    return RedisModule_ReplyWithLongLong(ctx,*myint);
}

/* Timeout callback for blocking command HELLO.BLOCK */
int HelloBlock_Timeout(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithSimpleString(ctx,"Request timedout");
}

/* Private data freeing callback for HELLO.BLOCK command. */
void HelloBlock_FreeData(RedisModuleCtx *ctx, void *privdata) {
    REDISMODULE_NOT_USED(ctx);
    RedisModule_Free(privdata);
}

/* The thread entry point that actually executes the blocking part
 * of the command HELLO.BLOCK. */
void *HelloBlock_ThreadMain(void *arg) {
    void **targ = arg;
    RedisModuleBlockedClient *bc = targ[0];
    long long delay = (unsigned long)targ[1];
    RedisModule_Free(targ);

    sleep(delay);
    int *r = RedisModule_Alloc(sizeof(int));
    *r = rand();
    RedisModule_UnblockClient(bc,r);
    return NULL;
}

/* An example blocked client disconnection callback.
 *
 * Note that in the case of the HELLO.BLOCK command, the blocked client is now
 * owned by the thread calling sleep(). In this specific case, there is not
 * much we can do, however normally we could instead implement a way to
 * signal the thread that the client disconnected, and sleep the specified
 * amount of seconds with a while loop calling sleep(1), so that once we
 * detect the client disconnection, we can terminate the thread ASAP. */
void HelloBlock_Disconnected(RedisModuleCtx *ctx, RedisModuleBlockedClient *bc) {
    RedisModule_Log(ctx,"warning","Blocked client %p disconnected!",
        (void*)bc);

    /* Here you should cleanup your state / threads, and if possible
     * call RedisModule_UnblockClient(), or notify the thread that will
     * call the function ASAP. */
}

/* HELLO.BLOCK <delay> <timeout> -- Block for <count> seconds, then reply with
 * a random number. Timeout is the command timeout, so that you can test
 * what happens when the delay is greater than the timeout. */
int HelloBlock_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) return RedisModule_WrongArity(ctx);
    long long delay;
    long long timeout;

    if (RedisModule_StringToLongLong(argv[1],&delay) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx,"ERR invalid count");
    }

    if (RedisModule_StringToLongLong(argv[2],&timeout) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx,"ERR invalid count");
    }

    pthread_t tid;
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx,HelloBlock_Reply,HelloBlock_Timeout,HelloBlock_FreeData,timeout);

    /* Here we set a disconnection handler, however since this module will
     * block in sleep() in a thread, there is not much we can do in the
     * callback, so this is just to show you the API. */
    RedisModule_SetDisconnectCallback(bc,HelloBlock_Disconnected);

    /* Now that we setup a blocking client, we need to pass the control
     * to the thread. However we need to pass arguments to the thread:
     * the delay and a reference to the blocked client handle. */
    void **targ = RedisModule_Alloc(sizeof(void*)*2);
    targ[0] = bc;
    targ[1] = (void*)(unsigned long) delay;

    if (pthread_create(&tid,NULL,HelloBlock_ThreadMain,targ) != 0) {
        RedisModule_AbortBlock(bc);
        return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return REDISMODULE_OK;
}

/* The thread entry point that actually executes the blocking part
 * of the command HELLO.KEYS.
 *
 * Note: this implementation is very simple on purpose, so no duplicated
 * keys (returned by SCAN) are filtered. However adding such a functionality
 * would be trivial just using any data structure implementing a dictionary
 * in order to filter the duplicated items. */
void *HelloKeys_ThreadMain(void *arg) {
    RedisModuleBlockedClient *bc = arg;
    RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(bc);
    long long cursor = 0;
    size_t replylen = 0;

    RedisModule_ReplyWithArray(ctx,REDISMODULE_POSTPONED_ARRAY_LEN);
    do {
        RedisModule_ThreadSafeContextLock(ctx);
        RedisModuleCallReply *reply = RedisModule_Call(ctx,
            "SCAN","l",(long long)cursor);
        RedisModule_ThreadSafeContextUnlock(ctx);

        RedisModuleCallReply *cr_cursor =
            RedisModule_CallReplyArrayElement(reply,0);
        RedisModuleCallReply *cr_keys =
            RedisModule_CallReplyArrayElement(reply,1);

        RedisModuleString *s = RedisModule_CreateStringFromCallReply(cr_cursor);
        RedisModule_StringToLongLong(s,&cursor);
        RedisModule_FreeString(ctx,s);

        size_t items = RedisModule_CallReplyLength(cr_keys);
        for (size_t j = 0; j < items; j++) {
            RedisModuleCallReply *ele =
                RedisModule_CallReplyArrayElement(cr_keys,j);
            RedisModule_ReplyWithCallReply(ctx,ele);
            replylen++;
        }
        RedisModule_FreeCallReply(reply);
    } while (cursor != 0);
    RedisModule_ReplySetArrayLength(ctx,replylen);

    RedisModule_FreeThreadSafeContext(ctx);
    RedisModule_UnblockClient(bc,NULL);
    return NULL;
}

/* HELLO.KEYS -- Return all the keys in the current database without blocking
 * the server. The keys do not represent a point-in-time state so only the keys
 * that were in the database from the start to the end are guaranteed to be
 * there. */
int HelloKeys_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    if (argc != 1) return RedisModule_WrongArity(ctx);

    pthread_t tid;

    /* Note that when blocking the client we do not set any callback: no
     * timeout is possible since we passed '0', nor we need a reply callback
     * because we'll use the thread safe context to accumulate a reply. */
    RedisModuleBlockedClient *bc = RedisModule_BlockClient(ctx,NULL,NULL,NULL,0);

    /* Now that we setup a blocking client, we need to pass the control
     * to the thread. However we need to pass arguments to the thread:
     * the reference to the blocked client handle. */
    if (pthread_create(&tid,NULL,HelloKeys_ThreadMain,bc) != 0) {
        RedisModule_AbortBlock(bc);
        return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return REDISMODULE_OK;
}

/* Example of background processing */
struct BackgroundProcessingRequest {
    int input;
    int success;
    int result;
    int timeout;
    pthread_t tid;
    RedisModuleBlockedClient *blocked_client;
};
typedef struct BackgroundProcessingRequest BackgroundProcessingRequest;
static int success_count = 0;
static int requests_count = 0;

/* Reply callback for blocking command HELLO.BACK.PROCESS */
int BackgroundProcess_Reply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    BackgroundProcessingRequest *req = RedisModule_GetBlockedClientPrivateData(ctx);
    if (req == NULL) {
        return RedisModule_ReplyWithSimpleString(ctx,"Blocked client data is NULL");
    }
    return RedisModule_ReplyWithLongLong(ctx,req->result);
}

/* Timeout callback for blocking command HELLO.BACK.PROCESS */
int BackgroundProcess_Timeout(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    return RedisModule_ReplyWithSimpleString(ctx,"Request timed out");
}

/* Private data freeing callback for HELLO.BACK.PROCESS command. */
void BackgroundProcess_FreeData(RedisModuleCtx *ctx, void *privdata) {
    REDISMODULE_NOT_USED(ctx);

    /* Parameter privdata is a pointer to a BackgroundProcessingRequest struct.
     * It will be freed in both cases: the background processing is completed
     * and the client is unblocked; and in case when background processing was
     * not completed and the client timed out */
    BackgroundProcessingRequest *req = (BackgroundProcessingRequest*)privdata;
    if (req == NULL) {
        RedisModule_Log(ctx,"error","BackgroundProcess_FreeData: request is NULL");
    }

    if (req->success) success_count++;
    requests_count++;

    /* It is tempting to call pthread_join(req->tid) here,
     * but this would potentially block the main thread. */

    RedisModule_Free(privdata);
}

/* The thread entry point that actually executes the blocking part
 * of the command HELLO.BACK.PROCESS. 
 * For demo purposes: the background processing succeeds if an input
 * number is odd; it fails if the number is even.
 * In case of "failed background processing", the thread will block
 * for time exceeding timeout, which will result in the timed out client.
 */
void *BackgroundProcess_ThreadMain(void *arg) {
    BackgroundProcessingRequest* req = (BackgroundProcessingRequest*)arg;

    req->result = req->input * 2;
    req->success = req->input % 2 ? 1 : 0;

    if (req->success == 0) {
        sleep(req->timeout+1);
    }

    RedisModule_UnblockClientKeepPrivData(req->blocked_client);
    return NULL;
}

/* HELLO.BACK.PROCESS <input> - Background procesing produces result
 * equal input*2. It succeeds if the input is an odd number and fails
 * if the input is an even number. In case of failed background processing,
 * the client will be blocked for a few seconds and it will timeout. */
int BackgroundProcess_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) return RedisModule_WrongArity(ctx);
    long long input;

    if (RedisModule_StringToLongLong(argv[1],&input) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx,"ERR invalid input");
    }

    BackgroundProcessingRequest *req = RedisModule_Alloc(sizeof(BackgroundProcessingRequest));
    req->input = input;
    req->success = 0;
    req->result = 0;
    req->timeout = 1;
    req->tid = 0;

    req->blocked_client = RedisModule_BlockClient(ctx,
        BackgroundProcess_Reply,
        BackgroundProcess_Timeout,
        BackgroundProcess_FreeData,
        req->timeout);

    RedisModule_SetBlockedClientPrivData(ctx,req);

    if (pthread_create(&req->tid,NULL,BackgroundProcess_ThreadMain,req) != 0) {
        RedisModule_AbortBlock(req->blocked_client);
        RedisModule_Free(req);
        return RedisModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return REDISMODULE_OK;
}

/* HELLO.BLOCK.STATS - Retrieve background processing stats */
int BackgroundProcessStats_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    char msg[256];
    snprintf(msg, sizeof(msg), "Background processing requests count: %d; success count: %d",
        requests_count, success_count);

    return RedisModule_ReplyWithSimpleString(ctx,msg);
}

/* This function must be present on each Redis module. It is used in order to
 * register the commands into the Redis server. */
int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx,"helloblock",1,REDISMODULE_APIVER_1)
        == REDISMODULE_ERR) return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"hello.block",
        HelloBlock_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"hello.keys",
        HelloKeys_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"hello.back.process",
        BackgroundProcess_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"hello.back.stats",
        BackgroundProcessStats_RedisCommand,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
