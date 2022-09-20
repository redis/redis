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

static void createFakeHashForDeleteIfCold(swapData *data) {
	if (swapDataIsCold(data)) {
        /* empty hash allowed */
		dbAdd(data->db,data->key,createHashObject());
	}
}

int hashSwapAna(swapData *data, struct keyRequest *req,
        int *intention, uint32_t *intention_flags, void *datactx_) {
    hashDataCtx *datactx = datactx_;
    int cmd_intention = req->cmd_intention;
    uint32_t cmd_intention_flags = req->cmd_intention_flags;
    serverAssert(req->num_subkeys >= 0);

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
        } else if (req->num_subkeys == 0) {
            if (cmd_intention_flags == SWAP_IN_DEL) {
                /* DEL/GETDEL: Lazy delete current key. */
                createFakeHashForDeleteIfCold(data);
                *intention = SWAP_DEL;
                *intention_flags = SWAP_FIN_DEL_SKIP;
            } else if (swapDataIsHot(data)) {
                /* No need to do swap for hot key(execept for SWAP_IN_DEl). */
                *intention = SWAP_NOP;
                *intention_flags = 0;
            } else if (cmd_intention_flags == SWAP_IN_META) {
                /* HLEN: swap in meta (with random field gets empty hash)
                 * also HLEN command will be modified like dbsize. */
                datactx->num = 0;
                datactx->subkeys = zmalloc(sizeof(robj*));
                datactx->subkeys[datactx->num++] = createStringObject("foo",3);
                *intention = SWAP_IN;
                *intention_flags = 0;
            } else {
                /* HKEYS/HVALS/..., swap in all fields */
                datactx->num = 0;
                datactx->subkeys = NULL;
                *intention = SWAP_IN;
                *intention_flags = 0;
            }
        } else { /* keyrequests with subkeys */
            datactx->num = 0;
            datactx->subkeys = zmalloc(req->num_subkeys * sizeof(robj*));
            for (int i = 0; i < req->num_subkeys; i++) {
                robj *subkey = req->subkeys[i];
                /* HDEL: even if field is hot (exists in value), we still
                 * need to do ROCKS_DEL on those fields. */
                if (cmd_intention_flags == SWAP_IN_DEL ||
                        data->value == NULL ||
                        !hashTypeExists(data->value,subkey->ptr)) {
                    incrRefCount(subkey);
                    datactx->subkeys[datactx->num++] = subkey;
                }
            }

            *intention = datactx->num > 0 ? SWAP_IN : SWAP_NOP;
            if (cmd_intention_flags == SWAP_IN_DEL)
                *intention_flags = SWAP_EXEC_IN_DEL;
            else
                *intention_flags = 0;
        }
        break;
    case SWAP_OUT:
        if (data->value == NULL) {
            *intention = SWAP_NOP;
            *intention_flags = 0;
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
                if (datactx->num >= server.swap_evict_step_max_subkeys ||
                        evict_memory >= server.swap_evict_step_max_memory) {
                    /* Evict big hash in small steps. */
                    break;
                }
            }
            hashTypeReleaseIterator(hi);

            /* create new meta if needed */
            if (!swapDataPersisted(data))
                data->new_meta = createHashObjectMeta(0);

            if (!data->value->dirty) {
                /* directly evict value from db.dict if not dirty. */
                swapDataTurnCold(data);
                swapDataCleanObject(data, datactx);
                swapDataSwapOut(data,datactx);
                *intention = SWAP_NOP;
                *intention_flags = 0;
            } else {
                *intention = SWAP_OUT;
                if (datactx->num == (int)hashTypeLength(data->value))
                    *intention_flags = SWAP_EXEC_OUT_META;
                else
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

static inline sds hashEncodeSubkey(redisDb *db, sds key, sds subkey) {
    return rocksEncodeDataKey(db,key,subkey);
}

static void hashEncodeDeleteRange(swapData *data, sds *start, sds *end) {
    *start = rocksEncodeDataKey(data->db,data->key->ptr,NULL);
    *end = rocksCalculateNextKey(*start);
    serverAssert(NULL != *end);
}

int hashEncodeKeys(swapData *data, int intention, void *datactx_,
        int *action, int *numkeys, int **pcfs, sds **prawkeys) {
    hashDataCtx *datactx = datactx_;
    sds *rawkeys = NULL;
    int *cfs = NULL;

    switch (intention) {
    case SWAP_IN:
        if (datactx->num > 0) { /* Swap in specific fields */
            int i;
            cfs = zmalloc(sizeof(int)*datactx->num);
            rawkeys = zmalloc(sizeof(sds)*datactx->num);
            for (i = 0; i < datactx->num; i++) {
                cfs[i] = DATA_CF;
                rawkeys[i] = hashEncodeSubkey(data->db,data->key->ptr,
                        datactx->subkeys[i]->ptr);
            }
            *numkeys = datactx->num;
            *pcfs = cfs;
            *prawkeys = rawkeys;
            *action = ROCKS_MULTIGET;
        } else {/* Swap in entire hash(HKEYS/HVALS...) */
            cfs = zmalloc(sizeof(int));
            rawkeys = zmalloc(sizeof(sds));
            cfs[0] = DATA_CF;
            rawkeys[0] = hashEncodeSubkey(data->db,data->key->ptr,NULL);
            *numkeys = datactx->num;
            *pcfs = cfs;
            *prawkeys = rawkeys;
            *action = ROCKS_SCAN;
        }
        return 0;
    case SWAP_DEL:
        if (swapDataPersisted(data)) {
            int *cfs = zmalloc(sizeof(int)*2);
            rawkeys = zmalloc(sizeof(sds)*2);
            hashEncodeDeleteRange(data, &rawkeys[0], &rawkeys[1]);
            cfs[0] = cfs[1] = DATA_CF;
            *numkeys = 2;
            *pcfs = cfs;
            *prawkeys = rawkeys;
            *action = ROCKS_DELETERANGE;
        } else {
            *action = 0;
            *numkeys = 0;
            *pcfs = NULL;
            *prawkeys = NULL;
        }
        return 0;
    case SWAP_OUT:
    default:
        /* Should not happen .*/
        *action = 0;
        *numkeys = 0;
        *pcfs = NULL;
        *prawkeys = NULL;
        return 0;
    }

    return 0;
}

static inline sds hashEncodeSubval(robj *subval) {
    return rocksEncodeValRdb(subval);
}

int hashEncodeData(swapData *data, int intention, void *datactx_,
        int *action, int *numkeys, int **pcfs, sds **prawkeys, sds **prawvals) {
    hashDataCtx *datactx = datactx_;
    int *cfs = zmalloc(datactx->num*sizeof(int));
    sds *rawkeys = zmalloc(datactx->num*sizeof(sds));
    sds *rawvals = zmalloc(datactx->num*sizeof(sds));
    serverAssert(intention == SWAP_OUT);
    for (int i = 0; i < datactx->num; i++) {
        cfs[i] = DATA_CF;
        rawkeys[i] = hashEncodeSubkey(data->db,data->key->ptr,
                datactx->subkeys[i]->ptr);
        robj *subval = hashTypeGetValueObject(data->value,
                datactx->subkeys[i]->ptr);
        serverAssert(subval);
        rawvals[i] = hashEncodeSubval(subval);
        decrRefCount(subval);
    }
    *action = ROCKS_WRITE;
    *numkeys = datactx->num;
    *pcfs = cfs;
    *prawkeys = rawkeys;
    *prawvals = rawvals;
    return 0;
}

/* decoded object move to exec module */
int hashDecodeData(swapData *data, int num, int *cfs, sds *rawkeys,
        sds *rawvals, robj **pdecoded) {
    int i;
    robj *decoded;
    serverAssert(num >= 0);
    UNUSED(cfs);

    /* Note that event if all subkeys are not found, still an empty hash
     * object will be returned: empty *warm* hash could can meta in memory,
     * so that we don't need to update rocks-meta right after call(). */
    decoded = createHashObject();

    for (i = 0; i < num; i++) {
        int dbid;
        sds subkey, subval;
        const char *keystr, *subkeystr;
        size_t klen, slen;
        robj *subvalobj;

        if (rawvals[i] == NULL)
            continue;
        if (rocksDecodeDataKey(rawkeys[i],sdslen(rawkeys[i]),
                &dbid,&keystr,&klen,&subkeystr,&slen) < 0)
            continue;
        if (!swapDataPersisted(data))
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
    return 0;
}

static inline robj *createSwapInObject(robj *newval) {
    robj *swapin = newval;
    serverAssert(newval && newval->type == OBJ_HASH);
    incrRefCount(newval);
    swapin->dirty = 0;
    return swapin;
}

/* Note: meta are kept as long as there are data in rocksdb. */
int hashSwapIn(swapData *data, robj *result, void *datactx) {
    UNUSED(datactx);
    /* hot key no need to swap in, this must be a warm or cold key. */
    serverAssert(swapDataPersisted(data));
    if (swapDataIsCold(data) && result != NULL /* may be empty */) {
        /* cold key swapped in result (may be empty). */
        robj *swapin = createSwapInObject(result);
        dbAdd(data->db,data->key,swapin);
        /* expire will be swapped in later by swap framework. */
        if (data->cold_meta) {
            dbAddMeta(data->db,data->key,data->cold_meta);
            data->cold_meta = NULL; /* moved */
        }
    }

    return 0;
}

/* subkeys already cleaned by cleanObject(to save cpu usage of main thread),
 * swapout only updates db.dict keyspace, meta (db.meta/db.expire) swapped
 * out by swap framework. */
int hashSwapOut(swapData *data, void *datactx) {
    UNUSED(datactx);
    serverAssert(!swapDataIsCold(data));

    if (hashTypeLength(data->value) == 0) {
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
    } else { /* not all fields swapped out. */
        if (data->new_meta) {
            dbAddMeta(data->db,data->key,data->new_meta);
            data->new_meta = NULL; /* moved to db.meta */
        }
    }

    return 0;
}

int hashSwapDel(swapData *data, void *datactx, int del_skip) {
    UNUSED(datactx);
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

/* Decoded moved back by exec to hashSwapData */
robj *hashCreateOrMergeObject(swapData *data, robj *decoded, void *datactx) {
    robj *result;
    UNUSED(datactx);
    serverAssert(decoded == NULL || decoded->type == OBJ_HASH);

    if (swapDataIsCold(data) || decoded == NULL) {
        /* decoded moved back to swap framework again (result will later be
         * pass as swapIn param). */
        result = decoded;
        if (decoded) {
            swapDataObjectMetaModifyLen(data,-hashTypeLength(decoded));
        }
    } else {
        hashTypeIterator *hi;
        hi = hashTypeInitIterator(decoded);
        while (hashTypeNext(hi) != C_ERR) {
            sds subkey = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_KEY);
            sds subval = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_VALUE);
            int updated = hashTypeSet(data->value, subkey, subval,
                    HASH_SET_TAKE_FIELD|HASH_SET_TAKE_VALUE);
            if (!updated) {
                swapDataObjectMetaModifyLen(data,-1);
            }
        }
        hashTypeReleaseIterator(hi);
        /* decoded merged, we can release it now. */
        decrRefCount(decoded);
        result = NULL;
    }
    return result;
}

int hashCleanObject(swapData *data, void *datactx_) {
    hashDataCtx *datactx = datactx_;
    if (swapDataIsCold(data)) return 0;
    for (int i = 0; i < datactx->num; i++) {
        if (hashTypeDelete(data->value,datactx->subkeys[i]->ptr)) {
            swapDataObjectMetaModifyLen(data,1);
        }
    }
    return 0;
}

/* Only free extend fields here, base fields (key/value/object_meta) freed
 * in swapDataFree */
void freeHashSwapData(swapData *data_, void *datactx_) {
    UNUSED(data_);
    hashDataCtx *datactx = datactx_;
    for (int i = 0; i < datactx->num; i++) {
        decrRefCount(datactx->subkeys[i]);
    }
    zfree(datactx->subkeys);
    zfree(datactx);
}

swapDataType hashSwapDataType = {
    .name = "hash",
    .swapAna = hashSwapAna,
    .encodeKeys = hashEncodeKeys,
    .encodeData = hashEncodeData,
    .decodeData = hashDecodeData,
    .swapIn = hashSwapIn,
    .swapOut = hashSwapOut,
    .swapDel = hashSwapDel,
    .createOrMergeObject = hashCreateOrMergeObject,
    .cleanObject = hashCleanObject,
    .free = freeHashSwapData,
};

int swapDataSetupHash(swapData *d, void **pdatactx) {
    d->type = &hashSwapDataType;
    d->omtype = &hashObjectMetaType;
    hashDataCtx *datactx = zmalloc(sizeof(hashDataCtx));
    datactx->num = 0;
    datactx->subkeys = NULL;
    *pdatactx = datactx;
    return 0;
}

/* Hash rdb save */
int hashSaveStart(rdbKeySaveData *save, rio *rdb) {
    robj *key = save->key;
    size_t nfields = 0;
    int ret = 0;

    /* save header */
    if (rdbSaveKeyHeader(rdb,key,key,RDB_TYPE_HASH,
                save->expire) == -1)
        return -1;

    /* nfields */
    if (save->value)
        nfields += hashTypeLength(save->value);
    if (save->object_meta)
        nfields += save->object_meta->len;
    if (rdbSaveLen(rdb,nfields) == -1)
        return -1;

    if (!save->value)
        return 0;

    /* save fields from value (db.dict) */
    hashTypeIterator *hi = hashTypeInitIterator(save->value);
    while (hashTypeNext(hi) != C_ERR) {
        sds subkey = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_KEY);
        sds subval = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_VALUE);
        if (rdbSaveRawString(rdb,(unsigned char*)subkey,
                    sdslen(subkey)) == -1) {
            sdsfree(subkey);
            sdsfree(subval);
            ret = -1;
            break;
        }
        if (rdbSaveRawString(rdb,(unsigned char*)subval,
                    sdslen(subval)) == -1) {
            sdsfree(subkey);
            sdsfree(subval);
            ret = -1;
            break;
        }
        sdsfree(subkey);
        sdsfree(subval);
    }
    hashTypeReleaseIterator(hi);

    return ret;
}

/* return 1 if hash still need to consume more rawkey. */
int hashSave(rdbKeySaveData *save, rio *rdb, decodedData *decoded) {
    robj *key = save->key;

    /* TODO remove */
    serverAssert(!sdscmp(decoded->key, key->ptr));

    if (decoded->rdbtype != RDB_TYPE_STRING) {
        /* check failed, skip this key */
        return 0;
    }

    if (save->value != NULL) {
        if (hashTypeExists(save->value,
                    decoded->subkey)) {
            /* already save in save_start, skip this subkey */
            return 0;
        }
    }

    if (rdbSaveRawString(rdb,(unsigned char*)decoded->subkey,
                sdslen(decoded->subkey)) == -1) {
        return -1;
    }

    if (rdbWriteRaw(rdb,(unsigned char*)decoded->rdbraw,
                sdslen(decoded->rdbraw)) == -1) {
        return -1;
    }

    save->saved++;
    return 0;
}

int hashSaveEnd(rdbKeySaveData *save, int save_result) {
    objectMeta *object_meta = save->object_meta;
    if (save->saved != object_meta->len) {
        sds key  = save->key->ptr;
        sds repr = sdscatrepr(sdsempty(), key, sdslen(key));
        serverLog(LL_WARNING,
                "hashSave %s: saved(%d) != object_meta.len(%ld)",
                repr, save->saved, (ssize_t)object_meta->len);
        sdsfree(repr);
        return -1;
    }
    return save_result;
}

rdbKeySaveType hashSaveType = {
    .save_start = hashSaveStart,
    .save = hashSave,
    .save_end = hashSaveEnd,
    .save_deinit = NULL,
};

int hashSaveInit(rdbKeySaveData *save, const char *extend, size_t extlen) {
    int retval = 0;
    save->type = &hashSaveType;
    save->omtype = &hashObjectMetaType;
    if (extend) {
        serverAssert(save->object_meta == NULL);
        retval = buildObjectMeta(OBJ_HASH,extend,
                extlen,&save->object_meta);
    }
    return retval;
}

/* Hash rdb load */
void hashLoadStartHT(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
	int isencode;
	unsigned long long len;
	sds hash_header, extend = NULL;

    hash_header = rdbVerbatimNew((unsigned char)load->rdbtype);

	/* nfield */
	if (rdbLoadLenVerbatim(rdb,&hash_header,&isencode,&len)) {
		sdsfree(hash_header);
        *error = RDB_LOAD_ERR_OTHER;
        return;
	}

    load->total_fields = len;

    extend = rocksEncodeObjectMetaLen(len);

    *cf = META_CF;
    *rawkey = rocksEncodeMetaKey(load->db,load->key);
    *rawval = rocksEncodeMetaVal(load->object_type,load->expire,extend);
    *error = 0;

    sdsfree(extend);
    sdsfree(hash_header);
}

void hashLoadStartZip(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    sds extend = NULL;

    load->value = rdbLoadObject(load->rdbtype,rdb,load->key,error);
    if (load->value == NULL) return;

    if (load->value->type != OBJ_HASH) {
        serverLog(LL_WARNING,"Load rdb with rdbtype(%d) got (%d)",
                load->rdbtype, load->value->type);
        *error = RDB_LOAD_ERR_OTHER;
        return;
    }

    load->iter = hashTypeInitIterator(load->value);
    if (hashTypeNext(load->iter) == C_ERR) {
        serverLog(LL_WARNING,"Load rdb iter not valid.");
        *error = RDB_LOAD_ERR_OTHER;
        return;
    }
    
    load->total_fields = hashTypeLength(load->value);
    extend = rocksEncodeObjectMetaLen(load->total_fields);
    *cf = META_CF;
    *rawkey = rocksEncodeMetaKey(load->db,load->key);
    *rawval = rocksEncodeMetaVal(load->object_type,load->expire,extend);
    sdsfree(extend);
}

void hashLoadStart(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    switch (load->rdbtype) {
    case RDB_TYPE_HASH_ZIPMAP:
        hashLoadStartZip(load,rdb,cf,rawkey,rawval,error);
        break;
    case RDB_TYPE_HASH_ZIPLIST:
        hashLoadStartZip(load,rdb,cf,rawkey,rawval,error);
        break;
    case RDB_TYPE_HASH:
        hashLoadStartHT(load,rdb,cf,rawkey,rawval,error);
        break;
    default:
        break;
    }
}

int hashLoadZip(struct rdbKeyLoadData *load, rio *rdb, int *cf, sds *rawkey,
        sds *rawval, int *error) {
    sds subkey, subval;
    robj subobj;
    
    UNUSED(rdb);

    subkey = hashTypeCurrentObjectNewSds(load->iter,OBJ_HASH_KEY);
    subval = hashTypeCurrentObjectNewSds(load->iter,OBJ_HASH_VALUE);
    initStaticStringObject(subobj,subval);

    *cf = DATA_CF;
    *rawkey = rocksEncodeDataKey(load->db,load->key,subkey);
    *rawval = rocksEncodeValRdb(&subobj);
    *error = 0;

    sdsfree(subkey);
    sdsfree(subval);

    return hashTypeNext(load->iter) != C_ERR;
}

int hashLoadHT(struct rdbKeyLoadData *load, rio *rdb, int *cf, sds *rawkey,
        sds *rawval, int *error) {
    sds subkey, rdbval;

    *error = RDB_LOAD_ERR_OTHER;
    if ((subkey = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
        return 0;
    }

    rdbval = rdbVerbatimNew(RDB_TYPE_STRING);
    if (rdbLoadStringVerbatim(rdb,&rdbval)) {
        sdsfree(rdbval);
        sdsfree(subkey);
        return 0;
    }

    *cf = DATA_CF;
    *rawkey = rocksEncodeDataKey(load->db,load->key,subkey);
    *rawval = rdbval;
    *error = 0;
    sdsfree(subkey);
    load->loaded_fields++;
    return load->loaded_fields < load->total_fields;
}

int hashLoad(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    int retval;

    switch (load->rdbtype) {
    case RDB_TYPE_HASH:
        retval = hashLoadHT(load,rdb,cf,rawkey,rawval,error);
        break;
    case RDB_TYPE_HASH_ZIPMAP:
    case RDB_TYPE_HASH_ZIPLIST:
        retval = hashLoadZip(load,rdb,cf,rawkey,rawval,error);
        break;
    default:
        retval = RDB_LOAD_ERR_OTHER;
    }

    return retval;
}

void hashLoadDeinit(struct rdbKeyLoadData *load) {
    if (load->iter) {
        hashTypeReleaseIterator(load->iter);
        load->iter = NULL;
    }

    if (load->value) {
        decrRefCount(load->value);
        load->value = NULL;
    }
}

rdbKeyLoadType hashLoadType = {
    .load_start = hashLoadStart,
    .load = hashLoad,
    .load_end = NULL,
    .load_deinit = hashLoadDeinit,
};

void hashLoadInit(rdbKeyLoadData *load) {
    load->type = &hashLoadType;
    load->omtype = &hashObjectMetaType;
    load->object_type = OBJ_HASH;
}

#ifdef REDIS_TEST

#define SWAP_EVICT_STEP 2
#define SWAP_EVICT_MEM  (1*1024*1024)

int swapDataHashTest(int argc, char **argv, int accurate) {
    UNUSED(argc), UNUSED(argv), UNUSED(accurate);

    initTestRedisServer();
    redisDb* db = server.db + 0;
    int error = 0;
    swapData *hash1_data, *cold1_data;
    hashDataCtx *hash1_ctx, *cold1_ctx = NULL;
    robj *key1, *hash1, *cold1;
    objectMeta *cold1_meta;
    keyRequest _kr1, *kr1 = &_kr1, _cold_kr1, *cold_kr1 = &_cold_kr1;
    int intention;
    uint32_t intention_flags;
    robj *subkeys1[4];
    sds f1,f2,f3,f4;
    sds sds1, sds2, int1, int2;
    int action, numkeys, *cfs;
    sds *rawkeys, *rawvals;
    server.child_pid = -1;
    
    TEST("hash - init") {
        server.swap_evict_step_max_subkeys = SWAP_EVICT_STEP;
        server.swap_evict_step_max_memory = SWAP_EVICT_MEM;

        key1 = createStringObject("key1",4);
        cold1 = createStringObject("cold1",5);
        cold1_meta = createHashObjectMeta(4);
        f1 = sdsnew("f1"), f2 = sdsnew("f2"), f3 = sdsnew("f3"), f4 = sdsnew("f4");
        sds1 = sdsnew("sds_v1"), sds2 = sdsnew("sds_v2");
        int1 = sdsnew("1"), int2 = sdsnew("2");
        hash1 = createHashObject();
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

        hash1_data = createSwapData(db,key1,hash1);
        swapDataSetupMeta(hash1_data,OBJ_HASH,-1,(void**)&hash1_ctx);
        swapDataSetObjectMeta(hash1_data, NULL);

        cold1_data = createSwapData(db,cold1,NULL);
        swapDataSetupMeta(cold1_data,OBJ_HASH,-1,(void**)&cold1_ctx);
        swapDataSetObjectMeta(cold1_data, cold1_meta);
    }

    TEST("hash - swapAna") {
        /* nop: NOP/IN_META/IN_DEL/IN hot/OUT cold... */
        kr1->cmd_intention = SWAP_NOP, kr1->cmd_intention_flags = 0;
        swapDataAna(hash1_data,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        kr1->cmd_intention = SWAP_IN, kr1->cmd_intention_flags = SWAP_IN_META;
        swapDataAna(hash1_data,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        kr1->cmd_intention = SWAP_IN, kr1->cmd_intention_flags = SWAP_IN_DEL;
        swapDataAna(hash1_data,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        kr1->cmd_intention = SWAP_IN, kr1->cmd_intention_flags = 0;
        swapDataAna(hash1_data,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        kr1->cmd_intention = SWAP_IN, kr1->cmd_intention_flags = 0;
        swapDataAna(hash1_data,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        cold_kr1->cmd_intention = SWAP_OUT, cold_kr1->cmd_intention_flags = 0;
        swapDataAna(cold1_data,cold_kr1,&intention,&intention_flags,cold1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        cold_kr1->cmd_intention = SWAP_DEL, cold_kr1->cmd_intention_flags = 0;
        swapDataAna(cold1_data,cold_kr1,&intention,&intention_flags,cold1_ctx);
        test_assert(intention == SWAP_DEL && intention_flags == 0);
        /* in: entire or with subkeys */
        cold_kr1->cmd_intention = SWAP_IN, cold_kr1->cmd_intention_flags = 0;
        swapDataAna(cold1_data,cold_kr1,&intention,&intention_flags,cold1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == 0);
        test_assert(cold1_ctx->num == 0 && cold1_ctx->subkeys == NULL);
        subkeys1[0] = createStringObject(f1,sdslen(f1));
        subkeys1[1] = createStringObject(f2,sdslen(f2));
        cold_kr1->num_subkeys = 2;
        cold_kr1->subkeys = subkeys1;
        cold_kr1->cmd_intention = SWAP_IN, cold_kr1->cmd_intention_flags = 0;
        swapDataAna(cold1_data,cold_kr1,&intention,&intention_flags,cold1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == 0);
        test_assert(cold1_ctx->num == 2 && cold1_ctx->subkeys != NULL);
        /* out: evict by small steps */
        kr1->num_subkeys = 0;
        kr1->subkeys = NULL;
        kr1->cmd_intention = SWAP_OUT, kr1->cmd_intention_flags = 0;
        swapDataAna(hash1_data,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_OUT && intention_flags == 0);
        test_assert(cold1_ctx->num == SWAP_EVICT_STEP && cold1_ctx->subkeys != NULL);
    }

    TEST("hash - encodeData/DecodeData") {
        robj *decoded;
        size_t old = server.swap_evict_step_max_subkeys;
        hashSwapData *hash1_data_ = (hashSwapData*)hash1_data;
        server.swap_evict_step_max_subkeys = 1024;
        kr1->num_subkeys = 0;
        kr1->subkeys = NULL;
        kr1->cmd_intention = SWAP_OUT, kr1->cmd_intention_flags = 0;
        zfree(hash1_ctx->subkeys), hash1_ctx->subkeys = NULL;
        hash1_ctx->num = 0;
        hash1_data_->d.object_meta = createHashObjectMeta(1);
        swapDataAna(hash1_data,kr1,&intention,&intention_flags,hash1_ctx);
        test_assert(intention == SWAP_OUT && intention_flags == SWAP_EXEC_OUT_META);
        test_assert(hash1_ctx->num == (int)hashTypeLength(hash1_data_->d.value));
        serverAssert(hash1_ctx->subkeys != NULL);

        hashEncodeData(hash1_data,intention,hash1_ctx,
                &action,&numkeys,&cfs,&rawkeys,&rawvals);
        test_assert(action == ROCKS_WRITE);
        test_assert(numkeys == hash1_ctx->num);

        hashDecodeData(hash1_data,numkeys,cfs,rawkeys,rawvals,&decoded);
        test_assert(hashTypeLength(decoded) == hashTypeLength(hash1));

        freeObjectMeta(hash1_data_->d.object_meta);
        hash1_data_->d.object_meta = NULL;
        server.swap_evict_step_max_subkeys = old;
    }

    TEST("hash - swapIn/swapOut") {
        robj *h;
        objectMeta *m, *sm = createHashObjectMeta(0);
        hashSwapData _data = *(hashSwapData*)hash1_data, *data = &_data;
        test_assert(lookupMeta(db,key1) == NULL);

        /* hot => warm => cold */
        hashTypeDelete(hash1,f1);
        hashTypeDelete(hash1,f2);
        data->d.object_meta = NULL, data->d.new_meta = sm, sm->len = 2;
        hashSwapOut((swapData*)data, hash1_ctx);
        test_assert((m =lookupMeta(db,key1)) != NULL && m->len == 2);
        test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) != NULL);

        hashTypeDelete(hash1,f3);
        hashTypeDelete(hash1,f4);
        data->d.object_meta = sm, data->d.new_meta = NULL, sm->len = 2;
        hashSwapOut((swapData*)data, hash1_ctx);
        test_assert((m = lookupMeta(db,key1)) == NULL);
        test_assert((h = lookupKey(db,key1,LOOKUP_NOTOUCH)) == NULL);

        /* cold => warm => hot */
        hash1 = createHashObject();
        hashTypeSet(hash1,f1,sds1,HASH_SET_COPY);
        hashTypeSet(hash1,f2,sds2,HASH_SET_COPY);
        data->d.value = h;
        data->d.object_meta = sm, data->d.new_meta = NULL, sm->len = 2;
        hashSwapIn((swapData*)data,hash1,hash1_ctx);
        test_assert((m = lookupMeta(db,key1)) != NULL && m->len == 2);
        test_assert((h = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(hashTypeLength(h) == 2);

        hashTypeSet(hash1,f3,int1,HASH_SET_COPY);
        hashTypeSet(hash1,f4,int2,HASH_SET_COPY);
        data->d.value = h;
        data->d.object_meta = m;
        hashCreateOrMergeObject((swapData*)data,hash1,hash1_ctx);
        hashSwapIn((swapData*)data,NULL,hash1_ctx);
        test_assert((m = lookupMeta(db,key1)) != NULL);
        test_assert((h = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(hashTypeLength(h) == 4);

        /* hot => cold */
        hashTypeDelete(hash1,f1);
        hashTypeDelete(hash1,f2);
        hashTypeDelete(hash1,f3);
        hashTypeDelete(hash1,f4);
        *data = *(hashSwapData*)hash1_data;
        data->d.object_meta = NULL, data->d.new_meta = sm, sm->len = 4;
        hashSwapOut((swapData*)data, hash1_ctx);
        test_assert((m = lookupMeta(db,key1)) == NULL);
        test_assert((h = lookupKey(db,key1,LOOKUP_NOTOUCH)) == NULL);

        /* cold => hot */
        hash1 = createHashObject();
        hashTypeSet(hash1,f1,sds1,HASH_SET_COPY);
        hashTypeSet(hash1,f2,sds2,HASH_SET_COPY);
        hashTypeSet(hash1,f3,int1,HASH_SET_COPY);
        hashTypeSet(hash1,f4,int2,HASH_SET_COPY);
        data->d.value = h;
        data->d.object_meta = createHashObjectMeta(0);
        hashSwapIn((swapData*)data,hash1,hash1_ctx);
        test_assert((m = lookupMeta(db,key1)) != NULL);
        test_assert((h = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(hashTypeLength(h) == 4);
    }

    TEST("hash - rdbLoad & rdbSave") {
        server.swap_big_hash_threshold = 0;
        int err = 0;
        sds myhash_key = sdsnew("myhash");
        robj *myhash = createHashObject();
        sds f1 = sdsnew("f1"), f2 = sdsnew("f2");
        sds rdbv1 = rocksEncodeValRdb(createStringObject("v1", 2));
        sds rdbv2 = rocksEncodeValRdb(createStringObject("v2", 2));
        hashTypeSet(myhash,f1,sdsnew("v1"),HASH_SET_COPY);
        hashTypeSet(myhash,f2,sdsnew("v2"),HASH_SET_COPY);
        hashTypeConvert(myhash,OBJ_ENCODING_HT);

        /* rdbLoad */
        rio sdsrdb;
        long long NOW = 1662552125000;
        sds rawval = rocksEncodeValRdb(myhash);
        rioInitWithBuffer(&sdsrdb,sdsnewlen(rawval+1,sdslen(rawval)-1));
        rdbKeyLoadData _load, *load = &_load;
        rdbKeyLoadDataInit(load,rawval[0],db,myhash_key,-1,NOW);
        sds metakey, metaval, subkey, subraw;
        int cont, cf;
        hashLoadStart(load,&sdsrdb,&cf,&metakey,&metaval,&err);
        test_assert(cf == META_CF && err == 0);
        test_assert(!sdscmp(metakey,rocksEncodeMetaKey(db,myhash_key)));
        cont = hashLoad(load,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cf == DATA_CF && cont == 1 && err == 0);
        cont = hashLoad(load,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cf == DATA_CF && cont == 0 && err == 0);
        test_assert(load->loaded_fields == 2);
        test_assert(load->object_type == OBJ_HASH);

        sds coldraw,warmraw,hotraw;
        objectMeta *object_meta = createHashObjectMeta(2);

        rio rdbcold, rdbwarm, rdbhot;
        rdbKeySaveData _save, *save = &_save;

        /* save cold */
        decodedMeta _decoded_meta, *decoded_meta = &_decoded_meta;
        decoded_meta->cf = META_CF;
        decoded_meta->dbid = db->id;
        decoded_meta->expire = -1;
        decoded_meta->extend = hashObjectMetaType.encodeObjectMeta(object_meta);
        decoded_meta->key = myhash_key;
        decoded_meta->object_type = OBJ_HASH;

        rioInitWithBuffer(&rdbcold,sdsempty());
        test_assert(!rdbKeySaveDataInit(save,db,(decodedResult*)decoded_meta));
        test_assert(rdbKeySaveStart(save,&rdbcold) == 0);

        decodedData _decoded_fx, *decoded_fx = &_decoded_fx;
        decoded_fx->cf = DATA_CF;
        decoded_fx->dbid = db->id;
        decoded_fx->key = myhash_key;
        decoded_fx->rdbtype = rdbv2[0];
        decoded_fx->subkey = f2;
        decoded_fx->rdbraw = sdsnewlen(rdbv2+1, sdslen(rdbv2)-1);
        test_assert(rdbKeySave(save,&rdbcold,decoded_fx) == 0);

        decoded_fx->key = myhash_key;
        decoded_fx->subkey = f1, decoded_fx->rdbraw = sdsnewlen(rdbv1+1,sdslen(rdbv1)-1);
        test_assert(rdbKeySave(save,&rdbcold,decoded_fx) == 0);
        coldraw = rdbcold.io.buffer.ptr;

        /* save warm */
        rioInitWithBuffer(&rdbwarm,sdsempty());
        robj *value = createHashObject(), keyobj;
        hashTypeSet(value,f2,sdsnew("v2"),HASH_SET_COPY);
        initStaticStringObject(keyobj,myhash_key);
        dbAdd(db,&keyobj,value);
        object_meta->len = 1;
        dbAddMeta(db,&keyobj,object_meta);
        test_assert(!rdbKeySaveDataInit(save,db,(decodedResult*)decoded_meta));
        test_assert(rdbKeySaveStart(save,&rdbwarm) == 0);
        test_assert(rdbKeySave(save,&rdbwarm,decoded_fx) == 0);
        warmraw = rdbwarm.io.buffer.ptr;

        /* save hot */
        rioInitWithBuffer(&rdbhot,sdsempty());
        initStaticStringObject(keyobj,myhash_key);
        test_assert(rdbSaveKeyValuePair(&rdbhot,&keyobj,myhash,-1) != -1);
        hotraw = rdbhot.io.buffer.ptr;

        test_assert(!sdscmp(hotraw,coldraw) && !sdscmp(hotraw,warmraw));
    }


    return error;
}

#endif

