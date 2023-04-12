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

static void createFakeSetForDeleteIfCold(swapData *data) {
    if (swapDataIsCold(data)) {
        /* empty set allowed */
        dbAdd(data->db,data->key,createSetObject());
    }
}

int setSwapAna(swapData *data, struct keyRequest *req,
                int *intention, uint32_t *intention_flags, void *datactx_) {
    setDataCtx *datactx = datactx_;
    int cmd_intention = req->cmd_intention;
    uint32_t cmd_intention_flags = req->cmd_intention_flags;

    serverAssert(req->type == KEYREQUEST_TYPE_SUBKEY);
    serverAssert(req->b.num_subkeys >= 0);

    switch (cmd_intention) {
        case SWAP_NOP:
            *intention = SWAP_NOP;
            *intention_flags = 0;
            break;
        case SWAP_IN:
            if (!swapDataPersisted(data)) {
                /* No need to swap for pure hot key */
                *intention = SWAP_NOP;
                *intention_flags = 0;
            } else if (req->b.num_subkeys == 0) {
                if (cmd_intention_flags == SWAP_IN_DEL_MOCK_VALUE) {
                    /* DEL/GETDEL: Lazy delete current key. */
                    datactx->ctx.ctx_flag |= BIG_DATA_CTX_FLAG_MOCK_VALUE;
                    *intention = SWAP_DEL;
                    *intention_flags = SWAP_FIN_DEL_SKIP;
                } else if (cmd_intention_flags & SWAP_IN_DEL
                    || cmd_intention_flags & SWAP_IN_OVERWRITE) {
                    objectMeta *meta = swapDataObjectMeta(data);
                    if (meta->len == 0) {
                        *intention = SWAP_DEL;
                        *intention_flags = SWAP_FIN_DEL_SKIP;
                    } else {
                        *intention = SWAP_IN;
                        *intention_flags = SWAP_EXEC_IN_DEL;
                    }
                } else if (swapDataIsHot(data)) {
                    /* No need to do swap for hot key(execept for SWAP_IN_DEl). */
                    *intention = SWAP_NOP;
                    *intention_flags = 0;
                } else if (cmd_intention_flags == SWAP_IN_META) {
                    /* SCARD: swap in meta (with random field gets empty set)
                     * also SCARD command will be modified like dbsize. */
                    datactx->ctx.num = 0;
                    datactx->ctx.subkeys = zmalloc(sizeof(robj*));
                    datactx->ctx.subkeys[datactx->ctx.num++] = createStringObject("foo",3);
                    *intention = SWAP_IN;
                    *intention_flags = 0;
                } else {
                    /* SMEMBERS,SINTER..., swap in all fields */
                    datactx->ctx.num = 0;
                    datactx->ctx.subkeys = NULL;
                    *intention = SWAP_IN;
                    *intention_flags = 0;
                }
            } else { /* keyrequests with subkeys */
                objectMeta *meta = swapDataObjectMeta(data);
                if (req->cmd_intention_flags == SWAP_IN_DEL) {
                    datactx->ctx.num = 0;
                    datactx->ctx.subkeys = zmalloc(req->b.num_subkeys * sizeof(robj *));
                    /* SREM: even if field is hot (exists in value), we still
                     * need to do ROCKS_DEL on those fields. */
                    for (int i = 0; i < req->b.num_subkeys; i++) {
                        robj *subkey = req->b.subkeys[i];
                        incrRefCount(subkey);
                        datactx->ctx.subkeys[datactx->ctx.num++] = subkey;
                    }
                    *intention = SWAP_IN;
                    *intention_flags = SWAP_EXEC_IN_DEL;
                } else if (meta->len == 0) {
                    *intention = SWAP_NOP;
                    *intention_flags = 0;
                } else {
                    datactx->ctx.num = 0;
                    datactx->ctx.subkeys = zmalloc(req->b.num_subkeys * sizeof(robj *));
                    for (int i = 0; i < req->b.num_subkeys; i++) {
                        robj *subkey = req->b.subkeys[i];
                        if (data->value == NULL || !setTypeIsMember(data->value, subkey->ptr)) {
                            incrRefCount(subkey);
                            datactx->ctx.subkeys[datactx->ctx.num++] = subkey;
                        }
                    }
                    *intention = datactx->ctx.num > 0 ? SWAP_IN : SWAP_NOP;
                    *intention_flags = 0;
                }
            }
            break;
        case SWAP_OUT:
            if (swapDataIsCold(data)) {
                *intention = SWAP_NOP;
                *intention_flags = 0;
            } else {
                unsigned long long evict_memory = 0;
                sds vstr;
                datactx->ctx.subkeys = zmalloc(
                        server.swap_evict_step_max_subkeys*sizeof(robj*));
                setTypeIterator *si;
                si = setTypeInitIterator(data->value);
                while (NULL != (vstr = setTypeNextObject(si))) {
                    size_t vlen = sdslen(vstr);
                    robj* subkey = createObject(OBJ_STRING, vstr);

                    evict_memory += vlen;
                    datactx->ctx.subkeys[datactx->ctx.num++] = subkey;

                    if (datactx->ctx.num >= server.swap_evict_step_max_subkeys ||
                        evict_memory >= server.swap_evict_step_max_memory) {
                        /* Evict in small steps. */
                        break;
                    }
                }
                setTypeReleaseIterator(si);

                /* create new meta if needed */
                if (!swapDataPersisted(data)) {
                    swapDataSetNewObjectMeta(data,
                            createSetObjectMeta(swapGetAndIncrVersion(),0));
                }

                if (!data->value->dirty) {
                    /* directly evict value from db.dict if not dirty. */
                    swapDataCleanObject(data, datactx);
                    if (setTypeSize(data->value) == 0) {
                        swapDataTurnCold(data);
                    }
                    swapDataSwapOut(data,datactx,NULL);
                    
                    *intention = SWAP_NOP;
                    *intention_flags = 0;
                } else {
                    *intention = SWAP_OUT;
                    *intention_flags = 0;
                }
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

int setSwapAnaAction(swapData *data, int intention, void *datactx_, int *action) {
    UNUSED(data);
    setDataCtx *datactx = datactx_;

    switch (intention) {
        case SWAP_IN:
            if (datactx->ctx.num > 0) *action = ROCKS_GET; /* Swap in specific fields */
            else *action = ROCKS_ITERATE; /* Swap in entire set(SMEMBERS) */
            break;
        case SWAP_DEL:
            *action = ROCKS_NOP;
            break;
        case SWAP_OUT:
            *action = ROCKS_PUT;
            break;
        default:
            /* Should not happen .*/
            *action = ROCKS_NOP;
            return SWAP_ERR_DATA_FAIL;
    }

    return 0;
}

static inline sds setEncodeSubkey(redisDb *db, sds key, uint64_t version, sds subkey) {
    return rocksEncodeDataKey(db,key,version,subkey);
}

int setEncodeKeys(swapData *data, int intention, void *datactx_,
                  int *numkeys, int **pcfs, sds **prawkeys) {
    UNUSED(intention);
    setDataCtx *datactx = datactx_;
    sds *rawkeys = NULL;
    int *cfs = NULL, i;
    uint64_t version = swapDataObjectVersion(data);

    serverAssert(datactx->ctx.num);
    cfs = zmalloc(sizeof(int)*datactx->ctx.num);
    rawkeys = zmalloc(sizeof(sds)*datactx->ctx.num);
    for (i = 0; i < datactx->ctx.num; i++) {
        cfs[i] = DATA_CF;
        rawkeys[i] = setEncodeSubkey(data->db,data->key->ptr,
                                     version,datactx->ctx.subkeys[i]->ptr);
    }
    *numkeys = datactx->ctx.num;
    *pcfs = cfs;
    *prawkeys = rawkeys;

    return 0;
}

int setEncodeData(swapData *data, int intention, void *datactx_,
                  int *numkeys, int **pcfs, sds **prawkeys, sds **prawvals) {
    setDataCtx *datactx = datactx_;
    uint64_t version = swapDataObjectVersion(data);
    int *cfs = zmalloc(datactx->ctx.num*sizeof(int));
    sds *rawkeys = zmalloc(datactx->ctx.num*sizeof(sds));
    sds *rawvals = zmalloc(datactx->ctx.num*sizeof(sds));
    serverAssert(intention == SWAP_OUT);
    for (int i = 0; i < datactx->ctx.num; i++) {
        cfs[i] = DATA_CF;
        rawkeys[i] = setEncodeSubkey(data->db,data->key->ptr,
                version,datactx->ctx.subkeys[i]->ptr);
        rawvals[i] = sdsempty();
    }
    *numkeys = datactx->ctx.num;
    *pcfs = cfs;
    *prawkeys = rawkeys;
    *prawvals = rawvals;
    return 0;
}

int setEncodeRange(struct swapData *data, int intention, void *datactx, int *limit,
        uint32_t *flags, int *pcf, sds *start, sds *end) {
    UNUSED(intention), UNUSED(datactx);
    uint64_t version = swapDataObjectVersion(data);

    *pcf = DATA_CF;
    *flags = 0;
    *start = rocksEncodeDataRangeStartKey(data->db,data->key->ptr,version);
    *end = rocksEncodeDataRangeEndKey(data->db,data->key->ptr,version);
    *limit = ROCKS_ITERATE_NO_LIMIT;
    return 0;
}

/* decoded object move to exec module */
int setDecodeData(swapData *data, int num, int *cfs, sds *rawkeys,
                   sds *rawvals, void **pdecoded) {
    int i;
    robj *decoded;
    uint64_t version = swapDataObjectVersion(data);

    serverAssert(num >= 0);
    UNUSED(cfs);

    decoded = NULL;
    for (i = 0; i < num; i++) {
        int dbid;
        sds subkey;
        const char *keystr, *subkeystr;
        size_t klen, slen;
        uint64_t subkey_version;

        if (rawvals[i] == NULL)
            continue;
        if (rocksDecodeDataKey(rawkeys[i],sdslen(rawkeys[i]),
                    &dbid,&keystr,&klen,&subkey_version,&subkeystr,&slen) < 0)
            continue;
        if (!swapDataPersisted(data))
            continue;
        if (version != subkey_version)
            continue;
        subkey = sdsnewlen(subkeystr,slen);

        if (NULL == decoded) decoded = setTypeCreate(subkey);
        setTypeAdd(decoded,subkey);
        sdsfree(subkey);
    }

    /* Note that event if all subkeys are not found, still an empty set
     * object will be returned: empty *warm* set could can meta in memory,
     * so that we don't need to update rocks-meta right after call(). */
    *pdecoded = NULL != decoded ? decoded : createSetObject();
    return 0;
}

static inline robj *createSwapInObject(robj *newval) {
    robj *swapin = newval;
    serverAssert(newval && newval->type == OBJ_SET);
    swapin->dirty = 0;
    return swapin;
}

/* Note: meta are kept as long as there are data in rocksdb. */
int setSwapIn(swapData *data, void *result_, void *datactx) {
    robj *result = result_;
    UNUSED(datactx);
    /* hot key no need to swap in, this must be a warm or cold key. */
    serverAssert(swapDataPersisted(data));
    if (swapDataIsCold(data) && result != NULL /* may be empty */) {
        /* cold key swapped in result (may be empty). */
        robj *swapin = createSwapInObject(result);
        /* mark persistent after data swap in without
         * persistence deleted, or mark non-persistent else */
        swapin->persistent = !data->persistence_deleted;
        dbAdd(data->db,data->key,swapin);
        /* expire will be swapped in later by swap framework. */
        if (data->cold_meta) {
            dbAddMeta(data->db,data->key,data->cold_meta);
            data->cold_meta = NULL; /* moved */
        }
    } else {
        if (result) decrRefCount(result);
        if (data->value) data->value->persistent = !data->persistence_deleted;
    }

    return 0;
}

/* subkeys already cleaned by cleanObject(to save cpu usage of main thread),
 * swapout only updates db.dict keyspace, meta (db.meta/db.expire) swapped
 * out by swap framework. */
int setSwapOut(swapData *data, void *datactx, int *totally_out) {
    UNUSED(datactx);
    serverAssert(!swapDataIsCold(data));

    if (setTypeSize(data->value) == 0) {
        /* all fields swapped out, key turnning into cold:
         * - rocks-meta should have already persisted.
         * - object_meta and value will be deleted by dbDelete, expire already
         *   deleted by swap framework. */
        dbDelete(data->db,data->key);
        /* new_meta exists if hot key turns cold directly, in which case
         * new_meta not moved to db.meta nor updated but abandonded. */
        if (data->new_meta) {
            freeObjectMeta(data->new_meta);
            data->new_meta = NULL;
        }
        if (totally_out) *totally_out = 1;
    } else { /* not all fields swapped out. */
        if (data->new_meta) {
            dbAddMeta(data->db,data->key,data->new_meta);
            data->new_meta = NULL; /* moved to db.meta */
            data->value->persistent = 1; /* loss pure hot and persistent data exist. */
        }
        if (totally_out) *totally_out = 0;
    }

    return 0;
}

int setSwapDel(swapData *data, void *datactx_, int del_skip) {
    setDataCtx* datactx = (setDataCtx*)datactx_;
    if (datactx->ctx.ctx_flag & BIG_DATA_CTX_FLAG_MOCK_VALUE) {
        createFakeSetForDeleteIfCold(data);
    }
    if (del_skip) {
        if (!swapDataIsCold(data))
            dbDeleteMeta(data->db,data->key);
        return 0;
    } else {
        if (!swapDataIsCold(data))
            /* both value/object_meta/expire are deleted */
            dbDelete(data->db,data->key);
        return 0;
    }
}

/* Decoded moved back by exec to setSwapData */
void *setCreateOrMergeObject(swapData *data, void *decoded_, void *datactx) {
    robj *result, *decoded = decoded_;
    UNUSED(datactx);
    serverAssert(decoded == NULL || decoded->type == OBJ_SET);

    if (swapDataIsCold(data) || decoded == NULL) {
        /* decoded moved back to swap framework again (result will later be
         * pass as swapIn param). */
        result = decoded;
        if (decoded) {
            swapDataObjectMetaModifyLen(data,-setTypeSize(decoded));
        }
    } else {
        setTypeIterator *si;
        sds subkey;
        si = setTypeInitIterator(decoded);
        while (NULL != (subkey = setTypeNextObject(si))) {
            int updated = setTypeAdd(data->value, subkey);
            if (updated) swapDataObjectMetaModifyLen(data,-1);
            sdsfree(subkey);
        }
        setTypeReleaseIterator(si);
        /* decoded merged, we can release it now. */
        decrRefCount(decoded);
        result = NULL;
    }
    return result;
}

int setCleanObject(swapData *data, void *datactx_) {
    setDataCtx *datactx = datactx_;
    if (swapDataIsCold(data)) return 0;
    for (int i = 0; i < datactx->ctx.num; i++) {
        if (setTypeRemove(data->value,datactx->ctx.subkeys[i]->ptr)) {
            swapDataObjectMetaModifyLen(data,1);
        }
    }
    return 0;
}

/* Only free extend fields here, base fields (key/value/object_meta) freed
 * in swapDataFree */
void freeSetSwapData(swapData *data_, void *datactx_) {
    UNUSED(data_);
    setDataCtx *datactx = datactx_;
    for (int i = 0; i < datactx->ctx.num; i++) {
        decrRefCount(datactx->ctx.subkeys[i]);
    }
    zfree(datactx->ctx.subkeys);
    zfree(datactx);
}

swapDataType setSwapDataType = {
    .name = "set",
    .swapAna = setSwapAna,
    .swapAnaAction = setSwapAnaAction,
    .encodeKeys = setEncodeKeys,
    .encodeData = setEncodeData,
    .encodeRange = setEncodeRange,
    .decodeData = setDecodeData,
    .swapIn = setSwapIn,
    .swapOut = setSwapOut,
    .swapDel = setSwapDel,
    .createOrMergeObject = setCreateOrMergeObject,
    .cleanObject = setCleanObject,
    .beforeCall = NULL,
    .free = freeSetSwapData,
    .rocksDel = NULL,
    .mergedIsHot = setMergedIsHot,
};

int swapDataSetupSet(swapData *d, OUT void **pdatactx) {
    d->type = &setSwapDataType;
    d->omtype = &setObjectMetaType;
    setDataCtx *datactx = zmalloc(sizeof(setDataCtx));
    datactx->ctx.num = 0;
    datactx->ctx.ctx_flag = BIG_DATA_CTX_FLAG_NONE;
    datactx->ctx.subkeys = NULL;
    *pdatactx = datactx;
    return 0;
}

/* Set rdb save */
int setSaveStart(rdbKeySaveData *save, rio *rdb) {
    robj *key = save->key;
    size_t nfields = 0;
    int ret = 0;

    /* save header */
    if (rdbSaveKeyHeader(rdb,key,key,RDB_TYPE_SET,
                         save->expire) == -1)
        return -1;

    /* nfields */
    if (save->value)
        nfields += setTypeSize(save->value);
    if (save->object_meta)
        nfields += save->object_meta->len;
    if (rdbSaveLen(rdb,nfields) == -1)
        return -1;

    if (!save->value)
        return 0;

    /* save fields from value (db.dict) */
    setTypeIterator *si = setTypeInitIterator(save->value);
    sds subkey;
    while (NULL != (subkey = setTypeNextObject(si))) {
        if (rdbSaveRawString(rdb,(unsigned char*)subkey,
                             sdslen(subkey)) == -1) {
            sdsfree(subkey);
            ret = -1;
            break;
        }
        sdsfree(subkey);
    }
    setTypeReleaseIterator(si);

    return ret;
}

int setSave(rdbKeySaveData *save, rio *rdb, decodedData *decoded) {
    robj *key = save->key;
    serverAssert(!sdscmp(decoded->key, key->ptr));

    if (save->value != NULL) {
        if (setTypeIsMember(save->value,decoded->subkey)) {
            /* already save in save_start, skip this subkey */
            return 0;
        }
    }

    if (rdbSaveRawString(rdb,(unsigned char*)decoded->subkey,
                         sdslen(decoded->subkey)) == -1) {
        return -1;
    }

    save->saved++;
    return 0;
}

int setSaveEnd(rdbKeySaveData *save, rio *rdb, int save_result) {
    objectMeta *object_meta = save->object_meta;
    UNUSED(rdb);
    if (save->saved != object_meta->len) {
        sds key  = save->key->ptr;
        sds repr = sdscatrepr(sdsempty(), key, sdslen(key));
        serverLog(LL_WARNING,
                  "setSave %s: saved(%d) != object_meta.len(%ld)",
                  repr, save->saved, (ssize_t)object_meta->len);
        sdsfree(repr);
        return -1;
    }
    return save_result;
}

rdbKeySaveType setSaveType = {
    .save_start = setSaveStart,
    .save = setSave,
    .save_end = setSaveEnd,
    .save_deinit = NULL,
};

int setSaveInit(rdbKeySaveData *save, uint64_t version, const char *extend,
        size_t extlen) {
    int retval = 0;
    save->type = &setSaveType;
    save->omtype = &setObjectMetaType;
    if (extend) {
        serverAssert(save->object_meta == NULL);
        retval = buildObjectMeta(OBJ_SET,version,extend,
                                 extlen,&save->object_meta);
    }
    return retval;
}

/* Set rdb load */
void setLoadStartIntset(struct rdbKeyLoadData *load, rio *rdb, int *cf,
                    sds *rawkey, sds *rawval, int *error) {
    sds extend = NULL;

    load->value = rdbLoadObject(load->rdbtype,rdb,load->key,error);
    if (load->value == NULL) return;

    if (load->value->type != OBJ_SET) {
        serverLog(LL_WARNING,"Load rdb with rdbtype(%d) got (%d)",
                  load->rdbtype, load->value->type);
        *error = RDB_LOAD_ERR_OTHER;
        return;
    }
    if (load->value->encoding != OBJ_ENCODING_INTSET && load->value->encoding != OBJ_ENCODING_HT) {
        serverLog(LL_WARNING,"Load rdb with rdbtype(%d) got obj encoding (%d)",
                  load->rdbtype, load->value->encoding);
        *error = RDB_LOAD_ERR_OTHER;
        return;
    }

    load->iter = setTypeInitIterator(load->value);
    load->total_fields = setTypeSize(load->value);
    if (load->total_fields == 0) {
        *error = RDB_LOAD_ERR_EMPTY_KEY;
        return;
    }

    extend = rocksEncodeObjectMetaLen(load->total_fields);
    *cf = META_CF;
    *rawkey = rocksEncodeMetaKey(load->db,load->key);
    *rawval = rocksEncodeMetaVal(load->object_type,load->expire,
            load->version,extend);
    sdsfree(extend);
}

void setLoadStart(struct rdbKeyLoadData *load, rio *rdb, int *cf,
                   sds *rawkey, sds *rawval, int *error) {
    switch (load->rdbtype) {
        case RDB_TYPE_SET:
            rdbLoadStartSet(load,rdb,cf,rawkey,rawval,error);
            break;
        case RDB_TYPE_SET_INTSET:
            setLoadStartIntset(load,rdb,cf,rawkey,rawval,error);
            break;
        default:
            break;
    }
}

int setLoadHT(struct rdbKeyLoadData *load, rio *rdb, int *cf, sds *rawkey,
               sds *rawval, int *error) {
    sds subkey;
    *error = RDB_LOAD_ERR_OTHER;
    if ((subkey = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
        return 0;
    }

    *cf = DATA_CF;
    *rawkey = rocksEncodeDataKey(load->db,load->key,load->version,subkey);
    *rawval = sdsempty();
    *error = 0;
    sdsfree(subkey);
    load->loaded_fields++;
    return load->loaded_fields < load->total_fields;
}

int setLoadIntset(struct rdbKeyLoadData *load, rio *rdb, int *cf, sds *rawkey,
               sds *rawval, int *error) {
    sds subkey;
    UNUSED(rdb);

    subkey = setTypeNextObject(load->iter);

    *cf = DATA_CF;
    *rawkey = rocksEncodeDataKey(load->db,load->key,load->version,subkey);
    *rawval = sdsempty();
    *error = 0;

    sdsfree(subkey);
    load->loaded_fields++;
    return load->loaded_fields < load->total_fields;
}

int setLoad(struct rdbKeyLoadData *load, rio *rdb, int *cf,
             sds *rawkey, sds *rawval, int *error) {
    int retval;

    switch (load->rdbtype) {
        case RDB_TYPE_SET:
            retval = setLoadHT(load,rdb,cf,rawkey,rawval,error);
            break;
        case RDB_TYPE_SET_INTSET:
            retval = setLoadIntset(load,rdb,cf,rawkey,rawval,error);
            break;
        default:
            retval = RDB_LOAD_ERR_OTHER;
    }

    return retval;
}

void setLoadDeinit(struct rdbKeyLoadData *load) {
    if (load->iter) {
        setTypeReleaseIterator(load->iter);
        load->iter = NULL;
    }

    if (load->value) {
        decrRefCount(load->value);
        load->value = NULL;
    }
}

rdbKeyLoadType setLoadType = {
        .load_start = setLoadStart,
        .load = setLoad,
        .load_end = NULL,
        .load_deinit = setLoadDeinit,
};

void setLoadInit(rdbKeyLoadData *load) {
    load->type = &setLoadType;
    load->omtype = &setObjectMetaType;
    load->object_type = OBJ_SET;
}

#ifdef REDIS_TEST

#include <stdarg.h>

robj **mockSubKeys(int num,...) {
    int i;
    va_list valist;
    robj **subkeys = zmalloc(num * sizeof(robj*));

    va_start(valist, num);
    for (i = 0; i < num; i++) {
        subkeys[i] = createObject(OBJ_STRING, va_arg(valist, sds));
    }
    va_end(valist);

    return subkeys;
}

int swapDataSetTest(int argc, char **argv, int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);
    initTestRedisServer();
    redisDb* db = server.db + 0;
    int error = 0;
    swapData *set1_data;
    setDataCtx *set1_ctx;
    robj *key1, *set1;
    void *decoded;
    keyRequest _kr1, *kr1 = &_kr1, _cold_kr1, *cold_kr1 = &_cold_kr1;
    sds f1, f2, f3, f4;
    int action, numkeys;
    int oldEvictStep = server.swap_evict_step_max_subkeys;

    TEST("set - init") {
        key1 = createStringObject("key1",4);
        f1 = sdsnew("f1"), f2 = sdsnew("f2"), f3 = sdsnew("f3"), f4 = sdsnew("f4");
        set1 = createSetObject();
        setTypeAdd(set1, f1);
        setTypeAdd(set1, f2);
        setTypeAdd(set1, f3);
        setTypeAdd(set1, f4);
        dbAdd(db,key1,set1);
    }

    TEST("set - encodeKeys/encodeData/DecodeData") {
        set1_data = createSwapData(db, key1,set1);
        swapDataSetupSet(set1_data, (void**)&set1_ctx);
        sds *rawkeys, *rawvals;
        sds empty = sdsempty();
        int *cfs, cf, flags;
        sds start, end;

        set1_ctx->ctx.num = 2;
        set1_ctx->ctx.subkeys = mockSubKeys(2, sdsdup(f1), sdsdup(f2));
        set1_data->object_meta = createSetObjectMeta(0,2);
        // encodeKeys - swap in subkeys
        setSwapAnaAction(set1_data, SWAP_IN, set1_ctx, &action);
        setEncodeKeys(set1_data, SWAP_IN, set1_ctx, &numkeys, &cfs, &rawkeys);
        test_assert(2 == numkeys);
        test_assert(DATA_CF == cfs[0] && DATA_CF == cfs[1]);
        test_assert(ROCKS_GET == action);
        sds expectEncodedKey = setEncodeSubkey(db, key1->ptr, 0, f1);
        test_assert(memcmp(expectEncodedKey,rawkeys[0],sdslen(rawkeys[0])) == 0
            || memcmp(expectEncodedKey,rawkeys[1],sdslen(rawkeys[1])) == 0);

        // encodeKeys - swap in whole key
        set1_ctx->ctx.num = 0;
        setSwapAnaAction(set1_data, SWAP_IN, set1_ctx, &action);
        setEncodeRange(set1_data, SWAP_IN, set1_ctx, &numkeys, &flags, &cf, &start, &end);
        test_assert(ROCKS_ITERATE == action);
        test_assert(DATA_CF == cf);
        expectEncodedKey = rocksEncodeDataRangeStartKey(db, key1->ptr, 0);
        test_assert(memcmp(expectEncodedKey, start, sdslen(start)) == 0);

        // encodeKeys - swap del
        set1_ctx->ctx.num = 2;
        setSwapAnaAction(set1_data, SWAP_DEL, set1_ctx, &action);
        test_assert(0 == action);

        // encodeData - swap out
        set1_ctx->ctx.num = 2;
        setSwapAnaAction(set1_data, SWAP_OUT, set1_ctx, &action);
        setEncodeData(set1_data, SWAP_OUT, set1_ctx, &numkeys, &cfs, &rawkeys, &rawvals);
        test_assert(action == ROCKS_PUT);
        test_assert(2 == numkeys);

        // decodeData - swap in
        setDecodeData(set1_data, set1_ctx->ctx.num, cfs, rawkeys, rawvals, &decoded);
        test_assert(NULL != decoded);
        test_assert(2 == setTypeSize(decoded));

        freeSetSwapData(set1_data, set1_ctx);
    }

    TEST("set - swapAna") {
        int intention;
        uint32_t intention_flags;
        objectMeta *set1_meta = createSetObjectMeta(0,0);
        set1_data = createSwapData(db, key1,set1);
        swapDataSetupSet(set1_data, (void**)&set1_ctx);

        kr1->key = key1;
        kr1->level = REQUEST_LEVEL_KEY;
        kr1->type = KEYREQUEST_TYPE_SUBKEY;
        kr1->b.num_subkeys = 0;
        kr1->b.subkeys = NULL;
        kr1->dbid = db->id;
        cold_kr1->key = key1;
        cold_kr1->level = REQUEST_LEVEL_KEY;
        cold_kr1->b.num_subkeys = 0;
        cold_kr1->b.subkeys = NULL;
        cold_kr1->dbid = db->id;

        // swap nop
        kr1->cmd_intention = SWAP_NOP;
        kr1->cmd_intention_flags = 0;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);

        // swap in while no persisted data
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = 0;
        set1_data->object_meta = NULL;
        set1_data->cold_meta = NULL;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);

        // swap in meta
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = SWAP_IN_META;
        set1_data->value = NULL;
        set1_data->object_meta = NULL;
        set1_data->cold_meta = set1_meta;
        set1_meta->len=4;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == 0);
        test_assert(set1_ctx->ctx.num > 0);

        // swap in del mock value
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = SWAP_IN_DEL_MOCK_VALUE;
        set1_data->value = set1;
        setSwapAna(set1_data, kr1, &intention, &intention_flags, set1_ctx);
        test_assert(intention == SWAP_DEL && intention_flags == SWAP_FIN_DEL_SKIP);

        // swap in del - all subkeys in memory
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = SWAP_IN_DEL;
        set1_data->value = set1;
        set1_data->object_meta = NULL;
        set1_data->cold_meta = set1_meta;
        set1_meta->len = 0;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_DEL && intention_flags == SWAP_FIN_DEL_SKIP);

        // swap in del - not all subkeys in memory
        set1_meta->len = 4;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == SWAP_EXEC_IN_DEL);

        // swap in whole key
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = 0;
        set1_data->value = NULL;
        set1_meta->len = 4;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == 0);

        // swap in with subkeys - swap in del
        kr1->b.num_subkeys = 2;
        kr1->b.subkeys = mockSubKeys(2, sdsdup(f1), sdsdup(f2));
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = SWAP_IN_DEL;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == SWAP_EXEC_IN_DEL);
        test_assert(set1_ctx->ctx.num == 2);

        // swap in with subkeys - subkeys already in mem
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = 0;
        set1_data->value = set1;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        test_assert(set1_ctx->ctx.num == 0);

        // swap in with subkeys - subkeys not in mem
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = 0;
        kr1->b.subkeys = mockSubKeys(2, sdsnew("new1"), sdsnew("new2"));
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == 0);
        test_assert(set1_ctx->ctx.num == 2);

        // swap out - data not in mem
        set1_data->value = NULL;
        kr1->cmd_intention = SWAP_OUT;
        kr1->cmd_intention_flags = 0;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);

        // swap out - first swap out
        set1_data->value = set1;
        set1->dirty = 1;
        set1_data->object_meta = NULL;
        set1_data->cold_meta = NULL;
        set1_data->new_meta = NULL;
        set1_ctx->ctx.num = 0;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_OUT && intention_flags == 0);
        test_assert(4 == set1_ctx->ctx.num);
        test_assert(NULL != set1_data->new_meta);

        // swap out - data not dirty
        set1->dirty = 0;
        set1_ctx->ctx.num = 0;
        set1_data->object_meta = createSetObjectMeta(0,0);
        set1_data->new_meta = NULL;
        int expectColdKey = db->cold_keys + 1;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        test_assert(0 == setTypeSize(set1));
        test_assert(4 == set1_data->object_meta->len);
        test_assert(expectColdKey == db->cold_keys);

        // recover data in set1
        setTypeAdd(set1, f1);
        setTypeAdd(set1, f2);
        setTypeAdd(set1, f3);
        setTypeAdd(set1, f4);
        dbAdd(db, key1, set1);

        // swap del
        kr1->cmd_intention = SWAP_DEL;
        kr1->cmd_intention_flags = 0;
        setSwapAna(set1_data,kr1,&intention,&intention_flags,set1_ctx);
        test_assert(intention == SWAP_DEL && intention_flags == 0);

        freeSetSwapData(set1_data, set1_ctx);
    }

    TEST("set - swapIn/swapOut") {
        robj *s, *result;
        objectMeta *m;
        set1_data = createSwapData(db, key1,set1);
        swapDataSetupSet(set1_data, (void**)&set1_ctx);
        test_assert(lookupMeta(db,key1) == NULL);
        test_assert((s = lookupKey(db, key1, LOOKUP_NOTOUCH)) != NULL);
        test_assert(setTypeSize(s) == 4);

        /* hot => warm => cold */
        set1_data->new_meta = createSetObjectMeta(0,0);
        set1_ctx->ctx.num = 2;
        set1_ctx->ctx.subkeys = mockSubKeys(2, sdsdup(f1), sdsdup(f2));
        setCleanObject(set1_data, set1_ctx);
        setSwapOut(set1_data, set1_ctx, NULL);
        test_assert((m =lookupMeta(db,key1)) != NULL && m->len == 2);
        test_assert((s = lookupKey(db, key1, LOOKUP_NOTOUCH)) != NULL);
        test_assert(setTypeSize(s) == 2);

        set1_data->new_meta = NULL;
        set1_data->object_meta = m;
        set1_ctx->ctx.subkeys = mockSubKeys(2, sdsdup(f3), sdsdup(f4));
        setCleanObject(set1_data, set1_ctx);
        setSwapOut(set1_data, set1_ctx, NULL);
        test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
        test_assert(lookupMeta(db,key1) == NULL);

        /* cold => warm => hot */
        decoded = createSetObject();
        setTypeAdd(decoded, f1);
        setTypeAdd(decoded, f2);
        set1_data->object_meta = NULL;
        set1_data->cold_meta = createSetObjectMeta(0,4);
        set1_data->value = NULL;
        result = setCreateOrMergeObject(set1_data, decoded, set1_ctx);
        setSwapIn(set1_data,result,set1_ctx);
        test_assert((m = lookupMeta(db,key1)) != NULL && m->len == 2);
        test_assert((s = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(setTypeSize(s) == 2);

        decoded = createSetObject();
        setTypeAdd(decoded,f3);
        setTypeAdd(decoded,f4);
        set1_data->cold_meta = NULL;
        set1_data->object_meta = m;
        set1_data->value = s;
        result = setCreateOrMergeObject(set1_data, decoded, set1_ctx);
        setSwapIn(set1_data,result,set1_ctx);
        test_assert((m = lookupMeta(db,key1)) != NULL && m->len == 0);
        test_assert((s = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(setTypeSize(s) == 4);

        /* hot => cold */
        set1_data->object_meta = m;
        set1_data->value = s;
        set1_ctx->ctx.num = 4;
        set1_ctx->ctx.subkeys = mockSubKeys(4, sdsdup(f1), sdsdup(f2), sdsdup(f3), sdsdup(f4));
        setCleanObject(set1_data, set1_ctx);
        setSwapOut(set1_data, set1_ctx, NULL);
        test_assert((m = lookupMeta(db,key1)) == NULL);
        test_assert((s = lookupKey(db,key1,LOOKUP_NOTOUCH)) == NULL);

        /* cold => hot */
        decoded = createSetObject();
        setTypeAdd(decoded, f1);
        setTypeAdd(decoded, f2);
        setTypeAdd(decoded, f3);
        setTypeAdd(decoded, f4);
        set1_data->object_meta = NULL;
        set1_data->cold_meta = createSetObjectMeta(0,4);
        set1_data->value = NULL;
        result = setCreateOrMergeObject(set1_data,decoded,set1_ctx);
        setSwapIn(set1_data,result,set1_ctx);
        test_assert((m = lookupMeta(db,key1)) != NULL);
        test_assert((s = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(setTypeSize(s) == 4);

        freeSetSwapData(set1_data, set1_ctx);
    }

    TEST("set - rdbLoad & rdbSave") {
        int err = 0;
        int cf;
        sds rdbv1 = rocksEncodeValRdb(createStringObject("f1", 2));
        sds rdbv2 = rocksEncodeValRdb(createStringObject("f2", 2));

        /* rdbLoad - RDB_TYPE_SET */
        set1 = createSetObject();
        setTypeAdd(set1, f1);
        setTypeAdd(set1, f2);
        setTypeAdd(set1, f3);
        setTypeAdd(set1, f4);

        rio sdsrdb;
        sds rawval = rocksEncodeValRdb(set1);
        rioInitWithBuffer(&sdsrdb,sdsnewlen(rawval+1,sdslen(rawval)-1));
        rdbKeyLoadData _loadData; rdbKeyLoadData *loadData = &_loadData;

        int cont;
        sds subkey, subraw;
        objectMeta *cold_meta;
        int t; long long e; const char *extend; size_t extlen;
        uint64_t v;
        rdbKeyLoadDataInit(loadData,RDB_TYPE_SET,db,key1->ptr,-1,1600000000);
        setLoadStart(loadData, &sdsrdb, &cf, &subkey, &subraw, &err);
        test_assert(0 == err && META_CF == cf);
        test_assert(memcmp(rocksEncodeMetaKey(db,key1->ptr), subkey, sdslen(subkey)) == 0);

        rocksDecodeMetaVal(subraw, sdslen(subraw), &t, &e, &v, &extend, &extlen);
        buildObjectMeta(t,v,extend,extlen,&cold_meta);
        test_assert(cold_meta->object_type == OBJ_SET && cold_meta->len == 4 && e == -1);

        cont = setLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = setLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = setLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = setLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 0 && err == 0 && cf == DATA_CF);
        test_assert(loadData->object_type == OBJ_SET);
        test_assert(loadData->total_fields == 4 && loadData->loaded_fields == 4);
        setLoadDeinit(loadData);

        /* rdbLoad - RDB_TYPE_SET_INTSET */
        set1 = createIntsetObject();
        setTypeAdd(set1, sdsnew("1"));
        setTypeAdd(set1, sdsnew("2"));
        setTypeAdd(set1, sdsnew("3"));
        setTypeAdd(set1, sdsnew("4"));

        rawval = rocksEncodeValRdb(set1);
        rioInitWithBuffer(&sdsrdb,sdsnewlen(rawval+1,sdslen(rawval)-1));
        rdbKeyLoadDataInit(loadData,RDB_TYPE_SET_INTSET, db,key1->ptr,-1,1600000000);
        setLoadStart(loadData, &sdsrdb, &cf, &subkey, &subraw, &err);
        test_assert(0 == err && META_CF == cf);
        test_assert(memcmp(rocksEncodeMetaKey(db,key1->ptr), subkey, sdslen(subkey)) == 0);

        rocksDecodeMetaVal(subraw, sdslen(subraw), &t, &e, &v, &extend, &extlen);
        buildObjectMeta(t,v,extend,extlen,&cold_meta);
        test_assert(cold_meta->object_type == OBJ_SET && cold_meta->len == 4 && e == -1);

        cont = setLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = setLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = setLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = setLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 0 && err == 0 && cf == DATA_CF);
        test_assert(loadData->object_type == OBJ_SET);
        test_assert(loadData->total_fields == 4 && loadData->loaded_fields == 4);
        setLoadDeinit(loadData);

        /* rdbSave */
        sds coldraw,warmraw,hotraw;
        rio rdbcold, rdbwarm, rdbhot;
        rdbKeySaveData _saveData; rdbKeySaveData  *saveData = &_saveData;

        decodedMeta _decoded_meta, *decoded_meta = &_decoded_meta;
        decodedData  _decoded_data, *decoded_data = &_decoded_data;
        decoded_meta->dbid = decoded_data->dbid = db->id;
        decoded_meta->key = decoded_data->key = key1->ptr;
        decoded_meta->cf = META_CF, decoded_data->cf = DATA_CF;
        decoded_meta->version = 0, decoded_data->version = 0;
        decoded_meta->object_type = OBJ_SET, decoded_meta->expire = -1;
        decoded_data->rdbtype = 0;

        /* rdbSave - save cold */
        dbDelete(db, key1);
        decoded_meta->extend = rocksEncodeObjectMetaLen(2);
        rioInitWithBuffer(&rdbcold,sdsempty());
        test_assert(rdbKeySaveDataInit(saveData, db, (decodedResult*)decoded_meta) == 0);

        test_assert(setSaveStart(saveData, &rdbcold) == 0);
        decoded_data->subkey = f2, decoded_data->rdbraw = sdsnewlen(rdbv2+1,sdslen(rdbv2)-1);
        decoded_data->version = saveData->object_meta->version;
        test_assert(rdbKeySave(saveData,&rdbcold,decoded_data) == 0);
        decoded_data->subkey = f1, decoded_data->rdbraw = sdsnewlen(rdbv1+1,sdslen(rdbv1)-1);
        test_assert(rdbKeySave(saveData,&rdbcold,decoded_data) == 0);
        coldraw = rdbcold.io.buffer.ptr;

        /* rdbSave - save warm */
        rioInitWithBuffer(&rdbwarm,sdsempty());
        robj *value = createSetObject();
        setTypeAdd(value,f2);
        dbAdd(db, key1, value);
        dbAddMeta(db, key1, createSetObjectMeta(0,1));
        test_assert(rdbKeySaveDataInit(saveData, db, (decodedResult*)decoded_meta) == 0);
        test_assert(rdbKeySaveStart(saveData,&rdbwarm) == 0);
        decoded_data->version = saveData->object_meta->version;
        test_assert(rdbKeySave(saveData,&rdbwarm,decoded_data) == 0);
        warmraw = rdbwarm.io.buffer.ptr;

        /* rdbSave - hot */
        robj keyobj;
        robj *wholeset = createSetObject();
        setTypeAdd(wholeset,f1);
        setTypeAdd(wholeset,f2);
        rioInitWithBuffer(&rdbhot,sdsempty());
        initStaticStringObject(keyobj,key1->ptr);
        test_assert(rdbSaveKeyValuePair(&rdbhot,&keyobj,wholeset,-1) != -1);
        hotraw = rdbhot.io.buffer.ptr;

        test_assert(!sdscmp(hotraw,coldraw) && !sdscmp(hotraw,warmraw) && !sdscmp(hotraw,hotraw));

        dbDelete(db,key1);
    }

    TEST("set - free") {
        decrRefCount(set1);
        server.swap_evict_step_max_subkeys = oldEvictStep;
    }

    return error;
}

#endif
