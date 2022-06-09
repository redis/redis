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

swapData *createSwapData(redisDb *db, robj *key, robj *value, robj *evict,
        objectMeta *meta, void **datactx) {
    int obj_type, big;
    swapData *data;

    obj_type = value ? value->type : evict->type;
    big = value ? value->big : evict->big;
    switch (obj_type) {
    case OBJ_STRING:
        data = createWholeKeySwapData(db,key,value,evict,datactx);
        break;
    case OBJ_HASH:
        if (big) data = createBigHashSwapData(db,key,value,evict,meta,datactx);
        else data = createWholeKeySwapData(db,key,value,evict,datactx);
        break;
    default:
        data = NULL;
        break;
    }

    return data;
}

/* Main-thread: analyze data and command intention & request to decide
 * final swap intention. e.g. command might want SWAP_IN but data not
 * evicted, then intention is decided as NOP. */ 
inline int swapDataAna(swapData *d, int cmd_intention,
        uint32_t cmd_intention_flags, struct keyRequest *key_request,
        int *intention, uint32_t *intention_flags, void *datactx) {
    if (d->type->swapAna)
        return d->type->swapAna(d,cmd_intention,cmd_intention_flags,
                key_request, intention,intention_flags,datactx);
    else
        return 0;
}

/* Swap-thread: decide how to encode keys by data and intention. */
inline int swapDataEncodeKeys(swapData *d, int intention, void *datactx,
        int *action, int *numkeys, sds **rawkeys) {
    if (d->type->encodeKeys)
        return d->type->encodeKeys(d,intention,datactx,action,numkeys,rawkeys);
    else
        return 0;
}

/* Swap-thread: decode how to encode val/subval by data and intention.
 * dataactx can be used store context of which subvals are encoded. */
inline int swapDataEncodeData(swapData *d, int intention, void *datactx,
        int *action, int *numkeys, sds **rawkeys, sds **rawvals) {
    if (d->type->encodeData)
        return d->type->encodeData(d,intention,datactx,action,
                numkeys,rawkeys,rawvals);
    else
        return 0;
}

/* Swap-thread: decode val/subval from rawvalss returned by rocksdb. */
inline int swapDataDecodeData(swapData *d, int num, sds *rawkeys,
        sds *rawvals, robj **decoded) {
    if (d->type->decodeData)
        return d->type->decodeData(d,num,rawkeys,rawvals,decoded);
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
inline int swapDataSwapDel(swapData *d, void *datactx) {
    if (d->type->swapDel)
        return d->type->swapDel(d, datactx);
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
    if (d->type->free)
        d->type->free(d,datactx);
}

#ifdef REDIS_TEST
#include <stdio.h>
#include <limits.h>


int swapDataTest(int argc, char **argv, int accurate) {
    int result = 0;
    result += swapDataWholeKeyTest(argc, argv, accurate);
    return result;
}
#endif
