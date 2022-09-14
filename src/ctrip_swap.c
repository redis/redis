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


list *clientRenewRequestLocks(client *c) {
    list *old = c->swap_locks;
    c->swap_locks = listCreate();
    return old;
}

void clientGotRequestIOAndLock(client *c, swapCtx *ctx, void *lock) {
    serverAssert(ctx->swap_lock == NULL);
    ctx->swap_lock = lock;
    switch (c->client_hold_mode) {
    case CLIENT_HOLD_MODE_CMD:
    case CLIENT_HOLD_MODE_REPL:
        serverAssert(c->swap_locks != NULL);
        listAddNodeTail(c->swap_locks,lock);
        break;
    case CLIENT_HOLD_MODE_EVICT:
    default:
        break;
    }
}

void clientReleaseRequestIO(client *c, swapCtx *ctx) {
    UNUSED(c);
    requestReleaseIO(ctx->swap_lock);
}

void clientReleaseRequestLocks(client *c, swapCtx *ctx) {
    list *locks;
    listNode *ln;
    listIter li;

    switch (c->client_hold_mode) {
    case CLIENT_HOLD_MODE_CMD:
    case CLIENT_HOLD_MODE_REPL:
        locks = clientRenewRequestLocks(c);
        listRewind(locks,&li);
        while ((ln = listNext(&li))) {
            requestReleaseLock(listNodeValue(ln));
        }
        listRelease(locks);
        break;
    case CLIENT_HOLD_MODE_EVICT:
        if (ctx->swap_lock) {
            requestReleaseLock(ctx->swap_lock);
        }
        break;
    default:
        break;
    }
}

/* SwapCtx manages context and data for swapping specific key. Note that:
 * - key_request copy to swapCtx.key_request
 * - swapdata moved to swapCtx,
 * - swapRequest managed by async/sync complete queue (not by swapCtx).
 * swapCtx released when keyRequest finishes. */
swapCtx *swapCtxCreate(client *c, keyRequest *key_request,
        clientKeyRequestFinished finished) {
    swapCtx *ctx = zcalloc(sizeof(swapCtx));
    ctx->c = c;
    moveKeyRequest(ctx->key_request,key_request);
    ctx->finished = finished;
    ctx->errcode = 0;
    ctx->swap_lock = NULL;
#ifdef SWAP_DEBUG
    char *key = key_request->key ? key_request->key->ptr : "(nil)";
    char identity[MAX_MSG];
    snprintf(identity,MAX_MSG,"[%s:%s:%.*s]",
            swapIntentionName(key_request->cmd_intention),c->cmd->name,MAX_MSG/2,key);
    swapDebugMsgsInit(&ctx->msgs, identity);
#endif
    return ctx;
}

void swapCtxSetSwapData(swapCtx *ctx, MOVE swapData *data, MOVE void *datactx) {
    ctx->data = data;
    ctx->datactx = datactx;
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
	if (c->swap_errcode) {
        rejectCommandFormat(c,"Swap failed (code=%d)",c->swap_errcode);
        c->swap_errcode = 0;
    } else {
		call(c,CMD_CALL_FULL);
		/* post call */
		c->woff = server.master_repl_offset;
		if (listLength(server.ready_keys))
			handleClientsBlockedOnKeys();
	}
    server.in_swap_cb = 0;

    /* unhold keys for current command. */
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_CMD);
    clientUnholdKeys(c);
    /* post command */
    commandProcessed(c);
    clientReleaseRequestLocks(c,NULL/*ctx unused*/);
    /* pipelined command might already read into querybuf, if process not
     * restarted, pending commands would not be processed again. */
    processInputBuffer(c);
}

void normalClientKeyRequestFinished(client *c, swapCtx *ctx) {
    robj *key = ctx->key_request->key;
    UNUSED(key);
    DEBUG_MSGS_APPEND(&ctx->msgs,"request-finished",
            "key=%s, keyrequests_count=%d, errcode=%d",
            key?(sds)key->ptr:"<nil>", c->keyrequests_count, ctx->errcode);
    c->keyrequests_count--;
    if (ctx->errcode) clientSwapError(c,ctx->errcode);
    if (c->keyrequests_count == 0) {
        if (!c->CLIENT_DEFERED_CLOSING) continueProcessCommand(c);
    }
}

void keyRequestSwapFinished(swapData *data, void *pd, int errcode) {
    UNUSED(data);
    swapCtx *ctx = pd;
	if (errcode) ctx->errcode = errcode;

    if (data) {
        swapDataKeyRequestFinished(data);
        DEBUG_MSGS_APPEND(&ctx->msgs,"swap-finished",
                "key=%s,propagate_expire=%d,set_dirty=%d",
                (sds)data->key->ptr,data->propagate_expire,data->set_dirty);
    }

    /* release io will trigger either another swap within the same tx or
     * command call, but never both. so swap and main thread will not
     * touch the same key in parallel. */
    clientReleaseRequestIO(ctx->c,ctx);

    ctx->finished(ctx->c,ctx);
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

void keyRequestProceed(void *listeners, redisDb *db, robj *key,
        client *c, void *pd) {
    int reason_num = 0, retval = 0, swap_intention;
    void *datactx = NULL;
    swapData *data = NULL;
    swapCtx *ctx = pd;
    robj *value;
    objectMeta *object_meta;
    char *reason;
    void *msgs = NULL;
    uint32_t swap_intention_flags;
    long long expire;
    UNUSED(reason), UNUSED(c);
    
#ifdef SWAP_DEBUG
    msgs = &ctx->msgs;
#endif

    serverAssert(c == ctx->c);
    clientGotRequestIOAndLock(c,ctx,listeners);

    if (db == NULL || key == NULL) {
        reason = "noswap needed for db/svr level request";
        reason_num = NOSWAP_REASON_NOTKEYLEVEL;
        goto noswap;
    }

    value = lookupKey(db,key,LOOKUP_NOTOUCH);

    data = createSwapData(db,key,value);
    swapCtxSetSwapData(ctx,data,datactx);

    if (value == NULL) {
        submitSwapMetaRequest(SWAP_MODE_ASYNC,ctx->key_request,
                ctx,data,datactx,keyRequestSwapFinished,ctx,msgs,-1);
        return;
    }

    expire = getExpire(db,key);

    retval = swapDataSetupMeta(data,value->type,expire,&datactx);
    swapCtxSetSwapData(ctx,data,datactx);
    if (retval == SWAP_DATA_UNSUPPORTED) {
        reason = "data not support swap";
        reason_num = NOSWAP_REASON_KEYNOTSUPPORT;
        goto noswap;
    }

    object_meta = lookupMeta(db,key);
    swapDataSetObjectMeta(data,object_meta);

    if (swapDataAna(data,ctx->key_request,&swap_intention,
                &swap_intention_flags,datactx)) {
        ctx->errcode = SWAP_ERR_DATA_ANA_FAIL;
        reason = "swap ana failed";
        reason_num = NOSWAP_REASON_UNEXPECTED;
        goto noswap;
    }

    if (swap_intention == SWAP_NOP) {
        reason = "swapana decided no swap";
        reason_num = NOSWAP_REASON_SWAPANADECIDED;
        goto noswap;
    }

    DEBUG_MSGS_APPEND(&ctx->msgs,"request-proceed","start swap=%s",
            swapIntentionName(swap_intention));

    submitSwapDataRequest(SWAP_MODE_ASYNC,swap_intention,swap_intention_flags,
            ctx,data,datactx,keyRequestSwapFinished,ctx,msgs,-1);

    return;

noswap:
    DEBUG_MSGS_APPEND(&ctx->msgs,"request-proceed",
            "no swap needed: %s", reason);
    if (ctx->key_request->cmd_intention == SWAP_IN &&
            reason_num == NOSWAP_REASON_SWAPANADECIDED) {
        server.stat_memory_hits++;
    }

    /* noswap is kinda swapfinished. */
    keyRequestSwapFinished(data,ctx,ctx->errcode);

    return;
}

void submitClientKeyRequests(client *c, getKeyRequestsResult *result,
        clientKeyRequestFinished cb) {
    int64_t txid = server.swap_txid++;
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

        requestGetIOAndLock(txid,db,key,keyRequestProceed,c,ctx,
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
    server.dbnum = 2;
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);
    /* Create the Redis databases, and initialize other internal state. */
    for (int j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType,NULL);
        server.db[j].expires = dictCreate(&dbExpiresDictType,NULL);
        server.db[j].meta = dictCreate(&objectMetaDictType, NULL);
        server.db[j].hold_keys = dictCreate(&objectKeyPointerValueDictType, NULL);
        server.db[j].evict_asap = listCreate();
        server.db[j].cold_keys = 0;
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
  result += swapWaitReentrantTest(argc, argv, accurate);
  result += swapWaitAckTest(argc, argv, accurate);
  result += swapCmdTest(argc, argv, accurate);
  result += swapExecTest(argc, argv, accurate);
  result += swapDataTest(argc, argv, accurate);
  result += swapDataWholeKeyTest(argc, argv, accurate);
  result += swapObjectTest(argc, argv, accurate);
  result += swapRdbTest(argc, argv, accurate);
  result += swapIterTest(argc, argv, accurate);
  result += swapDataHashTest(argc, argv, accurate);
  result += testRocksCalculateNextKey(argc, argv, accurate);
  return result;
}
#endif
