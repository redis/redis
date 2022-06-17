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

/* Load directly into db.evict for objects that supports swap. */
int dbAddEvictRDBLoad(redisDb* db, sds key, robj* evict) {
    /* Add to db.evict. Note that key is moved to db.evict. */ 
    int retval = dictAdd(db->evict,key,evict);
    if (retval != DICT_OK) return 0;
    if (server.cluster_enabled) slotToKeyAdd(key);
    return 1;
}

int rocksDecodeRaw(sds rawkey, unsigned char rdbtype, sds rdbraw,
        decodeResult *decoded) {
    uint64_t version = 0;
    const char *key = NULL, *subkey = NULL;
    size_t klen = 0, slen = 0;
    decoded->enc_type = rawkey[0];
    if (isSubkeyEncType(decoded->enc_type)) {
        if (rocksDecodeSubkey(rawkey,sdslen(rawkey),&version,&key,&klen,
                &subkey,&slen) == -1)
            return -1;
    } else {
        if (rocksDecodeKey(rawkey,sdslen(rawkey),&key,&klen) == -1)
            return -1;
    }
    decoded->key = sdsnewlen(key,klen);
    decoded->subkey = sdsnewlen(subkey,slen);
    decoded->version = version;
    decoded->rdbtype = rdbtype;
    decoded->rdbraw = rdbraw;
    return 0;
}

void decodeResultDeinit(decodeResult *decoded) {
    if (decoded->key) {
        sdsfree(decoded->key);
        decoded->key = NULL;
    }
    if (decoded->subkey) {
        sdsfree(decoded->subkey);
        decoded->subkey = NULL;
    }
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

/* --- rdbKeySave --- */
int rdbKeySaveStart(struct rdbKeyData *keydata, rio *rdb) {
    if (keydata->type->save_start)
        return keydata->type->save_start(keydata,rdb);
    else
        return C_OK;
}

/* return true if key not finished(needs to consume more decoded result). */
int rdbKeySave(struct rdbKeyData *keydata, rio *rdb, decodeResult *d, int *error) {
    if (keydata->type->save) {
        int ret = keydata->type->save(keydata,rdb,d,error);
        /* Delay return if required (for testing) */
        if (server.rdb_key_save_delay)
            debugDelay(server.rdb_key_save_delay);
        return ret;
    } else {
        *error = 0;
        return 0;
    }
}

int rdbKeySaveEnd(struct rdbKeyData *keydata) {
    if (keydata->type->save_end)
        return keydata->type->save_end(keydata);
    else
        return C_OK;
}

void rdbKeyDataDeinitSave(rdbKeyData *keydata) {
    if (keydata->type->save_deinit) keydata->type->save_deinit(keydata);
}

/* Bighash fields are located adjacent, which will be iterated next to each.
 * - Init savectx for key (bighash or wholekey)
 * - iterate fields untill finished (meta.len is num fields to iterate).  */
int rdbSaveRocks(rio *rdb, redisDb *db, int rdbflags) {
    rocksIter *it;
    sds rawkey, rawval;
    int error, init_result;
    sds errstr;
    long long decode_raw_failed = 0, init_key_save_failed = 0;
    long long stat_rawkeys = 0, stat_obseletes = 0, stats_skipped = 0, stat_keys = 0;

    if (db->id > 0) return C_OK; /*TODO support multi-db */

    if (!(it = rocksCreateIter(server.rocks,db))) {
        serverLog(LL_WARNING, "Create rocks iterator failed.");
        return C_ERR;
    }

    if (!rocksIterSeekToFirst(it)) return C_OK;

    do {
        int cont, key_ready;
        sds key;
        unsigned char rdbtype;
        rdbKeyData _keydata, *keydata = &_keydata;
        decodeResult _decoded, *decoded = &_decoded;

        rocksIterKeyTypeValue(it,&rawkey,&rdbtype,&rawval);
        stat_rawkeys++;
        if (rocksDecodeRaw(rawkey,rdbtype,rawval,decoded)) {
            if (decode_raw_failed++ < 10) {
                sds repr = sdscatrepr(sdsempty(),rawkey,sdslen(rawkey));
                serverLog(LL_WARNING, "Decode rocks raw failed: %s", repr);
                sdsfree(repr);
            }
            stats_skipped++;
            continue;
        }

        key = decoded->key;
        if ((init_result = rdbKeyDataInitSave(keydata,db,decoded->key))) {
            if (init_result == -2) {
                stat_obseletes++;
            } else if (init_key_save_failed++ < 10) {
                sds repr = sdscatrepr(sdsempty(),key,sdslen(key));
                serverLog(LL_WARNING, "Init rdb save key failed: %s", repr);
                sdsfree(repr);
            }
            decodeResultDeinit(decoded);
            stats_skipped++;
            continue;
        }

        if ((error = rdbKeySaveStart(keydata,rdb))) {
            errstr = sdscatfmt(sdsempty(),"Save key(%S) start failed: %s",
                    decoded->key, strerror(error));
            decodeResultDeinit(decoded);
            rdbKeyDataDeinitSave(keydata);
            goto err;
        }

        cont = 1, key_ready = 1;
        while (!error && cont) {
            if (!key_ready) { /* prepare next key. */
                if (!rocksIterNext(it)) /* iter finished.*/
                    break;
                rocksIterKeyTypeValue(it,&rawkey,&rdbtype,&rawval);
                stat_rawkeys++;
                if (rocksDecodeRaw(rawkey,rdbtype,rawval,decoded)) {
                    if (decode_raw_failed++ < 10) {
                        sds repr = sdscatrepr(sdsempty(),rawkey,sdslen(rawkey));
                        serverLog(LL_WARNING, "Decode rocks rawkey failed: %s", repr);
                        sdsfree(repr);
                    }
                    decodeResultDeinit(decoded);
                    continue;
                }
                key_ready = 1;
            }
            cont = rdbKeySave(keydata,rdb,decoded,&error);
            key_ready = 0; /* flag key consumed */
            decodeResultDeinit(decoded);
        }

        /* call save_end if save_start called, no matter error or not. */
        if (!error && (error = rdbKeySaveEnd(keydata))) {
            errstr = sdscatfmt(sdsempty(),"Save key(%S) end failed: %s",
                    decoded->key, strerror(error));
            rdbKeyDataDeinitSave(keydata);
            break;
        }

        rdbKeyDataDeinitSave(keydata);
        stat_keys++;
        rdbSaveProgress(rdb,rdbflags);
    } while (!error && rocksIterNext(it));

    serverLog(LL_NOTICE,"Rdb save keys from rocksdb finished:"
            "rawkey(iterated:%lld,obselete:%lld,skipped:%lld), key(saved:%lld).",
            stat_rawkeys, stat_obseletes, stats_skipped, stat_keys);
    return C_OK;

err:
    serverLog(LL_WARNING, "Save rocks data to rdb failed: %s", errstr);
    if (it) rocksReleaseIter(it);
    if (errstr) sdsfree(errstr);
    return C_ERR;
}

void rdbKeyDataInitSaveKey(rdbKeyData *keydata, robj *value, robj *evict,
        long long expire) {
    keydata->savectx.type = 0;
    keydata->savectx.evict = evict;
    keydata->savectx.value = value;
    keydata->savectx.expire = expire;
}

int rdbKeyDataInitSave(rdbKeyData *keydata, redisDb *db, sds keystr) {
    robj *evict, *value, key;
    objectMeta *meta;
    long long expire;

    initStaticStringObject(key,keystr);
    evict = lookupEvictKey(db,&key);
    value = lookupKey(db,&key,LOOKUP_NOTOUCH);
    meta = lookupMeta(db,&key);
    expire = getExpire(db,&key);

    if (evict != NULL && value != NULL) {
        serverPanic("evict and value both null");
    } else if (evict != NULL && value == NULL) {
        /* cold key, could be cold wholekey/bighash */
        if (evict->type != OBJ_STRING && evict->type != OBJ_HASH) {
            serverPanic("unsupported cold key type.");
        } else if (evict->type == OBJ_STRING) {
            serverAssert(meta == NULL);
            rdbKeyDataInitSaveWholeKey(keydata,value,evict,expire);
        } else if (evict->type == OBJ_HASH && evict->big == 0) {
            serverAssert(meta == NULL);
            rdbKeyDataInitSaveWholeKey(keydata,value,evict,expire);
        } else { /* bighash */
            serverAssert(meta != NULL);
            rdbKeyDataInitSaveBigHash(keydata,value,evict,meta,expire,keystr);
        }
    } else if (evict == NULL && value != NULL) {
        if (meta != NULL) {
            rdbKeyDataInitSaveBigHash(keydata,value,evict,meta,expire,keystr);
        } else {
            /* hot key */
            return -2;
        }
    } else {
        /* key not exists */
        return -1;
    }
    return 0;
}

/* ------------------------------ rdb load -------------------------------- */
typedef struct rdbLoadSwapData {
    swapData d;
    redisDb *db;
    size_t idx;
    int num;
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
    zfree(data->rawkeys);
    zfree(data->rawvals);
    zfree(data);
}

int rdbLoadSwapAna(swapData *data, int cmd_intention,
        uint32_t cmd_intention_flags, struct keyRequest *req,
        int *intention, uint32_t *intention_flags, void *datactx) {
    UNUSED(data), UNUSED(cmd_intention), UNUSED(cmd_intention_flags),
    UNUSED(req), UNUSED(intention_flags), UNUSED(datactx);
    *intention = SWAP_OUT;
    *intention_flags = 0;
    return 0;
}

int rdbLoadEncodeData(swapData *data_, int intention, void *datactx,
        int *action, int *numkeys, sds **prawkeys, sds **prawvals) {
    rdbLoadSwapData *data = (rdbLoadSwapData*)data_;
    UNUSED(intention), UNUSED(datactx);
    *action = ROCKS_WRITE;
    *numkeys = data->num;
    *prawkeys = data->rawkeys;
    *prawvals = data->rawvals;
    data->num = 0;
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
swapData *createRdbLoadSwapData(size_t idx, int num, sds *rawkeys, sds *rawvals) {
    rdbLoadSwapData *data = zcalloc(sizeof(rdbLoadSwapData));
    data->d.type = &rdbLoadSwapDataType;
    data->idx = idx;
    data->num = num;
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
            ctx->batch.rawkeys, ctx->batch.rawvals);

#ifdef SWAP_DEBUG
    msgs = &((rdbLoadSwapData*)data)->msgs;
#endif

    DEBUG_MSGS_APPEND(msgs,"request-start","idx=%ld,num=%ld",ctx->idx,
            ctx->batch.index);

    /* Submit to rio thread. */
    submitSwapRequest(SWAP_MODE_PARALLEL_SYNC,SWAP_OUT,0,data,NULL,
                ctripRdbLoadWriteFinished,NULL,msgs);
}

void ctripRdbLoadCtxFeed(ctripRdbLoadCtx *ctx, sds rawkey, sds rawval) {
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
int rdbKeyLoad(struct rdbKeyData *keydata, rio *rdb, sds *rawkey, sds *rawval,
        int *error) {
    if (keydata->type->load)
        return keydata->type->load(keydata,rdb,rawkey,rawval,error);
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

void rdbKeyDataInitLoadKey(rdbKeyData *keydata, int rdbtype, sds key) {
    keydata->loadctx.rdbtype = rdbtype;
    keydata->loadctx.key = key;
    keydata->loadctx.nfeeds = 0;
}

int rdbKeyDataInitLoad(rdbKeyData *keydata, rio *rdb, int rdbtype, sds key) {
    if (!rdbIsObjectType(rdbtype)) return RDB_LOAD_ERR_OTHER;
    switch(rdbtype) {
    case RDB_TYPE_STRING:
    case RDB_TYPE_HASH_ZIPMAP:
    case RDB_TYPE_HASH_ZIPLIST:
        rdbKeyDataInitLoadWholeKey(keydata,rdbtype,key);
        break;
    case RDB_TYPE_HASH:
        {
            int isencode;
            unsigned long long len;
            sds hash_header = rdbVerbatimNew((unsigned char)rdbtype);
            /* nfield */
            if (rdbLoadLenVerbatim(rdb,&hash_header,&isencode,&len)) {
                sdsfree(hash_header);
                return RDB_LOAD_ERR_OTHER;
            }
            if (len*DEFAULT_HASH_FIELD_SIZE < server.swap_big_hash_threshold) {
                rdbKeyDataInitLoadWholeKey(keydata,rdbtype,key);
                keydata->loadctx.wholekey.hash_header = hash_header;
                keydata->loadctx.wholekey.hash_nfields = (int)len;
            } else { /* big hash */
                rdbKeyDataInitLoadBigHash(keydata,rdbtype,key);
                keydata->loadctx.bighash.hash_nfields = (int)len;
                sdsfree(hash_header);
            }
        }
        break;
    default:
        rdbKeyDataInitLoadMemkey(keydata,rdbtype,key);
        break;
    }
    return 0;
}

robj *rdbKeyLoadGetObject(struct rdbKeyData *keydata) {
    robj *x = NULL;
    switch (keydata->loadctx.type) {
    case RDB_KEY_TYPE_MEMKEY:
        x = keydata->loadctx.memkey.value;
        break;
    case RDB_KEY_TYPE_BIGHASH:
        x = keydata->loadctx.bighash.evict;
        break;
    case RDB_KEY_TYPE_WHOLEKEY:
        x = keydata->loadctx.wholekey.evict;
        break;
    default:
        x = NULL;
        break;
    }
    return x;
}

int ctripRdbLoadObject(int rdbtype, rio *rdb, sds key, rdbKeyData *keydata) {
    int error = 0, cont;
    sds rawkey, rawval;
    if ((error = rdbKeyDataInitLoad(keydata,rdb,rdbtype,key)))
        return error;
    do {
        cont = rdbKeyLoad(keydata,rdb,&rawkey,&rawval,&error);
        if (!error && rawkey) {
            serverAssert(rawval);
            ctripRdbLoadCtxFeed(server.rdb_load_ctx,rawkey,rawval);
            keydata->loadctx.nfeeds++;
        }
    } while (!error && cont);
    if (!error) error = rdbKeyLoadEnd(keydata,rdb);
    return error;
}

/* mem key */
rdbKeyType memkeyRdbType = {
    .save_start = NULL,
    .save = NULL,
    .save_end = NULL,
    .load = memkeyRdbLoad,
    .load_end = NULL,
    .load_dbadd = memkeyRdbLoadDbAdd,
    .load_expired = memkeyRdbLoadExpired,
    .load_deinit = NULL,
};

void rdbKeyDataInitLoadMemkey(rdbKeyData *keydata, int rdbtype, sds key) {
    rdbKeyDataInitLoadKey(keydata,rdbtype,key);
    keydata->type = &memkeyRdbType;
    keydata->loadctx.type = RDB_KEY_TYPE_MEMKEY;
    keydata->loadctx.memkey.value = NULL;
}

int memkeyRdbLoad(struct rdbKeyData *keydata, rio *rdb, sds *rawkey,
        sds *rawval, int *error) {
    keydata->loadctx.memkey.value = rdbLoadObject(keydata->loadctx.rdbtype,
            rdb,keydata->loadctx.key,error);
    *rawkey = NULL;
    *rawval = NULL;
    return 0;
}

void memkeyRdbLoadExpired(struct rdbKeyData *keydata) {
    robj *value = keydata->loadctx.memkey.value;
    if (value) {
        decrRefCount(value);
        keydata->loadctx.memkey.value = NULL;
    }
}

int memkeyRdbLoadDbAdd(struct rdbKeyData *keydata, redisDb *db) {
    return dbAddRDBLoad(db,keydata->loadctx.key,keydata->loadctx.memkey.value);
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

