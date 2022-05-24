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
<<<<<<< HEAD

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


void debugSwapOutCommand(client *c) {
    int i, nevict = 0, evict_result;
    if (c->argc == 2) {
        int i, nevict = 0, evict_result;
        dictEntry* de;
        dictIterator* di = dictGetSafeIterator(c->db->dict);
        while((de = dictNext(di)) != NULL) {
            sds key = dictGetKey(de);
            evict_result = 0;
            robj* k = createRawStringObject(key, sdslen(key));
            nevict += dbEvict(c->db, k, &evict_result);
            serverLog(LL_NOTICE, "debug swapout all %s: %s.", key, evictResultToString(evict_result));
            decrRefCount(k);
        }
        dictReleaseIterator(di);
    } else {    
        for (i = 1; i < c->argc; i++) {
            evict_result = 0;
            nevict += dbEvict(c->db, c->argv[i], &evict_result);
            serverLog(LL_NOTICE, "debug swapout %s: %s.", (sds)c->argv[i]->ptr, evictResultToString(evict_result));
        }   
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
=======
    releaseKeyRequests(&result);
    getKeyRequestsFreeResult(&result);
    return result.num;
>>>>>>> refactor: add generic swapdata api.
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

