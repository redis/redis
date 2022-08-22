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

int wholeKeySwapAna(swapData *data_, struct keyRequest *req,
        int *intention, uint32_t *intention_flags, void *datactx) {
    wholeKeySwapData *data = (wholeKeySwapData*)data_;
    int cmd_intention = req->cmd_intention;
    uint32_t cmd_intention_flags = req->cmd_intention_flags;

    UNUSED(req), UNUSED(datactx);

    if (intention_flags) *intention_flags = cmd_intention_flags;

    switch(cmd_intention) {
    case SWAP_NOP:
        *intention = SWAP_NOP;
        break;
    case SWAP_IN:
        if (!data->d.value) {
            *intention = SWAP_IN;
        } else if (data->d.value) {
            if (cmd_intention_flags & INTENTION_CMD_IN_DEL) {
                *intention = SWAP_DEL;
                *intention_flags = INTENTION_FIN_NO_DEL;
            } else {
                *intention = SWAP_NOP;
            }
        } else {
            *intention = SWAP_NOP;
        }
        break;
    case SWAP_OUT:
        if (data->d.value) {
            if (data->d.value->dirty) {
                *intention = SWAP_OUT;
            } else {
                /* Not dirty: swapout right away without swap. */
                swapDataSwapOut(data_, NULL);
                *intention = SWAP_NOP;
            }
        } else {
            *intention = SWAP_NOP;
        }
        break;
    case SWAP_DEL:
        *intention = SWAP_DEL;
        *intention_flags |= INTENTION_EXEC_DEL_META;
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

swapDataType wholeKeySwapDataType = {
    .name = "wholekey",
    .swapAna = wholeKeySwapAna,
    .encodeKeys = wholeKeyEncodeKeys,
    .encodeData = wholeKeyEncodeData,
    .decodeData = wholeKeyDecodeData,
    .encodeMetaVal = NULL,
    .decodeMetaVal = NULL,
    .swapIn = wholeKeySwapIn,
    .swapOut = wholeKeySwapOut,
    .swapDel = wholeKeySwapDel,
    .createOrMergeObject = wholeKeyCreateOrMergeObject,
    .cleanObject = NULL,
    .free = NULL,
};

int swapDataSetupWholeKey(swapData *d, void *object_meta, void **datactx) {
    serverAssert(object_meta == NULL);
    d->type = &wholeKeySwapDataType;
    *datactx = NULL;
    return 0;
}

/* ------------------- whole key rdb swap -------------------------------- */
rdbKeyType wholekeyRdbType = {
    .save_start = NULL,
    .save = wholekeySave,
    .save_end = NULL,
    .save_deinit = NULL,
    .load_start = wholekeyRdbLoadStart,
    .load = wholekeyRdbLoad,
    .load_end = NULL,
    .load_dbadd = NULL,
    .load_expired = NULL, /* TODO opt: delete saved key in datacf. */
    .load_deinit = NULL,
};

void rdbKeyDataInitSaveWholeKey(rdbKeyData *keydata, robj *value,
        long long expire) {
    rdbKeyDataInitSaveKey(keydata,value,expire);
    keydata->type = &wholekeyRdbType;
    keydata->savectx.type = RDB_KEY_TYPE_WHOLEKEY;
    serverAssert(value == NULL);
}

int wholekeySave(rdbKeyData *keydata, rio *rdb, decodeResult *decoded) {
    robj keyobj;

    serverAssert(decoded->cfid == DATA_CF); 
    initStaticStringObject(keyobj,decoded->key);

    //TODO opt: save key LFU in metaCF
    if (rdbSaveKeyHeader(rdb,&keyobj,&keyobj,
                decoded->cf.data.rdbtype,
                keydata->savectx.expire) == -1) {
        return -1;
    }

    if (rdbWriteRaw(rdb,decoded->cf.data.rdbraw,
                sdslen(decoded->cf.data.rdbraw)) == -1) {
        return -1;
    }

    return 0;
}

void rdbKeyDataInitLoadWholeKey(rdbKeyData *keydata, int rdbtype, redisDb *db,
        sds key, long long expire, long long now) {
    rdbKeyDataInitLoadKey(keydata,rdbtype,db,key,expire,now);
    keydata->type = &wholekeyRdbType;
    keydata->loadctx.type = RDB_KEY_TYPE_WHOLEKEY;
}

sds empty_hash_ziplist_verbatim;

void initSwapWholeKey() {
    robj *emptyhash = createHashObject();
    empty_hash_ziplist_verbatim = rocksEncodeValRdb(emptyhash);
    decrRefCount(emptyhash);
}

static inline int hashZiplistVerbatimIsEmpty(sds verbatim) {
    return !sdscmp(empty_hash_ziplist_verbatim, verbatim);
}

int wholekeyRdbLoadStart(struct rdbKeyData *keydata, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    UNUSED(rdb);
    *cf = META_CF;
    *rawkey = rocksEncodeMetaKey(keydata->loadctx.db,keydata->loadctx.key);
    *rawval = rocksEncodeMetaVal(keydata->loadctx.type,keydata->loadctx.expire,NULL);
    *error = 0;
    return 0;
}

int wholekeyRdbLoad(struct rdbKeyData *keydata, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    *error = RDB_LOAD_ERR_OTHER;
    int rdbtype = keydata->loadctx.rdbtype;
    sds verbatim = NULL, key = keydata->loadctx.key;
    redisDb *db = keydata->loadctx.db;

    serverAssert(rdbtype == RDB_TYPE_STRING);

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


#ifdef REDIS_TEST0
#include <stdio.h>
#include <limits.h>
#include <assert.h>
int swapDataWholeKeyTest(int argc, char **argv, int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);
    initTestRedisServer();
    redisDb* db = server.db + 0;
    int error = 0;

    initSwapWholeKey();

    TEST("wholeKey SwapAna value = NULL and evict = NULL") {
        void* ctx = NULL;
        swapData* data = createWholeKeySwapData(db, NULL, NULL, NULL, &ctx);
        int intention;
        uint32_t intention_flags;
        wholeKeySwapAna(data, SWAP_NOP, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        wholeKeySwapAna(data, SWAP_IN, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        wholeKeySwapAna(data, SWAP_OUT, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        wholeKeySwapAna(data, SWAP_DEL, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        
        freeWholeKeySwapData(data, ctx);
    }

    TEST("wholeKey SwapAna value != NULL and evict = NULL") {
        void* ctx = NULL;
        robj* value  = createRawStringObject("value", 5);
        swapData* data = createWholeKeySwapData(db, NULL, value, NULL, &ctx);
        int intention;
        uint32_t intention_flags;
        wholeKeySwapAna(data, SWAP_NOP, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        wholeKeySwapAna(data, SWAP_IN, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        wholeKeySwapAna(data, SWAP_IN, INTENTION_IN_DEL, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_DEL);
        test_assert(intention_flags == INTENTION_DEL_ASYNC);
        wholeKeySwapAna(data, SWAP_OUT, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_OUT);
        wholeKeySwapAna(data, SWAP_DEL, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_DEL);
        wholeKeySwapAna(data, SWAP_DEL, INTENTION_DEL_ASYNC, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_DEL);
        test_assert(intention_flags == INTENTION_DEL_ASYNC);
        freeWholeKeySwapData(data, ctx);
    }

    TEST("wholeKey SwapAna value = NULL and evict != NULL") {
        void* ctx = NULL;
        robj* evict  = createRawStringObject("value", 5);
        swapData* data = createWholeKeySwapData(db, NULL, NULL, evict, &ctx);
        int intention;
        uint32_t intention_flags;
        wholeKeySwapAna(data, SWAP_NOP, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        wholeKeySwapAna(data, SWAP_IN, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_IN);
        wholeKeySwapAna(data, SWAP_IN, INTENTION_IN_DEL, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_IN);
        test_assert(intention_flags == INTENTION_IN_DEL);
        wholeKeySwapAna(data, SWAP_OUT, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_NOP);
        wholeKeySwapAna(data, SWAP_DEL, 0, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_DEL);
        wholeKeySwapAna(data, SWAP_DEL, INTENTION_DEL_ASYNC, NULL, &intention, &intention_flags, ctx);
        test_assert(intention == SWAP_DEL);
        test_assert(intention_flags == INTENTION_DEL_ASYNC);
        freeWholeKeySwapData(data, ctx);
    }

    TEST("wholeKey EncodeKeys String") {
        void* ctx = NULL;
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        robj* evict  = NULL;
        swapData* data = createWholeKeySwapData(db, key, value, evict, &ctx);
        int numkeys, action;
        sds *rawkeys = NULL;
        int result = wholeKeyEncodeKeys(data, SWAP_IN, ctx, &action, &numkeys, &rawkeys);
        test_assert(numkeys == 1);
        test_assert(strcmp(rawkeys[0], "Kkey")== 0);
        test_assert(result == C_OK);
        freeWholeKeySwapData(data, ctx);
    }

    TEST("wholeKey EncodeKeys String KEY + VALUE") {
        void* ctx = NULL;
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        robj* evict  = NULL;
        swapData* data = createWholeKeySwapData(db, key, value, evict, &ctx);
        int numkeys, action;
        sds *rawkeys = NULL;
        int result = wholeKeyEncodeKeys(data, SWAP_IN, ctx, &action, &numkeys, &rawkeys);
        test_assert(ROCKS_GET == action);
        test_assert(numkeys == 1);
        test_assert(strcmp(rawkeys[0], "Kkey")== 0);
        test_assert(result == C_OK);
        result = wholeKeyEncodeKeys(data, SWAP_DEL, ctx, &action, &numkeys, &rawkeys);
        test_assert(ROCKS_DEL == action);
        test_assert(numkeys == 1);
        test_assert(strcmp(rawkeys[0], "Kkey")== 0);
        test_assert(result == C_OK);
        // result = wholeKeyEncodeKeys(data, SWAP_OUT, ctx, &action, &numkeys, &rawkeys);
        // serverAssert(numkeys == 0);
        // serverAssert(result == C_ERR);
        freeWholeKeySwapData(data, ctx);
    }

    TEST("wholeKey EncodeKeys String KEY + EVICT") {
        void* ctx = NULL;
        robj* key = createRawStringObject("key", 3);
        robj* evict  = createRawStringObject("value", 5);
        robj* value  = NULL;
        swapData* data = createWholeKeySwapData(db, key, value, evict, &ctx);
        int numkeys, action;
        sds *rawkeys = NULL;
        int result = wholeKeyEncodeKeys(data, SWAP_IN, ctx, &action, &numkeys, &rawkeys);
        test_assert(ROCKS_GET == action);
        test_assert(numkeys == 1);
        test_assert(strcmp(rawkeys[0], "Kkey")== 0);
        test_assert(result == C_OK);
        result = wholeKeyEncodeKeys(data, SWAP_DEL, ctx, &action, &numkeys, &rawkeys);
        test_assert(ROCKS_DEL == action);
        test_assert(numkeys == 1);
        test_assert(strcmp(rawkeys[0], "Kkey")== 0);
        test_assert(result == C_OK);
        // result = wholeKeyEncodeKeys(data, SWAP_OUT, ctx, &action, &numkeys, &rawkeys);
        // serverAssert(numkeys == 0);
        // serverAssert(result == C_ERR);
        freeWholeKeySwapData(data, ctx);
    }

    TEST("wholeKey EncodeData + DecodeData") {
        void* ctx = NULL;
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        robj* evict  = NULL;
        swapData* data = createWholeKeySwapData(db, key, value, evict, &ctx);
        int numkeys, action;
        sds *rawkeys = NULL, *rawvals = NULL;
        int result = wholeKeyEncodeData(data, SWAP_OUT, ctx, &action, &numkeys, &rawkeys, &rawvals);
        test_assert(result == C_OK);
        test_assert(ROCKS_PUT == action);
        test_assert(numkeys == 1);
        test_assert(strcmp(rawkeys[0], "Kkey") == 0);
        robj* decoded;
        result = wholeKeyDecodeData(data, numkeys, rawkeys, rawvals, &decoded);
        test_assert(result == C_OK);
        test_assert(strcmp(decoded->ptr ,"value") == 0);
        freeWholeKeySwapData(data, ctx);
    }
    
    TEST("wholeKey swapIn (not exist expire)") {
        robj *decoded;
        robj* key = createRawStringObject("key", 3);
        robj* value  = NULL;
        robj* evict  = createRawStringObject("value", 5);
        swapData* data = createWholeKeySwapData(db, key, value, evict, NULL);
        decoded = createRawStringObject("value", 5);
        test_assert(wholeKeySwapIn(data, decoded, NULL) == 0);
        test_assert(dictFind(db->dict, key->ptr) != NULL);    
        decoded = NULL;
        freeWholeKeySwapData(data, NULL);
        clearTestRedisDb();
        
    }

    TEST("WHoleKey swapIn (exist expire)") {
        robj* key = createRawStringObject("key", 3);
        robj* evict  = createRawStringObject("value", 5);
        robj* value  = NULL, *decoded = NULL;
        test_assert(dictFind(db->dict, key->ptr) == NULL);   
        //mock 
        dbAddEvict(db, key, evict);
        setExpire(NULL,db, key, 1000000);

        swapData* data = createWholeKeySwapData(db, key, value, evict, NULL);
        decoded = createRawStringObject("value", 5);
        test_assert(wholeKeySwapIn(data,decoded,NULL) == 0);
        test_assert(dictFind(db->dict, key->ptr) != NULL);    
        test_assert(getExpire(db, key) > 0);
        decoded = NULL;
        freeWholeKeySwapData(data, NULL);
        clearTestRedisDb();

    }

    TEST("wholeKey swapout (not exist expire)") {
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        robj* evict  = NULL;
        test_assert(dictFind(db->dict, key->ptr) == NULL);  
        test_assert(dictFind(db->evict, key->ptr) == NULL);  
        //mock 
        dbAdd(db, key, value);
        test_assert(dictFind(db->dict, key->ptr) != NULL); 
        test_assert(dictFind(db->evict, key->ptr) == NULL);   

        swapData* data = createWholeKeySwapData(db, key, value, evict, NULL);
        test_assert(wholeKeySwapOut(data, NULL) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(dictFind(db->evict, key->ptr) != NULL);    
        test_assert(getExpire(db, key) < 0);
        freeWholeKeySwapData(data, NULL);
        clearTestRedisDb();
    }

    TEST("wholeKey swapout (exist expire)") {
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        robj* evict  = NULL;
        test_assert(dictFind(db->dict, key->ptr) == NULL);  
        test_assert(dictFind(db->evict, key->ptr) == NULL);  
        //mock 
        dbAdd(db, key, value);
        setExpire(NULL, db, key, 1000000);
        test_assert(dictFind(db->dict, key->ptr) != NULL); 
        test_assert(dictFind(db->evict, key->ptr) == NULL);   

        swapData* data = createWholeKeySwapData(db, key, value, evict, NULL);
        test_assert(wholeKeySwapOut(data, NULL) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(dictFind(db->evict, key->ptr) != NULL);    
        test_assert(getExpire(db, key) > 0);
        freeWholeKeySwapData(data, NULL);
        clearTestRedisDb();
    }
    
    TEST("wholeKey swapdelete (value and not exist expire)") {
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        robj* evict  = NULL;
        test_assert(dictFind(db->dict, key->ptr) == NULL);  
        test_assert(dictFind(db->evict, key->ptr) == NULL);  
        //mock 
        dbAdd(db, key, value);
        test_assert(dictFind(db->dict, key->ptr) != NULL); 
        test_assert(dictFind(db->evict, key->ptr) == NULL);   

        swapData* data = createWholeKeySwapData(db, key, value, evict, NULL);
        test_assert(wholeKeySwapDel(data, NULL, 0) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(dictFind(db->evict, key->ptr) == NULL);    
        test_assert(getExpire(db, key) < 0);
        freeWholeKeySwapData(data, NULL);
        clearTestRedisDb();
    }


    TEST("wholeKey swapdelete (value and exist expire)") {
        robj* key = createRawStringObject("key", 3);
        robj* value  = createRawStringObject("value", 5);
        robj* evict  = NULL;
        test_assert(dictFind(db->dict, key->ptr) == NULL);  
        test_assert(dictFind(db->evict, key->ptr) == NULL);  
        //mock 
        dbAdd(db, key, value);
        setExpire(NULL, db, key, 1000000);
        test_assert(dictFind(db->dict, key->ptr) != NULL); 
        test_assert(dictFind(db->evict, key->ptr) == NULL);   
        test_assert(getExpire(db, key) > 0); 

        swapData* data = createWholeKeySwapData(db, key, value, evict, NULL);
        test_assert(wholeKeySwapDel(data, NULL, 0) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(dictFind(db->evict, key->ptr) == NULL);    
        test_assert(getExpire(db, key) < 0);
        freeWholeKeySwapData(data, NULL);
        clearTestRedisDb();
    }

    TEST("wholeKey swapdelete (evict and not exist expire)") {
        robj* key = createRawStringObject("key", 3);
        robj* evict  = createRawStringObject("value", 5);
        robj* value  = NULL;
        test_assert(dictFind(db->dict, key->ptr) == NULL);  
        test_assert(dictFind(db->evict, key->ptr) == NULL);  
        //mock 
        dbAddEvict(db, key, evict);
        test_assert(dictFind(db->dict, key->ptr) == NULL); 
        test_assert(dictFind(db->evict, key->ptr) != NULL);   

        swapData* data = createWholeKeySwapData(db, key, value, evict, NULL);
        test_assert(wholeKeySwapDel(data, NULL, 0) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(dictFind(db->evict, key->ptr) == NULL);    
        test_assert(getExpire(db, key) < 0);
        freeWholeKeySwapData(data, NULL);
        clearTestRedisDb();
    }


    TEST("wholeKey swapdelete (evict and exist expire)") {
        robj* key = createRawStringObject("key", 3);
        robj* evict  = createRawStringObject("value", 5);
        robj* value  = NULL;
        test_assert(dictFind(db->dict, key->ptr) == NULL);  
        test_assert(dictFind(db->evict, key->ptr) == NULL);  
        //mock 
        dbAddEvict(db, key, evict);
        setExpire(NULL, db, key, 1000000);
        test_assert(dictFind(db->dict, key->ptr) == NULL); 
        test_assert(dictFind(db->evict, key->ptr) != NULL);   
        test_assert(getExpire(db, key) > 0); 

        swapData* data = createWholeKeySwapData(db, key, value, evict, NULL);
        test_assert(wholeKeySwapDel(data, NULL, 0) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(dictFind(db->evict, key->ptr) == NULL);    
        test_assert(getExpire(db, key) < 0);
        freeWholeKeySwapData(data, NULL);
        clearTestRedisDb();
    }

    TEST("wholeKey swapdelete (evict and exist expire)") {
    }

    int rocksDecodeRaw(sds rawkey, unsigned char rdbtype, sds rdbraw, decodeResult *decoded);

    TEST("wholeKey rdb save & load") {
        int err;
        rdbKeyData _keydata, *keydata = &_keydata;
        rio sdsrdb;
        robj *evict = createObject(OBJ_HASH,NULL);
        decodeResult _decoded, *decoded = &_decoded;

		robj *myhash;
		sds myhash_key;
		myhash_key = sdsnew("myhash");
        myhash = createHashObject();

        hashTypeSet(myhash,sdsnew("f1"),sdsnew("v1"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);
        hashTypeSet(myhash,sdsnew("f2"),sdsnew("v2"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);
        hashTypeSet(myhash,sdsnew("f3"),sdsnew("v3"),HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);

        sds rawkey = rocksEncodeKey(ENC_TYPE_HASH,myhash_key);
        sds rawval = rocksEncodeValRdb(myhash);

        sds rdbraw = sdsnewlen(rawval+1,sdslen(rawval)-1);
        rocksDecodeRaw(sdsdup(rawkey),rawval[0],rdbraw,decoded);
        test_assert(decoded->enc_type == ENC_TYPE_HASH);
        test_assert(!sdscmp(decoded->key,myhash_key));

        rioInitWithBuffer(&sdsrdb, sdsempty());
        rdbKeyDataInitSaveWholeKey(keydata,NULL,evict,-1);
        test_assert(wholekeySave(keydata,&sdsrdb,decoded) == 0);

        sds rawkey2, rawval2;
        rio sdsrdb2;
        rioInitWithBuffer(&sdsrdb2, rdbraw);
        rdbKeyDataInitLoadWholeKey(keydata,rawval[0],myhash_key);
        wholekeyRdbLoad(keydata,&sdsrdb2,&rawkey2,&rawval2,&err);
        test_assert(err == 0);
        test_assert(sdscmp(rawkey2,rawkey) == 0);
        test_assert(sdscmp(rawval2,rawval) == 0);
        test_assert(keydata->loadctx.wholekey.evict->type == OBJ_HASH);
    }

    return error;
}

#endif

