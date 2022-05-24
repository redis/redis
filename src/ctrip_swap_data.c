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

swapData *createSwapData(redisDb *db, robj *key, robj *value, robj *evict, void **datactx) {
    int obj_type;
    swapData *data;

    obj_type = value ? value->type : evict->type;
    switch (obj_type) {
    case OBJ_HASH:
    case OBJ_STRING:
        data = createWholeKeySwapData(db,key,value,evict,datactx);
        break;
    default:
        data = NULL;
        break;
    }

    return data;
}

inline int swapDataAna(swapData *d, int cmd_intention,
        struct keyRequest *key_request, int *intention) {
    return d->type->swapAna(d,cmd_intention,key_request,intention);
}

inline int swapDataEncodeKeys(swapData *d, int intention, int *action,
        int *numkeys, sds **rawkeys) {
    return d->type->encodeKeys(d,intention,action,numkeys,rawkeys);
}

inline int swapDataEncodeData(swapData *d, int intention, int *action,
        int *numkeys, sds **rawkeys, sds **rawvals, void *datactx) {
    return d->type->encodeData(d,intention,action,numkeys,rawkeys,
            rawvals,datactx);
}

inline int swapDataDecodeData(swapData *d, int num, sds *rawkeys,
        sds *rawvals, robj **decoded) {
    return d->type->decodeData(d,num,rawkeys,rawvals,decoded);
}

inline int swapDataSwapIn(swapData *d, void *datactx) {
    return d->type->swapIn(d,datactx);
}

inline int swapDataSwapOut(swapData *d, void *datactx) {
    return d->type->swapOut(d, datactx);
}

inline int swapDataSwapDel(swapData *d, void *datactx) {
    return d->type->swapDel(d, datactx);
}

inline int swapDataCreateDictObject(swapData *d, robj *decoded,
        int *finish_type, void *datactx) {
    return d->type->createDictObject(d,decoded,finish_type,datactx);
}

inline int swapDataCleanObject(swapData *d, int *finish_type, void *datactx) {
    return d->type->cleanObject(d,finish_type,datactx);
}

inline void swapDataFree(swapData *d, void *datactx) {
    d->type->free(d,datactx);
}

