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

typedef struct {
    rio *rdb;               /* rdb stream */
    redisDb *db;            /* db */
    robj *key;              /* key object */
    compVal *cv;            /* comp val */
    long long expire;       /* expire time */
    int totalswap;          /* # of needed swaps */
    int numswapped;         /* # of finished swaps */
    complementObjectFunc comp;  /* function to complent val with rocksdb swap result */
    void *pd;               /* comp function private data  */
} keyValuePairCtx;

keyValuePairCtx *keyValuePairCtxNew(rio *rdb, redisDb *db, robj *key,
        compVal *cv, int totalswap, long long expire,
        complementObjectFunc comp, void *pd) {
    keyValuePairCtx *ctx = zmalloc(sizeof(keyValuePairCtx));
    ctx->rdb = rdb;
    ctx->db = db;
    ctx->key = key;
    ctx->cv = cv;
    ctx->expire = expire;
    ctx->totalswap = totalswap;
    ctx->numswapped = 0;
    ctx->comp = comp;
    ctx->pd = pd;
    return ctx;
}

void keyValuePairCtxFree(keyValuePairCtx *kvp) {
    decrRefCount(kvp->key);
    compValFree(kvp->cv);
    zfree(kvp);
}

int rdbSaveKeyCompValPair(rio *rdb, redisDb *db, robj *key, compVal *cv, long long expiretime);

int rdbSaveSwapFinished(sds rawkey, sds rawval, void *_kvp) {
    keyValuePairCtx *kvp = _kvp;
    compVal *cv = kvp->cv;

    if (complementCompVal(cv, rawkey, rawval, kvp->comp, kvp->pd)) {
        serverLog(LL_WARNING, "[rdbSaveEvicted] comp object failed:%.*s %.*s",
                (int)sdslen(rawkey), rawkey, (int)sdslen(rawval), rawval);
        goto err;
    }

    /* ugly hack: rawval is moved into cv if it's a RAW comp, null rawval here
     * to avoid rawval double free. */
    if (cv && cv->type == COMP_TYPE_RAW) {
        rawval = NULL;
    }

    kvp->numswapped++;
    if (kvp->numswapped == kvp->totalswap) {
        if (rdbSaveKeyCompValPair(kvp->rdb, kvp->db, kvp->key, kvp->cv,
                    kvp->expire) == -1) {
            keyValuePairCtxFree(kvp);
            goto err;
        }
        keyValuePairCtxFree(kvp);
    }

    sdsfree(rawkey);
    sdsfree(rawval);
    return C_OK;

err:
    sdsfree(rawkey);
    sdsfree(rawval);
    return C_ERR;
}

int rdbSaveEvictDb(rio *rdb, int *error, redisDb *db, int rdbflags) {
    dictIterator *di = NULL;
    dictEntry *de;
    dict *d = db->evict;
    long long key_count = 0;
    static long long info_updated_time;
    char *pname = (rdbflags & RDBFLAGS_AOF_PREAMBLE) ? "AOF rewrite" :  "RDB";
    size_t processed = 0;

    parallelSwap *ps = parallelSwapNew(server.ps_parallism_rdb);

    di = dictGetSafeIterator(d);
    while((de = dictNext(di)) != NULL) {
        int i;
        long long expire;
        keyValuePairCtx *kvp;
        sds keystr = dictGetKey(de);
        robj *key, *val = dictGetVal(de);
        compVal *cv;
        complementObjectFunc comp;
        void *pd;
        getSwapsResult result = GETSWAPS_RESULT_INIT;

        /* skip if it's just a swapping key(not evicted), already saved it. */
        if (!val->evicted) continue;

        key = createStringObject(keystr, sdslen(keystr));
        expire = getExpire(db,key);

        /* swap result will be merged into duplicated object, to avoid messing
         * up keyspace and causing drastic COW. */
        cv = getComplementSwaps(db, key, COMP_MODE_RDB, &result, &comp, &pd);

        /* no need to swap, normally it should not happend, we'are just being
         * protective here. */
        if (result.numswaps == 0) {
            decrRefCount(key);
            if (cv) compValFree(cv);
            rdbSaveKeyValuePair(rdb, key, val, expire);
            continue;
        }

        kvp = keyValuePairCtxNew(rdb, db, key, cv, result.numswaps, expire,
                comp, pd);

        for (i = 0; i < result.numswaps; i++) {
            swap *s = &result.swaps[i];
            if (parallelSwapSubmit(ps, (sds)s->key, rdbSaveSwapFinished, kvp)) {
                goto werr;
            }
        }

        /* Note that complement swaps are refs to rawkey (moved to rocks). */
        getSwapsFreeResult(&result);

        /* When this RDB is produced as part of an AOF rewrite, move
         * accumulated diff from parent to child while rewriting in
         * order to have a smaller final write. */
        if (rdbflags & RDBFLAGS_AOF_PREAMBLE &&
                rdb->processed_bytes > processed+AOF_READ_DIFF_INTERVAL_BYTES)
        {
            processed = rdb->processed_bytes;
            aofReadDiffFromParent();
        }

        /* Update child info every 1 second (approximately).
         * in order to avoid calling mstime() on each iteration, we will
         * check the diff every 1024 keys */
        if ((key_count++ & 1023) == 0) {
            long long now = mstime();
            if (now - info_updated_time >= 1000) {
                sendChildInfo(CHILD_INFO_TYPE_CURRENT_INFO, key_count, pname);
                info_updated_time = now;
            }
        }
    }
    dictReleaseIterator(di);

    if (parallelSwapDrain(ps)) goto werr;
    parallelSwapFree(ps);

    if (key_count) serverLog(LL_WARNING, "[RKS] DB-%d saved %lld evicted key to rdb.",
            db->id, key_count);

    return C_OK;

werr:
    if (error) *error = errno;
    if (di) dictReleaseIterator(di);
    parallelSwapFree(ps);
    return C_ERR;
}

