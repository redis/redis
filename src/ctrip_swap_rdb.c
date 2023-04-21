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

void decodedResultInit(decodedResult *decoded) {
    memset(decoded,0,sizeof(decodedResult));
    decoded->cf = -1;
}

void decodedResultDeinit(decodedResult *decoded) {
    if (decoded->key) {
        sdsfree(decoded->key);
        decoded->key = NULL;
    }
    if (decoded->cf == META_CF) {
        decodedMeta *dm = (decodedMeta*)decoded;
        if (dm->extend) {
            sdsfree(dm->extend);
            dm->extend = NULL;
        }
    }
    if (decoded->cf == DATA_CF) {
        decodedData *dd = (decodedData*)decoded;
        if (dd->subkey) {
            sdsfree(dd->subkey);
            dd->subkey = NULL;
        }
        if (dd->rdbraw) {
            sdsfree(dd->rdbraw);
            dd->rdbraw = NULL;
        }
    }
    decodedResultInit(decoded);
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
int rdbKeySaveStart(struct rdbKeySaveData *save, rio *rdb) {
    if (save->type->save_start)
        return save->type->save_start(save,rdb);
    else
        return 0;
}

/* return -1 if save failed. */
int rdbKeySave(struct rdbKeySaveData *save, rio *rdb, decodedData *d) {
    uint64_t version;

    if (save->object_meta == NULL) {
        /* string version is always ZERO */
        version = SWAP_VERSION_ZERO;
    } else {
        version = save->object_meta->version;
    }
    /* skip obselete data key */
    if (version != d->version) return 0;

    if (save->type->save) {
        int ret = save->type->save(save,rdb,d);
        /* Delay return if required (for testing) */
        if (server.rdb_key_save_delay)
            debugDelay(server.rdb_key_save_delay);
        return ret;
    } else {
        return 0;
    }
}

/* return -1 if save_result is -1 or save end failed. */
int rdbKeySaveEnd(struct rdbKeySaveData *save, rio *rdb, int save_result) {
    if (save->type->save_end)
        return save->type->save_end(save,rdb,save_result);
    else
        return C_OK;
}

void rdbKeySaveDataDeinit(rdbKeySaveData *save) {
    if (save->key) {
        decrRefCount(save->key);
        save->key = NULL;
    }
    if (save->object_meta) {
        freeObjectMeta(save->object_meta);
        save->object_meta = NULL;
    }

    if (save->object_meta){
        freeObjectMeta(save->object_meta);
        save->object_meta = NULL;
    }

    if (save->type->save_deinit)
        save->type->save_deinit(save);
}

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

static void rdbKeySaveDataInitCommon(rdbKeySaveData *save,
        MOVE robj *key, robj *value, long long expire, objectMeta *om) {
    save->key = key;
    save->value = value;
    save->expire = expire;
    save->object_meta = dupObjectMeta(om);
    save->saved = 0;
    save->iter = NULL;
}

static int rdbKeySaveDataInitWarm(rdbKeySaveData *save, redisDb *db,
        MOVE robj *key, robj *value) {
    int retval = INIT_SAVE_OK;
    objectMeta *object_meta = lookupMeta(db,key);
    long long expire = getExpire(db,key);

    serverAssert(value && !keyIsHot(object_meta,value));

    rdbKeySaveDataInitCommon(save,key,value,expire,object_meta);

    switch (value->type) {
    case OBJ_STRING:
        wholeKeySaveInit(save);
        break;
    case OBJ_HASH:
        hashSaveInit(save,SWAP_VERSION_ZERO,NULL,0);
        break;
    case OBJ_SET:
        setSaveInit(save,SWAP_VERSION_ZERO,NULL,0);
        break;
    case OBJ_LIST:
        listSaveInit(save,SWAP_VERSION_ZERO,NULL,0);
        break;
    case OBJ_ZSET:
        zsetSaveInit(save,SWAP_VERSION_ZERO,NULL,0);
        break;
    default:
        retval = INIT_SAVE_ERR;
        break;
    }

    return retval;
}

static int rdbKeySaveDataInitCold(rdbKeySaveData *save, redisDb *db,
        MOVE robj *key, decodedMeta *dm) {
    int retval = INIT_SAVE_OK;
    UNUSED(db);

    rdbKeySaveDataInitCommon(save,key,NULL,dm->expire,NULL);

    switch (dm->object_type) {
    case OBJ_STRING:
        serverAssert(dm->extend == NULL);
        wholeKeySaveInit(save);
        break;
    case OBJ_HASH:
        serverAssert(dm->extend != NULL);
        retval = hashSaveInit(save,dm->version,dm->extend,sdslen(dm->extend));
        break;
    case OBJ_SET:
        serverAssert(dm->extend != NULL);
        retval = setSaveInit(save,dm->version,dm->extend,sdslen(dm->extend));
        break;
    case OBJ_LIST:
        serverAssert(dm->extend != NULL);
        retval = listSaveInit(save,dm->version,dm->extend,sdslen(dm->extend));
        break;
    case OBJ_ZSET:
        serverAssert(dm->extend != NULL);
        retval = zsetSaveInit(save,dm->version,dm->extend,sdslen(dm->extend));
        break;
    default:
        retval = INIT_SAVE_ERR;
        break;
    }

    return retval;
}

int rdbKeySaveDataInit(rdbKeySaveData *save, redisDb *db, decodedResult *dr) {
    robj *value, *key;
    objectMeta *object_meta;
    serverAssert(db->id == dr->dbid);

    if (dr->cf != META_CF) {
        /* skip orphan (sub)data keys: note that meta key is prefix of data
         * subkey, so rocksIter always start init with meta key, except for
         * orphan (sub)data key. */
        return INIT_SAVE_SKIP;
    }

    key = createStringObject(dr->key, sdslen(dr->key));
    value = lookupKey(db,key,LOOKUP_NOTOUCH);
    object_meta = lookupMeta(db,key);

    if (keyIsHot(object_meta,value)) { /* hot */
        decrRefCount(key);
        return INIT_SAVE_SKIP;
    } else if (value) { /* warm */
        return rdbKeySaveDataInitWarm(save,db,key,value);
    } else  { /* cold */
        serverAssert(dr->cf == META_CF);
        return rdbKeySaveDataInitCold(save,db,key,(decodedMeta*)dr);
    }
}

int rocksDecodeDataCF(sds rawkey, unsigned char rdbtype, sds rdbraw,
        decodedData *decoded) {
    int dbid, retval;
    const char *key, *subkey;
    size_t keylen, subkeylen;
    uint64_t version;

    retval = rocksDecodeDataKey(rawkey,sdslen(rawkey),&dbid,&key,&keylen,
            &version,&subkey,&subkeylen);
    if (retval) return retval;

    decoded->cf = DATA_CF;
    decoded->dbid = dbid;
    decoded->key = sdsnewlen(key,keylen);
    decoded->version = version;
    decoded->subkey = NULL == subkey ? NULL : sdsnewlen(subkey,subkeylen);
    decoded->rdbtype = rdbtype;
    decoded->rdbraw = rdbraw;

    sdsfree(rawkey);
    return 0;
}

int rocksDecodeMetaCF(sds rawkey, sds rawval, decodedMeta *decoded) {
    int dbid, retval, object_type;
    const char *key, *extend;
    size_t keylen, extlen;
    long long expire;
    uint64_t version;

    retval = rocksDecodeMetaKey(rawkey,sdslen(rawkey),&dbid,&key,&keylen);
    if (retval) return retval;

    retval = rocksDecodeMetaVal(rawval,sdslen(rawval),&object_type,&expire,
            &version,&extend,&extlen);
    if (retval) return retval;

    decoded->cf = META_CF;
    decoded->dbid = dbid;
    decoded->key = sdsnewlen(key,keylen);
    decoded->version = version;
    decoded->object_type = object_type;
    decoded->expire = expire;
    decoded->extend = extlen > 0 ? sdsnewlen(extend,extlen) : NULL;

    sdsfree(rawkey);
    sdsfree(rawval);
    return 0;
}

int rdbSaveRocksIterDecode(rocksIter *it, decodedResult *decoded,
        rdbSaveRocksStats *stats) {
    sds rawkey, rawval;
    int cf, retval;
    unsigned char rdbtype;

    rocksIterCfKeyTypeValue(it,&cf,&rawkey,&rdbtype,&rawval);

    /* rawkey,rawval moved from rocksIter to decoded if decode ok. */
    switch (cf) {
    case META_CF:
        retval = rocksDecodeMetaCF(rawkey,rawval,(decodedMeta*)decoded);
        break;
    case DATA_CF:
        retval = rocksDecodeDataCF(rawkey,rdbtype,rawval,(decodedData*)decoded);
        break;
    default:
        retval = C_ERR;
        break;
    }

    if (retval) {
        if (stats->iter_decode_err++ < 10) {
            sds repr = sdscatrepr(sdsempty(),rawkey,sdslen(rawkey));
            serverLog(LL_WARNING, "Decode rocks raw failed: %s", repr);
            sdsfree(repr);
        }
        sdsfree(rawkey);
        sdsfree(rawval);
    } else {
#ifdef SWAP_DEBUG
        if (decoded->cf == META_CF) {
            decodedMeta *meta = (decodedMeta*)decoded;
            serverLog(LL_NOTICE,
                    "[rdb] decoded meta: key=%s, type=%d, expire=%lld, extend=%s",
                    meta->key, meta->object_type, meta->expire, meta->extend);
        } else {
            decodedData *data = (decodedData*)decoded;
            sds repr = sdscatrepr(sdsempty(),data->rdbraw,sdslen(data->rdbraw));
            serverLog(LL_NOTICE,
                    "[rdb] decoded data: key=%s, subkey=%s, rdbtype=%d, rdbraw==%s",
                    data->key, data->subkey, data->rdbtype, repr);
            sdsfree(repr);
        }
#endif
        stats->iter_decode_ok++;
    }
    return retval;
}

/* Bighash/set/zset... fields are located adjacent, and will be iterated
 * next to each.
 * Note that only IO error aborts rdbSaveRocks, keys with decode/init_save
 * errors are skipped. */
int rdbSaveRocks(rio *rdb, int *error, redisDb *db, int rdbflags) {
    rocksIter *it = NULL;
    sds errstr = NULL;
    rdbSaveRocksStats _stats = {0}, *stats = &_stats;
    decodedResult  _cur, *cur = &_cur, _next, *next = &_next;
    decodedResultInit(cur);
    decodedResultInit(next);
    int iter_valid; /* true if current iter value is valid. */

    if (!(it = rocksCreateIter(server.rocks,db))) {
        serverLog(LL_WARNING, "Create rocks iterator failed.");
        return C_ERR;
    }

    iter_valid = rocksIterSeekToFirst(it);

    while (1) {
        int init_result, decode_result, save_result = 0;
        rdbKeySaveData _save, *save = &_save;
        serverAssert(next->key == NULL);

        if (cur->key == NULL) {
            if (!iter_valid) break;

            decode_result = rdbSaveRocksIterDecode(it,cur,stats);
            iter_valid = rocksIterNext(it);

            if (decode_result) continue;

            serverAssert(cur->key != NULL);
        }

        init_result = rdbKeySaveDataInit(save,db,cur);
        if (init_result == INIT_SAVE_SKIP) {
            stats->init_save_skip++;
            decodedResultDeinit(cur);
            // rdbKeySaveDataDeinit(save);
            continue;
        } else if (init_result == INIT_SAVE_ERR) {
            if (stats->init_save_err++ < 10) {
                sds repr = sdscatrepr(sdsempty(),cur->key,sdslen(cur->key));
                serverLog(LL_WARNING, "Init rdb save key failed: %s", repr);
                sdsfree(repr);
            }
            decodedResultDeinit(cur);
            // rdbKeySaveDataDeinit(save);
            continue;
        } else {
            stats->init_save_ok++;
        }

        if (rdbKeySaveStart(save,rdb) == -1) {
            errstr = sdscatfmt(sdsempty(),"Save key(%S) start failed: %s",
                    cur->key, strerror(errno));
            rdbKeySaveDataDeinit(save);
            decodedResultDeinit(cur);
            goto err; /* IO error, can't recover. */
        }

        /* There may be no rocks-meta for warm/hot hash(set/zset...), in
         * which case cur is decodedData. note that rdbKeySaveDataInit only
         * consumes decodedMeta. */
        if (cur->cf == DATA_CF) {
            if ((save_result = rdbKeySave(save,rdb,(decodedData*)cur)) == -1) {
                sds repr = sdscatrepr(sdsempty(),cur->key,sdslen(cur->key));
                errstr = sdscatfmt("Save key (%S) failed: %s", repr,
                        strerror(errno));
                sdsfree(repr);
                decodedResultDeinit(cur);
                goto saveend;
            }
        }

        while (1) {
            int key_switch;

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
                decodedResultDeinit(cur);
                break;
            }

            serverAssert(cur->key && next->key);
            key_switch = sdslen(cur->key) != sdslen(next->key) ||
                    sdscmp(cur->key,next->key);

            decodedResultDeinit(cur);
            _cur = _next;
            decodedResultInit(next);

            /* key switched, finish current & start another. */
            if (key_switch) break;

            /* key not switched, continue rdbSave. */
            if ((save_result = rdbKeySave(save,rdb,(decodedData*)cur)) == -1) {
                sds repr = sdscatrepr(sdsempty(),cur->key,sdslen(cur->key));
                errstr = sdscatfmt("Save key (%S) failed: %s", repr,
                        strerror(errno));
                sdsfree(repr);
                decodedResultDeinit(cur);
                break;
            }
        }

saveend:
        /* call save_end if save_start called, no matter error or not. */
        if (rdbKeySaveEnd(save,rdb,save_result) == -1) {
            if (errstr == NULL) {
                errstr = sdscatfmt(sdsempty(),"Save key end failed: %s",
                        strerror(errno));
            }
            rdbKeySaveDataDeinit(save);
            goto err;
        }

        rdbKeySaveDataDeinit(save);
        stats->save_ok++;
        rdbSaveProgress(rdb,rdbflags);
    };

    sds stats_dump = rdbSaveRocksStatsDump(stats);
    serverLog(LL_NOTICE,"Rdb save keys from rocksdb finished: %s",stats_dump);
    sdsfree(stats_dump);

    if (it) rocksReleaseIter(it);

    return C_OK;

err:
    if (error && *error == 0) *error = errno;
    serverLog(LL_WARNING, "Save rocks data to rdb failed: %s", errstr);
    if (it) rocksReleaseIter(it);
    if (errstr) sdsfree(errstr);
    return C_ERR;
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

int rdbLoadSwapAnaAction(swapData *data, int intention, void *datactx, int *action) {
    UNUSED(data), UNUSED(intention), UNUSED(datactx);
    *action = ROCKS_PUT;
    return 0;
}

int rdbLoadEncodeData(swapData *data_, int intention, void *datactx,
        int *numkeys, int **pcfs, sds **prawkeys, sds **prawvals) {
    rdbLoadSwapData *data = (rdbLoadSwapData*)data_;
    UNUSED(intention), UNUSED(datactx);
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
    .swapAnaAction = rdbLoadSwapAnaAction,
    .encodeKeys = NULL,
    .encodeData = rdbLoadEncodeData,
    .encodeRange = NULL,
    .decodeData = NULL,
    .swapIn =  NULL,
    .swapOut = NULL,
    .swapDel = NULL,
    .createOrMergeObject = NULL,
    .cleanObject = NULL,
    .beforeCall = NULL,
    .free = rdbLoadSwapDataFree,
};

/* rawkeys/rawvals moved, ptr array copied. */
swapData *createRdbLoadSwapData(size_t idx, int num, int *cfs, sds *rawkeys, sds *rawvals) {
    rdbLoadSwapData *data = zcalloc(sizeof(rdbLoadSwapData));
    data->d.type = &rdbLoadSwapDataType;
    data->idx = idx;
    data->num = num;
    data->cfs = zmalloc(sizeof(int)*data->num);
    memcpy(data->cfs,cfs,sizeof(int)*data->num);
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

void ctripRdbLoadWriteFinished(swapData *data, void *pd, int errcode) {
    UNUSED(pd), UNUSED(errcode);
#ifdef SWAP_DEBUG
    void *msgs = &((rdbLoadSwapData*)data)->msgs;
    DEBUG_MSGS_APPEND(msgs,"request-finish","ok");
#endif
    rdbLoadSwapDataFree(data,NULL);
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
    swapRequest *req = swapDataRequestNew(SWAP_OUT,0,NULL,data,NULL,NULL,
            ctripRdbLoadWriteFinished,NULL,msgs);
    submitSwapRequest(SWAP_MODE_PARALLEL_SYNC,req,-1);
}

void ctripRdbLoadCtxFeed(ctripRdbLoadCtx *ctx, int cf, MOVE sds rawkey, MOVE sds rawval) {
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

/* ------------------------------ rdb load start -------------------------------- */
void rdbLoadStartLenMeta(struct rdbKeyLoadData *load, rio *rdb, int *cf,
                    sds *rawkey, sds *rawval, int *error) {
    int isencode;
    unsigned long long len;
    sds header, extend = NULL;

    header = rdbVerbatimNew((unsigned char)load->rdbtype);

    /* nfield */
    if (rdbLoadLenVerbatim(rdb,&header,&isencode,&len)) {
        sdsfree(header);
        *error = RDB_LOAD_ERR_OTHER;
        return;
    }

    if (len == 0) {
        sdsfree(header);
        *error = RDB_LOAD_ERR_EMPTY_KEY;
        return;
    }

    load->total_fields = len;
    extend = rocksEncodeObjectMetaLen(len);

    *cf = META_CF;
    *rawkey = rocksEncodeMetaKey(load->db,load->key);
    *rawval = rocksEncodeMetaVal(load->object_type,load->expire,load->version,extend);
    *error = 0;

    sdsfree(extend);
    sdsfree(header);
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
int rdbKeyLoad(struct rdbKeyLoadData *load, rio *rdb, int *cf, sds *rawkey,
        sds *rawval, int *error) {
    if (load->type->load)
        return load->type->load(load,rdb,cf,rawkey,rawval,error);
    else
        return 0;
}

void rdbKeyLoadStart(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    if (load->type->load_start)
        load->type->load_start(load,rdb,cf,rawkey,rawval,error);
}

int rdbKeyLoadEnd(struct rdbKeyLoadData *load, rio *rdb) {
    if (load->type->load_end)
        return load->type->load_end(load,rdb);
    else
        return C_OK;
}

void rdbKeyLoadDataDeinit(struct rdbKeyLoadData *load) {
    if (load->type && load->type->load_deinit)
        load->type->load_deinit(load);
}

int rdbKeyLoadDataInit(rdbKeyLoadData *load, int rdbtype,
        redisDb *db, sds key, long long expire, long long now) {
    int retval = 0;
    if (!rdbIsObjectType(rdbtype)) return RDB_LOAD_ERR_OTHER;
    memset(load,0,sizeof(rdbKeyLoadData));

    load->rdbtype = rdbtype;
    load->db = db;
    load->key = key;
    load->expire = expire;
    load->now = now;
    load->value = NULL;
    load->iter = NULL;
    load->version = swapGetAndIncrVersion();

    switch(rdbtype) {
    case RDB_TYPE_STRING:
        wholeKeyLoadInit(load);
        break;
    case RDB_TYPE_HASH:
    case RDB_TYPE_HASH_ZIPMAP:
    case RDB_TYPE_HASH_ZIPLIST:
        hashLoadInit(load);
        break;
    case RDB_TYPE_SET:
    case RDB_TYPE_SET_INTSET:
        setLoadInit(load);
        break;
    case RDB_TYPE_LIST:
    case RDB_TYPE_LIST_ZIPLIST:
    case RDB_TYPE_LIST_QUICKLIST:
        listLoadInit(load);
        break;
    case RDB_TYPE_ZSET:
    case RDB_TYPE_ZSET_2:
    case RDB_TYPE_ZSET_ZIPLIST:
        zsetLoadInit(load);
        break;
    default:
        retval = SWAP_ERR_RDB_LOAD_UNSUPPORTED;
        break;
    }
    return retval;
}

int ctripRdbLoadObject(int rdbtype, rio *rdb, redisDb *db, sds key,
        long long expiretime, long long now, rdbKeyLoadData *load) {
    int error = 0, cont, cf;
    sds rawkey = NULL, rawval = NULL;

    if ((error = rdbKeyLoadDataInit(load,rdbtype,db,key,
                    expiretime,now))) {
        return error;
    }

    rdbKeyLoadStart(load,rdb,&cf,&rawkey,&rawval,&error);
    if (error) return error;
    if (rawkey) {
        ctripRdbLoadCtxFeed(server.rdb_load_ctx,cf,rawkey,rawval);
        load->nfeeds++;
    }

    do {
        cont = rdbKeyLoad(load,rdb,&cf,&rawkey,&rawval,&error);
        if (!error && rawkey) {
            serverAssert(rawval);
            ctripRdbLoadCtxFeed(server.rdb_load_ctx,cf,rawkey,rawval);
            load->nfeeds++;
        }
    } while (!error && cont);

    if (!error) error = rdbKeyLoadEnd(load,rdb);
    return error;
}

#ifdef REDIS_TEST

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

    int error = 0, retval;
    robj *myhash, *mystring;
    sds myhash_key, mystring_key;
    redisDb *db;
    long long NOW = 1661657836000;

    TEST("rdb: init") {
        initServerConfig();
        ACLInit();
        server.hz = 10;
        initTestRedisDb();
        db = server.db;

        myhash_key = sdsnew("myhash"), mystring_key = sdsnew("mystring");
        myhash = createHashObject();
        hashTypeSet(myhash,sdsnew("f1"),sdsnew("v1"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);
        hashTypeSet(myhash,sdsnew("f2"),sdsnew("v2"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);
        hashTypeSet(myhash,sdsnew("f3"),sdsnew("v3"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);
        hashTypeConvert(myhash, OBJ_ENCODING_HT);

        mystring = createStringObject("hello",5);
    }

    TEST("rdb: encode & decode ok in rocks format") {
        sds rawval = rocksEncodeValRdb(myhash);
        robj *decoded = rocksDecodeValRdb(rawval);
        test_assert(myhash->encoding != decoded->encoding);
        test_assert(hashTypeLength(myhash) == hashTypeLength(decoded));
    } 

    TEST("rdb: save&load string ok in rocks format") {
        rio sdsrdb;
        int rdbtype;
        sds rawval = rocksEncodeValRdb(mystring);
        rioInitWithBuffer(&sdsrdb, rawval);
        rdbKeyLoadData _keydata, *load = &_keydata;

        evictStartLoading();
        rdbtype = rdbLoadObjectType(&sdsrdb);
        retval = ctripRdbLoadObject(rdbtype,&sdsrdb,db,mystring_key,-1,NOW,load);
        test_assert(!retval);
        test_assert(load->rdbtype == RDB_TYPE_STRING);
        test_assert(load->object_type == OBJ_STRING);
        test_assert(load->nfeeds == 2);
    }

    TEST("rdb: save&load hash ok in rocks format") {
        rio sdsrdb;
        int rdbtype;
        sds rawval = rocksEncodeValRdb(myhash);
        rioInitWithBuffer(&sdsrdb, rawval);
        rdbKeyLoadData _keydata, *load = &_keydata;

        evictStartLoading();
        rdbtype = rdbLoadObjectType(&sdsrdb);
        retval = ctripRdbLoadObject(rdbtype,&sdsrdb,db,myhash_key,-1,NOW,load);
        test_assert(!retval);
        test_assert(load->rdbtype == RDB_TYPE_HASH);
        test_assert(load->object_type == OBJ_HASH);
        test_assert(load->nfeeds == 4);
    }

    return error;
}

#endif

