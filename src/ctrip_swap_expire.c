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

/* Note that expire clients are used only if key is decided to be expired. */
int isExpireClientRequest(client *c) {
    return c->cmd && c->cmd->proc == expiredCommand;
}

/* Passive expire */
void expireClientKeyRequestFinished(client *c, swapCtx *ctx) {
    robj *key = ctx->key_request->key;
    if (ctx->errcode) clientSwapError(c,ctx->errcode);
    incrRefCount(key);
    c->keyrequests_count--;
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_EVICT);
    clientUnholdKey(c,key);
    clientReleaseRequestLocks(c,ctx);
    decrRefCount(key);
}

int submitExpireClientRequest(client *c, robj *key) {
    getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
    getKeyRequestsPrepareResult(&result,1);
    incrRefCount(key);
    getKeyRequestsAppendSubkeyResult(&result,REQUEST_LEVEL_KEY,key,0,NULL,
            c->cmd->intention,c->cmd->intention_flags,c->db->id);
    c->keyrequests_count++;
    submitClientKeyRequests(c,&result,expireClientKeyRequestFinished);
    releaseKeyRequests(&result);
    getKeyRequestsFreeResult(&result);
    return 1;
}

/* See expireIfNeeded for more details */
int scanMetaExpireIfNeeded(redisDb *db, scanMeta *meta) {
    client *c;
    robj *key;

    if (!timestampIsExpired(meta->expire)) return 0;
    if (server.masterhost != NULL) return 1;
    if (checkClientPauseTimeoutAndReturnIfPaused()) return 1;

    /* Delete the key */
    c = server.expire_clients[db->id];
    key = createStringObject(meta->key,sdslen(meta->key));
    submitExpireClientRequest(c,key);
    decrRefCount(key);

    return 1;
}

void expiredCommand(client *c) {
    addReply(c, shared.ok);
}

/* expire */
typedef void (*expiredHandler)(sds key, long long expire, redisDb *db, long long now);

expireCandidates *expireCandidatesCreate(size_t capacity) {
    expireCandidates *ecs;
    robj *zobj = createZsetObject();
    serverAssert(capacity > 0);
    ecs = zmalloc(sizeof(expireCandidates));
    ecs->zobj = zobj;
    ecs->capacity = capacity;
    return ecs;
}

void freeExpireCandidates(expireCandidates *ecs) {
    if (!ecs) return;
    if (ecs->zobj) {
        decrRefCount(ecs->zobj);
        ecs->zobj = NULL;
    }
    zfree(ecs);
}

size_t expireCandidatesSize(expireCandidates *ecs) {
    return zsetLength(ecs->zobj);
}

int expireCandidatesAdd(expireCandidates *ecs, long long expire, sds key) {
    robj *zobj = ecs->zobj;
    int out_flags;
    serverAssert(zsetLength(zobj) <= ecs->capacity);
    if (zsetLength(zobj) == ecs->capacity) {
        double max_expire;
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *zln = zsl->tail;
        max_expire = zln->score;
        if (expire < max_expire) {
            zsetDel(zobj,zln->ele);
            zsetAdd(zobj,expire,key,ZADD_IN_NONE,&out_flags,NULL);
        } else {
            out_flags = ZADD_OUT_NOP;
        }
    } else {
        zsetAdd(zobj,expire,key,ZADD_IN_NONE,&out_flags,NULL);
    }
    return out_flags & ZADD_OUT_ADDED;
}

void zslFreeNode(zskiplistNode *node);
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update);

/* Seek zslDeleteRangeByScore for details */
unsigned long zslDeleteRangeByScoreWithLimitHandler(zskiplist *zsl,
        zrangespec *range, dict *dict, unsigned long limit,
        expiredHandler handler, redisDb *db, long long now) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;

    x = zsl->header;
    for (int i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            !zslValueGteMin(x->level[i].forward->score, range))
                x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x && zslValueLteMax(x->score, range) && removed < limit) {
        zskiplistNode *next = x->level[0].forward;
        if (handler) handler(x->ele,x->score,db,now);
        zslDeleteNode(zsl,x,update);
        dictDelete(dict,x->ele);
        zslFreeNode(x); /* Here is where x->ele is actually released. */
        removed++;
        x = next;
    }

    return removed;
}

size_t expireCandidatesRemoveExpired(expireCandidates *ecs, long long now,
        size_t limit, expiredHandler handler, redisDb *db) {
    zset *zs = ecs->zobj->ptr;
    zskiplist *zsl = zs->zsl;
    dict *dict = zs->dict;
    zrangespec _expired_range, *expired_range = &_expired_range;
    expired_range->min = 0;
    expired_range->minex = 0;
    expired_range->max = now;
    expired_range->maxex = 0;
    return zslDeleteRangeByScoreWithLimitHandler(zsl,expired_range,dict,
            limit,handler,db,now);
}


/* Scan Expire */
scanExpire *scanExpireCreate() {
    scanExpire *scan_expire = zcalloc(sizeof(scanExpire));
    scan_expire->nextseek = NULL;
    scan_expire->limit = EXPIRESCAN_DEFAULT_LIMIT;
    scan_expire->candidates = expireCandidatesCreate(EXPIRESCAN_DEFAULT_CANDIDATES);
    return scan_expire;
}

void scanExpireFree(scanExpire *scan_expire) {
    if (!scan_expire) return;
    if (scan_expire->candidates) {
        freeExpireCandidates(scan_expire->candidates);
        scan_expire->candidates = NULL;
    }
    if (scan_expire->nextseek) {
        sdsfree(scan_expire->nextseek);
        scan_expire->nextseek = NULL;
    }
    zfree(scan_expire);
}

void scanExpireEmpty(scanExpire *scan_expire) {
    freeExpireCandidates(scan_expire->candidates);
    scan_expire->candidates = expireCandidatesCreate(EXPIRESCAN_DEFAULT_CANDIDATES);
}

void metaScan4ScanExpireRequestFinished(client *c, swapCtx *ctx) {
    UNUSED(ctx);
    robj *key = ctx->key_request->key;
    if (ctx->errcode) clientSwapError(c,ctx->errcode);
    incrRefCount(key);
    c->keyrequests_count--;
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_EVICT);
    clientUnholdKey(c,key);
    clientReleaseRequestLocks(c,ctx);
    decrRefCount(key);
}

/* NOTE: expire-scan is designed not to run in-parallel. */
void startMetaScan4ScanExpire(client *c) {
    getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
    const char *expire_scan_key = "____expire_scan____";
    robj *key = createStringObject(expire_scan_key,strlen(expire_scan_key));
    getKeyRequestsPrepareResult(&result,1);
    getKeyRequestsAppendSubkeyResult(&result,REQUEST_LEVEL_KEY,key,0,NULL,
            SWAP_IN,SWAP_METASCAN_EXPIRE,c->db->id);
    c->keyrequests_count++;
    submitClientKeyRequests(c,&result,metaScan4ScanExpireRequestFinished);
    releaseKeyRequests(&result);
    getKeyRequestsFreeResult(&result);
}

void scanExpireCycleTryExpire(sds key, long long expire, redisDb *db,
        long long now) {
    robj *keyobj = createStringObject(key,sdslen(key));
    client *c = server.expire_clients[db->id];
    serverAssert(expire <= now);
    submitExpireClientRequest(c, keyobj);
    decrRefCount(keyobj);
}

#define SCAN_EXPIRE_CYCLE_KEYS_PER_LOOP 20
#define SCAN_EXPIRE_CYCLE_SLOW_TIME_PERC 10
#define SCAN_EXPIRE_CYCLE_KEYS_BASE 16
#define SCAN_EXPIRE_CYCLE_KEYS_MAX 256

int scanExpireDbCycle(redisDb *db, int type, long long timelimit) {
    scanExpire *scan_expire = db->scan_expire;
    client *c = server.scan_expire_clients[db->id];
    int timelimit_exit = 0;
    static long long stat_last_update_time = 0, stat_scan_keys = 0,
                stat_scan_expired_keys = 0;
    long long start = ustime(), scan_time, expire_time, elapsed;

    /* No need to scan empty db. */
    if (db->cold_keys <= 0) goto update_stats;

    unsigned long 
        effort = server.active_expire_effort-1, /* Rescale from 0 to 9. */
        expire_keys_per_loop = SCAN_EXPIRE_CYCLE_KEYS_PER_LOOP +
                           SCAN_EXPIRE_CYCLE_KEYS_PER_LOOP/4*effort,
        scan_expire_base = SCAN_EXPIRE_CYCLE_KEYS_BASE +
            SCAN_EXPIRE_CYCLE_KEYS_BASE/4*effort;

    /* Scan limit is related to active-expire effort and stale percent:
     * - higher effort result in scanning more keys
     * - higher stale percent result in scanning more keys
     * Note that 16*10/4 < 256 */
    scan_expire->limit = scan_expire_base +
            (SCAN_EXPIRE_CYCLE_KEYS_MAX - scan_expire_base) *
            scan_expire->stale_percent;

    /* Merge swap_metas into candidates */
    if (type != ACTIVE_EXPIRE_CYCLE_FAST && c->swap_metas != NULL) {
        metaScanResult *metas = c->swap_metas;
        serverAssert(scan_expire->inprogress);

        for (int i = 0; i < metas->num; i++) {
            scanMeta *meta = metas->metas + i;
            if (meta->expire != -1) {
                expireCandidatesAdd(scan_expire->candidates,
                        meta->expire,meta->key);
            }
        }

        stat_scan_keys += metas->num;
        freeScanMetaResult(metas);
        c->swap_metas = NULL;
        scan_expire->inprogress = 0;
    }

    /* Start new scan expire */
    if (type != ACTIVE_EXPIRE_CYCLE_FAST && !scan_expire->inprogress) {
        serverAssert(c->swap_metas == NULL);
        scan_expire->inprogress = 1;
        startMetaScan4ScanExpire(c);
    }
    scan_time = ustime() - start;

    size_t total_removed = 0,
           candidates = expireCandidatesSize(scan_expire->candidates);
    int drained = 0, iteration = 0;
    do {
        size_t removed = expireCandidatesRemoveExpired(
                scan_expire->candidates,start/1000,expire_keys_per_loop,
                scanExpireCycleTryExpire,db);

        total_removed += removed;
        iteration++;

        /* Candidates are store in expire order, if less keys is removed than
         * requested, then other keys will not expire.*/
        drained = removed < expire_keys_per_loop;

        /* Check timelimit every 16 iteration */
        if ((iteration & 0xf) == 0) {
            elapsed = ustime()-start;
            if (elapsed > timelimit) {
                timelimit_exit = 1;
                server.stat_expired_time_cap_reached_count++;
            }
        }
    } while (!drained && !timelimit_exit);
    elapsed = ustime() - start;
    expire_time = elapsed - scan_time;

    double current_perc;
    if (candidates > 0) 
        current_perc = (double)total_removed/candidates;
    else
        current_perc = 0;

    scan_expire->stale_percent = (current_perc*0.05) +
                                 (scan_expire->stale_percent*0.95);

    scan_expire->stat_scan_time_used += scan_time;
    scan_expire->stat_expire_time_used += expire_time;
    stat_scan_expired_keys += total_removed;

update_stats:
    /* Update scan_per_sec/expired_per_sec every second. */
    if (start/1000000 > stat_last_update_time) {
        stat_last_update_time = start/1000000;
        scan_expire->stat_scan_per_sec = stat_scan_keys;
        scan_expire->stat_expired_per_sec = stat_scan_expired_keys;
        stat_scan_keys = 0;
        stat_scan_expired_keys = 0;
    }
    if (scan_expire->stat_scan_per_sec) {
        scan_expire->stat_estimated_cycle_seconds = 
            db->cold_keys/scan_expire->stat_scan_per_sec;
    }

    return timelimit_exit;
}

void scanexpireCommand(client *c) {
    addReply(c,shared.ok);
}

size_t objectComputeSize(robj *o, size_t sample_size);

/* TODO support multi-db */
sds genScanExpireInfoString(sds info) {
	redisDb *db0 = server.db + 0;
	scanExpire *scan_expire = db0->scan_expire;
    size_t used_memory = sizeof(scanExpire) +  sizeof(expireCandidates);
    if (scan_expire->candidates && scan_expire->candidates->zobj) {
        used_memory += objectComputeSize(scan_expire->candidates->zobj,8);
    }

	info = sdscatprintf(info,
			"scan_expire_candidates:%ld\r\n"
			"scan_expire_used_memory:%ld\r\n"
			"scan_expire_stale_perc:%.2f%%\r\n"
			"scan_expire_scan_limit:%d\r\n"
			"scan_expire_estimated_cycle_seconds:%lld\r\n"
			"scan_expire_scan_key_per_second:%ld\r\n"
			"scan_expire_expired_key_per_second:%ld\r\n"
			"scan_expire_scan_used_time:%lld\r\n"
			"scan_expire_expire_used_time:%lld\r\n",
            expireCandidatesSize(scan_expire->candidates),
            used_memory,
            scan_expire->stale_percent*100,
            scan_expire->limit,
            scan_expire->stat_estimated_cycle_seconds,
            scan_expire->stat_scan_per_sec,
            scan_expire->stat_expired_per_sec,
            scan_expire->stat_scan_time_used,
            scan_expire->stat_expire_time_used);
    return info;
}

/* Slave expire cycle */
typedef struct slaveExpiringKey {
    redisDb *db;
    robj *key;
} slaveExpiringKey;

int activeExpireCycleTryExpire(redisDb *db, dictEntry *de, long long now);

list *slave_expiring_keys = NULL;

/* object meta should be in memory now, note that ttl may be triggered
 * multiple times if key is cold. */ 
void slaveExpireClientKeyRequestFinished(client *c, swapCtx *ctx) {
    redisDb *db = c->db;
    int dbid = db->id;
    uint64_t dbids;
    robj *key = ctx->key_request->key;
    long long expire = getExpire(db,key);
    dictEntry *dbids_entry = dictFind(slaveKeysWithExpire,key->ptr);

    if (expire < mstime()) {
        /* no expired or expired */
        if (expire >= 0) {
            if (slave_expiring_keys == NULL)
                slave_expiring_keys = listCreate();
            slaveExpiringKey *sek = zmalloc(sizeof(slaveExpiringKey));
            incrRefCount(key);
            sek->key = key;
            sek->db = db;
            listAddNodeTail(slave_expiring_keys, sek);
        }

        if (dbids_entry) {
            dbids = dictGetUnsignedIntegerVal(dbids_entry);
            dbids &= ~(1ULL << dbid);
            if (dbids) {
                dictSetUnsignedIntegerVal(dbids_entry,dbids);
            } else {
                dictDelete(slaveKeysWithExpire,key->ptr);
            }
        }
    } else {
        /* have expire but not expired yet */
    }

    incrRefCount(key);
    if (ctx->errcode) clientSwapError(c,ctx->errcode);
    c->keyrequests_count--;
    serverAssert(c->client_hold_mode == CLIENT_HOLD_MODE_EVICT);
    clientUnholdKey(c,key);
    clientReleaseRequestLocks(c,ctx);
    decrRefCount(key);
}

int submitSlaveExpireClientRequest(client *c, robj *key) {
    getKeyRequestsResult result = GET_KEYREQUESTS_RESULT_INIT;
    getKeyRequestsPrepareResult(&result,1);
    incrRefCount(key);
    getKeyRequestsAppendSubkeyResult(&result,REQUEST_LEVEL_KEY,key,0,NULL,
            c->cmd->intention,c->cmd->intention_flags,c->db->id);
    c->keyrequests_count++;
    submitClientKeyRequests(c,&result,slaveExpireClientKeyRequestFinished);
    releaseKeyRequests(&result);
    getKeyRequestsFreeResult(&result);
    return 1;
}

/* In most case, slave is readonly and no active-expire needed, so we don't
 * start a metascan to trigger expire. we try to expire slave keys by
 * triggering SLAVEEXPIRE command, which triggers expire even if current
 * role is slave. */
void expireSlaveKeysSwapMode(void) {
    if (slave_expiring_keys && listLength(slave_expiring_keys)) {
        listNode *ln;
        while ((ln = listFirst(slave_expiring_keys))) {
            slaveExpiringKey *sek = listNodeValue(ln);
            client *c = server.expire_clients[sek->db->id];
            /* FIXME expire_client can used only when determined to expire key
             * but key expire can only be determined after lock get.
             * similar issue exists for master active expire */
            submitExpireClientRequest(c,sek->key);
            decrRefCount(sek->key);
            zfree(sek);
            listDelNode(slave_expiring_keys,ln);
        }
    }

    if (slaveKeysWithExpire == NULL ||
        dictSize(slaveKeysWithExpire) == 0) return;

    serverAssert(server.swap_mode != SWAP_MODE_MEMORY);

    int cycles = 0, noexpire = 0, slaveexpire = 0;
    mstime_t start = mstime();
    while(1) {
        dictEntry *de = dictGetRandomKey(slaveKeysWithExpire);
        sds keyname = dictGetKey(de);
        uint64_t dbids = dictGetUnsignedIntegerVal(de);
        uint64_t new_dbids = 0;

        /* Check the key against every database corresponding to the
         * bits set in the value bitmap. */
        int dbid = 0;
        while(dbids && dbid < server.dbnum) {
            if ((dbids & 1) != 0) {
                robj *key, *val;
                redisDb *db = server.db+dbid;

                key = createStringObject(keyname,sdslen(keyname));
                val = lookupKey(db,key,LOOKUP_NOTOUCH);
                if (val) {
                    /* This is a hot/warm key, expire like memory mode, see 
                     * expireSlaveKeys for more detail. */
                    dictEntry *expire = dictFind(db->expires,keyname);
                    int expired = 0;

                    if (expire &&
                        activeExpireCycleTryExpire(server.db+dbid,expire,start))
                    {
                        expired = 1;
                    }

                    /* If the key was not expired in this DB, we need to set the
                     * corresponding bit in the new bitmap we set as value.
                     * At the end of the loop if the bitmap is zero, it means we
                     * no longer need to keep track of this key. */
                    if (expire && !expired) {
                        noexpire++;
                        new_dbids |= (uint64_t)1 << dbid;
                    }
                } else {
                    /* This is a cold/notexist key, start TTL command to swap
                     * in meta and expire if needed when swap finished. */
                    client *c = server.ttl_clients[dbid];
                    submitSlaveExpireClientRequest(c,key);
                    slaveexpire++;
                    new_dbids |= (uint64_t)1 << dbid;
                }
                decrRefCount(key);
            }
            dbid++;
            dbids >>= 1;
        }

        /* Set the new bitmap as value of the key, in the dictionary
         * of keys with an expire set directly in the writable slave. Otherwise
         * if the bitmap is zero, we no longer need to keep track of it. */
        if (new_dbids)
            dictSetUnsignedIntegerVal(de,new_dbids);
        else
            dictDelete(slaveKeysWithExpire,keyname);

        /* Stop conditions: found 3 keys we can't expire in a row or
         * time limit was reached. */
        cycles++;
        if (noexpire > 3) break;
        if (slaveexpire > 16) break;
        if ((cycles % 64) == 0 && mstime()-start > 1) break;
        if (dictSize(slaveKeysWithExpire) == 0) break;
    }
}


#ifdef REDIS_TEST

void printExpired(sds key, long long expire, redisDb *db, long long now) {
    UNUSED(db),UNUSED(now),UNUSED(key),UNUSED(expire);
    // printf("expired: %s=>%lld\n", key, expire);
}

int swapExpireTest(int argc, char *argv[], int accurate) {
    int error = 0;
    UNUSED(argc), UNUSED(argv), UNUSED(accurate);

    server.hz = 10;

    TEST("expire - candidates") {
        int i, removed;
        expireCandidates *candidates = expireCandidatesCreate(6);
        test_assert(expireCandidatesSize(candidates) == 0);
        for (i = 10; i < 18; i++) {
            sds key = sdsfromlonglong(i);
            expireCandidatesAdd(candidates,i,key);
            sdsfree(key);
        }
        test_assert(expireCandidatesSize(candidates) == 6);
        removed = expireCandidatesRemoveExpired(candidates,9,8,printExpired,NULL);
        test_assert(removed == 0);
        removed = expireCandidatesRemoveExpired(candidates,11,8,printExpired,NULL);
        test_assert(removed == 2);
        removed = expireCandidatesRemoveExpired(candidates,18,2,printExpired,NULL);
        test_assert(removed == 2);
        removed = expireCandidatesRemoveExpired(candidates,11,2,printExpired,NULL);
        test_assert(removed == 0);
        removed = expireCandidatesRemoveExpired(candidates,18,2,printExpired,NULL);
        test_assert(removed == 2);
        test_assert(expireCandidatesSize(candidates) == 0);
        freeExpireCandidates(candidates);
    }

    return error;
}


#endif


