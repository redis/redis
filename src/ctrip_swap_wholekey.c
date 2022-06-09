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

int wholeKeySwapAna(swapData *data_, int cmd_intention,
        uint32_t cmd_intention_flags, struct keyRequest *req,
        int *intention, uint32_t *intention_flags, void *datactx) {
    wholeKeySwapData *data = (wholeKeySwapData*)data_;
    UNUSED(req), UNUSED(datactx);
    if (intention_flags) *intention_flags = cmd_intention_flags;
    switch(cmd_intention) {
    case SWAP_NOP:
        *intention = SWAP_NOP;
        break;
    case SWAP_IN:
        if (data->evict) {
            serverAssert(!data->value);
            *intention = SWAP_IN;
        } else if (data->value) {
            serverAssert(!data->evict);
            if (cmd_intention_flags & INTENTION_IN_DEL) {
                *intention = SWAP_DEL;
                *intention_flags = INTENTION_DEL_ASYNC;
            } else {
                *intention = SWAP_NOP;
            }
        } else {
            *intention = SWAP_NOP;
        }
        break;
    case SWAP_OUT:
        if (data->value) {
            serverAssert(!data->evict);
            if (data->value->dirty) {
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
        if (data->value || data->evict) {
            *intention = SWAP_DEL;
        } else {
            *intention = SWAP_NOP;
        }
        break;
    default:
        break;
    }

    return 0;
}

static sds wholeKeyEncodeKey(swapData *data_) {
    int obj_type = 0;
    wholeKeySwapData *data = (wholeKeySwapData*)data_;
    if (data->value) obj_type = data->value->type;
    if (data->evict) obj_type = data->evict->type;
    return rocksEncodeKey(obj_type, data->key->ptr);
}

int wholeKeyEncodeKeys(swapData *data, int intention, void *datactx,
        int *action, int *numkeys, sds **prawkeys) {
    sds *rawkeys = zmalloc(sizeof(sds*));
    UNUSED(datactx);
    rawkeys[0] = wholeKeyEncodeKey(data);
    *numkeys = 1;
    *prawkeys = rawkeys;

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

static sds wholeKeyEncodeVal(swapData *data_) {
    wholeKeySwapData *data = (wholeKeySwapData*)data_;
    if (data->value) {
        return rocksEncodeValRdb(data->value);
    } else {
        return NULL;
    }
}

int wholeKeyEncodeData(swapData *data, int intention, void *datactx,
        int *action, int *numkeys, sds **prawkeys, sds **prawvals) {
    UNUSED(datactx);
    serverAssert(intention == SWAP_OUT);
    sds *rawkeys = zmalloc(sizeof(sds*));
    sds *rawvals = zmalloc(sizeof(sds*));
    rawkeys[0] = wholeKeyEncodeKey(data);
    rawvals[0] = wholeKeyEncodeVal(data);
    *action = ROCKS_PUT;
    *numkeys = 1;
    *prawkeys = rawkeys;
    *prawvals = rawvals;
    return C_OK;
}

int wholeKeyDecodeData(swapData *data, int num, sds *rawkeys,
        sds *rawvals, robj **pdecoded) {
    serverAssert(num == 1);
    UNUSED(data);
    UNUSED(rawkeys);
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

static robj *createSwapInObject(robj *newval, robj *evict) {
    robj *swapin = newval;
    serverAssert(newval);
    incrRefCount(newval);
    serverAssert(evict);
    serverAssert(evict->type == newval->type);
    /* Copy swapin object before modifing If newval is shared object. */
    if (newval->refcount == OBJ_SHARED_REFCOUNT)
        swapin = dupSharedObject(newval);
    swapin->lru = evict->lru;
    swapin->dirty = 0;
    return swapin;
}

int wholeKeySwapIn(swapData *data_, robj *result, void *datactx) {
    wholeKeySwapData *data = (wholeKeySwapData*)data_;
    UNUSED(datactx);
    robj *swapin;
    long long expire;
    expire = getExpire(data->db,data->key);
    swapin = createSwapInObject(result,data->evict);
    if (expire != -1) removeExpire(data->db,data->key);
    dictDelete(data->db->evict,data->key->ptr);
    dbAdd(data->db,data->key,swapin);
    if (expire != -1) setExpire(NULL,data->db,data->key,expire);
    return 0;
}

static robj *createSwapOutObject(robj *value, robj *evict) {
    robj *swapout;

    serverAssert(value);
    serverAssert(evict == NULL);

    if (evict == NULL) {
        swapout = createObject(value->type, NULL);
    } else {
        incrRefCount(evict);
        swapout = evict;
    }

    swapout->lru = value->lru;
    swapout->type = value->type;

    return swapout;
}

int wholeKeySwapOut(swapData *data_, void *datactx) {
    robj *swapout;
    long long expire;
    wholeKeySwapData *data = (wholeKeySwapData*)data_;
    UNUSED(datactx);
    expire = getExpire(data->db,data->key);
    if (expire != -1) removeExpire(data->db,data->key);
    swapout = createSwapOutObject(data->value, data->evict);
    if (data->evict) dictDelete(data->db->evict, data->key->ptr);
    if (data->value) dictDelete(data->db->dict, data->key->ptr);
    dbAddEvict(data->db,data->key,swapout);
    if (expire != -1) setExpire(NULL,data->db,data->key,expire);
    return 0;
}

int wholeKeySwapDel(swapData *data_, void *datactx) {
    wholeKeySwapData *data = (wholeKeySwapData*)data_;
    UNUSED(datactx);
    if (dictSize(data->db->expires) > 0)
        dictDelete(data->db->expires,data->key->ptr);
    if (data->value) dbDelete(data->db,data->key);
    if (data->evict) dbDeleteEvict(data->db,data->key);
    return 0;
}

robj *wholeKeyCreateOrMergeObject(swapData *data, robj *decoded, void *datactx) {
    UNUSED(data);
    UNUSED(datactx);
    serverAssert(decoded);
    return decoded;
}

void freeWholeKeySwapData(swapData *data_, void *datactx) {
    wholeKeySwapData *data = (wholeKeySwapData*)data_;
    UNUSED(datactx);
    if (data->key) decrRefCount(data->key);
    if (data->value) decrRefCount(data->value);
    if (data->evict) decrRefCount(data->evict);
    zfree(data);
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
    /* TODO OPT: zset/set/list could clean subkey in cleanObject to reduce
     * cpu usage in main thread. */
    .cleanObject = NULL,
    .free = freeWholeKeySwapData,
};

swapData *createWholeKeySwapData(redisDb *db, robj *key, robj *value,
        robj *evict, void **pdatactx) {
    wholeKeySwapData *data = zmalloc(sizeof(wholeKeySwapData));
    if (pdatactx) *pdatactx = NULL;
    data->d.type = &wholeKeySwapDataType;
    data->db = db;
    if (key) incrRefCount(key);
    data->key = key;
    if (value) incrRefCount(value);
    data->value = value;
    if (evict) incrRefCount(evict);
    data->evict = evict;
    return (swapData*)data;
}

#ifdef REDIS_TEST
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
        test_assert(wholeKeySwapDel(data, NULL) == 0);
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
        test_assert(wholeKeySwapDel(data, NULL) == 0);
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
        test_assert(wholeKeySwapDel(data, NULL) == 0);
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
        test_assert(wholeKeySwapDel(data, NULL) == 0);
        test_assert(dictFind(db->dict, key->ptr) == NULL);    
        test_assert(dictFind(db->evict, key->ptr) == NULL);    
        test_assert(getExpire(db, key) < 0);
        freeWholeKeySwapData(data, NULL);
        clearTestRedisDb();
    }

    TEST("wholeKey swapdelete (evict and exist expire)") {
    }
    return error;
}

#endif
