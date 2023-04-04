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
#include <math.h>
static void createFakeZsetForDeleteIfCold(swapData *data) {
	if (swapDataIsCold(data)) {
        /* empty zset allowed */
		dbAdd(data->db,data->key,createZsetObject());
	}
}

int zsetSwapAna(swapData *data, struct keyRequest *req,
        int *intention, uint32_t *intention_flags, void *datactx_) {
    zsetDataCtx *datactx = datactx_;
    int cmd_intention = req->cmd_intention;
    uint32_t cmd_intention_flags = req->cmd_intention_flags;

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
        } else if(req->type == KEYREQUEST_TYPE_SCORE ) {
            datactx->type = TYPE_ZS;
            datactx->zs.reverse = req->zs.reverse;
            datactx->zs.limit = req->zs.limit;
            datactx->zs.rangespec = req->zs.rangespec;
            req->zs.rangespec = NULL;
            *intention = SWAP_IN;
            *intention_flags = 0;
            if (cmd_intention_flags == SWAP_IN_DEL 
                || cmd_intention_flags & SWAP_IN_OVERWRITE) {
                objectMeta *meta = swapDataObjectMeta(data);
                if (meta->len == 0) {
                    *intention = SWAP_DEL;
                    *intention_flags = SWAP_FIN_DEL_SKIP;
                } else {
                    *intention = SWAP_IN;
                    *intention_flags = SWAP_EXEC_IN_DEL;
                } 
            } 
        } else if (req->b.num_subkeys == 0) {
            if (cmd_intention_flags == SWAP_IN_DEL_MOCK_VALUE) {
                /* DEL/GETDEL: Lazy delete current key. */
                datactx->bdc.ctx_flag |= BIG_DATA_CTX_FLAG_MOCK_VALUE;
                
                *intention = SWAP_DEL;
                *intention_flags = SWAP_FIN_DEL_SKIP;
            }  else if (cmd_intention_flags == SWAP_IN_DEL
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
                /* HLEN: swap in meta (with random field gets empty zset)
                 * also HLEN command will be modified like dbsize. */
                datactx->bdc.num = 0;
                datactx->bdc.subkeys = zmalloc(sizeof(robj*));
                datactx->bdc.subkeys[datactx->bdc.num++] = createStringObject("foo",3);
                *intention = SWAP_IN;
                *intention_flags = 0;
            } else {
                /* HKEYS/HVALS/..., swap in all fields */
                datactx->bdc.num = 0;
                datactx->bdc.subkeys = NULL;
                *intention = SWAP_IN;
                *intention_flags = 0;
            }
        } else { /* keyrequests with subkeys */
            objectMeta *meta = swapDataObjectMeta(data);
            if (req->cmd_intention_flags == SWAP_IN_DEL) {
                datactx->bdc.num = 0;
                datactx->bdc.subkeys = zmalloc(req->b.num_subkeys * sizeof(robj *));
                /* ZREM: even if field is hot (exists in value), we still
                    * need to do ROCKS_DEL on those fields. */
                for (int i = 0; i < req->b.num_subkeys; i++) {
                    robj *subkey = req->b.subkeys[i];
                    incrRefCount(subkey);
                    datactx->bdc.subkeys[datactx->bdc.num++] = subkey;
                }
                *intention = SWAP_IN;
                *intention_flags = SWAP_EXEC_IN_DEL;
            } else if (meta->len == 0) {
                *intention = SWAP_NOP;
                *intention_flags = 0;
            } else {
                datactx->bdc.num = 0;
                datactx->bdc.subkeys = zmalloc(req->b.num_subkeys * sizeof(robj *));
                for (int i = 0; i < req->b.num_subkeys; i++) {
                    robj *subkey = req->b.subkeys[i];
                    double score;
                    if (data->value == NULL || zsetScore(data->value, subkey->ptr, &score) == C_ERR) {
                        incrRefCount(subkey);
                        datactx->bdc.subkeys[datactx->bdc.num++] = subkey;
                    }
                }
                *intention = datactx->bdc.num > 0 ? SWAP_IN : SWAP_NOP;
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
            datactx->bdc.subkeys = zmalloc(
                    server.swap_evict_step_max_subkeys*sizeof(robj*));
            
            int len = zsetLength(data->value);
                robj *subkey;
            if (len > 0) {
                if (data->value->encoding == OBJ_ENCODING_ZIPLIST) {
                    unsigned char *zl = data->value->ptr;
                    unsigned char *eptr, *sptr;
                    unsigned char *vstr;
                    unsigned int vlen;
                    long long vlong;
                    eptr = ziplistIndex(zl, 0);
                    sptr = ziplistNext(zl, eptr);
                    while (eptr != NULL) {
                        vlong = 0;
                        ziplistGet(eptr, &vstr, &vlen, &vlong);
                        evict_memory += vlen;
                        if (vstr != NULL) {
                            subkey = createStringObject((const char*)vstr, vlen);
                        } else {
                            subkey = createObject(OBJ_STRING,sdsfromlonglong(vlong));
                        }
                        datactx->bdc.subkeys[datactx->bdc.num++] = subkey;
                        ziplistGet(sptr, &vstr, &vlen, &vlong);
                        evict_memory += vlen;
                        if (datactx->bdc.num >= server.swap_evict_step_max_subkeys ||
                                evict_memory >= server.swap_evict_step_max_memory) {
                            /* Evict big zset in small steps. */
                            break;
                        }
                        zzlNext(zl, &eptr, &sptr);
                    }
                } else if (data->value->encoding == OBJ_ENCODING_SKIPLIST) {
                    zset *zs = data->value->ptr;
                    dict* d = zs->dict;
                    dictIterator* di = dictGetIterator(d);
                    dictEntry *de;
                    while ((de = dictNext(di)) != NULL) {
                        sds skey = dictGetKey(de);
                        subkey = createStringObject(skey, sdslen(skey));
                        datactx->bdc.subkeys[datactx->bdc.num++] = subkey;
                        evict_memory += sizeof(zset) + sizeof(dictEntry);
                        if (datactx->bdc.num >= server.swap_evict_step_max_subkeys ||
                                evict_memory >= server.swap_evict_step_max_memory) {
                            /* Evict big zset in small steps. */
                            break;
                        }
                    }
                    dictReleaseIterator(di);
                } else {
                    *intention = SWAP_NOP;
                    return 0;
                }
            }

            /* create new meta if needed */
            if (!swapDataPersisted(data)) {
                swapDataSetNewObjectMeta(data,
                        createZsetObjectMeta(swapGetAndIncrVersion(),0));
            }

            if (!data->value->dirty) {
                /* directly evict value from db.dict if not dirty. */
                
                swapDataCleanObject(data, datactx);
                if (zsetLength(data->value) == 0) {
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

int zsetSwapAnaAction(swapData *data, int intention, void *datactx_, int *action) {
    UNUSED(data);
    zsetDataCtx *datactx = datactx_;
    switch (intention) {
        case SWAP_IN:
            if (datactx->type != TYPE_NONE) {
                *action = ROCKS_ITERATE;
            } else if (datactx->bdc.num) { /* Swap in specific fields */
                *action = ROCKS_GET;
            } else { /* Swap in entire zset. */
                *action = ROCKS_ITERATE;
            }
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

sds zsetEncodeScoreKey(redisDb* db, sds key, uint64_t version,
        sds subkey, double score) {
    return encodeScoreKey(db, key, version, score, subkey);
}

sds zsetEncodeIntervalScoreKey(redisDb* db, int ex, sds key, uint64_t version,
        double score) {
    return encodeIntervalSds(ex, zsetEncodeScoreKey(db, key, version,
                shared.emptystring->ptr, score));
}

sds zsetEncodeIntervalKey(redisDb* db, int ex, sds key, uint64_t version,
        sds subkey) {
    return encodeIntervalSds(ex, rocksEncodeDataKey(db, key, version, subkey));
}

int zsetDecodeScoreKey(const char* raw, int rawlen, int* dbid, const char** key,
        size_t* keylen, uint64_t *version, const char** subkey,
        size_t* subkeylen, double* score) {
    return decodeScoreKey(raw, rawlen, dbid, key, keylen, version, score,
            subkey,subkeylen);
}

#define SCORE_DEVIATION 0.001
double nextDouble(double value, uint64_t offset) {
    uint64_t u64;
    memcpy(&u64, &value, sizeof(value));
    u64 += offset;
    double next;
    memcpy(&next, &u64, sizeof(next));
    return next;
}

int zsetEncodeKeys(swapData *data, int intention, void *datactx_,
        int *numkeys, int **pcfs, sds **prawkeys) {
    zsetDataCtx *datactx = datactx_;
    sds *rawkeys = NULL;
    int *cfs = NULL, i;
    uint64_t version = swapDataObjectVersion(data);

    serverAssert(intention == SWAP_IN);
    serverAssert(datactx->type == TYPE_NONE);
    serverAssert(datactx->bdc.num);

    cfs = zmalloc(sizeof(int)*datactx->bdc.num);
    rawkeys = zmalloc(sizeof(sds)*datactx->bdc.num);
    for (i = 0; i < datactx->bdc.num; i++) {
        cfs[i] = DATA_CF;
        rawkeys[i] = rocksEncodeDataKey(data->db,data->key->ptr,version,datactx->bdc.subkeys[i]->ptr);
    }
    *numkeys = datactx->bdc.num;
    *prawkeys = rawkeys;
    *pcfs = cfs;

    return 0;
}

static sds zsetEncodeSubval(double score) {
    rio sdsrdb;
    rioInitWithBuffer(&sdsrdb,sdsempty());
    rdbSaveType(&sdsrdb,RDB_TYPE_STRING);
    rdbSaveBinaryDoubleValue(&sdsrdb, score);
    return sdsrdb.io.buffer.ptr;
}

sds zsetEncodeScoreValue(sds subkey, double score) {
    UNUSED(subkey);
    sds scoresds = sdsnewlen(SDS_NOINIT, sizeOfDouble);
    encodeDouble(scoresds, score);
    return scoresds;
}

int zsetDecodeScoreValue(sds rawval, int rawlen, double* score) {
    if (rawlen < (int)sizeOfDouble) {
        return 0;
    }
    return decodeDouble(rawval, score);
}

int zsetEncodeData(swapData *data, int intention, void *datactx_,
        int *numkeys, int **pcfs, sds **prawkeys, sds **prawvals) {
    zsetDataCtx *datactx = datactx_;
    uint64_t version = swapDataObjectVersion(data);
    if (datactx->bdc.num == 0) {
        *numkeys = 0;
        *prawkeys = NULL;
        *prawvals = NULL;
        return 0;
    }
    int *cfs = zmalloc(datactx->bdc.num*2*sizeof(int));
    sds *rawkeys = zmalloc(datactx->bdc.num*2*sizeof(sds));
    sds *rawvals = zmalloc(datactx->bdc.num*2*sizeof(sds));
    serverAssert(intention == SWAP_OUT);
    for (int i = 0; i < datactx->bdc.num; i++) {
        cfs[i * 2] = DATA_CF;
        rawkeys[i * 2] = rocksEncodeDataKey(data->db,data->key->ptr,version,
                datactx->bdc.subkeys[i]->ptr);
        double score = 0;
        serverAssert(C_OK == zsetScore(data->value,
                datactx->bdc.subkeys[i]->ptr, &score));
        rawvals[i * 2] = zsetEncodeSubval(score);

        cfs[i*2 + 1] = SCORE_CF;
        rawkeys[i * 2 + 1] = zsetEncodeScoreKey(data->db, data->key->ptr,
                version, datactx->bdc.subkeys[i]->ptr, score);
        rawvals[i * 2 + 1] = zsetEncodeScoreValue(datactx->bdc.subkeys[i]->ptr, score);
    }
    *numkeys = datactx->bdc.num*2;
    *pcfs = cfs;
    *prawkeys = rawkeys;
    *prawvals = rawvals;
    return 0;
}

int zsetEncodeRange(struct swapData *data, int intention, void *datactx_, int *limit,
                    uint32_t *flags, int *pcf, sds *start, sds *end) {
    zsetDataCtx *datactx = datactx_;
    uint64_t version = swapDataObjectVersion(data);
    serverAssert(intention == SWAP_IN);
    serverAssert(0 == datactx->bdc.num);

    *limit = ROCKS_ITERATE_NO_LIMIT;
    *flags = 0;
    if (datactx->type != TYPE_NONE) {
        if (datactx->type == TYPE_ZS) {
            *limit = datactx->zs.limit;
            *pcf = SCORE_CF;
            if (datactx->zs.reverse) *flags |= ROCKS_ITERATE_REVERSE;
            if (datactx->zs.rangespec->minex) *flags |= ROCKS_ITERATE_LOW_BOUND_EXCLUDE;

            *start = zsetEncodeScoreKey(data->db, data->key->ptr, version,
                                        shared.emptystring->ptr, datactx->zs.rangespec->min);
            if (datactx->zs.rangespec->maxex) {
                *end = zsetEncodeScoreKey(data->db, data->key->ptr, version,
                                          shared.emptystring->ptr, datactx->zs.rangespec->max);
            } else {
                /* key saved as "xxxx[score][subkey]", but end_key formatted as "xxxx[score]".
                 * So if we want keys with score datactx->zs.rangespec->max, we need search for score a bit wider.
                 * The logic after swap will filter key with wanted score exactly. */
                *end = zsetEncodeScoreKey(data->db, data->key->ptr, version,
                                          shared.emptystring->ptr, datactx->zs.rangespec->max + SCORE_DEVIATION);
            }

        }
    } else {
        *pcf = DATA_CF;
        *start = rocksEncodeDataRangeStartKey(data->db,data->key->ptr,version);
        *end = rocksEncodeDataRangeEndKey(data->db,data->key->ptr,version);
    }

    return 0;
}

static double zsetDecodeSubval(sds subval) {
    rio sdsrdb;
    rioInitWithBuffer(&sdsrdb, subval);
    serverAssert(rdbLoadType(&sdsrdb) == RDB_TYPE_STRING);
    double score = 0;
    serverAssert(rdbLoadBinaryDoubleValue(&sdsrdb, &score) != -1);
    return score;
}

int zsetDecodeBigData(swapData *data, int num, sds *rawkeys,
        sds *rawvals, robj **pdecoded) {
    int i;
    robj *decoded;
    uint64_t version = swapDataObjectVersion(data);
    /* Note that event if all subkeys are not found, still an empty zset
     * object will be returned: empty *warm* zset could can meta in memory,
     * so that we don't need to update rocks-meta right after call(). */
    decoded = createZsetZiplistObject();

    for (i = 0; i < num; i++) {
        sds subkey;
        const char *keystr, *subkeystr;
        size_t klen, slen;
        int dbid;
        uint64_t subkey_version;

        if (rawvals[i] == NULL || sdslen(rawvals[i]) == 0)
            continue;
        if (rocksDecodeDataKey(rawkeys[i],sdslen(rawkeys[i]),
                &dbid,&keystr,&klen,&subkey_version,&subkeystr,&slen) < 0)
            continue;
        if (!swapDataPersisted(data))
            continue;
        if (version != subkey_version)
            continue;
        subkey = sdsnewlen(subkeystr,slen);
        serverAssert(memcmp(data->key->ptr,keystr,klen) == 0); //TODO remove

        double score = zsetDecodeSubval(rawvals[i]);
        int flag = ZADD_IN_NONE;
        int retflags = 0;
        double newscore;
        serverAssert(zsetAdd(decoded,score, subkey, flag, &retflags, &newscore) == 1);
        sdsfree(subkey);
    }

    *pdecoded = decoded;
    return 0;
}


int zsetDecodeScoreData(swapData *data, int num, sds *rawkeys,
        sds *rawvals, robj **pdecoded) {
    int i;
    robj *decoded;
    uint64_t version = swapDataObjectVersion(data);
    /* Note that event if all subkeys are not found, still an empty zset
     * object will be returned: empty *warm* zset could can meta in memory,
     * so that we don't need to update rocks-meta right after call(). */
    decoded = createZsetZiplistObject();

    for (i = 0; i < num; i++) {
        sds subkey;
        const char *keystr, *subkeystr;
        size_t klen, slen;
        int dbid;
        double score;
        uint64_t subkey_version;

        if (rawvals[i] == NULL || sdslen(rawvals[i]) == 0)
            continue;
        if (zsetDecodeScoreKey(rawkeys[i],sdslen(rawkeys[i]),
                &dbid,&keystr,&klen,&subkey_version,&subkeystr,&slen, &score) < 0)
            continue;
        if (!swapDataPersisted(data))
            continue;
        if (version != subkey_version)
            continue;
        subkey = sdsnewlen(subkeystr,slen);
        serverAssert(strncmp(data->key->ptr,keystr,klen) == 0); //TODO remove

        int flag = ZADD_IN_NX;
        int retflags = 0;
        double newscore;
        serverAssert(zsetAdd(decoded,score, subkey, flag, &retflags, &newscore) == 1);
        sdsfree(subkey);
    }

    *pdecoded = decoded;
    return 0;
}

/* decoded object move to exec module */
int zsetDecodeData(swapData *data, int num, int *cfs, sds *rawkeys,
        sds *rawvals, void **pdecoded_) {
   robj **pdecoded = (robj**)pdecoded_; 

    serverAssert(num >= 0);
    if (num == 0) {
        *pdecoded = NULL;
        return 0;
    }

    if(cfs[0] == DATA_CF) {
        zsetDecodeBigData(data, num, rawkeys, rawvals, pdecoded);
    } else if (cfs[0] == SCORE_CF) {
        zsetDecodeScoreData(data, num, rawkeys, rawvals, pdecoded);
    } else {
        *pdecoded = NULL;
    }
    return 0;
}

static inline robj *createSwapInObject(robj *newval) {
    robj *swapin = newval;
    serverAssert(newval && newval->type == OBJ_ZSET);
    swapin->dirty = 0;
    return swapin;
}

int zsetSwapIn(swapData *data_, void *result_, void *datactx_) {    
    zsetSwapData *data = (zsetSwapData*)data_;
    robj *result = (robj*)result_;
    UNUSED(datactx_);
    /* hot key no need to swap in, this must be a warm or cold key. */
    serverAssert(swapDataPersisted(data_));

    if (swapDataIsCold(data_) && result != NULL) {
        /* cold key swapped in result (may be empty). */
        robj *swapin = createSwapInObject(result);
        /* mark persistent after data swap in without
         * persistence deleted, or mark non-persistent else */
        swapin->persistent = !data->sd.persistence_deleted;
        dbAdd(data->sd.db,data->sd.key,swapin);
        /* expire will be swapped in later by swap framework. */
        if (data->sd.cold_meta) {
            dbAddMeta(data->sd.db,data->sd.key,data->sd.cold_meta);
            data->sd.cold_meta = NULL; /* moved */
        }
    } else {
       if (result) decrRefCount(result);
       if (data->sd.value) data->sd.value->persistent = !data->sd.persistence_deleted;
    }
    return 0;
}

/* subkeys already cleaned by cleanObject(to save cpu usage of main thread),
 * swapout only updates db.dict keyspace, meta (db.meta/db.expire) swapped
 * out by swap framework. */
int zsetSwapOut(swapData *data, void *datactx, int *totally_out) {
    UNUSED(datactx);
    serverAssert(!swapDataIsCold(data));

    if (zsetLength(data->value) == 0) {
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

int zsetSwapDel(swapData *data, void *datactx_, int del_skip) {
    zsetDataCtx *datactx = datactx_;
    if (datactx->bdc.ctx_flag & BIG_DATA_CTX_FLAG_MOCK_VALUE) {
        createFakeZsetForDeleteIfCold(data);
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

/* Decoded moved back by exec to zsetSwapData */
void *zsetCreateOrMergeObject(swapData *data, void *decoded_, void *datactx) {
    robj *result, *decoded = (robj*)decoded_;
    UNUSED(datactx);
    serverAssert(decoded == NULL || decoded->type == OBJ_ZSET);

    if (swapDataIsCold(data) || decoded == NULL) {
        /* decoded moved back to swap framework again (result will later be
         * pass as swapIn param). */
        result = decoded;
        if (decoded) {
            swapDataObjectMetaModifyLen(data,-zsetLength(decoded));
        }
    } else {
        int decoded_len = zsetLength(decoded);
        int flag = ZADD_IN_NX;
        int retflags = 0;
        double newscore;
        if (decoded_len > 0) {
            
            if (decoded->encoding == OBJ_ENCODING_ZIPLIST) {
                unsigned char *zl = decoded->ptr;
                unsigned char *eptr, *sptr;
                unsigned char *vstr;
                unsigned int vlen;
                long long vlong;
                eptr = ziplistIndex(zl, 0);
                sptr = ziplistNext(zl, eptr);
                while(eptr != NULL) {
                    vlong = 0;
                    ziplistGet(eptr, &vstr, &vlen, &vlong);
                    sds subkey;
                    if (vstr != NULL) {
                        subkey = sdsnewlen(vstr, vlen);
                    } else {
                        subkey = sdsfromlonglong(vlong);
                    }
                    double score = zzlGetScore(sptr);
                    if (zsetAdd(data->value, score, subkey, flag, &retflags, &newscore) == 1) {
                        if (retflags & ZADD_OUT_ADDED) {
                            swapDataObjectMetaModifyLen(data, -1);
                        } 
                    }
                    sdsfree(subkey);
                    zzlNext(zl, &eptr, &sptr);
                }
            } else if (decoded->encoding == OBJ_ENCODING_SKIPLIST) {
                zset* zs = decoded->ptr;
                dict* d = zs->dict;
                dictIterator* di = dictGetIterator(d);
                dictEntry *de;
                while ((de = dictNext(di)) != NULL) {
                    sds subkey = dictGetKey(de);
                    double score = *(double*)dictGetVal(de);
                    if (zsetAdd(data->value, score, subkey, flag, &retflags, &newscore) == 1) {
                        if (retflags & ZADD_OUT_ADDED) {
                            swapDataObjectMetaModifyLen(data, -1);
                        }
                    }
                }
                dictReleaseIterator(di);
            }
        }
        /* decoded merged, we can release it now. */
        decrRefCount(decoded);
        result = NULL;
    }
    return result;
}

int zsetCleanObject(swapData *data, void *datactx_) {
    zsetDataCtx *datactx = datactx_;
    if (swapDataIsCold(data)) return 0;
    for (int i = 0; i < datactx->bdc.num; i++) {
        if (zsetDel(data->value,datactx->bdc.subkeys[i]->ptr)) {
            swapDataObjectMetaModifyLen(data,1);
        }
    }
    return 0;
}

/* Only free extend fields here, base fields (key/value/object_meta) freed
 * in swapDataFree */
void freeZsetSwapData(swapData *data_, void *datactx_) {
    zsetDataCtx *datactx = datactx_;
    UNUSED(data_);
    for (int i = 0; i < datactx->bdc.num; i++) {
        serverAssert(datactx->bdc.subkeys[i] != NULL);
        decrRefCount(datactx->bdc.subkeys[i]);
    }
    zfree(datactx->bdc.subkeys);
    switch(datactx->type) {
        case TYPE_ZS:
            if (datactx->zs.rangespec != NULL) {
                zfree(datactx->zs.rangespec);
                datactx->zs.rangespec = NULL;
            }
        break;
        
    }
    zfree(datactx);
}

int zsetRocksDel(struct swapData *data_,  void *datactx_, int inaction,
        int num, int* cfs, sds *rawkeys, sds *rawvals, OUT int *outaction,
        OUT int *outnum, OUT int** outcfs,OUT sds **outrawkeys) {
    zsetSwapData *data = (zsetSwapData*)data_;
    UNUSED(datactx_), UNUSED(inaction);
    sds* orawkeys = NULL;
    int* ocfs = NULL;
    int oindex = 0;
    int onum = 0;
    int multi = 2;
    *outaction = ROCKS_PUT;
    if (num > 0) {
        int dbid;
        size_t keylen, subkeylen;
        const char* keystr, *subkeystr;
        sds subkey;
        double score;
        onum = multi * num;
        orawkeys = zmalloc(sizeof(sds) * onum);
        ocfs = zmalloc(sizeof(int) * onum);
        for(int i = 0; i < num; i++) {
            uint64_t subkey_version;
            if (cfs[0] == SCORE_CF) {
                serverAssert(zsetDecodeScoreKey(rawkeys[i],sdslen(rawkeys[i]),
                            &dbid,&keystr,&keylen,&subkey_version,&subkeystr,&subkeylen,
                            &score) == 0);
                subkey = sdsnewlen(subkeystr, subkeylen);

                orawkeys[oindex] = rocksEncodeDataKey(data->sd.db,
                        data->sd.key->ptr,subkey_version,subkey);
                ocfs[oindex++] = DATA_CF;
                
                
                orawkeys[oindex] = sdsdup(rawkeys[i]);
                ocfs[oindex++] = SCORE_CF;
                
                sdsfree(subkey);

            } else if (cfs[0] == DATA_CF) {
                if (rawvals[i] != NULL) {
                    serverAssert(rocksDecodeDataKey(rawkeys[i],
                                sdslen(rawkeys[i]), &dbid,
                                &keystr, &keylen, &subkey_version,
                                &subkeystr,&subkeylen) == 0);
                    serverAssert(sdslen(data->sd.key->ptr) == keylen);
                    serverAssert(memcmp(data->sd.key->ptr, keystr, keylen) == 0);
                    double score = zsetDecodeSubval(rawvals[i]);
                    subkey = sdsnewlen(subkeystr, subkeylen);
                    
                    
                    orawkeys[oindex] = sdsdup(rawkeys[i]);
                    ocfs[oindex++] = DATA_CF;
                    
                    
                    orawkeys[oindex] = zsetEncodeScoreKey(data->sd.db,
                            data->sd.key->ptr, subkey_version, subkey, score);
                    ocfs[oindex++] = SCORE_CF;
                    
                    sdsfree(subkey);
                }
            } else {
                serverAssert(0);
            }
        }
        serverAssert(onum >= oindex);
    }
    *outnum = oindex;
    *outcfs = ocfs;
    *outrawkeys = orawkeys;
    return 0;
}

swapDataType zsetSwapDataType = {
    .name = "zset",
    .swapAna = zsetSwapAna,
    .swapAnaAction = zsetSwapAnaAction,
    .encodeKeys = zsetEncodeKeys,
    .encodeData = zsetEncodeData,
    .encodeRange = zsetEncodeRange,
    .decodeData = zsetDecodeData,
    .swapIn = zsetSwapIn,
    .swapOut = zsetSwapOut,
    .swapDel = zsetSwapDel,
    .createOrMergeObject = zsetCreateOrMergeObject,
    .cleanObject = zsetCleanObject,
    .rocksDel = zsetRocksDel,
    .free = freeZsetSwapData,
    .mergedIsHot = zsetMergedIsHot,
};



/**
 * @brief 
 * 
 * @param d 
 * @param pdatactx 
 * @return int 
 */
int swapDataSetupZSet(swapData *d, void **pdatactx) {
    d->type = &zsetSwapDataType;
    d->omtype = &zsetObjectMetaType;
    zsetDataCtx *datactx = zmalloc(sizeof(zsetDataCtx));
    datactx->bdc.num = 0;
    datactx->bdc.ctx_flag = BIG_DATA_CTX_FLAG_NONE;
    datactx->bdc.subkeys = NULL;
    datactx->type = TYPE_NONE;
    *pdatactx = datactx;
    return 0;
}



int zsetSaveStart(rdbKeySaveData *save, rio *rdb) {
    robj *key = save->key;
    size_t nfields = 0;

    if (rdbSaveKeyHeader(rdb,key,key,RDB_TYPE_ZSET_2,
                save->expire) == -1)
        return -1;
    if (save->value) 
        nfields += zsetLength(save->value);
    if (save->object_meta)
        nfields += save->object_meta->len;
    if (rdbSaveLen(rdb, nfields) == -1)
        return -1;

    if (!save->value || zsetLength(save->value) == 0)
        return 0;
    if (save->value->encoding == OBJ_ENCODING_ZIPLIST) {
        int len = zsetLength(save->value);
        unsigned char *zl = save->value->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        eptr = ziplistIndex(zl, 0);
        sptr = ziplistNext(zl, eptr);
        while(len > 0) {
            vlong = 0;
            ziplistGet(eptr, &vstr, &vlen, &vlong);
            double score = zzlGetScore(sptr);
            if (vstr != NULL) {
                if ((rdbSaveRawString(rdb,
                            vstr,vlen)) == -1) {
                    return -1;
                }
            } else {
                char buf[128];
                int len = ll2string(buf, 128, vlong);
                if ((rdbSaveRawString(rdb, (unsigned char*)buf, len)) == -1) {
                    return -1;
                }
            }
            
            if ((rdbSaveBinaryDoubleValue(rdb,score)) == -1)
                return -1;
            zzlNext(zl, &eptr, &sptr);
            len--;
        }
    } else if (save->value->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = save->value->ptr;
        zskiplist* zsl = zs->zsl;
        /* save fields from value (db.dict) */
        zskiplistNode* zn = zsl->tail;
        while (zn != NULL) {
            if ((rdbSaveRawString(rdb,
                        (unsigned char*)zn->ele,sdslen(zn->ele))) == -1)
            {
                return -1;
            }
            if ((rdbSaveBinaryDoubleValue(rdb,zn->score)) == -1)
                return -1;
            zn = zn->backward;
        }
    }
    return 0;
}

/**  zset Save **/
int zsetSave(rdbKeySaveData *save, rio *rdb, decodedData *decoded) {
    robj *key = save->key;
    
    serverAssert(!sdscmp(decoded->key, key->ptr));
    double score;
    if (save->value != NULL) {
        if (zsetScore(save->value,
                    decoded->subkey, &score) == C_OK) {
            /* already save in save_start, skip this subkey */
            return 0;

        }
    }

    char *subkey = decoded->subkey != NULL ? decoded->subkey: "";
    size_t slen = decoded->subkey != NULL? sdslen(decoded->subkey) : 0;

    if (rdbSaveRawString(rdb,(unsigned char*)subkey,slen) == -1) {
        return -1;
    }

    rio sdsrdb;
    rioInitWithBuffer(&sdsrdb, decoded->rdbraw);
    if (rdbWriteRaw(rdb, decoded->rdbraw, sizeof(double)) == -1) {
        return -1;
    }
    save->saved++;
    return 0;
}

int zsetSaveEnd(rdbKeySaveData *save, rio *rdb, int save_result) {
    objectMeta *object_meta = save->object_meta;
    UNUSED(rdb);
    if (save->saved != object_meta->len) {
        sds key  = save->key->ptr;
        sds repr = sdscatrepr(sdsempty(), key, sdslen(key));
        serverLog(LL_WARNING,
                "zsetSave %s: saved(%d) != object_meta.len(%ld)",
                repr, save->saved, (ssize_t)object_meta->len);
        sdsfree(repr);
        return -1;
    }
    return save_result;
}

rdbKeySaveType zsetSaveType = {
    .save_start = zsetSaveStart,
    .save = zsetSave,
    .save_end = zsetSaveEnd,
    .save_deinit = NULL,
};
#define zsetObjectMetaType lenObjectMetaType
int zsetSaveInit(rdbKeySaveData *save, uint64_t version,
        const char *extend, size_t extlen) {
    int retval = 0;
    save->type = &zsetSaveType;
    save->omtype = &zsetObjectMetaType;
    if (extend) {
        serverAssert(save->object_meta == NULL);
        retval = buildObjectMeta(OBJ_ZSET,version,extend,
                extlen,&save->object_meta);
    }
    return retval;
}

#define LOAD_NONE 0
#define LOAD_VALUE 1

struct ziplistIterator {
    sds subkey;
    double score;
    unsigned char* zl;
    unsigned char* eptr;
    unsigned char* sptr;
} ziplistIterator;


struct ziplistIterator* createZsetIter() {
    struct ziplistIterator* iterator = zmalloc(sizeof(struct ziplistIterator));
    iterator->zl = NULL;
    iterator->eptr = NULL;
    iterator->sptr = NULL;
    iterator->subkey = NULL;
    iterator->score = 0;
    return iterator;
}

struct ziplistIterator* ziplistInitIterator(robj* zobj) {
    struct ziplistIterator* iterator = createZsetIter();
    iterator->zl = zobj->ptr;
    return iterator;
}


int ziplistIteratorNext(struct ziplistIterator* iterator) {
    if (iterator->eptr == NULL) {
        iterator->eptr = ziplistIndex(iterator->zl, 0);
        iterator->sptr = ziplistNext(iterator->zl, iterator->eptr);
    } else {
        zzlNext(iterator->zl, &iterator->eptr, &iterator->sptr);
    }
    iterator->subkey = NULL;
    iterator->score = 0;
    return iterator->eptr != NULL? C_OK: C_ERR;
}

sds ziplistIteratorGetSubkey(struct ziplistIterator* iter) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    sds subkey;
    ziplistGet(iter->eptr, &vstr, &vlen, &vlong);
    if (vstr != NULL) {
        subkey = sdsnewlen((const char*)vstr, vlen);
    } else {
        subkey = sdsfromlonglong(vlong);
    }
    return subkey;
}

double ziplistIteratorGetScore(struct ziplistIterator* iter) {
    return zzlGetScore(iter->sptr);
}

void freeZsetZipListIter(struct ziplistIterator* iter) {
    if (iter->subkey) {
        sdsfree(iter->subkey);
    }
    zfree(iter);
}

void freeZsetIter(struct ziplistIterator* iter) {
    freeZsetZipListIter(iter);
}

/**  load **/
void zsetLoadStartZip(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    sds extend = NULL;

    load->value = rdbLoadObject(load->rdbtype,rdb,load->key,error);
    if (load->value == NULL) return;

    if (load->value->type != OBJ_ZSET) {
        serverLog(LL_WARNING,"Load rdb with rdbtype(%d) got (%d)",
                load->rdbtype, load->value->type);
        *error = RDB_LOAD_ERR_OTHER;
        return;
    }


    load->iter = ziplistInitIterator(load->value);
    if (ziplistIteratorNext(load->iter) == C_ERR) {
        serverLog(LL_WARNING,"Load rdb iter not valid.");
        *error = RDB_LOAD_ERR_OTHER;
        return;
    }
    
    load->total_fields = zsetLength(load->value);
    extend = rocksEncodeObjectMetaLen(load->total_fields);
    *cf = META_CF;
    *rawkey = rocksEncodeMetaKey(load->db,load->key);
    *rawval = rocksEncodeMetaVal(load->object_type,load->expire,
            load->version,extend);
    sdsfree(extend);
}

/* zset rdb load */
void zsetLoadStartHT(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
	int isencode;
	unsigned long long len;
	sds zset_header, extend = NULL;

    zset_header = rdbVerbatimNew((unsigned char)load->rdbtype);

	/* nfield */
	if (rdbLoadLenVerbatim(rdb,&zset_header,&isencode,&len)) {
		sdsfree(zset_header);
        *error = RDB_LOAD_ERR_OTHER;
        return;
	}

    if (len == 0) {
        sdsfree(zset_header);
        *error = RDB_LOAD_ERR_EMPTY_KEY;
        return;
    }

    load->total_fields = len;
    load->iter = createZsetIter();
    extend = rocksEncodeObjectMetaLen(len);

    *cf = META_CF;
    *rawkey = rocksEncodeMetaKey(load->db,load->key);
    *rawval = rocksEncodeMetaVal(load->object_type,load->expire,
            load->version,extend);
    *error = 0;

    sdsfree(extend);
    sdsfree(zset_header);
}
void zsetLoadStart(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    switch (load->rdbtype) {
        case RDB_TYPE_ZSET_ZIPLIST:
            zsetLoadStartZip(load,rdb,cf,rawkey,rawval,error);
            break;
        case RDB_TYPE_ZSET_2:
        case RDB_TYPE_ZSET:
            zsetLoadStartHT(load,rdb,cf,rawkey,rawval,error);
            break;
        default:
        break;
    }     
}


int zsetLoadZip(struct rdbKeyLoadData *load, rio *rdb, int *cf, sds *rawkey,
        sds *rawval, int *error) {
    struct ziplistIterator* iter = load->iter;
    UNUSED(rdb);
    sds subkey;
    double score;
    if (iter->subkey != NULL) {
        *cf = SCORE_CF;
        *rawkey = zsetEncodeScoreKey(load->db,load->key,load->version,
                iter->subkey,iter->score);
        *rawval = zsetEncodeScoreValue(iter->subkey, iter->score);
        *error = 0;
        sdsfree(iter->subkey);
        iter->subkey = NULL;
        iter->score = 0;
        load->loaded_fields++;
        return ziplistIteratorNext(iter) != C_ERR;
    }


    subkey = ziplistIteratorGetSubkey(load->iter);
    score = ziplistIteratorGetScore(load->iter);
    *cf = DATA_CF;
    *rawkey = rocksEncodeDataKey(load->db,load->key,load->version,subkey);
    *rawval = zsetEncodeSubval(score);
    *error = 0;

    iter->subkey = subkey;
    iter->score = score;
    return 1;
}
int zsetLoadHT(struct rdbKeyLoadData *load, rio *rdb, int *cf, sds *rawkey,
        sds *rawval, int *error) {
    *error = RDB_LOAD_ERR_OTHER;
    struct ziplistIterator* iter = load->iter;
    if (iter->subkey != NULL ) {
        *cf = SCORE_CF;
        *rawkey = zsetEncodeScoreKey(load->db,load->key,load->version,
                iter->subkey,iter->score);
        *rawval = zsetEncodeScoreValue(iter->subkey, iter->score);
        *error = 0;
        sdsfree(iter->subkey);
        iter->subkey = NULL;
        iter->score = 0;
        load->loaded_fields++;
        return load->loaded_fields < load->total_fields;
    }
    sds subkey;
    if ((subkey = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
        return 0;
    }
    double score;
    if (load->rdbtype == RDB_TYPE_ZSET_2) {
        if (rdbLoadBinaryDoubleValue(rdb,&score) == -1) {
            sdsfree(subkey);
            return 0;
        }
    } else if (load->rdbtype == RDB_TYPE_ZSET) {
        if (rdbLoadDoubleValue(rdb, &score) == -1) {
            sdsfree(subkey);
            return 0;
        }
    } else {
        return 0;
    }
    *error = 0;
    *rawkey = rocksEncodeDataKey(load->db,load->key,load->version,subkey);
    *rawval = zsetEncodeSubval(score);
    *cf = DATA_CF;
    iter->subkey = subkey;
    iter->score = score;
    return 1;
}
int zsetLoad(struct rdbKeyLoadData *load, rio *rdb, int *cf,
        sds *rawkey, sds *rawval, int *error) {
    int retval;

    switch (load->rdbtype) {
        case RDB_TYPE_ZSET_ZIPLIST:
            retval = zsetLoadZip(load,rdb,cf,rawkey,rawval,error);
        break;
        case RDB_TYPE_ZSET:
        case RDB_TYPE_ZSET_2:
            retval = zsetLoadHT(load,rdb,cf,rawkey,rawval,error);
        break;
    default:
        retval = RDB_LOAD_ERR_OTHER;
    }
    return retval;
}

void zsetLoadDeinit(struct rdbKeyLoadData *load) {
    if (load->iter) {
        switch (load->rdbtype) {
            case RDB_TYPE_ZSET_ZIPLIST:
                freeZsetZipListIter(load->iter);
            break;
            case RDB_TYPE_ZSET:
            case RDB_TYPE_ZSET_2:
                freeZsetIter(load->iter);
            break;
        }
    }

    if (load->value) {
        decrRefCount(load->value);
        load->value = NULL;
    }
}

rdbKeyLoadType zsetLoadType = {
    .load_start = zsetLoadStart,
    .load = zsetLoad,
    .load_end = NULL,
    .load_deinit = zsetLoadDeinit,
};

void zsetLoadInit(rdbKeyLoadData *load) {
    load->type = &zsetLoadType;
    load->omtype = &zsetObjectMetaType;
    load->object_type = OBJ_ZSET;
}


#ifdef REDIS_TEST

#define SWAP_EVICT_STEP 2
#define SWAP_EVICT_MEM  (1*1024*1024)


int swapDataZsetTest(int argc, char **argv, int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);
    initTestRedisServer();
    redisDb* db = server.db + 0;
    int error = 0;
    swapData *zset1_data;
    zsetDataCtx *zset1_ctx;
    robj *key1, *zset1, *decoded;
    keyRequest _kr1, *kr1 = &_kr1, _cold_kr1, *cold_kr1 = &_cold_kr1;
    sds f1, f2, f3, f4;
    int action, numkeys;
    int oldEvictStep = server.swap_evict_step_max_subkeys;

    TEST("zset - init") {
        key1 = createStringObject("key1",4);
        f1 = sdsnew("f1"), f2 = sdsnew("f2"), f3 = sdsnew("f3"), f4 = sdsnew("f4");
        zset1 = createZsetObject();
        int out_flags = 0;
        zsetAdd(zset1,1.0,f1,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,2.0,f2,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,3.0,f3,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,4.0,f4,ZADD_IN_NONE,&out_flags,NULL);
        dbAdd(db,key1,zset1);
    }

    TEST("set - encodeKeys/encodeData/DecodeData") {
        zset1_data = createSwapData(db, key1,zset1);
        swapDataSetupZSet(zset1_data, (void**)&zset1_ctx);
        sds *rawkeys, *rawvals;
        int *cfs, cf, flags;
        sds start, end;

        zset1_ctx->bdc.num = 2;
        zset1_ctx->bdc.subkeys = mockSubKeys(2, sdsdup(f1), sdsdup(f2));
        zset1_data->object_meta = createZsetObjectMeta(0,2);
        // encodeKeys - swap in subkeys
        zsetSwapAnaAction(zset1_data, SWAP_IN, zset1_ctx, &action);
        zsetEncodeKeys(zset1_data, SWAP_IN, zset1_ctx, &numkeys, &cfs, &rawkeys);
        test_assert(2 == numkeys);
        test_assert(DATA_CF == cfs[0] && DATA_CF == cfs[1]);
        test_assert(ROCKS_GET == action);
        sds expectEncodedKey = rocksEncodeDataKey(db, key1->ptr, 0, f1);
        test_assert(memcmp(expectEncodedKey,rawkeys[0],sdslen(rawkeys[0])) == 0
            || memcmp(expectEncodedKey,rawkeys[1],sdslen(rawkeys[1])) == 0);
        
        // encodeKeys - swap in whole key
        zset1_ctx->bdc.num = 0;
        zsetSwapAnaAction(zset1_data, SWAP_IN, zset1_ctx, &action);
        zsetEncodeRange(zset1_data, SWAP_IN, zset1_ctx, &numkeys, &flags, &cf, &start, &end);
        test_assert(ROCKS_ITERATE == action);
        test_assert(DATA_CF == cf);
        sds empty = sdsnewlen("", 0);
        expectEncodedKey = rocksEncodeDataRangeStartKey(db, key1->ptr, 0);
        test_assert(memcmp(expectEncodedKey, start, sdslen(start)) == 0);
        // encodeKeys - swap del
        zsetSwapAnaAction(zset1_data, SWAP_DEL, zset1_ctx, &action);
        test_assert(0 == action);

        // encodeData - swap out
        zset1_ctx->bdc.num = 2;
        zsetSwapAnaAction(zset1_data, SWAP_OUT, zset1_ctx, &action);
        zsetEncodeData(zset1_data, SWAP_OUT, zset1_ctx, &numkeys, &cfs, &rawkeys, &rawvals);
        test_assert(action == ROCKS_PUT);
        test_assert(4 == numkeys);

        //mock
        int* cfs_ = zmalloc(sizeof(int) * 2);
        sds* rawkeys_ = zmalloc(sizeof(sds) * 2);
        sds* rawvals_ = zmalloc(sizeof(sds) * 2);
        cfs_[0] = cfs[0];
        rawkeys_[0] = rawkeys[0];
        rawvals_[0] = rawvals[0];

        cfs_[1] = cfs[2];
        rawkeys_[1] = rawkeys[2];
        rawvals_[1] = rawvals[2];

        // decodeData - swap in
        zsetDecodeData(zset1_data, zset1_ctx->bdc.num, cfs_, rawkeys_, rawvals_, (void**)&decoded);
        test_assert(NULL != decoded);
        test_assert(2 == zsetLength(decoded));


        freeZsetSwapData(zset1_data, zset1_ctx);
    }

    TEST("zset - swapAna") {
        int intention;
        uint32_t intention_flags;
        objectMeta *zset1_meta = createZsetObjectMeta(0,0);
        zset1_data = createSwapData(db, key1,zset1);
        swapDataSetupZSet(zset1_data, (void**)&zset1_ctx);

        kr1->key = key1;
        kr1->level = REQUEST_LEVEL_KEY;
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
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);

        // swap in while no persisted data
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = 0;
        zset1_data->object_meta = NULL;
        zset1_data->cold_meta = NULL;
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);

        // swap in meta
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = SWAP_IN_META;
        zset1_data->object_meta = NULL;
        zset1_data->value = NULL;
        zset1_data->cold_meta = zset1_meta;
        zset1_meta->len = 4;
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == 0);
        test_assert(zset1_ctx->bdc.num > 0);

        // swap in del mock value
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = SWAP_IN_DEL_MOCK_VALUE;
        zset1_data->value = zset1;
        zsetSwapAna(zset1_data, kr1, &intention, &intention_flags, zset1_ctx);
        test_assert(intention == SWAP_DEL && intention_flags == SWAP_FIN_DEL_SKIP);

        // swap in del - all subkeys in memory
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = SWAP_IN_DEL;
        zset1_data->object_meta = NULL;
        zset1_data->cold_meta = zset1_meta;
        zset1_meta->len = 0;
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_DEL && intention_flags == SWAP_FIN_DEL_SKIP);

        // swap in del - not all subkeys in memory
        zset1_meta->len = 4;
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == SWAP_EXEC_IN_DEL);

        // swap in whole key
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = 0;
        zset1_data->value = NULL;
        zset1_meta->len = 4;
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == 0);

        // swap in with subkeys - swap in del
        kr1->b.num_subkeys = 2;
        kr1->b.subkeys = mockSubKeys(2, sdsdup(f1), sdsdup(f2));
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = SWAP_IN_DEL;
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == SWAP_EXEC_IN_DEL);
        test_assert(zset1_ctx->bdc.num == 2);

        // swap in with subkeys - subkeys already in mem
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = 0;
        zset1_data->value = zset1;
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        test_assert(zset1_ctx->bdc.num == 0);

        // swap in with subkeys - subkeys not in mem
        kr1->cmd_intention = SWAP_IN;
        kr1->cmd_intention_flags = 0;
        kr1->b.subkeys = mockSubKeys(2, sdsnew("new1"), sdsnew("new2"));
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_IN && intention_flags == 0);
        test_assert(zset1_ctx->bdc.num == 2);

        // swap out - data not in mem
        zset1_data->value = NULL;
        kr1->cmd_intention = SWAP_OUT;
        kr1->cmd_intention_flags = 0;
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);

        // swap out - first swap out
        zset1_data->value = zset1;
        zset1->dirty = 1;
        zset1_data->object_meta = NULL;
        zset1_data->cold_meta = NULL;
        zset1_data->new_meta = NULL;
        zset1_ctx->bdc.num = 0;
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_OUT && intention_flags == 0);
        test_assert(4 == zset1_ctx->bdc.num);
        test_assert(NULL != zset1_data->new_meta);

        // swap out - data not dirty
        zset1->dirty = 0;
        zset1_ctx->bdc.num = 0;
        zset1_data->object_meta = createZsetObjectMeta(0,0);
        zset1_data->new_meta = NULL;
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_NOP && intention_flags == 0);
        test_assert(0 == zsetLength(zset1));
        test_assert(4 == zset1_data->object_meta->len);

        // recover data in set1
        int out_flags = 0;
        zsetAdd(zset1,1.0,f1,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,2.0,f2,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,3.0,f3,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,4.0,f4,ZADD_IN_NONE,&out_flags,NULL);
        dbAdd(db, key1, zset1);

        // swap del
        kr1->cmd_intention = SWAP_DEL;
        kr1->cmd_intention_flags = 0;
        zsetSwapAna(zset1_data,kr1,&intention,&intention_flags,zset1_ctx);
        test_assert(intention == SWAP_DEL && intention_flags == 0);

        freeZsetSwapData(zset1_data, zset1_ctx);
    }

    TEST("zset - swapIn/swapOut") {
        robj *s, *result;
        objectMeta *m;
        zset1_data = createSwapData(db, key1,zset1);
        swapDataSetupZSet(zset1_data, (void**)&zset1_ctx);
        test_assert(lookupMeta(db,key1) == NULL);
        test_assert((s = lookupKey(db, key1, LOOKUP_NOTOUCH)) != NULL);
        test_assert(zsetLength(s) == 4);

        /* hot => warm => cold */
        zset1_data->new_meta = createZsetObjectMeta(0,0);
        zset1_ctx->bdc.num = 2;
        zset1_ctx->bdc.subkeys = mockSubKeys(2, sdsdup(f1), sdsdup(f2));
        zsetCleanObject(zset1_data, zset1_ctx);
        zsetSwapOut(zset1_data, zset1_ctx, NULL);
        test_assert((m =lookupMeta(db,key1)) != NULL && m->len == 2);
        test_assert((s = lookupKey(db, key1, LOOKUP_NOTOUCH)) != NULL);
        test_assert(zsetLength(s) == 2);

        zset1_data->new_meta = NULL;
        zset1_data->object_meta = m;
        zset1_ctx->bdc.subkeys = mockSubKeys(2, sdsdup(f3), sdsdup(f4));
        zsetCleanObject(zset1_data, zset1_ctx);
        zsetSwapOut(zset1_data, zset1_ctx, NULL);
        test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
        test_assert(lookupMeta(db,key1) == NULL);

        /* cold => warm => hot */
        decoded = createZsetObject();
        int out_flags = 0;
        zsetAdd(decoded,1.0,f1,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(decoded,2.0,f2,ZADD_IN_NONE,&out_flags,NULL);

        zset1_data->object_meta = NULL;
        zset1_data->cold_meta = createZsetObjectMeta(0,4);
        zset1_data->value = NULL;
        result = zsetCreateOrMergeObject(zset1_data, decoded, zset1_ctx);
        zsetSwapIn(zset1_data,result,zset1_ctx);
        test_assert((m = lookupMeta(db,key1)) != NULL && m->len == 2);
        test_assert((s = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(zsetLength(s) == 2);

        decoded = createZsetObject();
        zsetAdd(decoded,3.0,f3,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(decoded,4.0,f4,ZADD_IN_NONE,&out_flags,NULL);

        zset1_data->cold_meta = NULL;
        zset1_data->object_meta = m;
        zset1_data->value = s;
        result = zsetCreateOrMergeObject(zset1_data, decoded, zset1_ctx);
        zsetSwapIn(zset1_data,result,zset1_ctx);
        test_assert((m = lookupMeta(db,key1)) != NULL && m->len == 0);
        test_assert((s = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(zsetLength(s) == 4);

        /* hot => cold */
        zset1_data->object_meta = m;
        zset1_data->value = s;
        zset1_ctx->bdc.num = 4;
        zset1_ctx->bdc.subkeys = mockSubKeys(4, sdsdup(f1), sdsdup(f2), sdsdup(f3), sdsdup(f4));
        zsetCleanObject(zset1_data, zset1_ctx);
        zsetSwapOut(zset1_data, zset1_ctx, NULL);
        test_assert((m = lookupMeta(db,key1)) == NULL);
        test_assert((s = lookupKey(db,key1,LOOKUP_NOTOUCH)) == NULL);

        /* cold => hot */
        decoded = createZsetObject();
        zsetAdd(decoded,1.0,f1,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(decoded,2.0,f2,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(decoded,3.0,f3,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(decoded,4.0,f4,ZADD_IN_NONE,&out_flags,NULL);

        zset1_data->object_meta = NULL;
        zset1_data->cold_meta = createZsetObjectMeta(0,4);
        zset1_data->value = NULL;
        result = zsetCreateOrMergeObject(zset1_data,decoded,zset1_ctx);
        zsetSwapIn(zset1_data,result,zset1_ctx);
        test_assert((m = lookupMeta(db,key1)) != NULL);
        test_assert((s = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL);
        test_assert(zsetLength(s) == 4);

        freeZsetSwapData(zset1_data, zset1_ctx);
    }

    TEST("zset - rdbLoad & rdbSave") {
        int err = 0;
        int cf;
        sds rdbv1 = zsetEncodeSubval(1.0);
        sds rdbv2 = zsetEncodeSubval(2.0);

        /* rdbLoad - RDB_TYPE_SET */
        zset1 = createZsetObject();
        int out_flags = 0;
        zsetAdd(zset1,1.0,f1,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,2.0,f2,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,3.0,f3,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,4.0,f4,ZADD_IN_NONE,&out_flags,NULL);
        test_assert(zsetLength(zset1) == 4);

        rio sdsrdb;
        sds rawval = rocksEncodeValRdb(zset1);
        rioInitWithBuffer(&sdsrdb,sdsnewlen(rawval+1,sdslen(rawval)-1));
        rdbKeyLoadData _loadData; rdbKeyLoadData *loadData = &_loadData;
        test_assert(rawval[0] == RDB_TYPE_ZSET_2);

        int cont;
        sds subkey, subraw;
        objectMeta *cold_meta;
        int t; long long e; const char *extend; size_t extlen;
        uint64_t v;
        rdbKeyLoadDataInit(loadData,RDB_TYPE_ZSET_2,db,key1->ptr,-1,1600000000);
        zsetLoadStart(loadData, &sdsrdb, &cf, &subkey, &subraw, &err);
        test_assert(0 == err && META_CF == cf);
        test_assert(memcmp(rocksEncodeMetaKey(db,key1->ptr), subkey, sdslen(subkey)) == 0);

        rocksDecodeMetaVal(subraw, sdslen(subraw), &t, &e, &v, &extend, &extlen);
        buildObjectMeta(t,v,extend,extlen,&cold_meta);
        test_assert(cold_meta->object_type == OBJ_ZSET && cold_meta->len == 4 && e == -1);

        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == SCORE_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == SCORE_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == SCORE_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 0 && err == 0 && cf == SCORE_CF);
        test_assert(loadData->object_type == OBJ_ZSET);
        test_assert(loadData->total_fields == 4 && loadData->loaded_fields == 4);
        zsetLoadDeinit(loadData);

        /* rdbLoad - RDB_TYPE_SET_INTSET */
        zset1 = createZsetZiplistObject();
        zsetAdd(zset1,1.0,f1,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,2.0,f2,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,3.0,f3,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(zset1,4.0,f4,ZADD_IN_NONE,&out_flags,NULL);

        rawval = rocksEncodeValRdb(zset1);
        rioInitWithBuffer(&sdsrdb,sdsnewlen(rawval+1,sdslen(rawval)-1));
        rdbKeyLoadDataInit(loadData,RDB_TYPE_ZSET_ZIPLIST, db,key1->ptr,-1,1600000000);
        zsetLoadStart(loadData, &sdsrdb, &cf, &subkey, &subraw, &err);
        test_assert(0 == err && META_CF == cf);
        test_assert(memcmp(rocksEncodeMetaKey(db,key1->ptr), subkey, sdslen(subkey)) == 0);

        rocksDecodeMetaVal(subraw, sdslen(subraw), &t, &e, &v, &extend, &extlen);
        buildObjectMeta(t,v,extend,extlen,&cold_meta);
        test_assert(cold_meta->object_type == OBJ_ZSET && cold_meta->len == 4 && e == -1);

        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == SCORE_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == SCORE_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == SCORE_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 1 && err == 0 && cf == DATA_CF);
        cont = zsetLoad(loadData,&sdsrdb,&cf,&subkey,&subraw,&err);
        test_assert(cont == 0 && err == 0 && cf == SCORE_CF);
        test_assert(loadData->object_type == OBJ_ZSET);
        test_assert(loadData->total_fields == 4 && loadData->loaded_fields == 4);
        zsetLoadDeinit(loadData);

        /* rdbSave */
        sds coldraw,warmraw,hotraw;
        rio rdbcold, rdbwarm, rdbhot;
        rdbKeySaveData _saveData; rdbKeySaveData  *saveData = &_saveData;

        decodedMeta _decoded_meta, *decoded_meta = &_decoded_meta;
        decodedData  _decoded_data, *decoded_data = &_decoded_data;
        decoded_meta->dbid = decoded_data->dbid = db->id;
        decoded_meta->key = decoded_data->key = key1->ptr;
        decoded_meta->cf = META_CF, decoded_data->cf = DATA_CF;
        decoded_meta->object_type = OBJ_ZSET, decoded_meta->expire = -1;
        decoded_data->rdbtype = 0;

        /* rdbSave - save cold */
        dbDelete(db, key1);
        decoded_meta->extend = rocksEncodeObjectMetaLen(2);
        rioInitWithBuffer(&rdbcold,sdsempty());
        test_assert(rdbKeySaveDataInit(saveData, db, (decodedResult*)decoded_meta) == 0);
        test_assert(saveData->object_meta != NULL);        

        test_assert(zsetSaveStart(saveData, &rdbcold) == 0);

        decoded_data->version = saveData->object_meta->version;
        decoded_data->subkey = f2, decoded_data->rdbraw = sdsnewlen(rdbv2+1,sdslen(rdbv2)-1);
        test_assert(rdbKeySave(saveData,&rdbcold,decoded_data) == 0);
        decoded_data->subkey = f1, decoded_data->rdbraw = sdsnewlen(rdbv1+1,sdslen(rdbv1)-1);
        test_assert(rdbKeySave(saveData,&rdbcold,decoded_data) == 0);
        coldraw = rdbcold.io.buffer.ptr;

        /* rdbSave - save warm */
        rioInitWithBuffer(&rdbwarm,sdsempty());
        robj *value = createZsetObject();
        zsetAdd(value,2.0,f2,ZADD_IN_NONE,&out_flags,NULL);
        dbAdd(db, key1, value);
        dbAddMeta(db, key1, createZsetObjectMeta(0,1));
        test_assert(rdbKeySaveDataInit(saveData, db, (decodedResult*)decoded_meta) == 0);
        test_assert(rdbKeySaveStart(saveData,&rdbwarm) == 0);
        decoded_data->version = saveData->object_meta->version;
        test_assert(rdbKeySave(saveData,&rdbwarm,decoded_data) == 0);
        warmraw = rdbwarm.io.buffer.ptr;

        /* rdbSave - hot */
        robj keyobj;
        robj *wholeset = createZsetObject();
        zsetAdd(wholeset,1.0,f1,ZADD_IN_NONE,&out_flags,NULL);
        zsetAdd(wholeset,2.0,f2,ZADD_IN_NONE,&out_flags,NULL);

        rioInitWithBuffer(&rdbhot,sdsempty());
        initStaticStringObject(keyobj,key1->ptr);
        test_assert(rdbSaveKeyValuePair(&rdbhot,&keyobj,wholeset,-1) != -1);
        hotraw = rdbhot.io.buffer.ptr;

        test_assert(!sdscmp(hotraw,coldraw));
        test_assert(!sdscmp(hotraw,warmraw));
        test_assert(!sdscmp(hotraw,hotraw));

    }

    TEST("encode/decode scorekey/scoreval") {
        sds raw = zsetEncodeScoreKey(db, key1->ptr, 0, f1, 1.0);
        int dbid;
        size_t dkeylen, dsubkeylen;
        const char* dkey, *dsubkey;
        double score;
        uint64_t version;
        test_assert(0 == zsetDecodeScoreKey(raw, sdslen(raw), &dbid, &dkey,
                    &dkeylen, &version, &dsubkey, &dsubkeylen, &score));
        test_assert(db->id == dbid);
        test_assert(dkeylen == sdslen(key1->ptr));
        test_assert(strncmp(dkey, key1->ptr, dkeylen) == 0);
        test_assert(dsubkeylen == sdslen(f1));
        test_assert(strncmp(dsubkey, f1, dsubkeylen) == 0);
        test_assert(score == 1.0);

        //decode score val
        raw = zsetEncodeScoreValue(f1, 2.0);
        test_assert(zsetDecodeScoreValue(raw, sdslen(raw), &score) == sizeOfDouble);
        test_assert(score == 2.0);

    }

    TEST("zset - free") {
        decrRefCount(zset1);
        server.swap_evict_step_max_subkeys = oldEvictStep;
    }

    return error;
}


#endif
