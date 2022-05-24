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

static void dupKeyRequest(keyRequest *dst, keyRequest *src) {
    if (src->key) incrRefCount(src->key);
    dst->key = src->key;
    dst->num_subkeys = src->num_subkeys;
    for (int i = 0; i < src->num_subkeys; i++) {
        if (src->subkeys[i]) incrRefCount(src->subkeys[i]);
        dst->subkeys[i] = src->subkeys[i];
    }
}

static void keyRequestDeinit(keyRequest *key_request) {
    if (key_request->key) decrRefCount(key_request->key);
    key_request->key = NULL;
    key_request->num_subkeys = 0;
    for (int i = 0; i < key_request->num_subkeys; i++) {
        if (key_request->subkeys[i]) decrRefCount(key_request->subkeys[i]);
        key_request->subkeys[i] = NULL;
    }
}

swapCtx *swapCtxCreate(client *c, keyRequest *key_request) {
    swapCtx *ctx = zcalloc(sizeof(swapCtx));
    ctx->c = c;
    ctx->cmd_intention = c->cmd->swap_action;
    dupKeyRequest(ctx->key_request, key_request);
    ctx->listeners = NULL;
    ctx->swap_intention = SWAP_NOP;
    ctx->data = NULL;
    ctx->finish_type = SWAP_NOP;
    return ctx;
}

void swapCtxFree(swapCtx *ctx) {
    if (!ctx) return;
    keyRequestDeinit(ctx->key_request);
    if (ctx->data) {
        swapDataFree(ctx->data,ctx->datactx);
        ctx->data = NULL;
    }
    zfree(ctx);
}

/* ----------------------------- client swap ------------------------------ */
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
    /* post command */
    commandProcessed(c);
    /* unhold keys for current command. */
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_CMD);
    clientUnholdKeys(c);
    /* pipelined command might already read into querybuf, if process not
     * restarted, pending commands would not be processed again. */
    processInputBuffer(c);
}

void clientKeyRequestFinished(client *c, robj *key, swapCtx *ctx) {
    UNUSED(key);
    swapCtxFree(ctx);
    c->swapping_count--;
    if (c->swapping_count == 0) {
        if (!c->CLIENT_DEFERED_CLOSING) continueProcessCommand(c);
    }
}

int keyRequestSwapFinished(swapData *data, void *pd) {
    UNUSED(data);
    swapCtx *ctx = pd;
    clientKeyRequestFinished(ctx->c,ctx->key_request->key,ctx);
    requestNotify(ctx->listeners);
    return 0;
}

int keyRequestProceed(void *listeners, redisDb *db, robj *key, client *c,
        void *pd) {
    void *datactx;
    swapData *data;
    swapCtx *ctx = pd;
    robj *value = lookupKey(db,key,LOOKUP_NOTOUCH);
    robj *evict = lookupEvictKey(db,key);

    /* key not exists, noswap needed. */
    if (!value && !evict) {
        clientKeyRequestFinished(c,key,ctx);
        return 0;
    }

    data = createSwapData(db,key,value,evict,&datactx);
    ctx->listeners = listeners;
    ctx->data = data;
    ctx->datactx = datactx;
    swapDataAna(data,ctx->cmd_intention,ctx->key_request,&ctx->swap_intention);
    if (ctx->swap_intention == SWAP_NOP) {
        clientKeyRequestFinished(c,key,ctx);
    } else {
        submitSwapRequest(SWAP_MODE_ASYNC,ctx->swap_intention,data,datactx,
                keyRequestSwapFinished,ctx);
    }
    return 0;
}

/* Start swapping or schedule a swapping task for client:
 * - if client requires swapping key (some other client is doing rocksdb IO for
 *   this key), we defer and re-evaluate untill all preceding swap finished.
 * - if client requires cold(evicted) key, and there is no preceeding swap
 *   action, we start a new swapping task.
 * - if client requires hot or not-existing key, no swap is needed.
 *
 * this funcion returns num swapping needed for this client, we should pause
 * processCommand if swapping needed. */
int submitNormalClientRequest(client *c) {
    getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
    getKeyRequests(c, &result);
    c->swapping_count = result.num;
    for (int i = 0; i < result.num; i++) {
        keyRequest *key_request = result.key_requests + i;
        swapCtx *ctx = swapCtxCreate(c,key_request);
        requestWait(c->db,key_request->key,keyRequestProceed,c,ctx);
    }
    releaseKeyRequests(&result);
    getKeyRequestsFreeResult(&result);
    return result.num;
}

/* Different from original replication stream process, slave.master client
 * might trigger swap and block untill rocksdb IO finish. because there is
 * only one master client so rocksdb IO will be done sequentially, thus slave
 * can't catch up with master. 
 * In order to speed up replication stream processing, slave.master client
 * dispatches command to multiple worker client and execute commands when 
 * rocks IO finishes. Note that replicated commands swap in-parallel but we
 * still processed in received order. */
int dbSwap(client *c) {
    int swap_result;

    if (!(c->flags & CLIENT_MASTER)) {
        /* normal client swap */
        swap_result = submitNormalClientRequest(c);
    } else {
        /* repl client swap */
        swap_result = submitReplClientRequest(c);
    }

    if (swap_result) swapRateLimit(c);

    return swap_result;
}

void swapInit() {
    int i;
    char *swap_type_names[] = {"nop", "get", "put", "del"};

    server.swap_stats = zmalloc(SWAP_TYPES*sizeof(swapStat));
    for (i = 0; i < SWAP_TYPES; i++) {
        server.swap_stats[i].name = swap_type_names[i];
        server.swap_stats[i].started = 0;
        server.swap_stats[i].finished = 0;
        server.swap_stats[i].last_start_time = 0;
        server.swap_stats[i].last_finish_time = 0;
        server.swap_stats[i].started_rawkey_bytes = 0;
        server.swap_stats[i].finished_rawkey_bytes = 0;
        server.swap_stats[i].started_rawval_bytes = 0;
        server.swap_stats[i].finished_rawval_bytes = 0;
    }

    server.evict_clients = zmalloc(server.dbnum*sizeof(client*));
    for (i = 0; i < server.dbnum; i++) {
        client *c = createClient(NULL);
        c->cmd = lookupCommandByCString("EVICT");
        c->db = server.db+i;
        c->client_hold_mode = CLIENT_HOLD_MODE_EVICT;
        server.evict_clients[i] = c;
    }

    server.rksdel_clients = zmalloc(server.dbnum*sizeof(client*));
    for (i = 0; i < server.dbnum; i++) {
        client *c = createClient(NULL);
        c->db = server.db+i;
        c->cmd = lookupCommandByCString("RKSDEL");
        c->client_hold_mode = CLIENT_HOLD_MODE_EVICT;
        server.rksdel_clients[i] = c;
    }

    server.rksget_clients = zmalloc(server.dbnum*sizeof(client*));
    for (i = 0; i < server.dbnum; i++) {
        client *c = createClient(NULL);
        c->db = server.db+i;
        c->cmd = lookupCommandByCString("RKSGET");
        c->client_hold_mode = CLIENT_HOLD_MODE_EVICT;
        server.rksget_clients[i] = c;
    }

    server.dummy_clients = zmalloc(server.dbnum*sizeof(client*));
    for (i = 0; i < server.dbnum; i++) {
        client *c = createClient(NULL);
        c->db = server.db+i;
        c->client_hold_mode = CLIENT_HOLD_MODE_EVICT;
        server.dummy_clients[i] = c;
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

