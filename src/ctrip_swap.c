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

#include "server.h"

#define S2R(s)     (s)
#define R2S(r)     (r)

/* -------------------------- swapping clients ---------------------------- */
swapClient *swapClientCreate(client *c, swap *s) {
    swapClient *sc = zmalloc(sizeof(swapClient));
    sc->c = c;
    if (s->key) incrRefCount(s->key);
    sc->s.key = s->key;
    if (s->subkey) incrRefCount(s->subkey);
    sc->s.subkey = s->subkey;
    if (s->val) incrRefCount(s->val);
    sc->s.val = s->val;
    return sc;
}

void swapClientRelease(swapClient *sc) {
    if (sc->s.key) decrRefCount(sc->s.key);
    if (sc->s.subkey) decrRefCount(sc->s.subkey);
    if (sc->s.val) decrRefCount(sc->s.val);
    zfree(sc);
}

static swappingClients *swappingClientsCreate(redisDb *db, robj *key, robj *subkey, swappingClients *parent) {
    swappingClients *scs = zmalloc(sizeof(struct swappingClients));
    scs->db = db;
    if (key) incrRefCount(key);
    scs->key = key;
    if (subkey) incrRefCount(subkey);
    scs->subkey = subkey;
    scs->swapclients = listCreate();
    scs->nchild = 0;
    scs->parent = parent;
    if (parent) parent->nchild++;
    return scs;
}

sds swappingClientsDump(swappingClients *scs) {
    listIter li;
    listNode *ln;
    sds result = sdsempty();
    char *actions[] = {"NOP", "GET", "PUT", "DEL"};

    result = sdscat(result, "[");
    listRewind(scs->swapclients,&li);
    while ((ln = listNext(&li))) {
        swapClient *sc = listNodeValue(ln);
        if (ln != listFirst(scs->swapclients)) result = sdscat(result,",");
        result = sdscat(result,"("); 
        if (sc->c->cmd) result = sdscat(result,actions[sc->c->cmd->swap_action]); 
        result = sdscat(result,":"); 
        if (sc->s.key) result = sdscatsds(result,sc->s.key->ptr); 
        result = sdscat(result,":"); 
        if (sc->c->cmd) result = sdscat(result,sc->c->cmd->name); 
        result = sdscat(result,")"); 
    }
    result = sdscat(result, "]");
    return result;
}

/* create like mkdir -p */
swappingClients *swappingClientsCreateP(client *c, robj *key, robj *subkey) {
    swappingClients *scs = server.scs;

    serverAssert(key == NULL || sdsEncodedObject(key));
    serverAssert(subkey == NULL || sdsEncodedObject(subkey));

    /* create scs from root down, starting from global level. */
    if (scs == NULL) {
        scs = swappingClientsCreate(NULL, NULL, NULL, NULL);
        server.scs = scs;
    }

    if (key == NULL) return scs;

    /* then key level */
    if (!lookupEvictSCS(c->db, key)) {
        scs = swappingClientsCreate(c->db, key, NULL, scs);
        setupSwappingClients(c, key, NULL, scs);
    }
    if (subkey == NULL) return scs;

    /* TODO support subkey level */
    return scs;
}

void swappingClientsRelease(swappingClients *scs) {
    if (!scs) return;
    serverAssert(!listLength(scs->swapclients));
    listRelease(scs->swapclients);
    if (scs->parent) scs->parent->nchild--;
    if (scs->key) decrRefCount(scs->key);
    if (scs->subkey) decrRefCount(scs->subkey);
    zfree(scs);
}

void swappingClientsPush(swappingClients *scs, swapClient *sc) {
    serverAssert(scs);
    serverAssert(scs->db == NULL || scs->db == sc->c->db);
    listAddNodeTail(scs->swapclients, sc);
}

swapClient *swappingClientsPop(swappingClients *scs) {
    serverAssert(scs);
    if (!listLength(scs->swapclients)) return NULL;
    listNode *ln = listFirst(scs->swapclients);
    swapClient *sc = listNodeValue(ln);
    listDelNode(scs->swapclients, ln);
    return sc;
}

swapClient *swappingClientsPeek(swappingClients *scs) {
    serverAssert(scs);
    if (!listLength(scs->swapclients)) return NULL;
    listNode *ln = listFirst(scs->swapclients);
    swapClient *sc = listNodeValue(ln);
    return sc;
}

/* return true if current or lower level scs not finished (a.k.a treeblocking).
 * - swap should not proceed if current or lower level scs exists. (e.g. flushdb
 *   shoul not proceed if SWAP GET key exits.)
 * - can't release scs if current or lower level scs exists.  */
static inline int swappingClientsTreeBlocking(swappingClients *scs) {
    if (scs && (listLength(scs->swapclients) || scs->nchild > 0)) {
        return 1;
    } else {
        return 0;
    }
}

/* ----------------------------- swaps result ----------------------------- */

/* Prepare the getSwapsResult struct to hold numswaps, either by using the
 * pre-allocated swaps or by allocating a new array on the heap.
 *
 * This function must be called at least once before starting to populate
 * the result, and can be called repeatedly to enlarge the result array.
 */
void getSwapsPrepareResult(getSwapsResult *result, int numswaps) {
	/* GETKEYS_RESULT_INIT initializes keys to NULL, point it to the pre-allocated stack
	 * buffer here. */
	if (!result->swaps) {
		serverAssert(!result->numswaps);
		result->swaps = result->swapsbuf;
	}

	/* Resize if necessary */
	if (numswaps > result->size) {
		if (result->swaps != result->swapsbuf) {
			/* We're not using a static buffer, just (re)alloc */
			result->swaps = zrealloc(result->swaps, numswaps * sizeof(swap));
		} else {
			/* We are using a static buffer, copy its contents */
			result->swaps = zmalloc(numswaps * sizeof(swap));
			if (result->numswaps)
				memcpy(result->swaps, result->swapsbuf, result->numswaps * sizeof(swap));
		}
		result->size = numswaps;
	}
}

void getSwapsAppendResult(getSwapsResult *result, robj *key, robj *subkey, robj *val) {
    /* Check overflow */    
    if (result->numswaps == result->size) {
        int newsize = result->size + (result->size > 8192 ? 8192 : result->size);
        getSwapsPrepareResult(result, newsize);
    }

    swap *s = &result->swaps[result->numswaps++];
    s->key = key;
    s->subkey = subkey;
    s->val = val;
}

void releaseSwaps(getSwapsResult *result) {
    int i;
    for (i = 0; i < result->numswaps; i++) {
        robj *key = result->swaps[i].key, *subkey = result->swaps[i].subkey;
        if (key) decrRefCount(key);
        if (subkey) decrRefCount(subkey);
    }
}

void getSwapsFreeResult(getSwapsResult *result) {
    if (result && result->swaps != result->swapsbuf) {
        zfree(result->swaps);
    }
}

/* ----------------------------- client swap ------------------------------ */
typedef void (*clientSwapFinishedCallback)(client *c, robj *key, void *pd);

typedef struct {
    client *c;
    robj *key;
    robj *subkey;
    int cb_type;
    dataSwapFinishedCallback data_cb;
    void *data_pd;
    swappingClients *scs;
    size_t swap_memory;
} rocksPrivData;

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

void dbEvictAsapLater(redisDb *db, robj *key) {
    incrRefCount(key);
    listAddNodeTail(db->evict_asap, key);
}

static int dbEvictAsap(redisDb *db) {
    int evicted = 0;
    listIter li;
    listNode *ln;

    listRewind(db->evict_asap, &li);
    while ((ln = listNext(&li))) {
        int evict_result;
        robj *key = listNodeValue(ln);

        dbEvict(db, key, &evict_result);

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
            evicted += dbEvictAsap(db);
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

inline static void dbUnholdKey(redisDb *db, robj *key, int64_t hc) {
    dictDelete(db->hold_keys, key);

    /* Evict key as soon as command finishs and there is a saving child,
     * so that keys won't be swapped in and out frequently and causing
     * copy on write madness. */
    if (HC_SWAP_COUNT(hc) > 0 && hasActiveChildProcess()) {
        dbEvictAsapLater(db, key);
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
    serverLog(LL_DEBUG, "u %s (%ld,%ld)", (sds)key->ptr, HC_HOLD_COUNT(hc), HC_SWAP_COUNT(hc));
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
        serverLog(LL_DEBUG, "u. %s (%ld,%ld)", (sds) ((robj*)dictGetKey(cde))->ptr, HC_HOLD_COUNT(hc), HC_SWAP_COUNT(hc));
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

void sharedSwapClientUnholdKey(client *c, robj *key, void *pd) {
    UNUSED(pd);
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_EVICT);
    clientUnholdKey(c, key);
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
    /* post command */
    commandProcessed(c);
    /* unhold keys for current command. */
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_CMD);
    clientUnholdKeys(c);
    /* pipelined command might already read into querybuf, if process not
     * restarted, pending commands would not be processed again. */
    processInputBuffer(c);
}

int clientSwapProceed(client *c, swap *s, swappingClients **scs);
void rocksSwapFinished(int action, sds rawkey, sds rawval, void *privdata) {
    rocksPrivData *rocks_pd = privdata;
    client *c = rocks_pd->c;
    swapClient *sc, *nsc;
    swappingClients *scs = rocks_pd->scs, *nscs;
    robj *key = rocks_pd->key, *subkey = rocks_pd->subkey;
    clientSwapFinishedCallback client_cb = (clientSwapFinishedCallback)c->client_swap_finished_cb;
    void *client_pd = c->client_swap_finished_pd;

    server.swap_memory -= rocks_pd->swap_memory;
    updateStatsSwapFinish(R2S(action), rawkey, rawval);

    /* Current swapping client should be the head of swapping_clents. */
    sc = swappingClientsPeek(scs);
    serverAssert(sc->c == rocks_pd->c);

    /* Call data cb to swap in/out keyspace before client cb. */
    if (rocks_pd->data_cb) {
        dataSwapFinished(c, action, rawkey, rawval, rocks_pd->cb_type,
                rocks_pd->data_cb, rocks_pd->data_pd);
    }

    /* Note that client_cb might spawned new swap(typically by expire), those
     * swaps can be appended to scs because PUT will not happend unless scs
     * is empty. */
    if (server.verbosity == LL_DEBUG) {
        sds dump = swappingClientsDump(scs);
        serverLog(LL_DEBUG, "- client(id=%ld,cmd=%s,key=%s): %s",
                c->id, c->cmd->name, (sds)key->ptr, dump);
        sdsfree(dump);
    }
    

    if (client_cb)  client_cb(c, key, client_pd);

    swappingClientsPop(scs);
    swapClientRelease(sc);

    /* Re-evaluate and start swap action(if needed) for subsequent clients in
     * current scs. if all clients in current level scs are processed, then
     * we try to process upper level scs. */ 
    while (scs) {
        /* Note that we can't Pop client here, because we need to keep client
         * in front if clientSwapProceed with swap. */
        while ((nsc = swappingClientsPeek(scs))) {
            if (clientSwapProceed(nsc->c, &nsc->s, &scs)) {
                break;
            } else {
                client *nc = nsc->c;
                client_cb = (clientSwapFinishedCallback)nc->client_swap_finished_cb;
                client_pd = nc->client_swap_finished_pd;

                if (server.verbosity == LL_DEBUG) {
                    sds dump = swappingClientsDump(scs);
                    serverLog(LL_DEBUG, "-.client(id=%ld,cmd=%s,key=%s): %s",
                            nc->id, nc->cmd->name, (sds)key->ptr, dump);
                    sdsfree(dump);
                }

                if (client_cb) client_cb(nc, nsc->s.key, client_pd);

                swappingClientsPop(scs);
                swapClientRelease(nsc);
            }
        }

        if (scs->key == NULL) {
            /* If current scs is the global scs:
             * - no need to proceed upper level scs (this is the top).
             * - must not released or reset global scs */
            break;
        } else if (!swappingClientsTreeBlocking(scs)) {
            nscs = scs->parent;
            setupSwappingClients(c, scs->key, scs->subkey, NULL);
            swappingClientsRelease(scs); /* Note nchild changed here */
            if (nscs->nchild > 0)  {
                /* Can't process parent scs if sibiling scs exists. */
                break;
            } else {
                scs = nscs;
            }
        } else {
            setupSwappingClients(c, scs->key, scs->subkey, scs);
            break;
        }
    }

    sdsfree(rawkey);
    sdsfree(rawval);

    if (key) decrRefCount(key);
    if (subkey) decrRefCount(subkey);
    zfree(rocks_pd);
}

/* Estimate memory used for one swap action, server will slow down event
 * processing if swap consumed too much memory(i.e. server is generating
 * io requests faster than rocksdb can handle). */
#define SWAP_MEM_ESTMIATED_ZMALLOC_OVERHEAD   512
#define SWAP_MEM_INFLIGHT_BASE (                                    \
        /* db.evict store scs */                                    \
        sizeof(moduleValue) + sizeof(robj) + sizeof(dictEntry) +    \
        sizeof(swapClient) + sizeof(swappingClients) +              \
        sizeof(rocksPrivData) +                                     \
        sizeof(RIO) +                                               \
        /* link in scs, pending_rios, processing_rios */            \
        (sizeof(list) + sizeof(listNode))*3 )
static inline size_t estimateSwapMemory(sds rawkey, sds rawval, rocksPrivData *pd) {
    size_t result = 0;
    if (rawkey) result += sdsalloc(rawkey);
    if (rawval) result += sdsalloc(rawval);
    if (pd->key) {
        result += sizeof(robj);
        result += sdsalloc(pd->key->ptr);
        result += keyComputeSize(pd->c->db, pd->key);
    }
    if (pd->subkey) {
        result += sizeof(robj);
        result += sdsalloc(pd->subkey->ptr);
    }
    return SWAP_MEM_INFLIGHT_BASE + SWAP_MEM_ESTMIATED_ZMALLOC_OVERHEAD + result;
}

/* Called when there are no preceding swapping clients: swap action will be
 * re-evaluated according to keyspace status to decide whether & which swap
 * action should be triggered. */
int clientSwapProceed(client *c, swap *s, swappingClients **pscs) {
    int action, cb_type;
    sds rawkey, rawval = NULL;
    dataSwapFinishedCallback data_cb;
    void *data_pd;

    if (swapAna(c, s->key, s->subkey, &action, &rawkey, &rawval,
                &cb_type, &data_cb, &data_pd) || action == SWAP_NOP) {
        /* TODO: Something went wrong in swap ana, flag client to abort
         * process current command and reply with SWAP_FAILED_xx:
         * c->swap_result = SWAP_FAILED_xx. */ 
        return 0;
    }

    /* Async swap is necessary if we reached here. 
     * scs NULL means that current swap is not blocked and should be blocked
     * on deepest level of scs */
    if (*pscs == NULL)  *pscs = swappingClientsCreateP(c, s->key, s->subkey);

    rocksPrivData *rocks_pd = zmalloc(sizeof(rocksPrivData));
    rocks_pd->c = c;
    if (s->key) incrRefCount(s->key);
    rocks_pd->key = s->key;
    if (s->subkey) incrRefCount(s->subkey);
    rocks_pd->subkey = s->subkey;
    rocks_pd->cb_type = cb_type;
    rocks_pd->data_cb = data_cb;
    rocks_pd->data_pd = data_pd;
    rocks_pd->scs = *pscs;

    rocks_pd->swap_memory = estimateSwapMemory(rawkey, rawval, rocks_pd);
    server.swap_memory += rocks_pd->swap_memory;
    updateStatsSwapStart(action, rawkey, rawval);
    rocksIOSubmitAsync(crc16(rawkey, sdslen(rawkey)), S2R(action), rawkey,
           rawval, rocksSwapFinished, rocks_pd);
    return 1;
}

/* NOTE: swaps is swap intentions analyzed according to command (without query
 * keyspace). whether to start swap action is determined later in swapAna. */
int clientSwapSwaps(client *c, getSwapsResult *result, clientSwapFinishedCallback cb, void *pd) {
    int nswaps = 0, i;

    c->client_swap_finished_cb = (voidfuncptr)cb;
    c->client_swap_finished_pd = pd;

    for (i = 0; i < result->numswaps; i++) {
        int oswaps = nswaps;
        swappingClients *scs;
        swap *s = &result->swaps[i];

        scs = lookupSwappingClients(c, s->key, s->subkey);

        /* defer command processsing if there are preceeding swap clients. */
        if (swappingClientsTreeBlocking(scs)) {
            swappingClientsPush(scs, swapClientCreate(c,s));
            nswaps++;
        } else if (clientSwapProceed(c, s, &scs)) {
            serverAssert(scs); /* Proceed would create scs if swap needed. */
            swappingClientsPush(scs, swapClientCreate(c, s));
            nswaps++; 
        } else {
            /* no need to swap */
        }

        /* Hold key if:
         * - this is a normal client and ANY key swap needed. (note that we hold
         *   whether swap needed or not for now, will unhold all if no swap needed).
         * - this is a shared swap client and CURRENT key swap needed
         * - this is a repl worker client (no matter swap or not, keys will unhold in processFinishedReplCommands)
         * - Dont' hold if there is no cb (otherwise key will not unhold) */
        if (cb && s->key && ((c->client_hold_mode == CLIENT_HOLD_MODE_CMD) ||
                    (c->client_hold_mode == CLIENT_HOLD_MODE_EVICT && nswaps > oswaps) ||
                    (c->client_hold_mode == CLIENT_HOLD_MODE_REPL))) {
            clientHoldKey(c, s->key, nswaps - oswaps);
        }

        char *sign = nswaps > oswaps ? "+" : "=";
        if (server.verbosity == LL_DEBUG) {
            sds dump = scs ? swappingClientsDump(scs) : sdsempty();
            serverLog(LL_DEBUG, "%s client(id=%ld,cmd=%s,key=%s): %s",
                    sign, c->id, c->cmd->name, s->key ? (sds)s->key->ptr:"", dump);
            sdsfree(dump);
        }
    }

    if (cb && !nswaps && c->client_hold_mode == CLIENT_HOLD_MODE_CMD)
        clientUnholdKeys(c);

    c->swapping_count = nswaps;

    return nswaps;
}

void clientSwapFinished(client *c, robj *key, void *pd) {
    UNUSED(pd);
    UNUSED(key);
    c->swapping_count--;
    if (c->swapping_count == 0) {
        if (!c->CLIENT_DEFERED_CLOSING) continueProcessCommand(c);
    }
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
int clientSwap(client *c) {
    int swap_count;
    getSwapsResult result = GETSWAPS_RESULT_INIT;
    getSwaps(c, &result);
    swap_count = clientSwapSwaps(c, &result, clientSwapFinished, NULL);
    releaseSwaps(&result);
    getSwapsFreeResult(&result);
    return swap_count;
}

/* ----------------------------- repl swap ------------------------------ */
int replClientDiscardDispatchedCommands(client *c) {
    int discarded = 0, scanned = 0;
    listIter li;
    listNode *ln;

    serverAssert(c);

    listRewind(server.repl_worker_clients_used,&li);
    while ((ln = listNext(&li))) {
        client *wc = listNodeValue(ln);
        if (wc->repl_client == c) {
            wc->CLIENT_REPL_CMD_DISCARDED = 1;
            discarded++;
            serverLog(LL_NOTICE, "discarded: cmd_reploff(%lld)", wc->cmd_reploff);
        }
        scanned++;
    }

    if (discarded) {
        serverLog(LL_NOTICE,
            "discard (%d/%d) dispatched but not executed commands for repl client(reploff:%lld, read_reploff:%lld)",
            discarded, scanned, c->reploff, c->read_reploff);
    }

    return discarded;
}

void replClientDiscardSwappingState(client *c) {
    listNode *ln;

    ln = listSearchKey(server.repl_swapping_clients, c);
    if (ln == NULL) return;

    listDelNode(server.repl_swapping_clients,ln);
    c->flags &= ~CLIENT_SWAPPING;
    serverLog(LL_NOTICE, "discarded: swapping repl client (reploff=%lld, read_reploff=%lld)", c->reploff, c->read_reploff);
}

/* Move command from repl client to repl worker client. */
static void replCommandDispatch(client *wc, client *c) {
    if (wc->argv) zfree(wc->argv);

    wc->argc = c->argc, c->argc = 0;
    wc->argv = c->argv, c->argv = NULL;
    wc->cmd = c->cmd;
    wc->lastcmd = c->lastcmd;
    wc->flags = c->flags;
    wc->cmd_reploff = c->cmd_reploff;
    wc->repl_client = c;

    /* Move repl client mstate to worker client if needed. */
    if (c->flags & CLIENT_MULTI) {
        c->flags &= ~CLIENT_MULTI;
        wc->mstate = c->mstate;
        initClientMultiState(c);
    }

    /* Swapping count is dispatched command count. Note that free repl
     * client would be defered untill swapping count drops to 0. */
    c->swapping_count++;

    serverLog(LL_WARNING, "[xxx] dispatch cmd(%s) with cmd_reploff(%lld)", wc->cmd->name, wc->cmd_reploff);
}

static void processFinishedReplCommands() {
    listNode *ln;
    client *wc, *c;
    struct redisCommand *backup_cmd;

    serverLog(LL_DEBUG, "> processFinishedReplCommands");

    while ((ln = listFirst(server.repl_worker_clients_used))) {
        wc = listNodeValue(ln);
        if (wc->CLIENT_REPL_SWAPPING) break;
        c = wc->repl_client;

        wc->flags &= ~CLIENT_SWAPPING;
        c->swapping_count--;
        listDelNode(server.repl_worker_clients_used, ln);
        listAddNodeTail(server.repl_worker_clients_free, wc);

        /* Discard dispatched but not executed commands like we never reveived, if
         * - repl client is closing: client close defered untill all swapping
         *   dispatched cmds finished, those cmds will be discarded.
         * - repl client is cached: client cached but read_reploff will shirnk
         *   back and dispatched cmd will be discared. */
        if (wc->CLIENT_REPL_CMD_DISCARDED) {
            commandProcessed(wc);
            serverAssert(wc->client_hold_mode == CLIENT_HOLD_MODE_REPL);
            clientUnholdKeys(wc);
            wc->CLIENT_REPL_CMD_DISCARDED = 0;
            continue;
        } else {
            serverAssert(c->flags&CLIENT_MASTER);
        }

        backup_cmd = c->cmd;
        c->cmd = wc->cmd;
        server.current_client = c;

        call(wc, CMD_CALL_FULL);

        /* post call */
        c->woff = server.master_repl_offset;
        if (listLength(server.ready_keys))
            handleClientsBlockedOnKeys();

        c->db = wc->db;
        c->cmd = backup_cmd;

        struct redisCommand *cur_cmd = wc->cmd;

        commandProcessed(wc);

        serverAssert(wc->client_hold_mode == CLIENT_HOLD_MODE_REPL);
        clientUnholdKeys(wc);

        long long prev_offset = c->reploff;
        /* update reploff */
        if (c->flags&CLIENT_MASTER) {
            /* transaction commands wont dispatch to worker client untill
             * exec (queued by repl client), so worker client wont have 
             * CLIENT_MULTI flag after call(). */
            serverAssert(!(wc->flags & CLIENT_MULTI));
            /* Update the applied replication offset of our master. */
            c->reploff = wc->cmd_reploff;
        }

		/* If the client is a master we need to compute the difference
		 * between the applied offset before and after processing the buffer,
		 * to understand how much of the replication stream was actually
		 * applied to the master state: this quantity, and its corresponding
		 * part of the replication stream, will be propagated to the
		 * sub-replicas and to the replication backlog. */
		if ((c->flags&CLIENT_MASTER)) {
			size_t applied = c->reploff - prev_offset;
            serverLog(LL_WARNING, "[xxx] cmd(%s) prev_offset(%lld) -> reploff(%lld)", cur_cmd->name, prev_offset, c->reploff);
			if (applied) {
				if(!server.repl_slave_repl_all){
					replicationFeedSlavesFromMasterStream(server.slaves,
							c->pending_querybuf, applied);
				}
				sdsrange(c->pending_querybuf,applied,-1);
			}
		}
    }
    serverLog(LL_DEBUG, "< processFinishedReplCommands");
}

void replWorkerClientSwapFinished(client *wc, robj *key, void *pd) {
    client *c;
    listNode *ln;
    list *repl_swapping_clients;

    UNUSED(pd);
    UNUSED(key);

    serverLog(LL_DEBUG, "> replWorkerClientSwapFinished client(id=%ld,cmd=%s,key=%s)",
        wc->id,wc->cmd->name,wc->argc <= 1 ? "": (sds)wc->argv[1]->ptr);

    /* Flag swap finished, note that command processing will be defered to
     * processFinishedReplCommands becasue there might be unfinished preceeding swap. */
    wc->swapping_count--;
    if (wc->swapping_count == 0) wc->CLIENT_REPL_SWAPPING = 0;

    processFinishedReplCommands();

    /* Dispatch repl command again for repl client blocked waiting free
     * worker repl client, because repl client might already read repl requests
     * into querybuf, read event will not trigger if we do not parse and
     * process again.  */
    if (!listFirst(server.repl_swapping_clients) ||
            !listFirst(server.repl_worker_clients_free)) {
        serverLog(LL_DEBUG, "< replWorkerClientSwapFinished");
        return;
    }

    repl_swapping_clients = server.repl_swapping_clients;
    server.repl_swapping_clients = listCreate();
    while ((ln = listFirst(repl_swapping_clients))) {
        int swap_result;

        c = listNodeValue(ln);
        /* Swapping repl clients are bound to:
         * - have pending parsed but not processed commands
         * - in server.repl_swapping_client list
         * - flag have CLIENT_SWAPPING */
        serverAssert(c->argc);
        serverAssert(c->flags & CLIENT_SWAPPING);

        /* Must make sure swapping clients satistity above constrains. also
         * note that repl client never call(only dispatch). */
        c->flags &= ~CLIENT_SWAPPING;
        swap_result = replClientSwap(c);
        /* replClientSwap return 1 on dispatch fail, -1 on dispatch success,
         * never return 0. */
        if (swap_result > 0) {
            c->flags |= CLIENT_SWAPPING;
        } else {
            commandProcessed(c);
        }

        /* TODO confirm whether server.current_client == NULL possible */
        processInputBuffer(c);

        listDelNode(repl_swapping_clients,ln);
    }
    listRelease(repl_swapping_clients);

    serverLog(LL_DEBUG, "< replWorkerClientSwapFinished");
}

int replWorkerClientSwap(client *wc) {
    int swap_count;
    getSwapsResult result = GETSWAPS_RESULT_INIT;
    getSwaps(wc, &result);
    swap_count = clientSwapSwaps(wc, &result, replWorkerClientSwapFinished, NULL);
    releaseSwaps(&result);
    getSwapsFreeResult(&result);
    return swap_count;
}

int replClientSwap(client *c) {
    client *wc;
    listNode *ln;

    c->cmd_reploff = c->read_reploff - sdslen(c->querybuf) + c->qb_pos;
    serverAssert(!(c->flags & CLIENT_SWAPPING));
    if (!(ln = listFirst(server.repl_worker_clients_free))) {
        /* return swapping if there are no worker to dispatch, so command
         * processing loop would break out.
         * Note that peer client might register no rocks callback but repl
         * stream read and parsed, we need to processInputBuffer again. */
        listAddNodeTail(server.repl_swapping_clients, c);
        /* Note repl client will be flagged CLIENT_SWAPPING when return. */
        return 1;
    }

    wc = listNodeValue(ln);
    serverAssert(wc && !wc->CLIENT_REPL_SWAPPING);

    /* Because c is a repl client, only normal multi {cmd} exec will be
     * received (multiple multi, exec without multi, ... will no happen) */
    if (c->cmd->proc == multiCommand) {
        serverAssert(!(c->flags & CLIENT_MULTI));
        c->flags |= CLIENT_MULTI;
    } else if (c->flags & CLIENT_MULTI &&
            c->cmd->proc != execCommand) {
        serverPanic("command should be already queued.");
    } else {
        /* either vanilla command or transaction are stored in client state,
         * client is ready to dispatch now. */
        replCommandDispatch(wc, c);

        /* swap data for replicated commands, note that command will be
         * processed later in processFinishedReplCommands untill all preceeding
         * commands finished. */
        wc->CLIENT_REPL_SWAPPING = replWorkerClientSwap(wc);

        listDelNode(server.repl_worker_clients_free, ln);
        listAddNodeTail(server.repl_worker_clients_used, wc);
    }

    /* process repl commands in received order (not swap finished order) so
     * that slave is consistent with master. */
    processFinishedReplCommands();

    /* return dispatched(-1) when repl dispatched command to workers, caller
     * should skip call and continue command processing loop. */
    return -1;
}

/* ----------------------------- expire ------------------------------ */
/* Assumming that key is expired and deleted from db, we still need to del
 * from rocksdb. */
int rocksDeleteNoReply(client *c, robj *key) {
    int swap_count;
    getSwapsResult result = GETSWAPS_RESULT_INIT;
    getExpireSwaps(c, key, &result);
    swap_count = clientSwapSwaps(c, &result, sharedSwapClientUnholdKey, NULL);
    releaseSwaps(&result);
    getSwapsFreeResult(&result);
    return swap_count;
}

int rocksDelete(redisDb *db, robj *key) {
    client *c = server.rksdel_clients[db->id];
    return rocksDeleteNoReply(c, key);
}

/* Must make sure expire key or key shell not evicted (propagate needed) */
void expireKey(client *c, robj *key, void *pd) {
    UNUSED(pd);
    redisDb *db = c->db;
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_EVICT);
    clientUnholdKey(c, key);
    rocksDelete(db, key);
	deleteExpiredKeyAndPropagate(db,key);
}

/* Cases when clientExpireNoReply is called:
 * - active-expire: DEL swap will be append as if key is expired by dummy client.
 * - xxCommand: DEL swap will be append to scs of the key, bacause that key is
 *   holded before xxCommand, so scs of key will not have any PUT. so async
 *   DEL swap have the same effect of sync DEL.
 * - continueProcessCommand: same as xxCommand.
 * TODO opt: keys never evicted to rocksdb need not to be deleted from rocksdb. */
int clientExpireNoReply(client *c, robj *key) {
    int swap_count;
    getSwapsResult result = GETSWAPS_RESULT_INIT;
    getExpireSwaps(c, key, &result);
    swap_count = clientSwapSwaps(c, &result, expireKey, NULL);
    releaseSwaps(&result);
    getSwapsFreeResult(&result);
    return swap_count;
}

/* How key is expired:
 * 1. SWAP GET if expiring key is EVICTED (clientExpireNoReply), note that
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
    if (!server.in_swap_cb) nswap = clientExpireNoReply(c, key);

    /* when expiring key is in db.dict, we don't need to swapin key, but still
     * we need to do expireKey to remove key from db and rocksdb. */
    if (nswap == 0) expireKey(c, key, NULL);

    return nswap;
}

/* `rksdel` `rksget` are fake commands used only to provide flags for swap_ana,
 * use `touch` command to expire key actively instead. */
void rksdelCommand(client *c) {
    addReply(c, shared.ok);
}

void rksgetCommand(client *c) {
    addReply(c, shared.ok);
}

/* ----------------------------- eviction ------------------------------ */
int clientEvictNoReply(client *c, robj *key) {
    int swap_count;
    getSwapsResult result = GETSWAPS_RESULT_INIT;
    getEvictionSwaps(c, key, &result);
    swap_count = clientSwapSwaps(c, &result, sharedSwapClientUnholdKey, NULL);
    releaseSwaps(&result);
    getSwapsFreeResult(&result);
    return swap_count;
}

int dbEvict(redisDb *db, robj *key, int *evict_result) {
    int nswap, dirty;
    robj *o, *e;
    client *c = server.evict_clients[db->id];

    if (server.scs && listLength(server.scs->swapclients)) {
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

    if ((e = lookupEvict(db, key))) {
        if (e->evicted) {
            if (evict_result) *evict_result = EVICT_FAIL_EVICTED;
        } else {
            if (evict_result) *evict_result = EVICT_FAIL_SWAPPING;
        }
        return 0;
    }

    /* Trigger evict only if key is PRESENT && !SWAPPING && !HOLDED */
    dirty = o->dirty;
    nswap = clientEvictNoReply(c, key);
    if (!nswap) {
        /* Note that all unsupported object is dirty. */
        if (dirty) {
            if (evict_result) *evict_result = EVICT_FAIL_UNSUPPORTED;
        } else {
            if (evict_result) *evict_result = EVICT_SUCC_FREED;
        }
    } else {
        if (evict_result) *evict_result = EVICT_SUCC_SWAPPED;
    }

    return nswap;
}

const char* evictResultToString(int evict_result) {
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
        nevict += dbEvict(c->db, c->argv[i], &evict_result);
        serverLog(LL_NOTICE, "evict %s: %s.", (sds)c->argv[i]->ptr, evictResultToString(evict_result));
    }

    addReplyLongLong(c, nevict);
}

/* ----------------------------- statistics ------------------------------ */
int swapsPendingOfType(int type) {
    long long pending;
    serverAssert(type < SWAP_TYPES);
    pending = server.swap_stats[type].started - server.swap_stats[type].finished;
    return pending > 0 ? (int)pending : 0;
}

void updateStatsSwapStart(int type, sds rawkey, sds rawval) {
    serverAssert(type < SWAP_TYPES);
    size_t rawkey_bytes = rawkey == NULL ? 0 : sdslen(rawkey);
    size_t rawval_bytes = rawval == NULL ? 0 : sdslen(rawval);
    server.swap_stats[type].started++;
    server.swap_stats[type].last_start_time = server.mstime;
    server.swap_stats[type].started_rawkey_bytes += rawkey_bytes;
    server.swap_stats[type].started_rawval_bytes += rawval_bytes;
}

void updateStatsSwapFinish(int type, sds rawkey, sds rawval) {
    serverAssert(type < SWAP_TYPES);
    size_t rawkey_bytes = rawkey == NULL ? 0 : sdslen(rawkey);
    size_t rawval_bytes = rawval == NULL ? 0 : sdslen(rawval);
    server.swap_stats[type].finished++;
    server.swap_stats[type].last_finish_time = server.mstime;
    server.swap_stats[type].finished_rawkey_bytes += rawkey_bytes;
    server.swap_stats[type].finished_rawval_bytes += rawval_bytes;
}


/* ----------------------------- ratelimit ------------------------------ */
/* sleep 100us~100ms if current swap memory is (slowdown, stop). */
#define SWAP_RATELIMIT_DELAY_SLOW 1
#define SWAP_RATELIMIT_DELAY_STOP 10

int swapRateLimitState() {
    if (server.swap_memory < server.swap_memory_slowdown) {
        return SWAP_RL_NO;
    } else if (server.swap_memory < server.swap_memory_stop) {
        return SWAP_RL_SLOW;
    } else {
        return SWAP_RL_STOP;
    }
    return SWAP_RL_NO;
}

int swapRateLimit(client *c) {
    float pct;
    int delay;

    switch(swapRateLimitState()) {
    case SWAP_RL_NO:
        delay = 0;
        break;
    case SWAP_RL_SLOW:
        pct = ((float)server.swap_memory - server.swap_memory_slowdown) / ((float)server.swap_memory_stop - server.swap_memory_slowdown);
        delay = (int)(SWAP_RATELIMIT_DELAY_SLOW + pct*(SWAP_RATELIMIT_DELAY_STOP - SWAP_RATELIMIT_DELAY_SLOW));
        break;
    case SWAP_RL_STOP:
        delay = SWAP_RATELIMIT_DELAY_STOP;
        break;
    default:
        delay = 0;
        break;
    }

    if (delay > 0) {
        if (c) c->swap_rl_until = server.mstime + delay;
        serverLog(LL_VERBOSE, "[ratelimit] client(%ld) swap_memory(%ld) delay(%d)ms",
                c ? (int64_t)c->id:-2, server.swap_memory, delay);
    } else {
        if (c) c->swap_rl_until = 0;
    }
    
    return delay;
}

int swapRateLimited(client *c) {
    return c->swap_rl_until >= server.mstime;
}

/*  WHY do we need both getswaps & getdataswaps?
 *  - getswaps return swap intentions analyzed from command without querying
 *  keyspace; while getdataswaps return swaps based on data type (e.g.
 *  return partial fields for hash eviction). so getswaps corresponds
 *  to redis command, while getdataswaps corresponds to data type.
 *  - merge getdataswaps into getswaps means that we need to define
 *  getswaps_proc for whole key commands(e.g. set/incr) and lookup keyspace
 *  inside getswap_proc to determin what swap should be returned.
 */

int getSwapsNone(struct redisCommand *cmd, robj **argv, int argc, getSwapsResult *result) {
    UNUSED(cmd);
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(result);
    return 0;
}

/* Used by flushdb/flushall to get global scs(similar to table lock). */
int getSwapsGlobal(struct redisCommand *cmd, robj **argv, int argc, getSwapsResult *result) {
    UNUSED(cmd);
    UNUSED(argc);
    UNUSED(argv);
    getSwapsAppendResult(result, NULL, NULL, NULL);
    return 0;
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
        swap_result = clientSwap(c);
    } else {
        /* repl client swap */
        swap_result = replClientSwap(c);
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

    server.scs = swappingClientsCreateP(NULL, NULL, NULL);

    server.repl_workers = 256;
    server.repl_swapping_clients = listCreate();
    server.repl_worker_clients_free = listCreate();
    server.repl_worker_clients_used = listCreate();
    for (i = 0; i < server.repl_workers; i++) {
        client *c = createClient(NULL);
        c->client_hold_mode = CLIENT_HOLD_MODE_REPL;
        listAddNodeTail(server.repl_worker_clients_free, c);
    }
}

/* ------------------------ parallel swap -------------------------------- */
parallelSwap *parallelSwapNew(int parallel) {
    int i;
    parallelSwap *ps = zmalloc(sizeof(parallelSwap));

    ps->parallel = parallel;
    ps->entries = listCreate();

    for (i = 0; i < parallel; i++) {
        int fds[2];
        swapEntry *e;

        if (pipe(fds)) {
            serverLog(LL_WARNING, "create future pipe failed: %s",
                    strerror(errno));
            goto err;
        }

        e = zmalloc(sizeof(swapEntry));
        e->inprogress = 0;
        e->pipe_read_fd = fds[0];
        e->pipe_write_fd = fds[1];
        e->pd = NULL;

        listAddNodeTail(ps->entries, e);
    }
    return ps;

err:
    listRelease(ps->entries);
    return NULL;
}

void parallelSwapFree(parallelSwap *ps) {
    listNode *ln;
    while ((ln = listFirst(ps->entries))) {
        swapEntry *e = listNodeValue(ln);
        close(e->pipe_read_fd);
        close(e->pipe_write_fd);
        zfree(e);
        listDelNode(ps->entries, ln);
    }
    listRelease(ps->entries);
    zfree(ps);
}

static int parallelSwapProcess(swapEntry *e) {
    if (e->inprogress) {
        char c;
        sds rawkey, rawval;
        if (read(e->pipe_read_fd, &c, 1) != 1) {
            serverLog(LL_WARNING, "wait swap entry failed: %s",
                    strerror(errno));
            return C_ERR;
        }
        e->inprogress = 0;
        RIOReap(e->r, &rawkey, &rawval);
        return e->cb(rawkey, rawval, e->pd);
    }
    return C_OK;
}

/* Submit one swap (task). swap will start and finish in submit order. */
int parallelSwapSubmit(parallelSwap *ps, sds rawkey, parallelSwapFinishedCb cb, void *pd) {
    listNode *ln;
    swapEntry *e;
    static int rocksdist = 0;
    /* wait and handle previous swap */
    if (!(ln = listFirst(ps->entries))) return C_ERR;
    e = listNodeValue(ln);
    if (parallelSwapProcess(e)) return C_ERR;
    listRotateHeadToTail(ps->entries);
    /* load new swap */
    e->cb = cb;
    e->pd = pd;
    e->inprogress = 1;
    e->r = rocksIOSubmitSync(rocksdist++, ROCKS_GET, rawkey, NULL,
            e->pipe_write_fd);
    return C_OK;
}

int parallelSwapDrain(parallelSwap *ps) {
    listIter li;
    listNode *ln;

    listRewind(ps->entries, &li);
    while((ln = listNext(&li))) {
        swapEntry *e = listNodeValue(ln);
        if ((parallelSwapProcess(e)))
            return C_ERR;
    }

    return C_OK;
}

