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

#define HC_HOLD_BITS        32
#define HC_HOLD_MASK        ((1L<<HC_HOLD_BITS)-1)
#define HC_INIT(hold, swap) ((swap << HC_HOLD_BITS) + hold)
#define HC_HOLD(hc, swap)   (hc + (swap << HC_HOLD_BITS) + 1)
#define HC_UNHOLD(hc)       (hc - 1)
#define HC_HOLD_COUNT(hc)   ((hc) & HC_HOLD_MASK)
#define HC_SWAP_COUNT(hc)   ((hc) >> HC_HOLD_BITS)

void clientHoldKey(client *c, robj *key, int64_t swap) {
    dictEntry *de;
    redisDb *db = c->db;
    int64_t hc;

    /* No need to hold key if it has already been holded */
    if (dictFind(c->hold_keys, key)) return;
    incrRefCount(key);
    dictAdd(c->hold_keys, key, (void*)HC_INIT(1, swap));

    /* Add key to server & client hold_keys */
    if ((de = dictFind(db->hold_keys, key))) {
        hc = dictGetSignedIntegerVal(de);
        dictSetSignedIntegerVal(de, HC_HOLD(hc, swap));
        serverLog(LL_DEBUG, "h %s (%ld,%ld)", (sds)key->ptr, HC_HOLD_COUNT(hc), HC_SWAP_COUNT(hc));
    } else {
        incrRefCount(key);
        dictAdd(db->hold_keys, key, (void*)HC_INIT(1, swap));
        serverLog(LL_DEBUG, "h %s (%ld,%ld)", (sds)key->ptr, (int64_t)1, swap);
    }
}

static void dbUnholdKey(redisDb *db, robj *key, int64_t hc) {
    dictDelete(db->hold_keys, key);

    /* Evict key as soon as command finishs if there are saving child,
     * so that keys won't be swapped in and out frequently and causing
     * copy on write madness. */
    if (HC_SWAP_COUNT(hc) > 0 && hasActiveChildProcess()) {
        tryEvictKeyAsapLater(db, key);
    }
}

void clientUnholdKey(client *c, robj *key) {
    dictEntry *de;
    int64_t hc;
    redisDb *db = c->db;

    if (dictDelete(c->hold_keys, key) == DICT_ERR) return;
    serverAssert(de = dictFind(db->hold_keys, key));
    hc = HC_UNHOLD(dictGetSignedIntegerVal(de));

    if (HC_HOLD_COUNT(hc) > 0) {
        dictSetSignedIntegerVal(de, hc);
    } else {
        dbUnholdKey(db, key, hc);
    }
    serverLog(LL_DEBUG, "u %s (%ld,%ld)", (sds)key->ptr, HC_HOLD_COUNT(hc),
            HC_SWAP_COUNT(hc));
}

void clientUnholdKeys(client *c) {
    dictIterator *di;
    dictEntry *cde, *dde;
    int64_t hc;
    redisDb *db = c->db;

    di = dictGetIterator(c->hold_keys);
    while ((cde = dictNext(di))) {
        serverAssert(dde = dictFind(db->hold_keys, dictGetKey(cde)));
        hc = HC_UNHOLD(dictGetSignedIntegerVal(dde));
        if (HC_HOLD_COUNT(hc) > 0) {
            dictSetSignedIntegerVal(dde, hc);
        } else {
            dbUnholdKey(db, dictGetKey(cde), hc);
        }
        serverLog(LL_DEBUG, "u. %s (%ld,%ld)",
                (sds)((robj*)dictGetKey(cde))->ptr,
                HC_HOLD_COUNT(hc), HC_SWAP_COUNT(hc));
    }
    dictReleaseIterator(di);

    dictEmpty(c->hold_keys, NULL);
}

int keyIsHolded(redisDb *db, robj *key) {
    dictEntry *de;

    if ((de = dictFind(db->hold_keys, key))) {
        serverAssert(dictGetSignedIntegerVal(de) > 0);
        return 1;
    } else {
        return 0;
    }
}

void evictClientKeyRequestFinished(client *c, swapCtx *ctx) {
    UNUSED(ctx);
    robj *key = ctx->key_request->key;
    incrRefCount(key);
    c->keyrequests_count--;
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_EVICT);
    clientUnholdKey(c,key);
    clientReleaseRequestLocks(c,ctx);
    decrRefCount(key);
}

int submitEvictClientRequest(client *c, robj *key) {
    getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
    getKeyRequestsPrepareResult(&result,1);
    incrRefCount(key);
    getKeyRequestsAppendResult(&result,REQUEST_LEVEL_KEY,key,0,NULL,
            c->cmd->intention,c->cmd->intention_flags,KEYREQUESTS_DBID);
    c->keyrequests_count++;
    submitClientKeyRequests(c,&result,evictClientKeyRequestFinished);
    releaseKeyRequests(&result);
    getKeyRequestsFreeResult(&result);
    return 1;
}

int tryEvictKey(redisDb *db, robj *key, int *evict_result) {
    int dirty, old_keyrequests_count;
    robj *o;
    client *evict_client = server.evict_clients[db->id];

    if (requestLockWouldBlock(server.swap_txid++, db, key)) {
        if (evict_result) *evict_result = EVICT_FAIL_SWAPPING;
        return 0;
    }

    if ((o = lookupKey(db, key, LOOKUP_NOTOUCH)) == NULL) {
        if (evict_result) *evict_result = EVICT_FAIL_ABSENT;
        return 0;
    }

    if (keyIsHolded(db, key)) {
        if (evict_result) *evict_result = EVICT_FAIL_HOLDED;
        return 0;
    }

    /* transformt hash big property */
    if (o->type == OBJ_HASH) {
        objectMeta *m = lookupMeta(db, key);
        hashTransformBig(o, m);
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
void evictCommand(client *c) {
    int i, nevict = 0, evict_result;

    for (i = 1; i < c->argc; i++) {
        evict_result = 0;
        nevict += tryEvictKey(c->db,c->argv[i],&evict_result);
        serverLog(LL_NOTICE, "evict %s: %s.", (sds)c->argv[i]->ptr,
                evictResultToString(evict_result));
    }

    addReplyLongLong(c, nevict);
}

void tryEvictKeyAsapLater(redisDb *db, robj *key) {
    incrRefCount(key);
    listAddNodeTail(db->evict_asap, key);
}

static int evictKeyAsap(redisDb *db) {
    int evicted = 0;
    listIter li;
    listNode *ln;

    listRewind(db->evict_asap, &li);
    while ((ln = listNext(&li))) {
        int evict_result;
        robj *key = listNodeValue(ln);

        tryEvictKey(db, key, &evict_result);

        if (evict_result == EVICT_FAIL_HOLDED ||
                evict_result == EVICT_FAIL_SWAPPING) {
            /* Try evict again if key is holded or swapping */
            listAddNodeHead(db->evict_asap, key);
        } else {
            decrRefCount(key);
            evicted++;
        }
        listDelNode(db->evict_asap, ln);
    }
    return evicted;
}

int evictAsap() {
    static mstime_t stat_mstime;
    static long stat_evict, stat_scan, stat_loop;
    int i, evicted = 0;

    for (i = 0; i < server.dbnum; i++) {
        redisDb *db = server.db+i;
        if (listLength(db->evict_asap)) {
            stat_scan += listLength(db->evict_asap);
            evicted += evictKeyAsap(db);
        }
    }

    stat_loop++;
    stat_evict += evicted;

    if (server.mstime - stat_mstime > 1000) {
        if (stat_scan > 0) {
            serverLog(LL_VERBOSE, "EvictAsap loop=%ld,scaned=%ld,swapped=%ld",
                    stat_loop, stat_scan, stat_evict);
        }
        stat_mstime = server.mstime;
        stat_loop = 0, stat_evict = 0, stat_scan = 0;
    }

    return evicted;
}

void debugEvictKeys() {
    int i = 0, j, debug_evict_keys = server.debug_evict_keys;
    if (debug_evict_keys < 0) debug_evict_keys = INT_MAX;
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db + j;
        dictEntry *de;
        dictIterator *di = dictGetSafeIterator(db->dict);
        while ((de = dictNext(di)) && i++ < debug_evict_keys) {
            sds key = dictGetKey(de);
            robj *keyobj = createStringObject(key,sdslen(key));
            tryEvictKey(db, keyobj, NULL);
            decrRefCount(keyobj);
        }
        dictReleaseIterator(di);
        if (i >= debug_evict_keys) return;
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
