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

void evictClientKeyRequestFinished(client *c, swapCtx *ctx) {
    UNUSED(ctx);
    robj *key = ctx->key_request->key;
    if (ctx->errcode) clientSwapError(c,ctx->errcode);
    incrRefCount(key);
    c->keyrequests_count--;
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_EVICT);
    clientReleaseLocks(c,ctx);
    decrRefCount(key);

    server.swap_evict_inprogress_count--;
}

int submitEvictClientRequest(client *c, robj *key) {
    getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
    getKeyRequestsPrepareResult(&result,1);
    incrRefCount(key);
    getKeyRequestsAppendSubkeyResult(&result,REQUEST_LEVEL_KEY,key,0,NULL,
            c->cmd->intention,c->cmd->intention_flags,c->db->id);
    c->keyrequests_count++;
    submitDeferredClientKeyRequests(c,&result,evictClientKeyRequestFinished,NULL);
    releaseKeyRequests(&result);
    getKeyRequestsFreeResult(&result);

    server.swap_evict_inprogress_count++;
    return 1;
}

int tryEvictKey(redisDb *db, robj *key, int *evict_result) {
    int dirty, old_keyrequests_count;
    robj *o;
    client *evict_client = server.evict_clients[db->id];

    if (lockWouldBlock(server.swap_txid++, db, key)) {
        if (evict_result) *evict_result = EVICT_FAIL_SWAPPING;
        return 0;
    }

    if ((o = lookupKey(db, key, LOOKUP_NOTOUCH)) == NULL) {
        if (evict_result) *evict_result = EVICT_FAIL_ABSENT;
        return 0;
    }

    dirty = o->dirty;
    old_keyrequests_count = evict_client->keyrequests_count;
    submitEvictClientRequest(evict_client,key);
    /* Evit request finished right away, no swap triggered. */
    if (evict_client->keyrequests_count == old_keyrequests_count) {
        if (dirty) {
            if (evict_result) *evict_result = EVICT_FAIL_UNSUPPORTED;
        } else {
            if (evict_result) *evict_result = EVICT_SUCC_FREED;
        }
        return 0;
    } else {
        if (evict_result) *evict_result = EVICT_SUCC_SWAPPED;
        return 1;
    }
}

static const char* evictResultToString(int evict_result) {
    char *errstr;
    switch (evict_result) {
    case EVICT_SUCC_SWAPPED:
        errstr = "swapped";
        break;
    case EVICT_SUCC_FREED  :
        errstr = "freed";
        break;
    case EVICT_FAIL_ABSENT :
        errstr = "absent";
        break;
    case EVICT_FAIL_EVICTED:
        errstr = "evicted";
        break;
    case EVICT_FAIL_SWAPPING:
        errstr = "swapping";
        break;
    case EVICT_FAIL_HOLDED :
        errstr = "holded";
        break;
    case EVICT_FAIL_UNSUPPORTED:
        errstr = "unspported";
        break;
    default:
        errstr = "unexpected";
        break;
    }
    return errstr;
}

/* EVICT is a special command that getswaps returns nothing ('cause we don't
 * need to swap anything before command executes) but does swap out(PUT)
 * inside command func. Note that EVICT is the command of fake evict clients */
void swapEvictCommand(client *c) {
    int i, nevict = 0, evict_result;

    for (i = 1; i < c->argc; i++) {
        evict_result = 0;
        nevict += tryEvictKey(c->db,c->argv[i],&evict_result);
    }

    addReplyLongLong(c, nevict);
}

void tryEvictKeyAsapLater(redisDb *db, robj *key) {
    incrRefCount(key);
    listAddNodeTail(db->evict_asap, key);
}

void swapDebugEvictKeys() {
    int i = 0, j, swap_debug_evict_keys = server.swap_debug_evict_keys;
    if (swap_debug_evict_keys < 0) swap_debug_evict_keys = INT_MAX;
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db + j;
        dictEntry *de;
        dictIterator *di = dictGetSafeIterator(db->dict);
        while ((de = dictNext(di)) && i++ < swap_debug_evict_keys) {
            sds key = dictGetKey(de);
            robj *keyobj = createStringObject(key,sdslen(key));
            tryEvictKey(db, keyobj, NULL);
            decrRefCount(keyobj);
        }
        dictReleaseIterator(di);
        if (i >= swap_debug_evict_keys) return;
    }
}

void debugSwapOutCommand(client *c) {
    int i, nevict = 0, evict_result;
    if (c->argc == 2) {
        dictEntry* de;
        dictIterator* di = dictGetSafeIterator(c->db->dict);
        while((de = dictNext(di)) != NULL) {
            sds key = dictGetKey(de);
            evict_result = 0;
            robj* k = createRawStringObject(key, sdslen(key));
            nevict += tryEvictKey(c->db, k, &evict_result);
            serverLog(LL_NOTICE, "debug swapout all %s: %s.", key, evictResultToString(evict_result));
            decrRefCount(k);
        }
        dictReleaseIterator(di);
    } else {    
        for (i = 1; i < c->argc; i++) {
            evict_result = 0;
            nevict += tryEvictKey(c->db, c->argv[i], &evict_result);
            serverLog(LL_NOTICE, "debug swapout %s: %s.", (sds)c->argv[i]->ptr, evictResultToString(evict_result));
        }   
    }
    addReplyLongLong(c, nevict);
}

#ifdef REDIS_TEST

void initServerConfig(void);
int swapHoldTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    client *c;
    robj *key1, *key2;

    TEST("hold: init") {
        initServerConfig();
        ACLInit();
        server.hz = 10;
        c = createClient(NULL);
        initTestRedisDb();
        selectDb(c,0);

        key1 = createStringObject("key1",4);
        key2 = createStringObject("key2",4);
    }

    TEST("hold: deinit") {
        decrRefCount(key1);
        decrRefCount(key2);
    }

    return error;
}

#endif

