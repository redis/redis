/* Copyright (c) 2021, ctrip.com
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

#include "ctrip_swap.h"

/* SwapCtx manages context and data for swapping specific key. Note that:
 * - key_request copy to swapCtx.key_request
 * - swapdata moved to swapCtx,
 * - swapRequest managed by async/sync complete queue (not by swapCtx).
 * swapCtx released when keyRequest finishes. */
swapCtx *swapCtxCreate(client *c, keyRequest *key_request,
         clientKeyRequestFinished finished) {
    swapCtx *ctx = zcalloc(sizeof(swapCtx));
    ctx->c = c;
    ctx->cmd_intention = c->cmd->intention;
    ctx->cmd_intention_flags = c->cmd->intention_flags;
    moveKeyRequest(ctx->key_request,key_request);
    ctx->finished = finished;
#ifdef SWAP_DEBUG
    char *key = key_request->key ? key_request->key->ptr : "(nil)";
    char identity[MAX_MSG];
    snprintf(identity,MAX_MSG,"[%s:%s:%.*s]",
            swapIntentionName(ctx->cmd_intention),c->cmd->name,MAX_MSG/2,key);
    swapDebugMsgsInit(&ctx->msgs, identity);
#endif
    return ctx;
}

void swapCtxFree(swapCtx *ctx) {
    if (!ctx) return;
#ifdef SWAP_DEBUG
    swapDebugMsgsDump(&ctx->msgs);
#endif
    keyRequestDeinit(ctx->key_request);
    if (ctx->data) {
        swapDataFree(ctx->data,ctx->datactx);
        ctx->data = NULL;
    }
    zfree(ctx);
}

void continueProcessCommand(client *c) {
	c->flags &= ~CLIENT_SWAPPING;
    server.current_client = c;
    server.in_swap_cb = 1;
	call(c,CMD_CALL_FULL);
    server.in_swap_cb = 0;
    /* post call */
    c->woff = server.master_repl_offset;
    if (listLength(server.ready_keys))
        handleClientsBlockedOnKeys();
    /* unhold keys for current command. */
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_CMD);
    clientUnholdKeys(c);
    /* post command */
    commandProcessed(c);
    /* pipelined command might already read into querybuf, if process not
     * restarted, pending commands would not be processed again. */
    processInputBuffer(c);
}

void normalClientKeyRequestFinished(client *c, swapCtx *ctx) {
    robj *key = ctx->key_request->key;
    UNUSED(key);
    DEBUG_MSGS_APPEND(&ctx->msgs,"request-finished",
            "key=%s, keyrequests_count=%d",
            key?(sds)key->ptr:"<nil>", c->keyrequests_count);
    c->keyrequests_count--;
    if (c->keyrequests_count == 0) {
        if (!c->CLIENT_DEFERED_CLOSING) continueProcessCommand(c);
    }
}

int keyRequestSwapFinished(swapData *data, void *pd) {
    UNUSED(data);
    swapCtx *ctx = pd;
    redisDb *db = ctx->c->db;
    robj *key = ctx->key_request->key;

    if (ctx->expired && key) {
        deleteExpiredKeyAndPropagate(db,key);
        DEBUG_MSGS_APPEND(&ctx->msgs,"swap-finished",
                "expired=%s", (sds)key->ptr);
    }

    if (ctx->set_dirty) {
        dbSetDirty(db, key);
        DEBUG_MSGS_APPEND(&ctx->msgs,"swap-finished",
                "set_dirty=%s", (sds)key->ptr);
    }

    ctx->finished(ctx->c,ctx);
    requestNotify(ctx->listeners);
    return 0;
}

/* Expired key should delete only if server is master, check expireIfNeeded
 * for more details. */
int keyExpiredAndShouldDelete(redisDb *db, robj *key) {
    if (!keyIsExpired(db,key)) return 0;
    if (server.masterhost != NULL) return 0;
    if (checkClientPauseTimeoutAndReturnIfPaused()) return 0;
    return 1;
}

#define NOSWAP_REASON_KEYNOTEXISTS 1
#define NOSWAP_REASON_NOTKEYLEVEL 2
#define NOSWAP_REASON_KEYNOTSUPPORT 3
#define NOSWAP_REASON_SWAPANADECIDED 4
#define NOSWAP_REASON_UNEXPECTED 100

int genericRequestProceed(void *listeners, redisDb *db, robj *key,
        client *c, void *pd) {
    int retval = C_OK, reason_num = 0;
    void *datactx;
    swapData *data = NULL;
    swapCtx *ctx = pd;
    robj *value, *evict;
    objectMeta *meta;
    char *reason;
    void *msgs = NULL;
    UNUSED(reason), UNUSED(c);
    int cmd_intention = ctx->cmd_intention;
    uint32_t cmd_intention_flags = ctx->cmd_intention_flags;
    
    serverAssert(c == ctx->c);
    ctx->listeners = listeners;

    if (db == NULL || key == NULL) {
        reason = "noswap needed for db/svr level request";
        reason_num = NOSWAP_REASON_NOTKEYLEVEL;
        goto noswap;
    }

    value = lookupKey(db,key,LOOKUP_NOTOUCH);
    evict = lookupEvictKey(db,key);
    meta = lookupMeta(db,key);

    if (!value && !evict) {
        reason = "key not exists";
        reason_num = NOSWAP_REASON_KEYNOTEXISTS;
        goto noswap;
    }

    /* Expired key will be delete before execute command proc. */
    if ((c->cmd && c->cmd->proc == expiredCommand) ||
            keyExpiredAndShouldDelete(db,key)) {
        cmd_intention = SWAP_DEL;
        cmd_intention_flags = 0;
        ctx->expired = 1;
    }

    data = createSwapData(db,key,value,evict,meta,&datactx);

    if (data == NULL) {
        reason = "data not support swap";
        reason_num = NOSWAP_REASON_KEYNOTSUPPORT;
        goto noswap;
    }

    ctx->data = data;
    ctx->datactx = datactx;

    if (swapDataAna(data,cmd_intention,cmd_intention_flags,
                ctx->key_request, &ctx->swap_intention,
                &ctx->swap_intention_flags,ctx->datactx)) {
        ctx->errcode = SWAP_ERR_ANA_FAIL;
        retval = C_ERR;
        reason = "swap ana failed";
        reason_num = NOSWAP_REASON_UNEXPECTED;
        goto noswap;
    }

    if (ctx->swap_intention == SWAP_NOP) {
        reason = "swapana decided no swap";
        reason_num = NOSWAP_REASON_SWAPANADECIDED;
        goto noswap;
    }

    if ((ctx->swap_intention_flags & INTENTION_DEL_ASYNC) ||
            ctx->swap_intention_flags & INTENTION_IN_DEL) {
        /* rocksdb and mem differs after rocksdb del. */
        ctx->set_dirty = 1;
    }

    DEBUG_MSGS_APPEND(&ctx->msgs,"request-proceed","start swap=%s",
            swapIntentionName(ctx->swap_intention));

#ifdef SWAP_DEBUG
    msgs = &ctx->msgs;
#endif

    submitSwapRequest(SWAP_MODE_ASYNC,ctx->swap_intention,
            ctx->swap_intention_flags,
            data,datactx,keyRequestSwapFinished,ctx,msgs);

    return C_OK;

noswap:
    DEBUG_MSGS_APPEND(&ctx->msgs,"request-proceed",
            "no swap needed: %s", reason);
    if (ctx->cmd_intention == SWAP_IN &&
            reason_num == NOSWAP_REASON_SWAPANADECIDED)
        server.stat_memory_hits++;
    /* noswap is kinda swapfinished. */
    keyRequestSwapFinished(data,ctx);

    return retval;
}

void submitClientKeyRequests(client *c, getKeyRequestsResult *result,
        clientKeyRequestFinished cb) {
    for (int i = 0; i < result->num; i++) {
        void *msgs = NULL;
        keyRequest *key_request = result->key_requests + i;
        redisDb *db = key_request->level == REQUEST_LEVEL_SVR ? NULL : c->db;
        robj *key = key_request->key;
        swapCtx *ctx = swapCtxCreate(c,key_request,cb); /*key_request moved.*/
        if (key) clientHoldKey(c,key,0);
#ifdef SWAP_DEBUG
        msgs = &ctx->msgs;
#endif
        DEBUG_MSGS_APPEND(&ctx->msgs,"request-wait", "key=%s",
                key ? (sds)key->ptr : "<nil>");

        requestWait(db,key,genericRequestProceed,c,ctx,
                (freefunc)swapCtxFree,msgs);
    }
}

/* Returns submited keyrequest count, if any keyrequest submitted, command
 * gets called in contiunueProcessCommand instead of normal call(). */
int submitNormalClientRequests(client *c) {
    getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
    getKeyRequests(c,&result);
    c->keyrequests_count = result.num;
    submitClientKeyRequests(c,&result,normalClientKeyRequestFinished);
    releaseKeyRequests(&result);
    getKeyRequestsFreeResult(&result);
    return result.num;
}

int dbSwap(client *c) {
    int keyrequests_submit;
    if (!(c->flags & CLIENT_MASTER)) {
        keyrequests_submit = submitNormalClientRequests(c);
    } else {
        keyrequests_submit = submitReplClientRequests(c);
    }

    if (c->keyrequests_count) swapRateLimit(c);

    if (keyrequests_submit > 0) {
        /* Swapping command parsed but not processed, return C_ERR so that:
         * 1. repl stream will not propagate to sub-slaves
         * 2. client will not reset
         * 3. client will break out process loop. */
        if (c->keyrequests_count) c->flags |= CLIENT_SWAPPING;
        return C_ERR;    
    } else if (keyrequests_submit < 0) {
        /* Swapping command parsed and dispatched, return C_OK so that:
         * 1. repl client will skip call
         * 2. repl client will reset (cmd moved to worker).
         * 3. repl client will continue parse and dispatch cmd */
        return C_OK;
    } else {
        call(c,CMD_CALL_FULL);
        c->woff = server.master_repl_offset;
        if (listLength(server.ready_keys))
            handleClientsBlockedOnKeys();
        return C_OK;
    }

    return C_OK;
}

void swapInit() {
    int i;

    initStatsSwap();

    server.evict_clients = zmalloc(server.dbnum*sizeof(client*));
    for (i = 0; i < server.dbnum; i++) {
        client *c = createClient(NULL);
        c->cmd = lookupCommandByCString("EVICT");
        c->db = server.db+i;
        c->client_hold_mode = CLIENT_HOLD_MODE_EVICT;
        server.evict_clients[i] = c;
    }

    server.expire_clients = zmalloc(server.dbnum*sizeof(client*));
    for (i = 0; i < server.dbnum; i++) {
        client *c = createClient(NULL);
        c->db = server.db+i;
        c->cmd = lookupCommandByCString("EXPIRED");
        c->client_hold_mode = CLIENT_HOLD_MODE_EVICT;
        server.expire_clients[i] = c;
    }

    server.repl_workers = 256;
    server.repl_swapping_clients = listCreate();
    server.repl_worker_clients_free = listCreate();
    server.repl_worker_clients_used = listCreate();
    for (i = 0; i < server.repl_workers; i++) {
        client *c = createClient(NULL);
        c->client_hold_mode = CLIENT_HOLD_MODE_REPL;
        listAddNodeTail(server.repl_worker_clients_free, c);
    }

    server.rdb_load_ctx = NULL;
    server.request_listeners = serverRequestListenersCreate();

}



#ifdef REDIS_TEST
int clearTestRedisDb() {
    emptyDbStructure(server.db, -1, 0, NULL);
    return 1;
}

int initTestRedisDb() {
    server.dbnum = 1;
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);
    /* Create the Redis databases, and initialize other internal state. */
    for (int j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType,NULL);
        server.db[j].expires = dictCreate(&dbExpiresDictType,NULL);
        server.db[j].evict = dictCreate(&evictDictType, NULL);
        server.db[j].meta = dictCreate(&dbMetaDictType, NULL);
        server.db[j].hold_keys = dictCreate(&objectKeyPointerValueDictType, NULL);
        server.db[j].evict_asap = listCreate();
        server.db[j].expires_cursor = 0;
        server.db[j].id = j;
        server.db[j].avg_ttl = 0;
        server.db[j].defrag_later = listCreate();
        listSetFreeMethod(server.db[j].defrag_later,(void (*)(void*))sdsfree);
    }
    return 1;
}

void createSharedObjects(void);
int initTestRedisServer() {
    server.maxmemory_policy = MAXMEMORY_FLAG_LFU;
    server.logfile = zstrdup(CONFIG_DEFAULT_LOGFILE);
    server.meta_version = 1;
    createSharedObjects();
    initTestRedisDb();
    return 1;
}
int clearTestRedisServer() {
    return 1;
}
int swapTest(int argc, char **argv, int accurate) {
  int result = 0;
  result += swapWaitTest(argc, argv, accurate);
  result += swapCmdTest(argc, argv, accurate);
  result += swapExecTest(argc, argv, accurate);
  result += swapRdbTest(argc, argv, accurate);
  result += swapObjectTest(argc, argv, accurate);
  result += swapDataWholeKeyTest(argc, argv, accurate);
  result += swapDataBigHashTest(argc, argv, accurate);
  return result;
}
#endif
