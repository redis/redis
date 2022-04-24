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

/* ------------------------------ rdb save -------------------------------- */
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

/* ------------------------------ rdb load -------------------------------- */

#define RDB_LOAD_VTYPE_VERBATIM 0  /* Raw buffer that could be directly write to rocksdb. */
#define RDB_LOAD_VTYPE_OBJECT 1  /* Robj that should be encoded before write to rocksdb. */

int rdbLoadIntegerVerbatim(rio *rdb, sds *verbatim, int enctype, long long *val) {
    unsigned char enc[4];

    if (enctype == RDB_ENC_INT8) {
        if (rioRead(rdb,enc,1) == 0) return -1;
        *val = (signed char)enc[0];
        *verbatim = sdscatlen(*verbatim,enc,1);
    } else if (enctype == RDB_ENC_INT16) {
        uint16_t v;
        if (rioRead(rdb,enc,2) == 0) return -1;
        v = enc[0]|(enc[1]<<8);
        *val = (int16_t)v;
        *verbatim = sdscatlen(*verbatim,enc,2);
    } else if (enctype == RDB_ENC_INT32) {
        uint32_t v;
        if (rioRead(rdb,enc,4) == 0) return -1;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        *val = (int32_t)v;
        *verbatim = sdscatlen(*verbatim,enc,4);
    } else {
        return -1; /* Never reached. */
    }
    return 0;
}

int rdbLoadLenVerbatim(rio *rdb, sds *verbatim, int *isencoded, unsigned long long *lenptr) {
    unsigned char buf[2];
    int type;

    if (isencoded) *isencoded = 0;
    if (rioRead(rdb,buf,1) == 0) return -1;
    type = (buf[0]&0xC0)>>6;
    if (type == RDB_ENCVAL) {
        /* Read a 6 bit encoding type. */
        if (isencoded) *isencoded = 1;
        *lenptr = buf[0]&0x3F;
        *verbatim = sdscatlen(*verbatim,buf,1);
    } else if (type == RDB_6BITLEN) {
        /* Read a 6 bit len. */
        *lenptr = buf[0]&0x3F;
        *verbatim = sdscatlen(*verbatim,buf,1);
    } else if (type == RDB_14BITLEN) {
        /* Read a 14 bit len. */
        if (rioRead(rdb,buf+1,1) == 0) return -1;
        *lenptr = ((buf[0]&0x3F)<<8)|buf[1];
        *verbatim = sdscatlen(*verbatim,buf,2);
    } else if (buf[0] == RDB_32BITLEN) {
        /* Read a 32 bit len. */
        uint32_t len;
        if (rioRead(rdb,&len,4) == 0) return -1;
        *lenptr = ntohl(len);
        *verbatim = sdscatlen(*verbatim,buf,1);
        *verbatim = sdscatlen(*verbatim,&len,sizeof(len));
    } else if (buf[0] == RDB_64BITLEN) {
        /* Read a 64 bit len. */
        uint64_t len;
        if (rioRead(rdb,&len,8) == 0) return -1;
        *lenptr = ntohu64(len);
        *verbatim = sdscatlen(*verbatim,buf,1);
        *verbatim = sdscatlen(*verbatim,&len,sizeof(len));
    } else {
        return -1; /* Never reached. */
    }

    return 0;
}

int rdbLoadRawVerbatim(rio *rdb, sds *verbatim, unsigned long long len) {
    sdsMakeRoomFor(*verbatim, len);
    rioRead(rdb, *verbatim+sdslen(*verbatim), len);
    sdsIncrLen(*verbatim, len);
    return 0;
}

int rdbLoadLzfStringVerbatim(rio *rdb, sds *verbatim) {
    unsigned long long len, clen;
    int isencode;

    if ((rdbLoadLenVerbatim(rdb,verbatim,&isencode,&clen))) return -1;
    if ((rdbLoadLenVerbatim(rdb,verbatim,&isencode,&len))) return -1;
    if ((rdbLoadRawVerbatim(rdb,verbatim,clen))) return -1;

    return 0;
}

int rdbLoadStringVerbatim(rio *rdb, sds *verbatim) {
    int isencoded, retval;
    unsigned long long len;
    long long val;

     if ((retval = rdbLoadLenVerbatim(rdb,verbatim,&isencoded,&len)))
         return retval;

     if (isencoded) {
        switch(len) {
        case RDB_ENC_INT8:
        case RDB_ENC_INT16:
        case RDB_ENC_INT32:
            return rdbLoadIntegerVerbatim(rdb,verbatim,len,&val);
        case RDB_ENC_LZF:
            return rdbLoadLzfStringVerbatim(rdb,verbatim);
        default:
            return -1;
        }
     } else {
         return rdbLoadRawVerbatim(rdb,verbatim,len);
     }

     return 0;
}

int rdbLoadHashVerbatim(rio *rdb, sds *verbatim) {
    int isencode;
    unsigned long long len;

    /* nfield */
    if (rdbLoadLenVerbatim(rdb,verbatim,&isencode,&len)) return -1;
    while (len--) {
        if (rdbLoadStringVerbatim(rdb,verbatim)) return -1; /* field */
        if (rdbLoadStringVerbatim(rdb,verbatim)) return -1; /* value */
    }

    return 0;
}

int evictRdbLoadObject(int rdbtype, rio *rdb, sds key, int *vtype, void **val) {
    int error = 0;
    sds verbatim = sdsempty();

    if (!rdbIsObjectType(rdbtype)) return -1;

    if (rdbtype == RDB_TYPE_STRING) {
        *vtype = RDB_LOAD_VTYPE_VERBATIM;
        error = rdbLoadStringVerbatim(rdb,&verbatim);
        *val = verbatim;
    } else if (rdbtype == RDB_TYPE_HASH) {
        *vtype = RDB_LOAD_VTYPE_VERBATIM;
        error = rdbLoadHashVerbatim(rdb,&verbatim);
        *val = verbatim;
    } else if (rdbtype == RDB_TYPE_HASH_ZIPMAP || rdbtype == RDB_TYPE_HASH_ZIPLIST) {
        *vtype = RDB_LOAD_VTYPE_VERBATIM;
        error = rdbLoadStringVerbatim(rdb,&verbatim);
        *val = verbatim;
    } else {
        *vtype = RDB_LOAD_VTYPE_OBJECT;
        *val = rdbLoadObject(rdbtype, rdb, key, &error);
    }

    return error;
}

char *getTypeName(int type) {
    char* typename;
    switch(type) {
    case OBJ_STRING: typename = "string"; break;
    case OBJ_LIST: typename = "list"; break;
    case OBJ_SET: typename = "set"; break;
    case OBJ_ZSET: typename = "zset"; break;
    case OBJ_HASH: typename = "hash"; break;
    case OBJ_STREAM: typename = "stream"; break;
    default: typename = "unknown"; break;
    }
    return typename;
}

sds encodeKeyVerbatim(sds key, int type) {
    sds rawkey;
    char *typename = getTypeName(type);

    rawkey = sdscat(sdsempty(), typename);
    rawkey = sdscatsds(rawkey, key);
    return rawkey;
}

int rdbLoadSwapVerbatimFinished(sds rawkey, sds rawval, void *pd) {
    sdsfree(rawkey);
    sdsfree(rawval);
    return 0;
}

/* TODO support module. */
int evictAddRDBLoad(int vtype, redisDb *db, int type, sds key, void *val) {
    robj *evict;

    if (vtype == RDB_LOAD_VTYPE_VERBATIM) {
        sds rawkey, rawval;
        rawval = val;
        rawkey = encodeKeyVerbatim(key, type);
        if (parallelSwapSubmit(server.rdb_load_ps, rawkey,
                rdbLoadSwapVerbatimFinished, rawval)) {
            sdsfree(rawkey);
            sdsfree(rawval);
            return -1;
        }
    } else if (vtype == RDB_LOAD_VTYPE_OBJECT) {
        int swap_count, i;
        client *c = server.evict_clients[db->id];
        getSwapsResult result = GETSWAPS_RESULT_INIT;
        getRDBLoadSwaps(c, key, &result);
        /* key is rawkey, val is rawval */
        for (i = 0; i < swap_count; i++) {
            sds rawkey, rawval;
            rawkey = 
            if (parallelSwapSubmit(server.rdb_load_ps, ,
                        rdbLoadSwapVerbatimFinished, rawval)) {
                return -1;
            }
        }
    } else {
        serverPanic("unexpceted rdb load vtype.");
        return 0;
    }

    evict = createEvictObject(type, NULL);
    evict->evicted = 1;

    if (dictAdd(db->evict, key, evict) != DICT_OK) {
        decrRefCount(evict);
        return -1;
    }

    if (server.cluster_enabled) slotToKeyAdd(key);
    return 1;
}

void evictStartLoading() {
    server.rdb_load_ps = parallelSwapNew(server.ps_parallism_rdb);
}

void evictStopLoading(int success) {
    UNUSED(success);
    rocksIODrain(server.rocks, -1);
    parallelSwapFree(server.rdb_load_ps);
    server.rdb_load_ps = NULL;
}
