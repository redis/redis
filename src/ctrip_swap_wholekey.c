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
#include "server.h"

/* ------------------- whole key object meta ----------------------------- */
int wholeKeyIsHot(objectMeta *om, robj *value) {
    UNUSED(om);
    return value != NULL;
}

objectMetaType wholekeyObjectMetaType = {
    .encodeObjectMeta = NULL,
    .decodeObjectMeta = NULL,
    .objectIsHot = wholeKeyIsHot,
};

/* ------------------- whole key swap data ----------------------------- */
int wholeKeySwapAna(swapData *data_, struct keyRequest *req,
        int *intention, uint32_t *intention_flags, void *datactx) {
    wholeKeySwapData *data = (wholeKeySwapData*)data_;
    int cmd_intention = req->cmd_intention;
    uint32_t cmd_intention_flags = req->cmd_intention_flags;

    UNUSED(datactx);

    switch(cmd_intention) {
    case SWAP_NOP:
        *intention = SWAP_NOP;
        *intention_flags = 0;
        break;
    case SWAP_IN:
        if (!data->d.value) {
            *intention = SWAP_IN;
            if ((cmd_intention_flags & SWAP_IN_DEL) ||
                    (cmd_intention_flags & SWAP_IN_DEL_MOCK_VALUE)) {
                *intention_flags = SWAP_EXEC_IN_DEL;
            } else {
                *intention_flags = 0;
            }
        } else if (data->d.value) {
            if (cmd_intention_flags & SWAP_IN_DEL) {
                *intention = SWAP_DEL;
                *intention_flags = SWAP_FIN_DEL_SKIP;
            } else {
                *intention = SWAP_NOP;
                *intention_flags = 0;
            }
        } else {
            *intention = SWAP_NOP;
            *intention_flags = 0;
        }
        break;
    case SWAP_OUT:
        if (data->d.value) {
            if (data->d.value->dirty) {
                *intention = SWAP_OUT;
                *intention_flags = SWAP_EXEC_OUT_META;
            } else {
                /* Not dirty: swapout right away without swap. */
                swapDataTurnCold(data_);
                swapDataSwapOut(data_, NULL);
                *intention = SWAP_NOP;
                *intention_flags = 0;
            }
        } else {
            *intention = SWAP_NOP;
            *intention_flags = 0;
        }
        break;
    case SWAP_DEL:
        *intention = SWAP_DEL;
        *intention_flags = 0;
        break;
    default:
        break;
    }

    return 0;
}

int wholeKeyEncodeKeys(swapData *data, int intention, void *datactx,
        int *action, int *numkeys, int **pcfs, sds **prawkeys) {
    sds *rawkeys = zmalloc(sizeof(sds));
    int *cfs = zmalloc(sizeof(int));

    UNUSED(datactx);
    rawkeys[0] = rocksEncodeDataKey(data->db,data->key->ptr,NULL);
    cfs[0] = DATA_CF;
    *numkeys = 1;
    *prawkeys = rawkeys;
    *pcfs = cfs;

    switch (intention) {
    case SWAP_IN:
        *action = ROCKS_GET;
        return 0;
    case SWAP_DEL:
        *action = ROCKS_DEL;
        return 0;
    case SWAP_OUT:
    default:
        sdsfree(rawkeys[0]);
        zfree(rawkeys);
        rawkeys = NULL;
        *action = SWAP_NOP;
        *numkeys = 0;
        *prawkeys = NULL;
        return SWAP_ERR_DATA_FAIL;
    }
    return 0;
}

static sds wholeKeyEncodeDataKey(swapData *data) {
    return data->key ? rocksEncodeDataKey(data->db,data->key->ptr,NULL) : NULL;
}

static sds wholeKeyEncodeDataVal(swapData *data) {
    return data->value ? rocksEncodeValRdb(data->value) : NULL;
}

int wholeKeyEncodeData(swapData *data, int intention, void *datactx,
        int *action, int *numkeys, int **pcfs, sds **prawkeys, sds **prawvals) {
    UNUSED(datactx);
    serverAssert(intention == SWAP_OUT);
    sds *rawkeys = zmalloc(sizeof(sds));
    sds *rawvals = zmalloc(sizeof(sds));
    int *cfs = zmalloc(sizeof(int));
    rawkeys[0] = wholeKeyEncodeDataKey(data);
    rawvals[0] = wholeKeyEncodeDataVal(data);
    cfs[0] = DATA_CF;
    *action = ROCKS_PUT;
    *numkeys = 1;
    *prawkeys = rawkeys;
    *prawvals = rawvals;
    *pcfs = cfs;
    return 0;
}

/* decoded move to exec module */
int wholeKeyDecodeData(swapData *data, int num, int *cfs, sds *rawkeys,
        sds *rawvals, void **pdecoded) {
    serverAssert(num == 1);
    UNUSED(data);
    UNUSED(rawkeys);
    UNUSED(cfs);
    sds rawval = rawvals[0];
    *pdecoded = rocksDecodeValRdb(rawval);
    return 0;
}

/* If maxmemory policy is not LRU/LFU, rdbLoadObject might return shared
 * object, but swap needs individual object to track dirty/evict flags. */
robj *dupSharedObject(robj *o) {
    switch(o->type) {
    case OBJ_STRING:
        return dupStringObject(o);
    case OBJ_HASH:
    case OBJ_LIST:
    case OBJ_SET:
    case OBJ_ZSET:
    default:
        return NULL;
    }
}

static robj *createSwapInObject(MOVE robj *newval) {
    robj *swapin = newval;
    serverAssert(newval);
    /* Copy swapin object before modifing If newval is shared object. */
    if (newval->refcount == OBJ_SHARED_REFCOUNT)
        swapin = dupSharedObject(newval);
    swapin->dirty = 0;
    return swapin;
}

int wholeKeySwapIn(swapData *data_, MOVE void *result, void *datactx) {
    wholeKeySwapData *data = (wholeKeySwapData*)data_;
    UNUSED(datactx);
    robj *swapin;
    serverAssert(data->d.value == NULL);
    swapin = createSwapInObject(result);
    dbAdd(data->d.db,data->d.key,swapin);
    //TODO remove
    if (data->d.expire != -1)
        setExpire(NULL,data->d.db,data->d.key,data->d.expire);
    return 0;
}

int wholeKeySwapOut(swapData *data, void *datactx) {
    UNUSED(datactx);
    redisDb *db = data->db;
    robj *key = data->key;
    //TODO remove
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    /* TODO opt lazyfree_lazy_swap_del */
    if (dictSize(db->dict) > 0) dictDelete(db->dict,key->ptr);
    return 0;
}

int wholeKeySwapDel(swapData *data, void *datactx, int async) {
    UNUSED(datactx);
    redisDb *db = data->db;
    robj *key = data->key;
    if (async) return 0;
    //TODO remove
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    if (data->value) dictDelete(db->dict,key->ptr);
    return 0;
}

/* decoded moved back by exec to wholekey then moved to exec again. */
void *wholeKeyCreateOrMergeObject(swapData *data, void *decoded, void *datactx) {
    UNUSED(data);
    UNUSED(datactx);
    serverAssert(decoded);
    return decoded;
}

swapDataType wholeKeySwapDataType = {
    .name = "wholekey",
    .swapAna = wholeKeySwapAna,
    .encodeKeys = wholeKeyEncodeKeys,
    .encodeData = wholeKeyEncodeData,
    .decodeData = wholeKeyDecodeData,
    .swapIn = wholeKeySwapIn,
    .swapOut = wholeKeySwapOut,
    .swapDel = wholeKeySwapDel,
    .createOrMergeObject = wholeKeyCreateOrMergeObject,
    .cleanObject = NULL,
    .beforeCall = NULL,
    .free = NULL,
};

int swapDataSetupWholeKey(swapData *d, void **datactx) {
    d->type = &wholeKeySwapDataType;
    d->omtype = &wholekeyObjectMetaType;
    if (datactx) *datactx = NULL;
    return 0;
}

/* ------------------- whole key rdb save -------------------------------- */
int wholekeySave(rdbKeySaveData *keydata, rio *rdb, decodedData *decoded) {
    robj keyobj = {0};

    serverAssert(decoded->cf == DATA_CF); 
    serverAssert((NULL == decoded->subkey));
    initStaticStringObject(keyobj,decoded->key);

    if (rdbSaveKeyHeader(rdb,&keyobj,&keyobj,
                RDB_TYPE_STRING,
                keydata->expire) == -1) {
        return -1;
    }

    if (rdbWriteRaw(rdb,decoded->rdbraw,
                sdslen(decoded->rdbraw)) == -1) {
        return -1;
    }

    return 0;
}

rdbKeySaveType wholekeyRdbSaveType = {
    .save_start = NULL,
    .save = wholekeySave,
    .save_end = NULL,
    .save_deinit = NULL,
};

void wholeKeySaveInit(rdbKeySaveData *keydata) {
    keydata->type = &wholekeyRdbSaveType;
    keydata->omtype = &wholekeyObjectMetaType;
}

/* ------------------- whole key rdb load -------------------------------- */
void wholekeyLoadStart(struct rdbKeyLoadData *keydata, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    UNUSED(rdb);
    *cf = META_CF;
    *rawkey = rocksEncodeMetaKey(keydata->db,keydata->key);
    *rawval = rocksEncodeMetaVal(keydata->object_type,keydata->expire,NULL);
    *error = 0;
}

int wholekeyLoad(struct rdbKeyLoadData *keydata, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    *error = RDB_LOAD_ERR_OTHER;
    int rdbtype = keydata->rdbtype;
    sds verbatim = NULL, key = keydata->key;
    redisDb *db = keydata->db;

    verbatim = rdbVerbatimNew((unsigned char)rdbtype);
    if (rdbLoadStringVerbatim(rdb,&verbatim)) goto err;

    *error = 0;
    *cf = DATA_CF;
    *rawkey = rocksEncodeDataKey(db,key,NULL);
    *rawval = verbatim;
    return 0;

err:
    if (verbatim) sdsfree(verbatim);
    return 0;
}

rdbKeyLoadType wholekeyLoadType = {
    .load_start = wholekeyLoadStart,
    .load = wholekeyLoad,
    .load_end = NULL,
    .load_deinit = NULL,
};

void wholeKeyLoadInit(rdbKeyLoadData *keydata) {
    keydata->type = &wholekeyLoadType;
    keydata->omtype = &wholekeyObjectMetaType;
    keydata->object_type = OBJ_STRING;
}


#ifdef REDIS_TEST
#include <stdio.h>
#include <limits.h>
#include <assert.h>

#define FREE_SDSARRAY(sdss,n) do {    \
    for (int i = 0; i < n; i++) sdsfree(sdss[i]);    \
    zfree(sdss), sdss = NULL; \
} while (0)

swapData *createWholeKeySwapDataWithExpire(redisDb *db, robj *key, robj *value,
        long long expire, void **datactx) {
    swapData *data = createSwapData(db,key,value);
    swapDataSetupMeta(data,OBJ_STRING,expire,datactx);
    return data;
}

swapData *createWholeKeySwapData(redisDb *db, robj *key, robj *value, void **datactx) {
    return createWholeKeySwapDataWithExpire(db,key,value,-1,datactx);
}

int wholeKeySwapAna_(swapData *data_,
        int cmd_intention, uint32_t cmd_intention_flags,
        int *intention, uint32_t *intention_flags, void *datactx) {
    int retval;
    struct keyRequest req_, *req = &req_;
    req->level = REQUEST_LEVEL_KEY;
    req->num_subkeys = 0;
    req->key = createStringObject("key1",4);
    req->subkeys = NULL;
    req->cmd_intention = cmd_intention;
    req->cmd_intention_flags = cmd_intention_flags;
    retval = wholeKeySwapAna(data_,req,intention,intention_flags,datactx);
    decrRefCount(req->key);
    return retval;
}

int swapDataWholeKeyTest(int argc, char **argv, int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    initTestRedisServer();
    redisDb* db = server.db + 0;
    int error = 0;

    TEST("wholeKey - SwapAna hot key") {
        void* ctx = NULL;
        robj* value  = createRawStringObject("value", 5);
        swapData* data = createWholeKeySwapData(db, NULL, value, &ctx);
        int intention;
        uint32_t intention_flags;
        wholeKeySwapAna_(data, SWAP_NOP, 0, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        wholeKeySwapAna_(data, SWAP_IN, 0, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        wholeKeySwapAna_(data, SWAP_IN, SWAP_IN_DEL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_DEL);
        test_assert(intention_flags == SWAP_FIN_DEL_SKIP);
        wholeKeySwapAna_(data, SWAP_OUT, 0, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_OUT);
        test_assert(intention_flags == SWAP_EXEC_OUT_META);
        wholeKeySwapAna_(data, SWAP_DEL, 0, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_DEL);
        swapDataFree(data, ctx);
    }

    TEST("wholeKey - SwapAna cold key") {
        void* ctx = NULL;
        swapData* data = createWholeKeySwapData(db, NULL, NULL, &ctx);
        int intention;
        uint32_t intention_flags;
        wholeKeySwapAna_(data, SWAP_NOP, 0, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        wholeKeySwapAna_(data, SWAP_IN, 0, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_IN);
        wholeKeySwapAna_(data, SWAP_IN, SWAP_IN_DEL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_IN);
        test_assert(intention_flags == SWAP_EXEC_IN_DEL);
        wholeKeySwapAna_(data, SWAP_OUT, 0, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        wholeKeySwapAna_(data, SWAP_DEL, 0, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_DEL);
        swapDataFree(data, ctx);
    }

    TEST("wholeKey - EncodeKeys (hot)") {
        void* ctx = NULL;
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        swapData* data = createWholeKeySwapData(db, key, value, &ctx);
        int numkeys, action;
        int *cfs;
        sds *rawkeys = NULL;
        int result = wholeKeyEncodeKeys(data, SWAP_IN, ctx, &action, &numkeys, &cfs, &rawkeys);
        test_assert(ROCKS_GET == action);
        test_assert(numkeys == 1);
        test_assert(cfs[0] == DATA_CF);
#if (BYTE_ORDER == LITTLE_ENDIAN)
        test_assert(!memcmp(rawkeys[0], "\x00\x00\x00\x00\x03\x00\x00\x00key", 11));
#else
        test_assert(!memcmp(rawkeys[0], "\x00\x00\x00\x00\x00\x00\x00\x03key", 11));
#endif
        test_assert(result == C_OK);
        FREE_SDSARRAY(rawkeys,1);
        result = wholeKeyEncodeKeys(data, SWAP_DEL, ctx, &action, &numkeys, &cfs, &rawkeys);
        test_assert(ROCKS_DEL == action);
        test_assert(numkeys == 1);
        test_assert(cfs[0] == DATA_CF);
#if (BYTE_ORDER == LITTLE_ENDIAN)
        test_assert(!memcmp(rawkeys[0], "\x00\x00\x00\x00\x03\x00\x00\x00key", 11));
#else
        test_assert(!memcmp(rawkeys[0], "\x00\x00\x00\x00\x00\x00\x00\x03key", 11));
#endif
        test_assert(result == C_OK);
        FREE_SDSARRAY(rawkeys,1);
        swapDataFree(data, ctx);
    }

    TEST("wholeKey - EncodeKeys (cold)") {
        void* ctx = NULL;
        robj* key = createRawStringObject("key", 3);
        swapData* data = createWholeKeySwapData(db, key, NULL, &ctx);
        int numkeys, action;
        sds *rawkeys = NULL;
        int *cfs;
        int result = wholeKeyEncodeKeys(data, SWAP_IN, ctx, &action, &numkeys, &cfs, &rawkeys);
        test_assert(ROCKS_GET == action);
        test_assert(numkeys == 1);
        test_assert(cfs[0] == DATA_CF);
#if (BYTE_ORDER == LITTLE_ENDIAN)
        test_assert(!memcmp(rawkeys[0], "\x00\x00\x00\x00\x03\x00\x00\x00key", 11));
#else
        test_assert(!memcmp(rawkeys[0], "\x00\x00\x00\x00\x00\x00\x00\x03key", 11));
#endif
        test_assert(result == C_OK);
        FREE_SDSARRAY(rawkeys,1);
        result = wholeKeyEncodeKeys(data, SWAP_DEL, ctx, &action, &numkeys, &cfs, &rawkeys);
        test_assert(ROCKS_DEL == action);
        test_assert(numkeys == 1);
        test_assert(cfs[0] == DATA_CF);
#if (BYTE_ORDER == LITTLE_ENDIAN)
        test_assert(!memcmp(rawkeys[0], "\x00\x00\x00\x00\x03\x00\x00\x00key", 11));
#else
        test_assert(!memcmp(rawkeys[0], "\x00\x00\x00\x00\x00\x00\x00\x03key", 11));
#endif
        test_assert(result == C_OK);
        FREE_SDSARRAY(rawkeys,1);
        swapDataFree(data, ctx);
    }

    TEST("wholeKey - EncodeData + DecodeData") {
        void* ctx = NULL;
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        swapData* data = createWholeKeySwapData(db, key, value, &ctx);
        int numkeys, action;
        sds *rawkeys = NULL, *rawvals = NULL;
        int *cfs;
        int result = wholeKeyEncodeData(data, SWAP_OUT, ctx, &action, &numkeys, &cfs, &rawkeys, &rawvals);
        test_assert(result == C_OK);
        test_assert(ROCKS_PUT == action);
        test_assert(numkeys == 1);
        test_assert(cfs[0] == DATA_CF);
#if (BYTE_ORDER == LITTLE_ENDIAN)
        test_assert(!memcmp(rawkeys[0], "\x00\x00\x00\x00\x03\x00\x00\x00key", 11));
#else
        test_assert(!memcmp(rawkeys[0], "\x00\x00\x00\x00\x00\x00\x00\x03key", 11));
#endif

        void* decoded;
        result = wholeKeyDecodeData(data, numkeys, cfs, rawkeys, rawvals, &decoded);
        test_assert(result == C_OK);
        test_assert(strcmp(((robj*)decoded)->ptr ,"value") == 0);
        swapDataFree(data, ctx);
    }
    
    TEST("wholeKey - swapIn cold non-volatie key") {
        robj *decoded;
        robj* key = createRawStringObject("key", 3);
        swapData* data = createWholeKeySwapData(db, key, NULL, NULL);
        decoded = createRawStringObject("value", 5);
        test_assert(wholeKeySwapIn(data, decoded, NULL) == 0);
        test_assert(dictFind(db->dict, key->ptr) != NULL);    
        decoded = NULL;
        swapDataFree(data, NULL);
        clearTestRedisDb();
    }

    TEST("wholekey - swapIn cold volatile key") {
        robj *key = createRawStringObject("key", 3);
        robj *decoded = NULL;
        test_assert(dictFind(db->dict, key->ptr) == NULL);   

        swapData* data = createWholeKeySwapDataWithExpire(db, key, NULL, 1000000, NULL);
        decoded = createRawStringObject("value", 5);
        test_assert(wholeKeySwapIn(data,decoded,NULL) == 0);
        test_assert(dictFind(db->dict, key->ptr) != NULL);    
        test_assert(getExpire(db, key) > 0);
        decoded = NULL;
        swapDataFree(data, NULL);
        clearTestRedisDb();

    }

    TEST("wholeKey - swapout hot non-volatile key") {
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        test_assert(dictFind(db->dict, key->ptr) == NULL);  
        dbAdd(db, key, value);
        test_assert(dictFind(db->dict, key->ptr) != NULL); 

        swapData* data = createWholeKeySwapData(db, key, value, NULL);
        test_assert(wholeKeySwapOut(data, NULL) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(getExpire(db, key) < 0);
        swapDataFree(data, NULL);
        clearTestRedisDb();
    }

    TEST("wholeKey - swapout hot volatile key") {
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        test_assert(dictFind(db->dict, key->ptr) == NULL);  
        dbAdd(db, key, value);
        setExpire(NULL, db, key, 1000000);
        test_assert(dictFind(db->dict, key->ptr) != NULL); 

        swapData* data = createWholeKeySwapDataWithExpire(db, key, value, 1000000, NULL);
        test_assert(wholeKeySwapOut(data, NULL) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(getExpire(db, key) < 0);
        swapDataFree(data, NULL);
        clearTestRedisDb();
    }
    
    TEST("wholeKey - swapdelete hot non-volatile key") {
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        test_assert(dictFind(db->dict, key->ptr) == NULL);  
        dbAdd(db, key, value);
        test_assert(dictFind(db->dict, key->ptr) != NULL); 

        swapData* data = createWholeKeySwapData(db, key, value, NULL);
        test_assert(wholeKeySwapDel(data, NULL, 0) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(getExpire(db, key) < 0);
        swapDataFree(data, NULL);
        clearTestRedisDb();
    }

    TEST("wholeKey - swapdelete hot volatile key") {
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        test_assert(dictFind(db->dict, key->ptr) == NULL);  
        dbAdd(db, key, value);
        setExpire(NULL, db, key, 1000000);
        test_assert(dictFind(db->dict, key->ptr) != NULL); 
        test_assert(getExpire(db, key) > 0); 

        swapData* data = createWholeKeySwapDataWithExpire(db, key, value, 1000000, NULL);
        test_assert(wholeKeySwapDel(data, NULL, 0) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(getExpire(db, key) < 0);
        swapDataFree(data, NULL);
        clearTestRedisDb();
    }

    TEST("wholeKey - swapdelete cold key") {
        robj* key = createRawStringObject("key", 3);
        test_assert(dictFind(db->dict, key->ptr) == NULL);  
        swapData* data = createWholeKeySwapData(db, key, NULL, NULL);
        test_assert(wholeKeySwapDel(data, NULL, 0) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(getExpire(db, key) < 0);
        swapDataFree(data, NULL);
        clearTestRedisDb();
    }

    int rocksDecodeMetaCF(sds rawkey, sds rawval, decodedMeta *decoded);
    int rocksDecodeDataCF(sds rawkey, unsigned char rdbtype, sds rdbraw, decodedData *decoded);

    TEST("wholeKey rdb save & load") {
        int err, feed_cf;
        rio sdsrdb;
        rdbKeySaveData _savedata, *savedata = &_savedata;
        rdbKeyLoadData _loaddata, *loaddata = &_loaddata;
        decodedMeta _dm, *dm = &_dm;
        decodedData _dd, *dd = &_dd;
        sds feed_rawkey, feed_rawval, rdb_key, rdbraw;

        sds key = sdsnew("key");
        robj *val = createStringObject("val",3);
        sds meta_rawkey = rocksEncodeMetaKey(db,key);
        sds meta_rawval = rocksEncodeMetaVal(OBJ_STRING,-1,NULL);
        sds data_rawkey = rocksEncodeDataKey(db,key,NULL);
        sds data_rawval = rocksEncodeValRdb(val);

        test_assert(!rocksDecodeMetaCF(sdsdup(meta_rawkey),sdsdup(meta_rawval),dm));
        test_assert(dm->expire == -1);
        test_assert(dm->extend == NULL);
        test_assert(!sdscmp(dm->key,key));

        rdbraw = sdsnewlen(data_rawval+1,sdslen(data_rawval)-1);
        test_assert(!rocksDecodeDataCF(sdsdup(data_rawkey),data_rawval[0],rdbraw,dd));
        test_assert(!sdscmp(dd->key,key));
        test_assert(dd->subkey == NULL);
        test_assert(!memcmp(dd->rdbraw,data_rawval+1,sdslen(dd->rdbraw)));

        rioInitWithBuffer(&sdsrdb, sdsempty());
        test_assert(!rdbKeySaveDataInit(savedata,db,(decodedResult*)dm));
        test_assert(!wholekeySave(savedata,&sdsrdb,dd));

        rioInitWithBuffer(&sdsrdb,sdsrdb.io.buffer.ptr);
        /* LFU */
        uint8_t byte;
        test_assert(rdbLoadType(&sdsrdb) == RDB_OPCODE_FREQ);
        test_assert(rioRead(&sdsrdb,&byte,1));
        /* rdbtype */
        test_assert(rdbLoadType(&sdsrdb) == RDB_TYPE_STRING);
        /* key */
        rdb_key = rdbGenericLoadStringObject(&sdsrdb,RDB_LOAD_SDS,NULL);
        test_assert(!sdscmp(rdb_key, key));

        rdbKeyLoadDataInit(loaddata,RDB_TYPE_STRING,db,rdb_key,-1,1600000000);

        wholekeyLoadStart(loaddata,&sdsrdb,&feed_cf,&feed_rawkey,&feed_rawval,&err);
        test_assert(err == 0);
        test_assert(feed_cf == META_CF);
        test_assert(!sdscmp(feed_rawkey,meta_rawkey));
        test_assert(!sdscmp(feed_rawval,meta_rawval));

        wholekeyLoad(loaddata,&sdsrdb,&feed_cf,&feed_rawkey,&feed_rawval,&err);
        test_assert(err == 0);
        test_assert(feed_cf == DATA_CF);
        test_assert(!sdscmp(feed_rawkey,data_rawkey));
        test_assert(!sdscmp(feed_rawval,data_rawval));
    }

    return error;
}

#endif

