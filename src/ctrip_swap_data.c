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

/* See keyIsExpired for more details */
int swapDataExpired(swapData *d) {
    mstime_t when = d->expire;
    mstime_t now;

    if (when < 0) return 0;
    if (server.loading) return 0;
    if (server.lua_caller) {
        now = server.lua_time_snapshot;
    } else if (server.fixed_time_expire > 0) {
        now = server.mstime;
    } else {
        now = mstime();
    }
    return now > when;
}

swapData *createSwapData(redisDb *db, robj *key, robj *value) {
    swapData *data = zmalloc(sizeof(swapData));
    data->db = db;
    if (key) incrRefCount(key);
    data->key = key;
    if (value) incrRefCount(value);
    data->value = value;
    return data;
}

int swapDataAlreadySetup(swapData *data) {
    return data->type != NULL;
}

static int swapDataExpiredAndShouldDelete(swapData *data) {
    if (!swapDataExpired(data)) return 0;
    if (server.masterhost != NULL) return 0;
    if (checkClientPauseTimeoutAndReturnIfPaused()) return 0;
    return 1;
}

int swapDataKeyRequestFinished(swapData *data) {
    if (data->propagate_expire) {
        deleteExpiredKeyAndPropagate(data->db,data->key);
    }

    if (data->set_dirty) {
        dbSetDirty(data->db,data->key);
    }
    return 0;
}

/* Main/swap-thread: analyze data and command intention & request to decide
 * final swap intention. e.g. command might want SWAP_IN but data not
 * evicted, then intention is decided as NOP. */ 
int swapDataAna(swapData *d, struct keyRequest *key_request,
        int *intention, uint32_t *intention_flags, void *datactx) {
    int retval = 0;

    serverAssert(swapDataAlreadySetup(d));

    if (swapDataExpiredAndShouldDelete(d)) {
        key_request->cmd_intention = SWAP_DEL;
        key_request->cmd_intention_flags = 0;
        d->propagate_expire = 1;
    }

    if (d->type->swapAna) {
        retval = d->type->swapAna(d,key_request,intention,
                intention_flags,datactx);
        
        //TODO confirm & opt
        if ((*intention_flags & INTENTION_FIN_NO_DEL) ||
                (*intention_flags & INTENTION_EXEC_DEL_DATA)) {
            /* rocksdb and mem differs after rocksdb del. */
            d->set_dirty = 1;
        }
    }

    return retval;
}

/* Swap-thread: decide how to encode keys by data and intention. */
inline int swapDataEncodeKeys(swapData *d, int intention, void *datactx,
        int *action, int *numkeys, int **cfs, sds **rawkeys) {
    if (d->type->encodeKeys)
        return d->type->encodeKeys(d,intention,datactx,action,numkeys,cfs,rawkeys);
    else
        return 0;
}

/* Swap-thread: decode how to encode val/subval by data and intention.
 * dataactx can be used store context of which subvals are encoded. */
inline int swapDataEncodeData(swapData *d, int intention, void *datactx,
        int *action, int *numkeys, int **cfs, sds **rawkeys, sds **rawvals) {
    if (d->type->encodeData)
        return d->type->encodeData(d,intention,datactx,action,
                numkeys,cfs,rawkeys,rawvals);
    else
        return 0;
}

/* Swap-thread: decode val/subval from rawvalss returned by rocksdb. */
inline int swapDataDecodeData(swapData *d, int num, int *cfs, sds *rawkeys,
        sds *rawvals, robj **decoded) {
    if (d->type->decodeData)
        return d->type->decodeData(d,num,cfs,rawkeys,rawvals,decoded);
    else
        return 0;
}

/* Main-thread: swap in created or merged result into keyspace. */
inline int swapDataSwapIn(swapData *d, robj *result, void *datactx) {
    if (d->type->swapIn)
        return d->type->swapIn(d,result,datactx);
    else
        return 0;
}

/* Main-thread: swap out data out of keyspace. */
inline int swapDataSwapOut(swapData *d, void *datactx) {
    if (d->type->swapOut)
        return d->type->swapOut(d, datactx);
    else
        return 0;
}

/* Main-thread: swap del data out of keyspace. */
inline int swapDataSwapDel(swapData *d, void *datactx, int async) {
    if (d->type->swapDel)
        return d->type->swapDel(d, datactx, async);
    else
        return 0;
}

/* Swap-thread: prepare robj to be merged.
 * - create new object: return newly created object.
 * - merge fields into robj: subvals merged into db.value, returns NULL */
inline robj *swapDataCreateOrMergeObject(swapData *d, robj *decoded,
        void *datactx) {
    if (d->type->createOrMergeObject)
        return d->type->createOrMergeObject(d,decoded,datactx);
    else
        return NULL;
}

/* Swap-thread: clean data.value. */
inline int swapDataCleanObject(swapData *d, void *datactx) {
    if (d->type->cleanObject)
        return d->type->cleanObject(d,datactx);
    else
        return 0;
}

inline void swapDataFree(swapData *d, void *datactx) {
    /* free extend */
    if (d->type->free) d->type->free(d,datactx);
    /* free base */
    if (d->key) decrRefCount(d->key);
    if (d->value) decrRefCount(d->value);
    zfree(d);
}

sds swapDataEncodeMetaVal(swapData *d) {
    if (d->type->encodeMetaVal)
        return d->type->encodeMetaVal(d);
    else
        return NULL;
}

sds swapDataEncodeMetaKey(swapData *d) {
    return rocksEncodeMetaKey(d->db,(sds)d->key->ptr);
}

int swapDataDecodeMetaVal(swapData *d, sds rawval, int *object_type,
        long long *expire, void **object_meta) {
    if (d->type->decodeMetaVal)
        return d->type->decodeMetaVal(d,rawval,object_type,expire,object_meta);
    else 
        return 0;
}

int swapDataSetupMeta(swapData *d, int object_type, long long expire,
        void *object_meta, void **datactx) {
    serverAssert(d->type == NULL);

    d->expire = expire;

    switch (object_type) {
    case OBJ_STRING:
        swapDataSetupWholeKey(d,object_meta,datactx);
        break;
    default:
        break;
    }
    return 0;
}

