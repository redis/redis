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

/* ----------------------------- expire ------------------------------ */
/* Assumming that key is expired and deleted from db, we still need to del
 * from rocksdb. */

void expireClientKeyRequestFinished(client *c, robj *key, swapCtx *ctx) {
    swapCtxFree(ctx);
    c->keyrequests_count--;
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_EVICT);
    clientUnholdKey(c, key);
}

int expireKeyRequestSwapFinished(swapData *data, void *pd) {
    UNUSED(data);
    swapCtx *ctx = pd;
    expireClientKeyRequestFinished(ctx->c,ctx->key_request->key,ctx);
    requestNotify(ctx->listeners);
    return 0;
}

int expireKeyRequestProceed(void *listeners, redisDb *db, robj *key, client *c,
        void *pd) {
    void *datactx;
    swapCtx *ctx = pd;
    robj *value = lookupKey(db,key,LOOKUP_NOTOUCH);
    robj *evict = lookupEvictKey(db,key);
    swapData *data = createSwapData(db,key,value,evict,&datactx);
    ctx->listeners = listeners;
    ctx->data = data;
    ctx->datactx = datactx;
    swapDataAna(data,ctx->cmd_intention,ctx->key_request,&ctx->swap_intention);
    if (ctx->swap_intention == SWAP_NOP) {
        expireClientKeyRequestFinished(c,key,ctx);
    } else {
        submitSwapRequest(SWAP_MODE_ASYNC,ctx->swap_intention,data,datactx,
                expireKeyRequestSwapFinished,ctx);
    }
    return 0;
}

/* Cases when submitExpireClientRequest is called:
 * - active-expire: DEL swap will be append as if key is expired by dummy client.
 * - xxCommand: DEL swap will be append to scs of the key, bacause that key is
 *   holded before xxCommand, so scs of key will not have any PUT. so async
 *   DEL swap have the same effect of sync DEL.
 * - continueProcessCommand: same as xxCommand.
 * TODO opt: keys never evicted to rocksdb need not to be deleted from rocksdb. */
int submitExpireClientRequest(client *c, robj *key) {
    int old_keyrequests_count = c->keyrequests_count;
    keyRequest key_request = {REQUEST_LEVEL_KEY,0,key,NULL};
    swapCtx *ctx = swapCtxCreate(c,&key_request);
    c->keyrequests_count++;
    requestWait(c->db,key_request.key,expireKeyRequestProceed,c,ctx);
    return c->keyrequests_count > old_keyrequests_count;
}

/* Must make sure expire key or key shell not evicted (propagate needed) */
void expireKey(client *c, robj *key, void *pd) {
    UNUSED(pd);
    redisDb *db = c->db;
    client *expire_client = server.rksdel_clients[db->id];
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_EVICT);
    clientUnholdKey(c, key);
    submitExpireClientRequest(expire_client, key);
	deleteExpiredKeyAndPropagate(db,key);
}

/* How key is expired:
 * 1. SWAP GET if expiring key is EVICTED (submitExpireClientRequest), note that
 *    this expire key evicted would only happend for active expire, because
 *    key have already been loaded before cmd->proc, so xxCommand won't trigger
 *    SWAP GET.
 * 2. Do dbDelete & propagate & notify  (expireKey) 
 * 3. Delete expired key from rocksdb (rocksDelete). NOTE that although
 *    DEL swap appended to scs tail rather than scs head, but because PUT
 *    will not happend if scs is not empty, so PUT will not happen if DEL
 *    append to tail, thus key would not be evicted to before DEL. so DEL
 *    is started right away.
 *
 * Note that currently we can only generate ONE action for each swap, so we
 * can't do both GET+propagate & DEL+nop in step 1, so rocks DEL+nop is
 * defered untill GET+propagate finished.
 */
int dbExpire(redisDb *db, robj *key) {
    int nswap = 0;
    client *c = server.rksget_clients[db->id];

    /* No need to do SWAP GET if called in swap callback(keys should have already
     * been swapped in) */
    if (!server.in_swap_cb) nswap = submitExpireClientRequest(c, key);

    /* when expiring key is in db.dict, we don't need to swapin key, but still
     * we need to do expireKey to remove key from db and rocksdb. */
    if (nswap == 0) expireKey(c, key, NULL);

    return nswap;
}

