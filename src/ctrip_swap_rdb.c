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

int rocksDecodeDataCF(sds rawkey, unsigned char rdbtype, sds rdbraw,
        decodeResult *decoded) {
    UNUSED(rawkey),UNUSED(rdbtype),UNUSED(rdbraw),UNUSED(decoded);
    //TODO impl
    return 0;
}

int rocksDecodeMetaCF(sds rawkey, sds rdbraw, decodeResult *decoded) {
    UNUSED(rawkey),UNUSED(rdbraw),UNUSED(decoded);
    //TODO impl
    return 0;
}

int rocksDecodeRaw(sds rawkey, int cf, unsigned char rdbtype, sds rdbraw,
        decodeResult *decoded) {
    int retval;

    switch (cf) {
    case META_CF:
        retval = rocksDecodeMetaCF(rawkey,rdbraw,decoded);
        break;
    case DATA_CF:
        retval = rocksDecodeDataCF(rawkey,rdbtype,rdbraw,decoded);
        break;
    default:
        break;
    }
    return retval;
}

void decodeResultInit(decodeResult *decoded) {
    memset(decoded,0,sizeof(decodeResult));
}

void decodeResultDeinit(decodeResult *decoded) {
    if (decoded->key) sdsfree(decoded->key);
    if (decoded->subkey) sdsfree(decoded->subkey);
    //if (decoded->rdbraw) sdsfree(decoded->rdbraw);
    decodeResultInit(decoded);
}

/* ------------------------------ rdb save -------------------------------- */
/* Whole key encoding in rocksdb is the same as in rdb, so we skip encoding
 * and decoding to reduce cpu usage. */ 
int rdbSaveKeyHeader(rio *rdb, robj *key, robj *x, unsigned char rdbtype,
        long long expiretime) {
    int savelru = server.maxmemory_policy & MAXMEMORY_FLAG_LRU;
    int savelfu = server.maxmemory_policy & MAXMEMORY_FLAG_LFU;

    /* save expire/type/key */
    if (expiretime != -1) {
        if (rdbSaveType(rdb,RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;
    }    

    /* Save the LRU info. */
    if (savelru) {
        uint64_t idletime = estimateObjectIdleTime(x);
        idletime /= 1000; /* Using seconds is enough and requires less space.*/
        if (rdbSaveType(rdb,RDB_OPCODE_IDLE) == -1) return -1;
        if (rdbSaveLen(rdb,idletime) == -1) return -1;
    }

    /* Save the LFU info. */
    if (savelfu) {
        uint8_t buf[1];
        buf[0] = LFUDecrAndReturn(x);
        /* We can encode this in exactly two bytes: the opcode and an 8
         * bit counter, since the frequency is logarithmic with a 0-255 range.
         * Note that we do not store the halving time because to reset it
         * a single time when loading does not affect the frequency much. */
        if (rdbSaveType(rdb,RDB_OPCODE_FREQ) == -1) return -1;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
    }

    /* Save type, key, value */
    if (rdbSaveType(rdb,rdbtype) == -1) return -1;
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    return 1;
}

/* return -1 if save start failed. */
int rdbKeySaveStart(struct rdbKeyData *keydata, rio *rdb) {
    if (keydata->type->save_start)
        return keydata->type->save_start(keydata,rdb);
    else
        return 0;
}

/* return -1 if save failed. */
int rdbKeySave(struct rdbKeyData *keydata, rio *rdb, decodeResult *d) {
    if (keydata->type->save) {
        int ret = keydata->type->save(keydata,rdb,d);
        /* Delay return if required (for testing) */
        if (server.rdb_key_save_delay)
            debugDelay(server.rdb_key_save_delay);
        return ret;
    } else {
        return 0;
    }
}

/* return -1 if save_result is -1 or save end failed. */
int rdbKeySaveEnd(struct rdbKeyData *keydata, int save_result) {
    if (keydata->type->save_end)
        return keydata->type->save_end(keydata, save_result);
    else
        return C_OK;
}

void rdbKeyDataDeinitSave(rdbKeyData *keydata) {
    if (keydata->type->save_deinit) keydata->type->save_deinit(keydata);
}

typedef struct rdbSaveRocksStats {
    long long iter_decode_ok;
    long long iter_decode_err;
    long long init_save_ok;
    long long init_save_skip;
    long long init_save_err;
    long long save_ok;
} rdbSaveRocksStats;

sds rdbSaveRocksStatsDump(rdbSaveRocksStats *stats) {
    return sdscatprintf(sdsempty(),
            "decoded.ok=%lld,"
            "decoded.err=%lld,"
            "init.ok=%lld,"
            "init.skip=%lld,"
            "init.err=%lld,"
            "save.ok=%lld,",
            stats->iter_decode_ok,
            stats->iter_decode_err,
            stats->init_save_ok,
            stats->init_save_skip,
            stats->init_save_err,
            stats->save_ok);
}

#define INIT_SAVE_OK 0
#define INIT_SAVE_ERR -1
#define INIT_SAVE_SKIP -2

int rdbKeyDataInitSave(rdbKeyData *keydata, redisDb *db, decodeResult *decoded) {
    robj *value, key;
    long long expire;
    sds keystr = decoded->key;
    objectMeta *object_meta;
    UNUSED(object_meta);

    serverAssert(decoded->cfid == META_CF);

    initStaticStringObject(key,keystr);
    value = lookupKey(db,&key,LOOKUP_NOTOUCH);
    object_meta = lookupMeta(db,&key);
    expire = getExpire(db,&key);

    //TODO serverAssert(!value || !keyIsHot(object_type,object_meta));
    switch (decoded->cf.meta.object_type) {
    case OBJ_STRING:
        rdbKeyDataInitSaveWholeKey(keydata,value,expire);
        break;
    default:
        break;
    }

    return INIT_SAVE_OK;
}

int rdbSaveRocksIterDecode(rocksIter *it, decodeResult *decoded,
        rdbSaveRocksStats *stats) {
    sds rawkey, rawval;
    unsigned char rdbtype;
    int cf;

    /* rawkey,rawval moved from rocksIter to decoded. */
    rocksIterKeyTypeValue(it,&cf,&rawkey,&rdbtype,&rawval);
    if (rocksDecodeRaw(rawkey,cf,rdbtype,rawval,decoded)) {
        if (stats->iter_decode_err++ < 10) {
            sds repr = sdscatrepr(sdsempty(),rawkey,sdslen(rawkey));
            serverLog(LL_WARNING, "Decode rocks raw failed: %s", repr);
            sdsfree(repr);
        }
        return C_ERR;
    } else {
        stats->iter_decode_ok++;
        return C_OK;
    }
}

/* Bighash fields are located adjacent, which will be iterated next to each.
 * - Init savectx for key (bighash or wholekey)
 * - iterate fields untill finished (meta.len is num fields to iterate).
 * Note that only IO error aborts rdbSaveRocks, keys with decode/init_save
 * errors are skipped. */
int rdbSaveRocks(rio *rdb, int *error, redisDb *db, int rdbflags) {
    rocksIter *it = NULL;
    sds errstr = NULL;
    rdbSaveRocksStats _stats = {0}, *stats = &_stats;
    decodeResult  _cur, *cur = &_cur, _next, *next = &_next;
    decodeResultInit(cur);
    decodeResultInit(next);
    int iter_valid; /* true if current iter value is valid. */

    if (db->id > 0) return C_OK; /*TODO support multi-db */

    if (!(it = rocksCreateIter(server.rocks,db))) {
        serverLog(LL_WARNING, "Create rocks iterator failed.");
        return C_ERR;
    }

    iter_valid = rocksIterSeekToFirst(it);

    while (1) {
        int init_result, decode_result, save_result;
        rdbKeyData _keydata, *keydata = &_keydata;
        serverAssert(next->key == NULL);

        if (cur->key == NULL) {
            if (!iter_valid) break;

            decode_result = rdbSaveRocksIterDecode(it,cur,stats);
            iter_valid = rocksIterNext(it);

            if (decode_result) continue;

            serverAssert(cur->key != NULL);
        }

        init_result = rdbKeyDataInitSave(keydata,db,cur);
        if (init_result == INIT_SAVE_SKIP) {
            stats->init_save_skip++;
            decodeResultDeinit(cur);
            continue;
        } else if (init_result == INIT_SAVE_ERR) {
            if (stats->init_save_err++ < 10) {
                sds repr = sdscatrepr(sdsempty(),cur->key,sdslen(cur->key));
                serverLog(LL_WARNING, "Init rdb save key failed: %s", repr);
                sdsfree(repr);
            }
            decodeResultDeinit(cur);
            continue;
        } else {
            stats->init_save_ok++;
        }

        if (rdbKeySaveStart(keydata,rdb) == -1) {
            errstr = sdscatfmt(sdsempty(),"Save key(%S) start failed: %s",
                    cur->key, strerror(errno));
            rdbKeyDataDeinitSave(keydata);
            decodeResultDeinit(cur);
            goto err; /* IO error, can't recover. */
        }

        while (1) {
            int key_switch;

            if ((save_result = rdbKeySave(keydata,rdb,cur)) == -1) {
                sds repr = sdscatrepr(sdsempty(),cur->key,sdslen(cur->key));
                errstr = sdscatfmt("Save key (%S) failed: %s", repr,
                        strerror(errno));
                sdsfree(repr);
                decodeResultDeinit(cur);
                break;
            }

            /* Iterate untill next valid rawkey found or eof. */
            while (1) {
                if (!iter_valid) break; /* eof */

                decode_result = rdbSaveRocksIterDecode(it,next,stats);
                iter_valid = rocksIterNext(it);

                if (decode_result) {
                    continue;
                } else { /* next found */
                    serverAssert(next->key != NULL);
                    break;
                }
            }

            /* Can't find next valid rawkey, break to finish saving cur key.*/
            if (next->key == NULL) {
                decodeResultDeinit(cur);
                break;
            }

            serverAssert(cur->key && next->key);
            key_switch = sdslen(cur->key) != sdslen(next->key) ||
                    sdscmp(cur->key,next->key);

            decodeResultDeinit(cur);
            _cur = _next;
            decodeResultInit(next);

            if (key_switch) {
                /* key switched, finish current & start another. */
                break;
            } else {
                /* key not switched, continue rdbSave. */
                continue;
            }
        }

        /* call save_end if save_start called, no matter error or not. */
        if (rdbKeySaveEnd(keydata,save_result) == -1) {
            if (errstr == NULL) {
                errstr = sdscatfmt(sdsempty(),"Save key end failed: %s",
                        strerror(errno));
            }
            rdbKeyDataDeinitSave(keydata);
            goto err;
        }

        rdbKeyDataDeinitSave(keydata);
        stats->save_ok++;
        rdbSaveProgress(rdb,rdbflags);
    };

    sds stats_dump = rdbSaveRocksStatsDump(stats);
    serverLog(LL_NOTICE,"Rdb save keys from rocksdb finished: %s",stats_dump);
    sdsfree(stats_dump);

    // if (it) rocksReleaseIter(it);

    return C_OK;

err:
    if (error && *error == 0) *error = errno;
    serverLog(LL_WARNING, "Save rocks data to rdb failed: %s", errstr);
    if (it) rocksReleaseIter(it);
    if (errstr) sdsfree(errstr);
    return C_ERR;
}

void rdbKeyDataInitSaveKey(rdbKeyData *keydata, robj *value,
        long long expire) {
    keydata->savectx.type = 0;
    keydata->savectx.value = value;
    keydata->savectx.expire = expire;
}

/* ------------------------------ rdb load -------------------------------- */
typedef struct rdbLoadSwapData {
    swapData d;
    redisDb *db;
    size_t idx;
    int num;
    int *cfs;
    sds *rawkeys;
    sds *rawvals;
#ifdef SWAP_DEBUG
  struct swapDebugMsgs msgs;
#endif
} rdbLoadSwapData;

void rdbLoadSwapDataFree(swapData *data_, void *datactx) {
    rdbLoadSwapData *data = (rdbLoadSwapData*)data_;
    UNUSED(datactx);
    for (int i = 0; i < data->num; i++) {
        sdsfree(data->rawkeys[i]);
        sdsfree(data->rawvals[i]);
    }
    zfree(data->cfs);
    zfree(data->rawkeys);
    zfree(data->rawvals);
    zfree(data);
}

int rdbLoadSwapAna(swapData *data, struct keyRequest *req,
        int *intention, uint32_t *intention_flags, void *datactx) {
    UNUSED(data), UNUSED(req), UNUSED(intention_flags), UNUSED(datactx);
    *intention = SWAP_OUT;
    *intention_flags = 0;
    return 0;
}

int rdbLoadEncodeData(swapData *data_, int intention, void *datactx,
        int *action, int *numkeys, int **pcfs, sds **prawkeys, sds **prawvals) {
    rdbLoadSwapData *data = (rdbLoadSwapData*)data_;
    UNUSED(intention), UNUSED(datactx);
    *action = ROCKS_WRITE;
    *numkeys = data->num;
    *pcfs = data->cfs;
    *prawkeys = data->rawkeys;
    *prawvals = data->rawvals;
    data->num = 0;
    data->cfs = NULL;
    data->rawkeys = NULL;
    data->rawvals = NULL;
    return 0;
}

swapDataType rdbLoadSwapDataType = {
    .name = "rdbload",
    .swapAna = rdbLoadSwapAna,
    .encodeKeys = NULL,
    .encodeData = rdbLoadEncodeData,
    .decodeData = NULL,
    .swapIn =  NULL,
    .swapOut = NULL,
    .swapDel = NULL,
    .createOrMergeObject = NULL,
    .cleanObject = NULL,
    .free = rdbLoadSwapDataFree,
};

/* rawkeys/rawvals moved, ptr array copied. */
swapData *createRdbLoadSwapData(size_t idx, int num, int *cfs, sds *rawkeys, sds *rawvals) {
    rdbLoadSwapData *data = zcalloc(sizeof(rdbLoadSwapData));
    data->d.type = &rdbLoadSwapDataType;
    data->idx = idx;
    data->num = num;
    data->cfs = zmalloc(sizeof(int)*data->num);
    memcpy(data->cfs,cfs,sizeof(sds)*data->num);
    data->rawkeys = zmalloc(sizeof(sds)*data->num);
    memcpy(data->rawkeys,rawkeys,sizeof(sds)*data->num);
    data->rawvals = zmalloc(sizeof(sds)*data->num);
    memcpy(data->rawvals,rawvals,sizeof(sds)*data->num);
#ifdef SWAP_DEBUG
    char identity[MAX_MSG];
    snprintf(identity,MAX_MSG,"[rdbload:%ld:%d]",idx,num);
    swapDebugMsgsInit(&data->msgs,identity);
#endif
    return (swapData*)data;
}

ctripRdbLoadCtx *ctripRdbLoadCtxNew() {
    ctripRdbLoadCtx *ctx = zmalloc(sizeof(ctripRdbLoadCtx));
    ctx->errors = 0;
    ctx->idx = 0;
    ctx->batch.count = RDB_LOAD_BATCH_COUNT;
    ctx->batch.index = 0;
    ctx->batch.capacity = RDB_LOAD_BATCH_CAPACITY;
    ctx->batch.memory = 0;
    ctx->batch.cfs = zmalloc(sizeof(int)*ctx->batch.count);
    ctx->batch.rawkeys = zmalloc(sizeof(sds)*ctx->batch.count);
    ctx->batch.rawvals = zmalloc(sizeof(sds)*ctx->batch.count);
    return ctx;
}

int ctripRdbLoadWriteFinished(swapData *data, void *pd) {
    UNUSED(pd);
#ifdef SWAP_DEBUG
    void *msgs = &((rdbLoadSwapData*)data)->msgs;
    DEBUG_MSGS_APPEND(msgs,"request-finish","ok");
#endif
    rdbLoadSwapDataFree(data,NULL);
    return 0;
}

void ctripRdbLoadSendBatch(ctripRdbLoadCtx *ctx) {
    swapData *data;
    void *msgs = NULL;

    if (ctx->batch.index == 0)
        return;

    data = createRdbLoadSwapData(ctx->idx,ctx->batch.index,
            ctx->batch.cfs,ctx->batch.rawkeys,ctx->batch.rawvals);

#ifdef SWAP_DEBUG
    msgs = &((rdbLoadSwapData*)data)->msgs;
#endif

    DEBUG_MSGS_APPEND(msgs,"request-start","idx=%ld,num=%ld",ctx->idx,
            ctx->batch.index);

    /* Submit to rio thread. */
    submitSwapDataRequest(SWAP_MODE_PARALLEL_SYNC,SWAP_OUT,0,data,NULL,
            ctripRdbLoadWriteFinished,NULL,msgs);
}

void ctripRdbLoadCtxFeed(ctripRdbLoadCtx *ctx, int cf, sds rawkey, sds rawval) {
    ctx->batch.cfs[ctx->batch.index] = cf;
    ctx->batch.rawkeys[ctx->batch.index] = rawkey;
    ctx->batch.rawvals[ctx->batch.index] = rawval;
    ctx->batch.index++;
    ctx->batch.memory = ctx->batch.memory + sdslen(rawkey) + sdslen(rawval);

    if (ctx->batch.index >= ctx->batch.count ||
            ctx->batch.memory >= ctx->batch.capacity) {
        ctripRdbLoadSendBatch(ctx);
        /* Reset batch state */
        ctx->batch.index = 0;
        ctx->batch.memory = 0;
    }
}

void ctripRdbLoadCtxFree(ctripRdbLoadCtx *ctx) {
    zfree(ctx->batch.cfs);
    zfree(ctx->batch.rawkeys);
    zfree(ctx->batch.rawvals);
    zfree(ctx);
}

void evictStartLoading() {
    server.rdb_load_ctx = ctripRdbLoadCtxNew();
}

void evictStopLoading(int success) {
    UNUSED(success);
    /* send last buffered batch. */
    ctripRdbLoadSendBatch(server.rdb_load_ctx);
    asyncCompleteQueueDrain(-1); /* CONFIRM */
    parallelSyncDrain();
    ctripRdbLoadCtxFree(server.rdb_load_ctx);
    server.rdb_load_ctx = NULL;
}

/* ------------------------------ rdb verbatim -------------------------------- */
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
    size_t oldlen = sdslen(*verbatim);
    *verbatim = sdsMakeRoomForExact(*verbatim, len);
    rioRead(rdb, *verbatim+oldlen, len);
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

int rdbLoadHashFieldsVerbatim(rio *rdb, unsigned long long len, sds *verbatim) {
    while (len--) {
        if (rdbLoadStringVerbatim(rdb,verbatim)) return -1; /* field */
        if (rdbLoadStringVerbatim(rdb,verbatim)) return -1; /* value */
    }
    return 0;
}

/* return 1 if load not load finished (needs to continue load). */
int rdbKeyLoad(struct rdbKeyData *keydata, rio *rdb, int *cf, sds *rawkey,
        sds *rawval, int *error) {
    if (keydata->type->load)
        return keydata->type->load(keydata,rdb,cf,rawkey,rawval,error);
    else
        return 0;
}

int rdbKeyLoadStart(struct rdbKeyData *keydata, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    if (keydata->type->load_start)
        return keydata->type->load_start(keydata,rdb,cf,rawkey,rawval,error);
    else
        return 0;
}

int rdbKeyLoadEnd(struct rdbKeyData *keydata, rio *rdb) {
    if (keydata->type->load_end)
        return keydata->type->load_end(keydata,rdb);
    else
        return C_OK;
}

int rdbKeyLoadDbAdd(struct rdbKeyData *keydata, redisDb *db) {
    if (keydata->type->load_dbadd)
        return keydata->type->load_dbadd(keydata, db);
    else
        return C_OK;
}

void rdbKeyLoadExpired(struct rdbKeyData *keydata) {
    if (keydata->type->load_expired)
        keydata->type->load_expired(keydata);
}

void rdbKeyDataDeinitLoad(struct rdbKeyData *keydata) {
    if (keydata->type->load_deinit)
        keydata->type->load_deinit(keydata);
}

void rdbKeyDataInitLoadKey(rdbKeyData *keydata, int rdbtype, redisDb *db,
        sds key, long long expire, long long now) {
    keydata->loadctx.rdbtype = rdbtype;
    keydata->loadctx.key = key;
    keydata->loadctx.nfeeds = 0;
    keydata->loadctx.db = db;
    keydata->loadctx.expire = expire;
    keydata->loadctx.now = now;
}

int rdbKeyDataInitLoad(rdbKeyData *keydata, rio *rdb, int rdbtype,
        redisDb *db, sds key, long long expire, long long now) {
    UNUSED(rdb);//TODO remove?
    if (!rdbIsObjectType(rdbtype)) return RDB_LOAD_ERR_OTHER;
    switch(rdbtype) {
    case RDB_TYPE_STRING:
        rdbKeyDataInitLoadWholeKey(keydata,rdbtype,db,key,expire,now);
        break;
    default:
        serverPanic("not implemented");
        break;
    }
    return 0;
}

int ctripRdbLoadObject(int rdbtype, rio *rdb, redisDb *db, sds key,
        long long expiretime, long long now, rdbKeyData *keydata) {
    int error = 0, cont, cf;
    sds rawkey, rawval;

    if ((error = rdbKeyDataInitLoad(keydata,rdb,rdbtype,db,key,
                    expiretime,now))) {
        return error;
    }

    rdbKeyLoadStart(keydata,rdb,&cf,&rawkey,&rawval,&error);
    if (error) return error;
    if (rawkey) {
        ctripRdbLoadCtxFeed(server.rdb_load_ctx,cf,rawkey,rawval);
        keydata->loadctx.nfeeds++;
    }

    do {
        cont = rdbKeyLoad(keydata,rdb,&cf,&rawkey,&rawval,&error);
        if (!error && rawkey) {
            serverAssert(rawval);
            ctripRdbLoadCtxFeed(server.rdb_load_ctx,cf,rawkey,rawval);
            keydata->loadctx.nfeeds++;
        }
    } while (!error && cont);

    if (!error) error = rdbKeyLoadEnd(keydata,rdb);
    return error;
}

#ifdef REDIS_TEST0

sds dumpHashObject(robj *o) {
    hashTypeIterator *hi;

    sds repr = sdsempty();
    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != C_ERR) {
        sds subkey = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_KEY);
        sds subval = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_VALUE);
        repr = sdscatprintf(repr, "(%s=>%s),",subkey,subval);
        sdsfree(subkey);
        sdsfree(subval);
    }
    hashTypeReleaseIterator(hi);
    return repr;
}

void initServerConfig(void);
int swapRdbTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    robj *myhash;
    sds myhash_key;

    TEST("rdb: init") {
        initServerConfig();
        ACLInit();
        server.hz = 10;

        myhash_key = sdsnew("myhash");
        myhash = createHashObject();
        hashTypeSet(myhash,sdsnew("f1"),sdsnew("v1"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);
        hashTypeSet(myhash,sdsnew("f2"),sdsnew("v2"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);
        hashTypeSet(myhash,sdsnew("f3"),sdsnew("v3"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);
        hashTypeConvert(myhash, OBJ_ENCODING_HT);
    }

    TEST("rdb: encode & decode ok in rocks format") {
        sds rawval = rocksEncodeValRdb(myhash);
        robj *decoded = rocksDecodeValRdb(rawval);
        test_assert(myhash->encoding != decoded->encoding);
        test_assert(hashTypeLength(myhash) == hashTypeLength(decoded));
    } 

    TEST("rdb: memkey load") {
        rio sdsrdb;
        int err, rdbtype;
        rdbKeyData _keydata, *keydata = &_keydata;
        sds rawkey, rawval;
        robj *loaded;
        rawval = rocksEncodeValRdb(myhash);
        rioInitWithBuffer(&sdsrdb, rawval);
        rdbtype = rdbLoadObjectType(&sdsrdb);
        rdbKeyDataInitLoadMemkey(keydata,rdbtype,myhash_key);
        memkeyRdbLoad(keydata,&sdsrdb,&rawkey,&rawval,&err);
        loaded = keydata->loadctx.memkey.value;
        test_assert(loaded != NULL && loaded->type == OBJ_HASH);
        test_assert(hashTypeLength(loaded) == 3);
        memkeyRdbLoadExpired(keydata);
        test_assert(keydata->loadctx.memkey.value == NULL);
    }

    TEST("rdb: save&load object ok in rocks format") {
        rio sdsrdb;
        int rdbtype;
        robj *evict;
        sds rawval = rocksEncodeValRdb(myhash);
        rioInitWithBuffer(&sdsrdb, rawval);
        rdbKeyData _keydata, *keydata = &_keydata;

        evictStartLoading();
        rdbtype = rdbLoadObjectType(&sdsrdb);
        ctripRdbLoadObject(rdbtype,&sdsrdb,myhash_key,keydata);
        test_assert(keydata->loadctx.type == RDB_KEY_TYPE_WHOLEKEY);
        evict = keydata->loadctx.wholekey.evict;
        test_assert(evict && evict->type == OBJ_HASH);
        test_assert(keydata->loadctx.nfeeds == 1);
    }

    return error;
}

#endif

