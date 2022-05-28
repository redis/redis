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

/* ------------------------------ rdb save -------------------------------- */
/* Whole key encoding in rocksdb is the same as in rdb, so we skip encoding
 * and decoding to reduce cpu usage. */ 
int rdbSaveKeyRawPair(rio *rdb, robj *key, robj *evict, sds raw, 
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
        uint64_t idletime = estimateObjectIdleTime(evict);
        idletime /= 1000; /* Using seconds is enough and requires less space.*/
        if (rdbSaveType(rdb,RDB_OPCODE_IDLE) == -1) return -1;
        if (rdbSaveLen(rdb,idletime) == -1) return -1;
    }

    /* Save the LFU info. */
    if (savelfu) {
        uint8_t buf[1];
        buf[0] = LFUDecrAndReturn(evict);
        /* We can encode this in exactly two bytes: the opcode and an 8
         * bit counter, since the frequency is logarithmic with a 0-255 range.
         * Note that we do not store the halving time because to reset it
         * a single time when loading does not affect the frequency much. */
        if (rdbSaveType(rdb,RDB_OPCODE_FREQ) == -1) return -1;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
    }

    /* Save type, key, value */
    if (rdbSaveObjectType(rdb,evict) == -1) return -1;
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    if (rdbWriteRaw(rdb,raw,sdslen(raw)) == -1) return -1;

    /* Delay return if required (for testing) */
    if (server.rdb_key_save_delay)
        debugDelay(server.rdb_key_save_delay);

    return 1;
}

int rdbSaveRocks(rio *rdb, redisDb *db, int rdbflags) {
    rocksIter *it;
    sds cached_key;
    sds rawkey, rawval;

    if (db->id > 0) return C_OK; /*TODO support multi-db */

    if (!(it = rocksCreateIter(server.rocks, db))) {
        serverLog(LL_WARNING, "Create rocks iterator failed.");
        return C_ERR;
    }

    cached_key = sdsnewlen(NULL,ITER_CACHED_MAX_KEY_LEN);

    if (!rocksIterSeekToFirst(it)) goto end;

    do {
        robj keyobj, *evict;
        long long expire;
        int obj_type;
        const char *keyptr;
        size_t klen;
        sds key;
        int retval;

        rocksIterKeyValue(it, &rawkey, &rawval);

        obj_type = rocksDecodeKey(rawkey, sdslen(rawkey), &keyptr, &klen);
        if (klen > ITER_CACHED_MAX_KEY_LEN) {
            key = sdsnewlen(keyptr, klen);
        } else {
            memcpy(cached_key, keyptr, klen);
            cached_key[klen] = '\0';
            sdssetlen(cached_key, klen);
            key = cached_key;
        }

        initStaticStringObject(keyobj, key);
        evict = lookupEvictKey(db, &keyobj);
        if (evict == NULL || evict->type != obj_type) {
            if (evict != NULL) {
                serverLog(LL_WARNING,
                        "Save object rocks type(%d) not match "
                        "evict type(%d) for key: %s",
                        obj_type, evict->type, key);
            }
            if (key != cached_key) sdsfree(key);
            continue;
        }

        if (NULL != lookupKey(db, &keyobj, LOOKUP_NOTOUCH)) {
            continue;
        }


        expire = getExpire(db, &keyobj);
        retval = rdbSaveKeyRawPair(rdb,&keyobj,evict,rawval,expire);
        if (key != cached_key) sdsfree(key);

        if (retval == -1) {
            serverLog(LL_WARNING, "Save Raw value failed for key: %s.",key);
            goto err;
        }
        rdbSaveProgress(rdb,rdbflags);
    } while(rocksIterNext(it));

end:
    sdsfree(cached_key);
    rocksReleaseIter(it);
    return C_OK;

err:
    sdsfree(cached_key);
    rocksReleaseIter(it);
    return C_ERR;
}

/* --- rdb load --- */
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
        struct keyRequest *req, int *intention) {
    UNUSED(data), UNUSED(cmd_intention), UNUSED(req);
    *intention = SWAP_OUT;
    return 0;
}

int rdbLoadEncodeData(swapData *data_, int intention, int *action,
        int *numkeys, sds **prawkeys, sds **prawvals, void *datactx) {
    rdbLoadSwapData *data = (rdbLoadSwapData*)data_;
    UNUSED(intention), UNUSED(datactx);
    *action = ROCKS_WRITE;
    *numkeys = data->num;
    *prawkeys = data->rawkeys;
    *prawvals = data->rawvals;
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
    submitSwapRequest(SWAP_MODE_PARALLEL_SYNC,SWAP_OUT,data,NULL,
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

/* Load directly into db.evict for objects that supports swap. */
int dbAddEvictRDBLoad(redisDb* db, sds key, robj* evict, sds rawval) {
    /* Add to db.evict. Note that key is moved to db.evict. */ 
    int retval = dictAdd(db->evict,key,evict);
    if (retval != DICT_OK) return 0;
    if (server.cluster_enabled) slotToKeyAdd(key);
    evict->evicted = 1;
    /* Add to rocksdb. */
    sds rawkey = rocksEncodeKey(evict->type,key);
    ctripRdbLoadCtxFeed(server.rdb_load_ctx, rawkey, rawval);
    return 1;
}

/* Note that key,val,rawval moved. */
int ctripDbAddRDBLoad(int vtype, redisDb* db, sds key, robj* val, sds rawval) {
    if (vtype == RDB_LOAD_VTYPE_VERBATIM)
        return dbAddEvictRDBLoad(db,key,val,rawval);
    else /* RDB_LOAD_VTYPE_OBJECT */
        return dbAddRDBLoad(db,key,val);
}

int ctripRdbLoadObject(int rdbtype, rio *rdb, sds key, int *vtype, robj **val, sds *rawval) {
    robj *evict;
    int error = 0;

    if (!rdbIsObjectType(rdbtype)) return RDB_LOAD_ERR_OTHER;

    *val = NULL;
    if (rdbtype == RDB_TYPE_STRING) {
        *vtype = RDB_LOAD_VTYPE_VERBATIM;
        sds verbatim = sdsempty();
        if (rdbLoadStringVerbatim(rdb,&verbatim)) {
            sdsfree(verbatim);
            error = RDB_LOAD_ERR_OTHER;
        } else {
            *val = createObject(OBJ_STRING, NULL);
            *rawval = verbatim;
        }
    } else if (rdbtype == RDB_TYPE_HASH) {
        *vtype = RDB_LOAD_VTYPE_VERBATIM;
        sds verbatim = sdsempty();
        if (rdbLoadHashVerbatim(rdb,&verbatim)) {
            sdsfree(verbatim);
            error = RDB_LOAD_ERR_OTHER;
        } else {
            evict = createObject(OBJ_HASH,NULL);
            evict->encoding = OBJ_ENCODING_HT;
            *val = evict;
            *rawval = verbatim;
        }
    } else if (rdbtype == RDB_TYPE_HASH_ZIPLIST) {
        *vtype = RDB_LOAD_VTYPE_VERBATIM;
        sds verbatim = sdsempty();
        if (rdbLoadStringVerbatim(rdb,&verbatim)) {
            sdsfree(verbatim);
            error = RDB_LOAD_ERR_OTHER;
        } else {
            evict = createObject(OBJ_HASH, NULL);
            evict->encoding = OBJ_ENCODING_ZIPLIST;
            *val = evict;
            *rawval = verbatim;
        }
    } else {
        *vtype = RDB_LOAD_VTYPE_OBJECT;
        *val = rdbLoadObject(rdbtype, rdb, key, &error);
        *rawval = NULL;
    }

    return error;
}
