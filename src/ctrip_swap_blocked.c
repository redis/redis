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

typedef struct swapUnblockedKeyChain  {
    robj* key; /* key root */
    redisDb* db;
    long long version;
    int keyrequests_count;
    dict* keys;
    long long swap_err_count;
} swapUnblockedKeyChain ;

#define waitIoDictType  objectKeyPointerValueDictType

void incrSwapUnBlockCtxVersion() {
    server.swap_unblock_ctx->version++;
}

swapUnblockedKeyChain* createSwapUnblockedKeyChain(redisDb* db, robj* key) {
    swapUnblockedKeyChain* chain = zmalloc(sizeof(swapUnblockedKeyChain));
    chain->version = server.swap_unblock_ctx->version;
    incrRefCount(key);
    chain->key = key;
    chain->db = db;
    chain->keyrequests_count = 0;
    chain->swap_err_count = 0;
    chain->keys = dictCreate(&waitIoDictType, NULL);
    return chain;
}

void releaseSwapUnblockedKeyChain(void* val) {
    swapUnblockedKeyChain* chain = val;
    // freeClient(wait_io->c);
    if (chain->key) {
        decrRefCount(chain->key);
    }
    if (chain->keys) {
        dictRelease(chain->keys);
    }
    zfree(chain);
}

swapUnblockCtx* createSwapUnblockCtx() {
    swapUnblockCtx* swap_unblock_ctx = zmalloc(sizeof(swapUnblockCtx));
    swap_unblock_ctx->version = 0;
    swap_unblock_ctx->swap_total_count = 0;
    swap_unblock_ctx->swapping_count = 0;
    /* version change will retry swap*/
    swap_unblock_ctx->swap_retry_count = 0;
    swap_unblock_ctx->swap_err_count = 0;
    swap_unblock_ctx->unblock_clients = zmalloc(server.dbnum*sizeof(client*));
    for (int i = 0; i < server.dbnum; i++) {
        client *c = createClient(NULL);
        c->cmd = lookupCommandByCString("brpoplpush");
        c->db = server.db+i;
        swap_unblock_ctx->unblock_clients[i] = c;
    }
    return swap_unblock_ctx;
}

void releaseSwapUnblockCtx(swapUnblockCtx* swap_unblock_ctx) {
    for (int i = 0; i < server.dbnum; i++) {
        freeClient(swap_unblock_ctx->unblock_clients[i]);
    }
    zfree(swap_unblock_ctx->unblock_clients);
    zfree(swap_unblock_ctx);
}

void findSwapBlockedListKeyChain(redisDb* db, robj* key, dict* key_sets) {
    dictEntry *de = dictFind(db->blocking_keys,key);
    if(de) {
        list *clients = dictGetVal(de);
        int numclients = listLength(clients);
        while(numclients--) {
            listNode *clientnode = listFirst(clients);
            client *receiver = clientnode->value;

            if (receiver->btype != BLOCKED_LIST) {
                /* Put at the tail, so that at the next call
                 * we'll not run into it again. */
                listRotateHeadToTail(clients);
                continue;
            }
            robj *dstkey = receiver->bpop.target;
            if (dstkey == NULL || dictAdd(key_sets, dstkey, NULL) != C_OK) continue;
            incrRefCount(dstkey);
            findSwapBlockedListKeyChain(db, dstkey, key_sets);
        }
    }

}

void handleBlockedOnListKey(redisDb* db, robj* key) {
    robj *o = lookupKeyWrite(db, key);
    if (o == NULL || o->type != OBJ_LIST) {
        return;
    }
    readyList rl = {
        .db = db,
        .key = key
    };
    serveClientsBlockedOnListKey(o, &rl);
}

void continueServeClientsBlockedOnListKeys(redisDb* db, robj* key) {
    list* _ready_keys = server.ready_keys;
    server.ready_keys = listCreate();
    handleBlockedOnListKey(db, key);

    handleClientsBlockedOnKeys();
    serverAssert(listLength(server.ready_keys) == 0);
    listRelease(server.ready_keys);
    server.ready_keys = _ready_keys;
}

void blockedOnListKeyClientKeyRequestFinished(client *c, swapCtx *ctx) {
    swapUnblockedKeyChain* chain = ctx->pd;
    dictIterator* di = NULL;
    dictEntry *de = NULL;
    if (ctx->errcode != 0) {
        chain->swap_err_count++;
    } else {
        keyRequestBeforeCall(c, ctx);
    }
    dictAdd(chain->keys, ctx->data->key, ctx->swap_lock);
    chain->keyrequests_count--;
    
    if (chain->keyrequests_count == 0) {
        if (chain->swap_err_count > 0) {
            server.swap_unblock_ctx->swap_err_count++;
            signalKeyAsReady(chain->db, chain->key, OBJ_LIST);
        } else if(chain->version != server.swap_unblock_ctx->version) {
            server.swap_unblock_ctx->swap_retry_count++;
            signalKeyAsReady(chain->db, chain->key, OBJ_LIST);
        } else {
            continueServeClientsBlockedOnListKeys(chain->db, chain->key);
        }
        di = dictGetIterator(chain->keys);
        while ((de = dictNext(di)) != NULL) {
            lockUnlock(dictGetVal(de));
        }
        dictReleaseIterator(di);
        releaseSwapUnblockedKeyChain(chain);
        server.swap_unblock_ctx->swapping_count--;
    }

}

/**
 * 
 * handle no need to swap data scene
 * 1. blpop command (without target)
 * 2. brpoplpush src target (when src == target) 
 *  
*/
int serveClientsBlockedOnListKeyWithoutTargetKey(robj *o, readyList *rl) {
    int exists_list_blocked_with_target_key = 0;
    dictEntry *de = dictFind(rl->db->blocking_keys, rl->key);
    objectMeta *om = NULL;
    if(de) {
        list *clients = dictGetVal(de);
        int numclients = listLength(clients);
        while(numclients--) {
            listNode *clientnode = listFirst(clients);
            client *receiver = clientnode->value;

            if (receiver->btype != BLOCKED_LIST) {
                /* Put at the tail, so that at the next call
                 * we'll not run into it again. */
                listRotateHeadToTail(clients);
                continue;
            }
            robj *dstkey = receiver->bpop.target;
            if (dstkey != NULL && sdscmp((sds)dstkey->ptr, (sds)rl->key->ptr) != 0) {
                exists_list_blocked_with_target_key = 1;
                goto end;
            }
            int wherefrom = receiver->bpop.listpos.wherefrom;
            int whereto = receiver->bpop.listpos.whereto;
            robj *value = ctripListTypePop(o, wherefrom, rl->db, rl->key);
            if (value) {
                /* Protect receiver->bpop.target, that will be
                 * freed by the next unblockClient()
                 * call. */
                if (dstkey) incrRefCount(dstkey);

                monotime replyTimer;
                elapsedStart(&replyTimer);
                if (serveClientBlockedOnList(receiver,
                    rl->key,dstkey,rl->db,value,
                    wherefrom, whereto) == C_ERR)
                {
                    /* If we failed serving the client we need
                     * to also undo the POP operation. */
                    ctripListTypePush(o,value,wherefrom, rl->db, rl->key);
                }
                updateStatsOnUnblock(receiver, 0, elapsedUs(replyTimer));
                unblockClient(receiver);

                if (dstkey) decrRefCount(dstkey);
                decrRefCount(value);
            } else {
                break;
            }
            
        }
    }
end:
    om = lookupMeta(rl->db, rl->key);
    if (ctripListTypeLength(o, om) == 0) {
        dbDelete(rl->db,rl->key);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",rl->key,rl->db->id);
        exists_list_blocked_with_target_key = 0;
    }
    return exists_list_blocked_with_target_key;
}

void submitSwapBlockedClientRequest(client* c, readyList *rl, dict* key_sets) {
    int dbid = rl->db->id;
    dictIterator* di = dictGetIterator(key_sets);
    dictEntry* de;
    getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
    getKeyRequestsPrepareResult(&result, dictSize(key_sets));
    while (NULL != (de = dictNext(di))) {
        robj* rkey = dictGetKey(de);
        incrRefCount(rkey);
        getKeyRequestsSwapBlockedLmove(dbid, SWAP_IN, 0, 
            rkey, &result, -1, 
            -1, 1, -1, -1);
    }
    dictReleaseIterator(di);
    swapUnblockedKeyChain* chain = createSwapUnblockedKeyChain(rl->db, rl->key);
    chain->keyrequests_count = result.num;
    server.swap_unblock_ctx->swap_total_count++;
    server.swap_unblock_ctx->swapping_count++;
    submitClientKeyRequests(c, &result, blockedOnListKeyClientKeyRequestFinished, chain);
    releaseKeyRequests(&result);
    getKeyRequestsFreeResult(&result);
}
/* Helper function for handleClientsBlockedOnKeys(). This function is called
 * when there may be clients blocked on a list key, and there may be new
 * data to fetch (the key is ready). */
void swapServeClientsBlockedOnListKey(robj *o, readyList *rl) {
    if (server.swap_mode == SWAP_MODE_MEMORY) {
        serveClientsBlockedOnListKey(o, rl);
        return;
    }
    /* We serve clients in the same order they blocked for
     * this key, from the first blocked to the last. */
    if (!serveClientsBlockedOnListKeyWithoutTargetKey(o, rl)) return;
    dict* key_sets = dictCreate(&waitIoDictType, NULL); 
    serverAssert(dictAdd(key_sets, rl->key, NULL) == C_OK);
    incrRefCount(rl->key);
    findSwapBlockedListKeyChain(rl->db, rl->key, key_sets);
    if (dictSize(key_sets) == 1) goto end;
    //create submit
    client* mock_client = server.swap_unblock_ctx->unblock_clients[rl->db->id];
    submitSwapBlockedClientRequest(mock_client, rl, key_sets);

end:
    dictRelease(key_sets);
}





