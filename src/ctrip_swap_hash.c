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

void hashTransformBig(robj *o, objectMeta *m) {
    size_t hash_size;
    serverAssert(o && o->type == OBJ_HASH);
    if (m != NULL) return;
    hash_size = objectComputeSize(o,8);
    if (hash_size > server.swap_big_hash_threshold) {
        o->big = 1;
    } else {
        o->big = 0;
    }
}

static void createFakeHashForDeleteIfNeeded(bigHashSwapData *data) {
    if (data->evict) {
        redisDb *db = data->db;
        robj *key = data->key;
        if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
        if (dictSize(db->meta) > 0) dictDelete(db->meta,key->ptr);
        dictDelete(db->evict,key);
        dbAdd(db,key,createHashObject());
    }
}

int bigHashSwapAna(swapData *data_, int cmd_intention,
        uint32_t cmd_intention_flags, struct keyRequest *req,
        int *intention, uint32_t *intention_flags, void *datactx_) {
    bigHashSwapData *data = (bigHashSwapData*)data_;
    bigHashDataCtx *datactx = datactx_;
    UNUSED(req);
    serverAssert(req->num_subkeys >= 0);

    if (intention_flags) *intention_flags = cmd_intention_flags;

    switch(cmd_intention) {
    case SWAP_NOP:
        *intention = SWAP_NOP;
        *intention_flags = 0;
        break;
    case SWAP_IN:
        if (data->meta == NULL) { /* No need to swap in for hot key */
            *intention = SWAP_NOP;
            *intention_flags = 0;
        } else if (req->num_subkeys == 0) {
            if (cmd_intention_flags == INTENTION_IN_DEL) {
                /* DEL/GETDEL: Lazy delete current key. */
                createFakeHashForDeleteIfNeeded(data);
                *intention = SWAP_NOP;
                *intention_flags = 0;
            } else if (cmd_intention_flags == INTENTION_IN_META) {
                /* HSTRLEN: no need to swap in anything, hstrlen command will
                 * be modified like dbsize. */
                *intention = SWAP_NOP;
                *intention_flags = 0;
            } else {
                /* HKEYS/HVALS/..., swap in all fields */
                *intention = SWAP_IN;
                datactx->num = 0;
                datactx->subkeys = NULL;
            }
        } else { /* keyrequests with subkeys */
            datactx->num = 0;
            datactx->subkeys = zmalloc(req->num_subkeys * sizeof(robj*));
            for (int i = 0; i < req->num_subkeys; i++) {
                robj *subkey = req->subkeys[i];
                if (data->value == NULL ||
                        !hashTypeExists(data->value,subkey->ptr)) {
                    incrRefCount(subkey);
                    datactx->subkeys[datactx->num++] = subkey;
                }
            }
            *intention = datactx->num > 0 ? SWAP_IN : SWAP_NOP;
        }
        break;
    case SWAP_OUT:
        if (data->value == NULL) {
            *intention = SWAP_NOP;
        } else {
            unsigned long long evict_memory = 0;
            datactx->subkeys = zmalloc(
                    server.swap_evict_step_max_subkeys*sizeof(robj*));
            hashTypeIterator *hi;
            hi = hashTypeInitIterator(data->value);
            while (hashTypeNext(hi) != C_ERR) {
                robj *subkey;
                unsigned char *vstr;
                unsigned int vlen;
                long long vll;

                hashTypeCurrentObject(hi,OBJ_HASH_KEY,&vstr,&vlen,&vll);
                if (vstr) {
                    subkey = createStringObject((const char*)vstr,vlen);
                } else {
                    subkey = createStringObjectFromLongLong(vll);
                    subkey = unshareStringValue(subkey);
                }
                datactx->subkeys[datactx->num++] = subkey;

                hashTypeCurrentObject(hi,OBJ_HASH_VALUE,&vstr,&vlen,&vll);
                if (vstr)
                    evict_memory += vlen;
                else
                    evict_memory += sizeof(vll);
                if (datactx->num > server.swap_evict_step_max_subkeys ||
                        evict_memory > server.swap_evict_step_max_memory) {
                    /* Evict big hash in small steps. */
                    break;
                }
            }
            hashTypeReleaseIterator(hi);
            *intention = SWAP_OUT;
        }
        break;
    case SWAP_DEL:
        /* lazy expire TODO: confirm why we need to delete now */
        if (cmd_intention_flags != INTENTION_DEL_ASYNC) {
            swapDataSwapDel(data_,datactx);
        }
        *intention = SWAP_NOP;
        *intention_flags = 0;
        break;
    default:
        break;
    }

    return 0;
}

static sds bigHashEncodeSubkey(swapData *data_, sds subkey) {
    bigHashSwapData *data = (bigHashSwapData*)data_;
    return rocksEncodeSubkey(OBJ_HASH,data->key->ptr,subkey);
}

int bigHashEncodeKeys(swapData *data, int intention, void *datactx_,
        int *action, int *numkeys, sds **prawkeys) {
    bigHashDataCtx *datactx = datactx_;
    sds *rawkeys = NULL;

    switch (intention) {
    case SWAP_IN:
        if (datactx->num) { /* Swap in specific fields */
            int i;
            rawkeys = zmalloc(sizeof(sds)*datactx->num);
            for (i = 0; i < datactx->num; i++) {
                rawkeys[i] = bigHashEncodeSubkey(data,
                        datactx->subkeys[i]->ptr);
            }
            *numkeys = datactx->num;
            *prawkeys = rawkeys;
            *action = ROCKS_MULTIGET;
        } else { /* Swap in entire hash. */
            rawkeys = zmalloc(sizeof(sds));
            rawkeys[0] = bigHashEncodeSubkey(data,NULL);
            *numkeys = 1;
            *prawkeys = rawkeys;
            *action = ROCKS_SCAN;
        }
        return C_OK;
    case SWAP_DEL:
    case SWAP_OUT:
    default:
        /* Should not happen .*/
        *action = SWAP_NOP;
        *numkeys = 0;
        *prawkeys = NULL;
        return C_OK;
    }

    return C_OK;
}

static sds bigHashEncodeSubval(robj *subval) {
    return rocksEncodeValRdb(subval);
}

int bigHashEncodeData(swapData *data_, int intention, void *datactx_,
        int *action, int *numkeys, sds **prawkeys, sds **prawvals) {
    bigHashSwapData *data = (bigHashSwapData*)data_;
    bigHashDataCtx *datactx = datactx_;
    if (datactx->num == 0) {
        *action = 0;
        *numkeys = 0;
        *prawkeys = NULL;
        *prawvals = NULL;
        return C_OK;
    }
    sds *rawkeys = zmalloc(datactx->num*sizeof(sds));
    sds *rawvals = zmalloc(datactx->num*sizeof(sds));
    serverAssert(intention == SWAP_OUT);
    for (int i = 0; i < datactx->num; i++) {
        rawkeys[i] = bigHashEncodeSubkey(data_, datactx->subkeys[i]->ptr);
        robj *subval = hashTypeGetValueObject(data->value,datactx->subkeys[i]->ptr);
        serverAssert(subval);
        rawvals[i] = bigHashEncodeSubval(subval);
        decrRefCount(subval);
    }
    *action = ROCKS_WRITE;
    *numkeys = datactx->num;
    *prawkeys = rawkeys;
    *prawvals = rawvals;
    return C_OK;
}

int bigHashDecodeData(swapData *data_, int num, sds *rawkeys,
        sds *rawvals, robj **pdecoded) {
    int i;
    robj *decoded;
    bigHashSwapData *data = (bigHashSwapData*)data_;
    serverAssert(num >= 0);

    if (num == 0) {
        *pdecoded = NULL;
        return C_OK;
    }

    decoded = createHashObject();
    for (i = 0; i < num; i++) {
        sds subkey, subval;
        const char *keystr, *subkeystr;
        size_t klen, slen;
        robj *subvalobj;

        if (rocksDecodeSubkey(rawkeys[i],sdslen(rawkeys[i]),
                &keystr,&klen,&subkeystr,&slen) < 0)
            continue;
        subkey = sdsnewlen(subkeystr,slen);
        serverAssert(memcmp(data->key->ptr,keystr,klen) == 0); //TODO remove

        subvalobj = rocksDecodeValRdb(rawvals[i]);
        serverAssert(subvalobj->type == OBJ_STRING);
        /* subvalobj might be shared integer, unshared it before
         * add to decoded. */
        subvalobj = unshareStringValue(subvalobj);
        /* steal subvalobj sds */
        subval = subvalobj->ptr;
        subvalobj->ptr = NULL;
        decrRefCount(subvalobj);

        hashTypeSet(decoded,subkey,subval,
                HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);
    }
    *pdecoded = decoded;
    return C_OK;
}

static robj *createSwapInObject(robj *newval, robj *evict) {
    robj *swapin = newval;
    serverAssert(newval && newval->type == OBJ_HASH);
    serverAssert(evict && evict->type == OBJ_HASH);
    incrRefCount(newval);
    swapin->lru = evict->lru;
    swapin->big = evict->big;
    return swapin;
}

int bigHashSwapIn(swapData *data_, robj *result, void *datactx_) {
    bigHashSwapData *data = (bigHashSwapData*)data_;
    bigHashDataCtx *datactx = datactx_;
    /* hot key no need to swap in, this must be a warm or cold key. */
    serverAssert(data->meta);
    if (!data->value && result == NULL) {
        /* cold key swapped in nothing: nop. */
    } else if (!data->value && result != NULL) {
        /* cold key swapped in fields */
        /* dup expire/meta satellites before evict deleted. */
        objectMeta *newmeta = dupObjectMeta(data->meta);
        newmeta->len += datactx->meta_len_delta;
        serverAssert(newmeta->len >= 0);
        long long expire = getExpire(data->db,data->key);
        robj *swapin = createSwapInObject(result,data->evict);
        if (expire != -1) removeExpire(data->db,data->key);
        dbDeleteMeta(data->db,data->key);
        dictDelete(data->db->evict,data->key->ptr);
        dbAdd(data->db,data->key,swapin);
        /* re-add expire/meta satellites for db.dict .*/
        if (expire != -1) setExpire(NULL,data->db,data->key,expire);
        if (newmeta->len)
            dbAddMeta(data->db,data->key,newmeta);
        else
            freeObjectMeta(newmeta);
    } else {
        data->meta->len += datactx->meta_len_delta;
        /* all subkeys are swapped in, hash is a hot key now. */
        if (!data->meta->len)
            dbDeleteMeta(data->db,data->key);
    }
    return C_OK;
}

static robj *createSwapOutObject(robj *value, robj *evict) {
    serverAssert(value && !evict);
    robj *swapout = createObject(value->type,NULL);
    swapout->lru = value->lru;
    swapout->big = value->big;
    return swapout;
}

/* subkeys already cleaned by cleanObject(to save cpu usage of main thread),
 * swapout updates keyspace (db.meta/db.evict/db.dict/db.expire). */
int bigHashSwapOut(swapData *data_, void *datactx_) {
    bigHashSwapData *data = (bigHashSwapData*)data_;
    bigHashDataCtx *datactx = datactx_;
    serverAssert(data->value && !data->evict);
    serverAssert(datactx->meta_len_delta >= 0);
    if (!hashTypeLength(data->value)) {
        /* all fields swapped out, key turns into cold. */
        robj *swapout;
        long long expire;
        objectMeta *newmeta = NULL;
        /* dup satellite (expire/meta) before value delete */
        if (data->meta) {
            newmeta = dupObjectMeta(data->meta);
            newmeta->len += datactx->meta_len_delta;
            dbDeleteMeta(data->db,data->key);
        } else {
            /* must have swapped out some fields, otherwise value should
             * not be empty. */
            serverAssert(datactx->meta_len_delta);
            newmeta = createObjectMeta(datactx->meta_len_delta);
        }
        expire = getExpire(data->db,data->key);
        if (expire != -1) removeExpire(data->db,data->key);
        dbDeleteMeta(data->db,data->key);
        dictDelete(data->db->dict,data->key->ptr);
        swapout = createSwapOutObject(data->value,data->evict);
        dbAddEvict(data->db,data->key,swapout);
        if (expire != -1) setExpire(NULL,data->db,data->key,expire);
        dbAddMeta(data->db,data->key,newmeta);
    } else {
        /* not all fields swapped out. */
        if (!data->meta) {
            if (datactx->meta_len_delta == 0) {
                /* hot hash stays hot: nop */
            } else {
                /* hot key turns warm: add meta. */
                objectMeta *meta = createObjectMeta(datactx->meta_len_delta);
                dbAddMeta(data->db,data->key,meta);
            }
        } else {
            /* swap out some. */
            data->meta += datactx->meta_len_delta;
        }
    }
    return C_OK;
}

int bigHashSwapDel(swapData *data_, void *datactx) {
    bigHashSwapData *data = (bigHashSwapData*)data_;
    UNUSED(datactx);
    if (data->value) dbDelete(data->db,data->key);
    if (data->evict) dbDeleteEvict(data->db,data->key);
    return C_OK;
}

robj *bigHashCreateOrMergeObject(swapData *data_, robj *decoded, void *datactx_) {
    robj *result;
    bigHashSwapData *data = (bigHashSwapData*)data_;
    bigHashDataCtx *datactx = datactx_;
    serverAssert(decoded == NULL || decoded->type == OBJ_HASH);
    if (!data->value) {
        result = decoded;
    } else if (decoded == NULL) {
        result = data->value;
    } else {
        hashTypeIterator *hi;
        hi = hashTypeInitIterator(data->value);
        while (hashTypeNext(hi) != C_ERR) {
            sds subkey = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_KEY);
            sds subval = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_VALUE);
            int updated = hashTypeSet(data->value, subkey, subval,
                    HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);
            if (!updated)
                datactx->meta_len_delta--;
        }
        hashTypeReleaseIterator(hi);
        decrRefCount(decoded);
        result = data->value;
    }
    return result;
}

int bigHashCleanObject(swapData *data_, void *datactx_) {
    bigHashSwapData *data = (bigHashSwapData*)data_;
    bigHashDataCtx *datactx = datactx_;
    if (!data->value) return C_OK;
    for (int i = 0; i < datactx->num; i++) {
        if (hashTypeDelete(data->value,datactx->subkeys[i]->ptr)) {
            datactx->meta_len_delta++;
        }
    }
    return C_OK;
}

void freeBigHashSwapData(swapData *data_, void *datactx_) {
    bigHashSwapData *data = (bigHashSwapData*)data_;
    if (data->key) decrRefCount(data->key);
    if (data->value) decrRefCount(data->value);
    if (data->evict) decrRefCount(data->evict);
    /* db.meta is a ref, no need to free. */
    zfree(data);
    bigHashDataCtx *datactx = datactx_;
    for (int i = 0; i < datactx->num; i++) {
        decrRefCount(datactx->subkeys[i]);
    }
    zfree(datactx->subkeys);
    zfree(datactx);
}

swapDataType bigHashSwapDataType = {
    .name = "wholekey",
    .swapAna = bigHashSwapAna,
    .encodeKeys = bigHashEncodeKeys,
    .encodeData = bigHashEncodeData,
    .decodeData = bigHashDecodeData,
    .swapIn = bigHashSwapIn,
    .swapOut = bigHashSwapOut,
    .swapDel = bigHashSwapDel,
    .createOrMergeObject = bigHashCreateOrMergeObject,
    .cleanObject = bigHashCleanObject,
    .free = freeBigHashSwapData,
};

swapData *createBigHashSwapData(redisDb *db, robj *key, robj *value,
        robj *evict, objectMeta *meta, void **pdatactx) {
    bigHashSwapData *data = zmalloc(sizeof(bigHashSwapData));
    data->d.type = &bigHashSwapDataType;
    data->db = db;
    if (key) incrRefCount(key);
    data->key = key;
    if (value) incrRefCount(value);
    data->value = value;
    if (evict) incrRefCount(evict);
    data->evict = evict;
    data->meta = meta;
    bigHashDataCtx *datactx = zmalloc(sizeof(bigHashDataCtx));
    datactx->meta_len_delta = 0;
    datactx->num = 0;
    datactx->subkeys = NULL;
    if (pdatactx) *pdatactx = datactx;

    return (swapData*)data;
}

#ifdef REDIS_TEST

#define SWAP_EVICT_STEP 2
#define SWAP_EVICT_MEM  (1*1024*1024)

int swapDataBigHashTest(int argc, char **argv, int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);
    initTestRedisServer();
    redisDb* db = server.db + 0;
    int error = 0;
    swapData *hash1_data, *cold1_data;
    bigHashDataCtx *hash1_ctx, *cold1_ctx;
    robj *key1, *hash1, *cold1, *cold1_evict;
    objectMeta *cold1_meta;
    keyRequest _kr1, *kr1 = &_kr1, _cold_kr1, *cold_kr1 = &_cold_kr1;
    int intention;
    uint32_t intention_flags;
    robj *subkeys1[4];
    sds f1,f2,f3,f4;
    sds sds1, sds2, int1, int2;
    int action, numkeys;
    sds *rawkeys, *rawvals;
    
    TEST("bigHash - init") {
        server.swap_evict_step_max_subkeys = SWAP_EVICT_STEP;
        server.swap_evict_step_max_memory = SWAP_EVICT_MEM;

        key1 = createStringObject("key1",4);
        cold1 = createStringObject("cold1",5);
        cold1_evict = createObject(OBJ_HASH,NULL);
        cold1_evict->big = 1;
        cold1_meta = createObjectMeta(4);
        f1 = sdsnew("f1"), f2 = sdsnew("f2"), f3 = sdsnew("f3"), f4 = sdsnew("f4");
        sds1 = sdsnew("sds_v1"), sds2 = sdsnew("sds_v2");
        int1 = sdsnew("1"), int2 = sdsnew("2");
        hash1 = createHashObject();
        hash1->big = 1;
        hashTypeSet(hash1,f1,sds1,HASH_SET_COPY);
        hashTypeSet(hash1,f2,sds2,HASH_SET_COPY);
        hashTypeSet(hash1,f3,int1,HASH_SET_COPY);
        hashTypeSet(hash1,f4,int2,HASH_SET_COPY);
        incrRefCount(key1);
        kr1->key = key1;
        kr1->level = REQUEST_LEVEL_KEY;
        kr1->num_subkeys = 0;
        kr1->subkeys = NULL;
        incrRefCount(key1);
        cold_kr1->key = key1;
        cold_kr1->level = REQUEST_LEVEL_KEY;
        cold_kr1->num_subkeys = 0;
        cold_kr1->subkeys = NULL;
        dbAdd(db,key1,hash1);
        hash1_data = createBigHashSwapData(db,key1,hash1,NULL,NULL,(void**)&hash1_ctx);
        cold1_data = createBigHashSwapData(db,cold1,NULL,cold1_evict,cold1_meta,(void**)&cold1_ctx);
    }

    TEST("bigHash - swapAna") {
        /* nop: NOP/IN_META/IN_DEL/IN hot/OUT cold/DEL_ASYNC... */
        swapDataAna(hash1_data,SWAP_NOP,0,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        swapDataAna(hash1_data,SWAP_IN,INTENTION_IN_META,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        swapDataAna(hash1_data,SWAP_IN,INTENTION_IN_DEL,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        swapDataAna(hash1_data,SWAP_IN,0,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        swapDataAna(hash1_data,SWAP_IN,0,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        swapDataAna(cold1_data,SWAP_OUT,0,cold_kr1,&intention,&intention_flags,cold1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        swapDataAna(cold1_data,SWAP_DEL,INTENTION_DEL_ASYNC,cold_kr1,&intention,&intention_flags,cold1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        /* in: entire or with subkeys */
        swapDataAna(cold1_data,SWAP_IN,0,cold_kr1,&intention,&intention_flags,cold1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == 0);
        test_assert(cold1_ctx->num == 0 && cold1_ctx->subkeys == NULL);
        subkeys1[0] = createStringObject(f1,sdslen(f1));
        subkeys1[1] = createStringObject(f2,sdslen(f2));
        cold_kr1->num_subkeys = 2;
        cold_kr1->subkeys = subkeys1;
        swapDataAna(cold1_data,SWAP_IN,0,cold_kr1,&intention,&intention_flags,cold1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == 0);
        test_assert(cold1_ctx->num == 2 && cold1_ctx->subkeys != NULL);
        /* out: evict by small steps */
        kr1->num_subkeys = 0;
        kr1->subkeys = NULL;
        swapDataAna(hash1_data,SWAP_OUT,0,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_OUT && intention_flags == 0);
        test_assert(cold1_ctx->num == SWAP_EVICT_STEP && cold1_ctx->subkeys != NULL);
    }

    TEST("bigHash - encodeData/DecodeData") {
        robj *decoded;
        size_t old = server.swap_evict_step_max_subkeys;
        server.swap_evict_step_max_subkeys = 1024;
        kr1->num_subkeys = 0;
        kr1->subkeys = NULL;
        zfree(hash1_ctx->subkeys), hash1_ctx->subkeys = NULL;
        hash1_ctx->num = 0;
        swapDataAna(hash1_data,SWAP_OUT,0,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_OUT && intention_flags == 0);
        test_assert(hash1_ctx->num == (int)hashTypeLength(((bigHashSwapData*)hash1_data)->value));
        serverAssert(hash1_ctx->subkeys != NULL);

        bigHashEncodeData(hash1_data,intention,hash1_ctx,
                &action,&numkeys,&rawkeys,&rawvals);
        test_assert(action == ROCKS_WRITE);
        test_assert(numkeys == hash1_ctx->num);

        bigHashDecodeData(hash1_data,numkeys,rawkeys,rawvals,&decoded);
        test_assert(hashTypeLength(decoded) == hashTypeLength(hash1));

        server.swap_evict_step_max_subkeys = old;
    }

    TEST("bigHash - swapIn/swapOut") {
        robj *h, *e;
        objectMeta *m;
        bigHashSwapData _data = *(bigHashSwapData*)hash1_data, *data = &_data;
        test_assert(lookupMeta(db,key1) == NULL);

        /* hot => warm => cold */
        hashTypeDelete(hash1,f1);
        hashTypeDelete(hash1,f2);
        hash1_ctx->meta_len_delta = 2;
        bigHashSwapOut((swapData*)data, hash1_ctx);
        test_assert((m =lookupMeta(db,key1)) != NULL && m->len == 2);
        test_assert(lookupEvictKey(db,key1) == NULL);

        hashTypeDelete(hash1,f3);
        hashTypeDelete(hash1,f4);
        hash1_ctx->meta_len_delta = 2;
        data->meta = lookupMeta(data->db,data->key);
        bigHashSwapOut((swapData*)data, hash1_ctx);
        test_assert((m = lookupMeta(db,key1)) != NULL && m->len == 4);
        test_assert((e = lookupEvictKey(db,key1)) != NULL);
        test_assert((h = lookupKey(db,key1,LOOKUP_NOTOUCH)) == NULL);

        /* cold => warm => hot */
        hash1 = createHashObject();
        hashTypeSet(hash1,f1,sds1,HASH_SET_COPY);
        hashTypeSet(hash1,f2,sds2,HASH_SET_COPY);
        hash1_ctx->meta_len_delta = -2;
        data->value = h;
        data->evict = e;
        data->meta = m;
        bigHashSwapIn((swapData*)data,hash1,hash1_ctx);
        test_assert((m = lookupMeta(db,key1)) != NULL && m->len == 2);
        test_assert((e = lookupEvictKey(db,key1)) == NULL);
        test_assert((h = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(h->big && hashTypeLength(h) == 2);

        hashTypeSet(hash1,f3,int1,HASH_SET_COPY);
        hashTypeSet(hash1,f4,int2,HASH_SET_COPY);
        hash1_ctx->meta_len_delta = -2;
        data->value = h;
        data->evict = e;
        data->meta = m;
        bigHashSwapIn((swapData*)data,hash1,hash1_ctx);
        test_assert((m = lookupMeta(db,key1)) == NULL);
        test_assert((e = lookupEvictKey(db,key1)) == NULL);
        test_assert((h = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(h->big && hashTypeLength(h) == 4);

        /* hot => cold */
        hashTypeDelete(hash1,f1);
        hashTypeDelete(hash1,f2);
        hashTypeDelete(hash1,f3);
        hashTypeDelete(hash1,f4);
        hash1_ctx->meta_len_delta = 4;
        *data = *(bigHashSwapData*)hash1_data;
        bigHashSwapOut((swapData*)data, hash1_ctx);
        test_assert((m =lookupMeta(db,key1)) != NULL && m->len == 4);
        test_assert((e = lookupEvictKey(db,key1)) != NULL);
        test_assert((h = lookupKey(db,key1,LOOKUP_NOTOUCH)) == NULL);

        /* cold => hot */
        hash1 = createHashObject();
        hashTypeSet(hash1,f1,sds1,HASH_SET_COPY);
        hashTypeSet(hash1,f2,sds2,HASH_SET_COPY);
        hashTypeSet(hash1,f3,int1,HASH_SET_COPY);
        hashTypeSet(hash1,f4,int2,HASH_SET_COPY);
        data->value = h;
        data->meta = m;
        data->evict = e;
        hash1_ctx->meta_len_delta = -4;
        bigHashSwapIn((swapData*)data,hash1,hash1_ctx);
        test_assert((m = lookupMeta(db,key1)) == NULL);
        test_assert((e = lookupEvictKey(db,key1)) == NULL);
        test_assert((h = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(h->big && hashTypeLength(h) == 4);
    }

    return error;
}

#endif

