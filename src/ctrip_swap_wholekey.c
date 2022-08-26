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
            if (cmd_intention_flags & SWAP_IN_DEL)
                *intention_flags = SWAP_EXEC_IN_DEL;
            else
                *intention_flags = 0;
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
        return C_OK;
    case SWAP_DEL:
        *action = ROCKS_DEL;
        return C_OK;
    case SWAP_OUT:
    default:
        sdsfree(rawkeys[0]);
        zfree(rawkeys);
        rawkeys = NULL;
        *action = SWAP_NOP;
        *numkeys = 0;
        *prawkeys = NULL;
        return C_ERR;
    }
    return C_OK;
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
    return C_OK;
}

/* decoded move to exec module */
int wholeKeyDecodeData(swapData *data, int num, int *cfs, sds *rawkeys,
        sds *rawvals, robj **pdecoded) {
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

static robj *createSwapInObject(robj *newval) {
    robj *swapin = newval;
    serverAssert(newval);
    incrRefCount(newval);
    /* Copy swapin object before modifing If newval is shared object. */
    if (newval->refcount == OBJ_SHARED_REFCOUNT)
        swapin = dupSharedObject(newval);
    swapin->dirty = 0;
    return swapin;
}

int wholeKeySwapIn(swapData *data_, robj *result, void *datactx) {
    wholeKeySwapData *data = (wholeKeySwapData*)data_;
    UNUSED(datactx);
    robj *swapin;
    serverAssert(data->d.value == NULL);
    swapin = createSwapInObject(result);
    dbAdd(data->d.db,data->d.key,swapin);
    if (data->d.expire != -1)
        setExpire(NULL,data->d.db,data->d.key,data->d.expire);
    return 0;
}

int wholeKeySwapOut(swapData *data, void *datactx) {
    UNUSED(datactx);
    redisDb *db = data->db;
    robj *key = data->key;
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
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    if (data->value) dictDelete(db->dict,key->ptr);
    return 0;
}

/* decoded moved back by exec to wholekey then moved to exec again. */
robj *wholeKeyCreateOrMergeObject(swapData *data, robj *decoded, void *datactx) {
    UNUSED(data);
    UNUSED(datactx);
    serverAssert(decoded);
    return decoded;
}

int swapDataSetupWholeKey(swapData *d, void **datactx) {
    d->type = &wholeKeySwapDataType;
    if (datactx) *datactx = NULL;
    return 0;
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
    .free = NULL,
};

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

/* ------------------- whole key rdb save -------------------------------- */
rdbKeySaveType wholekeyRdbSaveType = {
    .save_start = NULL,
    .save = wholekeySave,
    .save_end = NULL,
    .save_deinit = NULL,
};

int wholeKeySaveInit(rdbKeySaveData *keydata) {
    keydata->type = &wholekeyRdbSaveType;
    keydata->omtype = &wholekeyObjectMetaType;
    return 0;
}

int wholekeySave(rdbKeySaveData *keydata, rio *rdb, decodedData *decoded) {
    robj keyobj = {0};

    serverAssert(decoded->cf == DATA_CF); 
    initStaticStringObject(keyobj,decoded->key);

    if (rdbSaveKeyHeader(rdb,&keyobj,&keyobj,
                decoded->rdbtype,
                keydata->expire) == -1) {
        return -1;
    }

    if (rdbWriteRaw(rdb,decoded->rdbraw,
                sdslen(decoded->rdbraw)) == -1) {
        return -1;
    }

    return 0;
}

/* ------------------- whole key rdb load -------------------------------- */
rdbKeyLoadType wholekeyLoadType = {
    .load_start = wholekeyLoadStart,
    .load = wholekeyLoad,
    .load_end = NULL,
    .load_dbadd = NULL,
    .load_expired = NULL, /* TODO opt: delete saved key in datacf. */
    .load_deinit = NULL,
};

void wholeKeyLoadInit(rdbKeyLoadData *keydata) {
    keydata->type = &wholekeyLoadType;
    keydata->omtype = &wholekeyObjectMetaType;
    keydata->object_type = OBJ_STRING;
}

int wholekeyLoadStart(struct rdbKeyLoadData *keydata, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    UNUSED(rdb);
    *cf = META_CF;
    *rawkey = rocksEncodeMetaKey(keydata->db,keydata->key);
    *rawval = rocksEncodeMetaVal(keydata->object_type,keydata->expire,NULL);
    *error = 0;
    return 0;
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

    //initSwapWholeKey();

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

        robj* decoded;
        result = wholeKeyDecodeData(data, numkeys, cfs, rawkeys, rawvals, &decoded);
        test_assert(result == C_OK);
        test_assert(strcmp(decoded->ptr ,"value") == 0);
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

    /* int rocksDecodeRaw(sds rawkey, unsigned char rdbtype, sds rdbraw, decodeResult *decoded); */

    /* TEST("wholeKey rdb save & load") { */
        /* int err; */
        /* rdbKeyData _keydata, *keydata = &_keydata; */
        /* rio sdsrdb; */
        /* robj *evict = createObject(OBJ_HASH,NULL); */
        /* decodeResult _decoded, *decoded = &_decoded; */

		/* robj *myhash; */
		/* sds myhash_key; */
		/* myhash_key = sdsnew("myhash"); */
        /* myhash = createHashObject(); */

        /* hashTypeSet(myhash,sdsnew("f1"),sdsnew("v1"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE); */
        /* hashTypeSet(myhash,sdsnew("f2"),sdsnew("v2"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE); */
        /* hashTypeSet(myhash,sdsnew("f3"),sdsnew("v3"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE); */

        /* sds rawkey = rocksEncodeKey(ENC_TYPE_HASH,myhash_key); */
        /* sds rawval = rocksEncodeValRdb(myhash); */

        /* sds rdbraw = sdsnewlen(rawval+1,sdslen(rawval)-1); */
        /* rocksDecodeRaw(sdsdup(rawkey),rawval[0],rdbraw,decoded); */
        /* test_assert(decoded->enc_type == ENC_TYPE_HASH); */
        /* test_assert(!sdscmp(decoded->key,myhash_key)); */

        /* rioInitWithBuffer(&sdsrdb, sdsempty()); */
        /* rdbKeyDataInitSaveWholeKey(keydata,NULL,evict,-1); */
        /* test_assert(wholekeySave(keydata,&sdsrdb,decoded) == 0); */

        /* sds rawkey2, rawval2; */
        /* rio sdsrdb2; */
        /* rioInitWithBuffer(&sdsrdb2, rdbraw); */
        /* rdbKeyDataInitLoadWholeKey(keydata,rawval[0],myhash_key); */
        /* wholekeyRdbLoad(keydata,&sdsrdb2,&rawkey2,&rawval2,&err); */
        /* test_assert(err == 0); */
        /* test_assert(sdscmp(rawkey2,rawkey) == 0); */
        /* test_assert(sdscmp(rawval2,rawval) == 0); */
        /* test_assert(keydata->loadctx.wholekey.evict->type == OBJ_HASH); */
    /* } */

    return error;
}

#endif

