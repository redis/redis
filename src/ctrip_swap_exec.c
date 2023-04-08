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

swapRequest *swapRequestNew(keyRequest *key_request, int intention,
        uint32_t intention_flags, swapCtx *ctx, swapData *data,
        void *datactx,swapTrace *trace,
        swapRequestFinishedCallback cb, void *pd, void *msgs) {
    swapRequest *req = zcalloc(sizeof(swapRequest));
    UNUSED(msgs);
    req->key_request = key_request;
    req->intention = intention;
    req->intention_flags = intention_flags;
    req->swap_ctx = ctx;
    req->data = data;
    req->datactx = datactx;
    req->result = NULL;
    req->finish_cb = cb;
    req->finish_pd = pd;
    req->swap_memory = 0;
#ifdef SWAP_DEBUG
    req->msgs = msgs;
#endif
    req->errcode = 0;
    req->trace = trace;
    return req;
}

void swapRequestFree(swapRequest *req) {
    zfree(req);
}

void swapRequestSetIntention(swapRequest *req, int intention,
        uint32_t intention_flags) {
    req->intention = intention;
    req->intention_flags = intention_flags;
}

void swapRequestUpdateStatsRIO(swapRequest *req, RIO *rio) {
    size_t payload_size = RIOEstimatePayloadSize(rio);
    req->swap_memory += payload_size;
    atomicIncr(server.swap_inprogress_memory,payload_size);
}

/* void swapRequestUpdateStatsExecuted(swapRequest *req) { */
    /* [> req->intention may be negative when key doesn't exist <] */
    /* int intention = req->intention < 0 ? req->key_request->cmd_intention : req->intention; */
    /* const long duration = elapsedUs(req->swap_timer); */
    /* atomicIncr(server.ror_stats->swap_stats[intention].count,1); */
    /* atomicIncr(server.ror_stats->swap_stats[intention].batch,1); */
    /* atomicIncr(server.ror_stats->swap_stats[intention].memory,req->swap_memory); */
    /* atomicIncr(server.ror_stats->swap_stats[intention].time, duration); */
/* } */

void swapRequestUpdateStatsCallback(swapRequest *req) {
    atomicDecr(server.swap_inprogress_batch,1);
    atomicDecr(server.swap_inprogress_count,1);
    atomicDecr(server.swap_inprogress_memory,req->swap_memory);
}

void swapRequestUpdateStatsSwapInNotFound(swapRequest *req) {
    /* key confirmed not exists, no need to execute swap request. */
    serverAssert(!swapDataAlreadySetup(req->data));
    if (isSwapHitStatKeyRequest(req->key_request)) {
        atomicIncr(server.swap_hit_stats->stat_swapin_not_found_cachemiss_count,1);
    }
}

void swapRequestExecuted(swapRequest *req) {
    // swapRequestUpdateStatsExecuted(req);
    if (req->trace) swapTraceNotify(req->trace, req->intention);
}

void swapRequestCallback(swapRequest *req) {
    swapRequestMerge(req);
    if (req->trace) swapTraceCallback(req->trace);
    req->finish_cb(req->data, req->finish_pd, req->errcode);
    swapRequestUpdateStatsCallback(req);
}

void swapRequestDoInMeta(swapRequest *req) {
    RIO _rio = {0}, *rio = &_rio;
    swapData *data = req->data;
    sds rawkey = swapDataEncodeMetaKey(data), rawval = NULL;
    int cfs[1] = {META_CF}, errcode;
    sds rawkeys[1] = {rawkey};

    RIOInitGet(rio,1,cfs,rawkeys);
    RIODo(rio);
    if (rio->get.rawvals) rawval = rio->get.rawvals[0];

    if ((errcode = RIOGetError(rio))) {
        swapRequestSetError(req,errcode);
        DEBUG_MSGS_APPEND(req->msgs,"exec-swapinmeta", "failed. errcode=%d",
                (sds)data->key->ptr,errcode);
        goto end;
    }

    if (RIOGetNotFound(rio)) {
        /* No swap needed if meta not found. */
        swapRequestSetIntention(req,SWAP_NOP,0);
        DEBUG_MSGS_APPEND(req->msgs,"exec-swapinmeta", "%s: notfound",
                (sds)data->key->ptr);
        goto end;
    }

    if ((errcode = swapDataDecodeAndSetupMeta(data,rawval,&req->datactx))) {
        swapRequestSetError(req,errcode);
        DEBUG_MSGS_APPEND(req->msgs,"exec-swapinmeta", "decode failed:%d",
                errcode);
        goto end;
    }

#ifdef SWAP_DEBUG
    objectMeta *object_meta = swapDataObjectMeta(data);
    sds dump = dumpObjectMeta(object_meta);
    DEBUG_MSGS_APPEND(req->msgs,"exec-swapinmeta", "%s => %s",
            (sds)data->key->ptr,dump);
    sdsfree(dump);
#endif

    swapCtxSetSwapData(req->swap_ctx,req->data,req->datactx);

end:
    if (rawkey) sdsfree(rawkey);
    if (rawval) sdsfree(rawval);
    if (rio->err) sdsfree(rio->err);
}

void swapRequestDoDelMeta(swapRequest *req) {
    RIO rio_, *rio = &rio_;
    sds rawkey = swapDataEncodeMetaKey(req->data);
    int cfs[1] = {META_CF}, errcode;
    sds rawkeys[1] = {rawkey};

    RIOInitDel(rio,1,cfs,rawkeys);
    RIODo(rio);
    if ((errcode = RIOGetError(rio))) {
        swapRequestSetError(req,errcode);
        if (rio->err) sdsfree(rio->err);
    }

    if (rawkey) sdsfree(rawkey);
}

void swapRequestDoOutMeta(swapRequest *req) {
    RIO rio_, *rio = &rio_;
    sds rawkey = swapDataEncodeMetaKey(req->data);
    sds rawval = swapDataEncodeMetaVal(req->data);
    int cfs[1] = {META_CF}, errcode;
    sds rawkeys[1] = {rawkey}, rawvals[1] = {rawval};

    RIOInitPut(rio,1,cfs,rawkeys,rawvals);
    RIODo(rio);
    if ((errcode = RIOGetError(rio))) {
        swapRequestSetError(req,errcode);
        if (rio->err) sdsfree(rio->err);
    }

    if (rawkey) sdsfree(rawkey);
    if (rawval) sdsfree(rawval);
}

void swapRequestDoDel(swapRequest *req, int numkeys, int *cfs,
        sds *rawkeys) {
    int errcode;
    RIO _rio = {0}, *rio = &_rio;
    UNUSED(req);

    RIOInitDel(rio,numkeys,cfs,rawkeys);
    swapRequestUpdateStatsRIO(req,rio);
    RIODo(rio);
    if ((errcode = swapRequestGetError(req))) {
        swapRequestSetError(req, errcode);
    }
    RIODeinit(rio);
    DEBUG_MSGS_APPEND(req->msgs,"exec-in.del","numkeys=%d,errcode=%d",
            numkeys, errcode);
}

void swapRequestDoAuxDelSubCustom(swapRequest *req, int action, int numkeys,
        int* cfs, sds* rawkeys, sds* rawvals) {

    int errcode = 0;
    int outaction, outnum;
    int* outcfs = NULL;
    sds* outrawkeys = NULL;

    if ((errcode = req->data->type->rocksDel(req->data,req->datactx,action,
                    numkeys,cfs,rawkeys,rawvals,
                    &outaction,&outnum,&outcfs,&outrawkeys))) {
        swapRequestSetError(req,errcode);
        goto end;
    }

    switch(outaction) {
    case ROCKS_DEL:
        swapRequestDoDel(req,outnum,outcfs,outrawkeys);
        outcfs = NULL, outrawkeys = NULL;
        if ((errcode = swapRequestGetError(req))) goto end;
        break;
    default:
        goto end;
        break;
    }

end:
    if (outrawkeys) {
        for (int i = 0; i < outnum; i++) {
            sdsfree(outrawkeys[i]);
        }
        zfree(outrawkeys);
    }
    if (outcfs) {
        zfree(outcfs);
    }
}

void swapRequestDoAuxDelSub(swapRequest *req, int action, int numkeys, int* cfs,
        sds* rawkeys, sds* rawvals) {
    if (req->data->type->rocksDel == NULL) {
        serverAssert(rawvals == NULL);
        swapRequestDoDel(req,numkeys,cfs,rawkeys);
    } else {
        swapRequestDoAuxDelSubCustom(req,action,numkeys,cfs,rawkeys,rawvals);
    }
}

void swapRequestDoAuxDel(swapRequest *req, RIO *rio) {
    sds *rawkeys, *rawvals;
    int numkeys, *cfs, *tmpcfs = NULL, i;

    switch (rio->action) {
    case ROCKS_GET:
        numkeys = rio->get.numkeys;
        cfs = rio->get.cfs;
        rawkeys = rio->get.rawkeys;
        rawvals = rio->get.rawvals;
        break;
    case ROCKS_ITERATE:
        tmpcfs = zmalloc(sizeof(int)*rio->iterate.numkeys);
        for (i = 0; i < rio->iterate.numkeys; i++) tmpcfs[i] = rio->iterate.cf;
        numkeys = rio->iterate.numkeys;
        cfs = tmpcfs;
        rawkeys = rio->iterate.rawkeys;
        rawvals = rio->iterate.rawvals;
        break;
    default:
        numkeys = 0;
        break;
    }

    if (numkeys == 0) goto end;

    swapRequestDoAuxDelSub(req,rio->action,numkeys,cfs,rawkeys,rawvals);

end:
    if (tmpcfs) zfree(tmpcfs);
}

void swapRequestExecuteIn(swapRequest *req) {
    void *decoded;
    int errcode, action;
    RIO _rio = {0}, *rio = &_rio;
    swapData *data = req->data;

    serverAssert(!swapRequestGetError(req));

    if ((errcode = swapDataSwapAnaAction(data,req->intention,
                    req->datactx,&action))) {
        swapRequestSetError(req,errcode);
        goto end;
    }

    DEBUG_MSGS_APPEND(req->msgs,"exec-in-encodekeys","action=%s",
            rocksActionName(action));

    if (action == ROCKS_GET) {
        int numkeys, *cfs = NULL;
        sds *rawkeys = NULL;

        if ((errcode = swapDataEncodeKeys(data,req->intention,
                    req->datactx,&numkeys,&cfs,&rawkeys))) {
            swapRequestSetError(req,errcode);
            goto end;
        }

        RIOInitGet(rio,numkeys,cfs,rawkeys);
        RIODo(rio);
        if ((errcode = RIOGetError(rio))) {
            swapRequestSetError(req,errcode);
            goto end;
        }

        DEBUG_MSGS_APPEND(req->msgs,"exec-in-get",
                "numkeys=%d,rio=ok", numkeys);

        if ((errcode = swapDataDecodeData(data,rio->get.numkeys,rio->get.cfs,
                    rio->get.rawkeys,rio->get.rawvals,&decoded))) {
            swapRequestSetError(req,errcode);
            goto end;
        }

        DEBUG_MSGS_APPEND(req->msgs,"exec-in-decodedata","decoded=%p",
                (void*)decoded);
    }  else if (action == ROCKS_ITERATE) {
        int *tmpcfs, i;
        int limit, cf;
        uint32_t flags = 0;
        sds start = NULL,end = NULL;

        if ((errcode = swapDataEncodeRange(data,req->intention,req->datactx,
                    &limit,&flags,&cf,&start,&end))) {
            swapRequestSetError(req,errcode);
            goto end;
        }

        RIOInitIterate(rio, cf, flags, start, end, limit);
        RIODo(rio);
        if ((errcode = RIOGetError(rio))) {
            swapRequestSetError(req,errcode);
            goto end;
        }

        tmpcfs = zmalloc(sizeof(int)*rio->iterate.numkeys);
        for (i = 0; i < rio->iterate.numkeys; i++) tmpcfs[i] = cf;
        if (rio->iterate.nextseek) {
            data->nextseek = rio->iterate.nextseek;
            rio->iterate.nextseek = NULL;
        }

        DEBUG_MSGS_APPEND(req->msgs,"exec-in-iterate",
                "start=%s,end=%s,limit=%d,flags=%d,rio=ok",
                start,end,limit,flags);

        if ((errcode = swapDataDecodeData(data,rio->iterate.numkeys,tmpcfs,
                        rio->iterate.rawkeys,rio->iterate.rawvals,&decoded))) {
            swapRequestSetError(req,errcode);
            zfree(tmpcfs);
            goto end;
        }
        zfree(tmpcfs);

        DEBUG_MSGS_APPEND(req->msgs,"exec-in-decodedata", "decoded=%p",
                (void*)decoded);
    } else {
        swapRequestSetError(req,SWAP_ERR_EXEC_UNEXPECTED_ACTION);
        goto end;
    }

    req->result = swapDataCreateOrMergeObject(data,decoded,req->datactx);
    DEBUG_MSGS_APPEND(req->msgs,"exec-in-createormerge","result=%p",
            (void*)req->result);

    if (req->intention_flags & SWAP_EXEC_IN_DEL) {
        int is_hot = swapDataMergedIsHot(data,req->result,req->datactx);

        /* There is no need to delete subkey if meta gets deleted,
         * subkeys will be deleted by compaction filter. */

        if (is_hot) {
            req->data->del_meta = 1;
            req->data->persistence_deleted = 1;
            swapRequestDoDelMeta(req);
            if (data->object_type != OBJ_STRING) {
                /* String is not versioned */
                goto end;
            }
        }

        swapRequestDoAuxDel(req,rio);

        if ((errcode = swapRequestGetError(req))) {
            goto end;
        }
    }

end:
    swapRequestUpdateStatsRIO(req,rio);
    DEBUG_MSGS_APPEND(req->msgs,"exec-in-end","errcode=%d",
            swapRequestGetError(req));
    swapRequestExecuted(req);
    RIODeinit(rio);
}

void swapRequestExecuteOut(swapRequest *req) {
    int numkeys, errcode = 0, action, *cfs = NULL;
    sds *rawkeys = NULL, *rawvals = NULL;
    RIO _rio = {0}, *rio = &_rio;
    swapData *data = req->data;

    serverAssert(!swapRequestGetError(req));

    if ((errcode = swapDataSwapAnaAction(data,req->intention,
                    req->datactx,&action))) {
        swapRequestSetError(req,errcode);
        goto end;
    }

    if ((errcode = swapDataEncodeData(data,req->intention,req->datactx,
                    &numkeys,&cfs,&rawkeys,&rawvals))) {
        swapRequestSetError(req,errcode);
        goto end;
    }

    DEBUG_MSGS_APPEND(req->msgs,"exec-out-encodedata",
            "action=%s, numkeys=%d", rocksActionName(action), numkeys);

    if (action == ROCKS_PUT) {
#ifdef SWAP_DEBUG
        sds rawval_repr = sdscatrepr(sdsempty(), rawvals[0], sdslen(rawvals[0]));
        DEBUG_MSGS_APPEND(req->msgs,"exec-out-put","rawkey=%s,rawval=%s",
                rawkeys[0], rawval_repr);
        sdsfree(rawval_repr);
#endif
        RIOInitPut(rio,numkeys,cfs,rawkeys,rawvals);
    } else {
        swapRequestSetError(req,SWAP_ERR_EXEC_UNEXPECTED_ACTION);
        goto end;
    }

    swapRequestUpdateStatsRIO(req,rio);
    RIODo(rio);
    if ((errcode = RIOGetError(rio))) {
        swapRequestSetError(req,errcode);
        goto end;
    }

    if ((errcode = swapDataCleanObject(data,req->datactx))) {
        swapRequestSetError(req,errcode);
        goto end;
    }
    DEBUG_MSGS_APPEND(req->msgs,"exec-out-cleanobject","ok");

    if (data->db && data->key) {
        swapRequestDoOutMeta(req);

#ifdef SWAP_DEBUG
    objectMeta *object_meta = swapDataObjectMeta(data);
    sds dump = dumpObjectMeta(object_meta);
    DEBUG_MSGS_APPEND(req->msgs,"exec-swapoutmeta","%s => %s",
            (sds)data->key->ptr, dump);
    sdsfree(dump);
#endif

        if (swapRequestGetError(req)) goto end;
    }

end:

    DEBUG_MSGS_APPEND(req->msgs,"exec-out-end","errcode=%d",errcode);
    if (server.swap_debug_swapout_notify_delay_micro)
        usleep(server.swap_debug_swapout_notify_delay_micro);
    swapRequestExecuted(req);
    RIODeinit(rio);
}

void swapRequestExecuteDel(swapRequest *req) {
    int numkeys, errcode = 0, action = ROCKS_NOP;
    int *cfs = NULL;
    sds *rawkeys = NULL;
    RIO _rio = {0}, *rio = &_rio;

    serverAssert(!swapRequestGetError(req));

    if ((errcode = swapDataSwapAnaAction(req->data,req->intention,
                    req->datactx,&action))) {
        swapRequestSetError(req,errcode);
        goto end;
    }
    if (action == ROCKS_NOP) goto end;

    DEBUG_MSGS_APPEND(req->msgs,"exec-del-encodekeys","action=%s",
            rocksActionName(action));

    if ((errcode = swapDataEncodeKeys(req->data,req->intention,req->datactx,
                    &numkeys,&cfs,&rawkeys))) {
        swapRequestSetError(req,errcode);
        goto end;
    }

    if (action == ROCKS_DEL) {
        DEBUG_MSGS_APPEND(req->msgs,"exec-del-del","numkeys=%d,rawkey[0]=%s",
                numkeys,rawkeys[0]);
        RIOInitDel(rio,numkeys,cfs,rawkeys);
    } else {
        swapRequestSetError(req,SWAP_ERR_EXEC_UNEXPECTED_ACTION);
        goto end;
    }

    swapRequestUpdateStatsRIO(req, rio);
    RIODo(rio);
    if ((errcode = RIOGetError(rio))) {
        swapRequestSetError(req,errcode);
        goto end;
    }

    swapRequestDoDelMeta(req);

end:
    swapRequestExecuted(req);
    RIODeinit(rio);
}

void swapRequestExecuteUtil_CompactRange(swapRequest *req) {
    char dir[ROCKS_DIR_MAX_LEN];
    UNUSED(req);
    snprintf(dir, ROCKS_DIR_MAX_LEN, "%s/%d", ROCKS_DATA, server.rocksdb_epoch);
    serverLog(LL_WARNING, "[rocksdb compact range before] dir(%s) size(%ld)", dir, get_dir_size(dir));
    for (int i = 0; i < CF_COUNT; i++) {
        rocksdb_compact_range_cf(server.rocks->db, server.rocks->cf_handles[i] ,NULL, 0, NULL, 0);
    }
    serverLog(LL_WARNING, "[rocksdb compact range after] dir(%s) size(%ld)", dir, get_dir_size(dir));
}

void swapRequestExecuteUtil_GetRocksdbStats(swapRequest* req) {
    char** result = zmalloc(sizeof(char*) * CF_COUNT);
    for(int i = 0; i < CF_COUNT; i++) {
        result[i] = rocksdb_property_value_cf(server.rocks->db, server.rocks->cf_handles[i], "rocksdb.stats");
    }
    req->finish_pd = result;
}

void swapRequestExecuteUtil_CreateCheckpoint(swapRequest* req) {
    rocksdbCreateCheckpointPayload *pd = req->finish_pd;
    sds checkpoint_dir = sdscatprintf(sdsempty(), "%s/tmp_%lld", ROCKS_DATA, ustime());
    rocksdb_checkpoint_t* checkpoint = NULL;

    char* err = NULL;
    checkpoint = rocksdb_checkpoint_object_create(server.rocks->db, &err);
    if (err != NULL) {
        serverLog(LL_WARNING, "[rocks] checkpoint object create fail :%s\n", err);
        goto error;
    }
    rocksdb_checkpoint_create(checkpoint, checkpoint_dir, 0, &err);
    if (err != NULL) {
        serverLog(LL_WARNING, "[rocks] checkpoint %s create fail: %s", checkpoint_dir, err);
        goto error;
    }
    pd->checkpoint = checkpoint;
    pd->checkpoint_dir = checkpoint_dir;
    return;

error:
    if(checkpoint != NULL) {
        rocksdb_checkpoint_object_destroy(checkpoint);
    }
    sdsfree(checkpoint_dir);
    pd->checkpoint = NULL;
    pd->checkpoint_dir = NULL;
}

void swapRequestExecuteUtil(swapRequest *req) {
    switch(req->intention_flags) {
    case COMPACT_RANGE_TASK:
        swapRequestExecuteUtil_CompactRange(req);
        break;
    case GET_ROCKSDB_STATS_TASK:
        swapRequestExecuteUtil_GetRocksdbStats(req);
        break;
    case CREATE_CHECKPOINT:
        swapRequestExecuteUtil_CreateCheckpoint(req);
        break;
    default:
        swapRequestSetError(req,SWAP_ERR_EXEC_UNEXPECTED_UTIL);
        break;
    }
}

void swapRequestExecute(swapRequest *req) {
    serverAssert(!swapRequestGetError(req));

    if (server.swap_debug_before_exec_swap_delay_micro)
        usleep(server.swap_debug_before_exec_swap_delay_micro);
    /* do execute swap */
    switch (req->intention) {
    case SWAP_NOP:
        /* TODO finish req? */
        break;
    case SWAP_IN:
        swapRequestExecuteIn(req);
        break;
    case SWAP_OUT:
        swapRequestExecuteOut(req);
        break;
    case SWAP_DEL:
        swapRequestExecuteDel(req);
        break;
    case SWAP_UTILS:
        swapRequestExecuteUtil(req);
        break;
    default:
        /* TODO finish req? */
        break;
    }
}

/* Called by async-complete-queue or parallel-sync in server thread
 * to swap in/out/del data */
void swapRequestMerge(swapRequest *req) {
    DEBUG_MSGS_APPEND(req->msgs,"exec-finish","intention=%s",
            swapIntentionName(req->intention));
    int retval = 0, del_skip = 0, swap_out_completely = 0;
    if (req->errcode) return;

    swapData *data = req->data;
    void *datactx = req->datactx;

    switch (req->intention) {
    case SWAP_NOP:
        /* No swap for req if meta not found. */
        if (!swapDataAlreadySetup(data)) {
            if (data->db->swap_absent_cache)
                absentsCachePut(data->db->swap_absent_cache,data->key->ptr);
        }

        break;
    case SWAP_IN:
        retval = swapDataSwapIn(data,req->result,datactx);
        if (retval == 0) {
            if (swapDataIsCold(data) && req->result) {
                swapDataTurnWarmOrHot(data);
            }
        }
        break;
    case SWAP_OUT:
        retval = swapDataSwapOut(data,datactx,&swap_out_completely);
        if (!swapDataIsCold(data) && swap_out_completely) {
            swapDataTurnCold(data);
        }
        break;
    case SWAP_DEL:
        del_skip = req->intention_flags & SWAP_FIN_DEL_SKIP;
        swapDataTurnDeleted(data,del_skip);
        retval = swapDataSwapDel(data,datactx,del_skip);
        break;
    case -1: //swap meta
        break;
    default:
        retval = SWAP_ERR_DATA_FIN_FAIL;
    }
    req->errcode = retval;
}

void swapExecBatchUpdateStatsRIOBatch(swapExecBatch *exec_batch,
        RIOBatch *rios) {
    size_t payload_size = 0;
    serverAssert(exec_batch->count == rios->count);
    for (size_t i = 0; i < rios->count; i++) {
        RIO *rio = rios->rios+i;
        swapRequest *req = exec_batch->reqs[i];
        size_t rio_memory = RIOEstimatePayloadSize(rio);
        payload_size += rio_memory;
        req->swap_memory += rio_memory;
    }
    atomicIncr(server.swap_inprogress_memory,payload_size);
}

void swapExecBatchExecuteInGet(swapExecBatch *exec_batch) {
    void *decoded;
    int errcode;
    RIOBatch _rios = {0}, *rios = &_rios;

    serverAssert(exec_batch->intention == SWAP_IN);
    serverAssert(exec_batch->action == ROCKS_GET);
    serverAssert(!swapExecBatchGetError(exec_batch));

    RIOBatchInit(rios,ROCKS_GET);

    for (size_t i = 0; i < exec_batch->count; i++) {
        int numkeys, *cfs = NULL;
        sds *rawkeys = NULL;
        swapRequest *req = exec_batch->reqs[i];
        RIO *rio = RIOBatchAlloc(rios);

        if ((errcode = swapDataEncodeKeys(req->data,req->intention,
                    req->datactx,&numkeys,&cfs,&rawkeys))) {
            swapRequestSetError(req,errcode);
            /* rio corresponds req even if no rio needed */
            RIOInitGet(rio,0,NULL,NULL); //TODO test
        } else {
            RIOInitGet(rio,numkeys,cfs,rawkeys);
        }
    }

    RIOBatchDo(rios);

    for (size_t i = 0; i < exec_batch->count; i++) {
        swapRequest *req = exec_batch->reqs[i];
        RIO *rio = rios->rios+i;

        if (swapRequestGetError(req)) continue;

        if ((errcode = RIOGetError(rio))) {
            swapRequestSetError(req,errcode);
            continue;
        }

        if ((errcode = swapDataDecodeData(req->data,rio->get.numkeys,
                        rio->get.cfs,rio->get.rawkeys,rio->get.rawvals,
                        &decoded))) {
            swapRequestSetError(req,errcode);
            continue;
        }

        req->result = swapDataCreateOrMergeObject(req->data,decoded,req->datactx);

        if (req->intention_flags & SWAP_EXEC_IN_DEL) {
            int is_hot = swapDataMergedIsHot(req->data,req->result,req->datactx);

            /* There is no need to delete subkey if meta gets deleted,
             * subkeys will be deleted by compaction filter (except for
             * string type, which is not deleted by compaction filter). */

            if (is_hot) {
                req->data->del_meta = 1;
                req->data->persistence_deleted = 1;
                swapRequestDoDelMeta(req);
            }

            if (!is_hot || req->data->object_type == OBJ_STRING) {
                swapRequestDoAuxDel(req,rio);
            }
        }
    }

    swapExecBatchUpdateStatsRIOBatch(exec_batch,rios);

    RIOBatchDeinit(rios);
}

void swapExecBatchExecuteOutPut(swapExecBatch *exec_batch) {
    int errcode = 0;
    RIOBatch _rios = {0}, *rios = &_rios;
    RIO _meta_rio = {0}, *meta_rio = &_meta_rio;
    int *meta_cfs = NULL, num_metas = 0;
    sds *meta_rawkeys = NULL, *meta_rawvals = NULL;
    size_t count = exec_batch->count;

    serverAssert(exec_batch->intention == SWAP_OUT);
    serverAssert(exec_batch->action == ROCKS_PUT);
    serverAssert(!swapExecBatchGetError(exec_batch));

    RIOBatchInit(rios,ROCKS_PUT);

    for (size_t i = 0; i < count; i++) {
        int numkeys, *cfs = NULL;
        sds *rawkeys = NULL, *rawvals = NULL;
        swapRequest *req = exec_batch->reqs[i];
        RIO *rio = RIOBatchAlloc(rios);

        if ((errcode = swapDataEncodeData(req->data,req->intention,
                        req->datactx,&numkeys,&cfs,&rawkeys,&rawvals))) {
            swapRequestSetError(req,errcode);
            /* rio corresponds req even if no rio needed */
            RIOInitPut(rio,0,NULL,NULL,NULL);
        } else {
            RIOInitPut(rio,numkeys,cfs,rawkeys,rawvals);
        }
    }

    RIOBatchDo(rios);

    for (size_t i = 0; i < count; i++) {
        swapRequest *req = exec_batch->reqs[i];
        RIO *rio = rios->rios+i;

        if (swapRequestGetError(req)) continue;

        if ((errcode = RIOGetError(rio))) {
            swapRequestSetError(req,errcode);
            continue;
        }

        if ((errcode = swapDataCleanObject(req->data,req->datactx))) {
            swapRequestSetError(req,errcode);
            continue;
        }
    }

    meta_cfs = zmalloc(sizeof(int)*count);
    meta_rawkeys = zmalloc(sizeof(sds)*count);
    meta_rawvals = zmalloc(sizeof(sds)*count);
    for (size_t i = 0; i < count; i++) {
        swapRequest *req = exec_batch->reqs[i];
        meta_cfs[num_metas] = META_CF;
        meta_rawkeys[num_metas] = swapDataEncodeMetaKey(req->data);
        meta_rawvals[num_metas] = swapDataEncodeMetaVal(req->data);
        num_metas++;
    }
    RIOInitPut(meta_rio,num_metas,meta_cfs,meta_rawkeys,meta_rawvals);
    RIODo(meta_rio);
    if ((errcode = RIOGetError(meta_rio))) {
        swapExecBatchSetError(exec_batch,errcode);
    }
    RIODeinit(meta_rio);

    swapExecBatchUpdateStatsRIOBatch(exec_batch,rios);

    RIOBatchDeinit(rios);
}

void swapExecBatchExecuteIndividually(swapExecBatch *exec_batch) {
    for (size_t i = 0; i < exec_batch->count; i++) {
        swapRequest *req = exec_batch->reqs[i];
        serverAssert(req->intention == SWAP_UTILS);
        if (!swapRequestGetError(req)) {
            swapRequestExecuteUtil(req);
        }
    }
}

static inline void swapExecBatchExecuteStart(swapExecBatch *exec_batch) {
    elapsedStart(&exec_batch->swap_timer);
}

static inline void swapExecBatchExecuteEnd(swapExecBatch *exec_batch) {
    int intention = exec_batch->intention;
    size_t swap_memory = 0;

    const long duration = elapsedUs(exec_batch->swap_timer);
    for (size_t i = 0; i < exec_batch->count; i++) {
        swapRequest *req = exec_batch->reqs[i];
        swap_memory += req->swap_memory;
    }

    atomicIncr(server.ror_stats->swap_stats[intention].batch,1);
    atomicIncr(server.ror_stats->swap_stats[intention].count,exec_batch->count);
    atomicIncr(server.ror_stats->swap_stats[intention].time, duration);
    atomicIncr(server.ror_stats->swap_stats[intention].memory,swap_memory);
}

void swapExecBatchExecute(swapExecBatch *exec_batch) {
    serverAssert(exec_batch->intention != SWAP_NOP);

    swapExecBatchExecuteStart(exec_batch);

    if (exec_batch->intention == SWAP_IN &&
            exec_batch->action == ROCKS_GET) {
        swapExecBatchExecuteInGet(exec_batch);
    } else if (exec_batch->intention == SWAP_OUT &&
            exec_batch->action == ROCKS_PUT) {
        swapExecBatchExecuteOutPut(exec_batch);
        if (server.swap_debug_swapout_notify_delay_micro)
            usleep(server.swap_debug_swapout_notify_delay_micro);
    } else if (exec_batch->intention == SWAP_UTILS) {
        swapExecBatchExecuteIndividually(exec_batch);
    } else {
        swapExecBatchSetError(exec_batch,SWAP_ERR_EXEC_FAIL);
        serverLog(LL_WARNING,
                "unexpected execute batch intention(%d) action(%d)\n",
                exec_batch->intention, exec_batch->action);
    }

    swapExecBatchExecuteEnd(exec_batch);
}

void swapExecBatchPreprocess(swapExecBatch *meta_batch) {
    swapRequest *req;
    RIO _rio = {0}, *rio = &_rio;
    int *cfs = zmalloc(meta_batch->count*sizeof(int)), errcode;
    sds *rawkeys = zmalloc(meta_batch->count*sizeof(sds));
    int intention;
    uint32_t intention_flags;


    for (size_t i = 0; i < meta_batch->count; i++) {
        req = meta_batch->reqs[i];
        cfs[i] = META_CF;
        rawkeys[i] = swapDataEncodeMetaKey(req->data);
    }

    RIOInitGet(rio,meta_batch->count,cfs,rawkeys);
    RIODo(rio);
    if ((errcode = RIOGetError(rio))) {
        swapExecBatchSetError(meta_batch,errcode);
        DEBUG_BATCH_MSGS_APPEND(meta_batch,"exec-swapinmeta",
                "failed. errcode=%d",errcode);
        goto end;
    }

    for (size_t i = 0; i < meta_batch->count; i++) {
        req = meta_batch->reqs[i];
        sds rawval = rio->get.rawvals[i];
        if (rawval == NULL) {
            /* No swap needed if meta not found. */
            swapRequestSetIntention(req,SWAP_NOP,0);
            DEBUG_MSGS_APPEND(req->msgs,"exec-swapinmeta", "%s: notfound",
                    (sds)data->key->ptr);
            swapRequestUpdateStatsSwapInNotFound(req);
            continue;
        }

        if ((errcode = swapDataDecodeAndSetupMeta(req->data,rawval,
                        &req->datactx))) {
            DEBUG_MSGS_APPEND(req->msgs,"exec-swapinmeta", "decode failed:%d",
                    errcode);
            swapRequestSetError(req,errcode);
            continue;
        }

#ifdef SWAP_DEBUG
        objectMeta *object_meta = swapDataObjectMeta(data);
        sds dump = dumpObjectMeta(object_meta);
        DEBUG_MSGS_APPEND(req->msgs,"exec-swapinmeta", "%s => %s",
                (sds)data->key->ptr,dump);
        sdsfree(dump);
#endif

        swapCtxSetSwapData(req->swap_ctx,req->data,req->datactx);

        if ((errcode = swapDataAna(req->data,req->key_request,
                        &intention,&intention_flags,req->datactx))) {
            swapRequestSetError(req, errcode);
            continue;
        }

        swapRequestSetIntention(req,intention,intention_flags);
    }

end:
    /* cfs & rawkeys moved to rio */
    RIODeinit(rio);
}

#ifdef REDIS_TEST

/* void mockNotifyCallback(swapRequest *req, void *pd) { */
    /* UNUSED(req),UNUSED(pd); */
/* } */

/* int wholeKeyRocksDataExists(redisDb *db, robj *key) { */
    /* size_t vlen; */
    /* char *err = NULL, *rawval = NULL; */
    /* sds rawkey = rocksEncodeDataKey(db,key->ptr,0,NULL); */
    /* rawval = rocksdb_get_cf(server.rocks->db, server.rocks->ropts,swapGetCF(DATA_CF),rawkey,sdslen(rawkey),&vlen,&err); */
    /* serverAssert(err == NULL); */
    /* zlibc_free(rawval); */
    /* return rawval != NULL; */
/* } */

/* int wholeKeyRocksMetaExists(redisDb *db, robj *key) { */
    /* size_t vlen; */
    /* char *err = NULL, *rawval = NULL; */
    /* sds rawkey = rocksEncodeMetaKey(db,key->ptr); */
    /* rawval = rocksdb_get_cf(server.rocks->db, server.rocks->ropts,swapGetCF(META_CF),rawkey,sdslen(rawkey),&vlen,&err); */
    /* serverAssert(err == NULL); */
    /* zlibc_free(rawval); */
    /* return rawval != NULL; */
/* } */

int doRocksdbFlush() {
    int i;
    char *err = NULL;
    rocksdb_flushoptions_t *flushopts = rocksdb_flushoptions_create();
    for (i = 0; i < CF_COUNT; i++) {
        rocksdb_flush_cf(server.rocks->db, flushopts, server.rocks->cf_handles[i], &err);
    }
    serverAssert(err == NULL);
    rocksdb_flushoptions_destroy(flushopts);
    return 0;
}

/* void initServer(void); */
/* void initServerConfig(void); */
/* void InitServerLast(); */
/* int swapExecTest(int argc, char *argv[], int accurate) { */
    /* UNUSED(argc); */
    /* UNUSED(argv); */
    /* UNUSED(accurate); */

    /* int error = 0; */
    /* server.hz = 10; */
    /* sds rawkey1 = sdsnew("rawkey1"), rawkey2 = sdsnew("rawkey2"); */
    /* sds rawval1 = sdsnew("rawval1"), rawval2 = sdsnew("rawval2"); */
    /* sds prefix = sdsnew("rawkey"); */
    /* robj *key1 = createStringObject("key1",4); */
    /* robj *val1 = createStringObject("val1",4); */
    /* initTestRedisDb(); */
    /* monotonicInit(); */
    /* redisDb *db = server.db; */
    /* long long EXPIRE = 3000000000LL * 1000; */

    /* keyRequest key1_req_, *key1_req = &key1_req_; */
    /* key1_req->level = REQUEST_LEVEL_KEY; */
    /* key1_req->b.num_subkeys = 0; */
    /* key1_req->key = createStringObject("key1",4); */
    /* key1_req->b.subkeys = NULL; */
    /* swapCtx *ctx = swapCtxCreate(NULL,key1_req,NULL,NULL); */

    /* TEST("exec: init") { */
        /* initServerConfig(); */
        /* incrRefCount(val1); */
        /* dbAdd(db,key1,val1); */
        /* setExpire(NULL,db,key1,EXPIRE); */
        /* if (!server.rocks) rocksInit(); */
        /* initStatsSwap(); */
    /* } */

   /* TEST("exec: swap-out hot string") { */
       /* val1 = lookupKey(db,key1,LOOKUP_NOTOUCH); */
       /* test_assert(val1 != NULL); */
       /* test_assert(getExpire(db,key1) == EXPIRE); */
       /* swapData *data = createWholeKeySwapDataWithExpire(db,key1,val1,EXPIRE,NULL); */
       /* swapRequest *req = swapRequestNew(NULL[>!cold<],SWAP_OUT,0,ctx,data,NULL,NULL,NULL,NULL,NULL); */
       /* req->notify_cb = mockNotifyCallback; */
       /* req->notify_pd = NULL; */
       /* processSwapRequest(req); */
       /* test_assert(req->errcode == 0); */
       /* finishSwapRequest(req); */
       /* test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL); */
       /* test_assert(getExpire(db,key1) == -1); */
       /* test_assert(wholeKeyRocksDataExists(db,key1)); */
       /* test_assert(wholeKeyRocksMetaExists(db,key1)); */
   /* } */

   /* TEST("exec: swap-in cold string") { */
       /* [> rely on data swap out to rocksdb by previous case <] */
       /* swapData *data = createSwapData(db,key1,NULL); */
       /* key1_req->cmd_intention = SWAP_IN; */
       /* key1_req->cmd_intention_flags = 0; */
       /* swapRequest *req = swapRequestNew(key1_req,-1,-1,ctx,data,NULL,NULL,NULL,NULL,NULL); */
       /* req->notify_cb = mockNotifyCallback; */
       /* req->notify_pd = NULL; */
       /* processSwapRequest(req); */
       /* test_assert(req->errcode == 0); */
       /* finishSwapRequest(req); */
       /* test_assert((val1 = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL && !objectIsDirty(val1)); */
       /* test_assert(getExpire(db,key1) == EXPIRE); */
       /* test_assert(wholeKeyRocksDataExists(db,key1)); */
       /* test_assert(wholeKeyRocksMetaExists(db,key1)); */
   /* }  */

   /* TEST("exec: swap-del hot string") { */
       /* [> rely on data swap out to rocksdb by previous case <] */
       /* val1 = lookupKey(db,key1,LOOKUP_NOTOUCH); */
       /* swapData *data = createWholeKeySwapData(db,key1,val1,NULL); */
       /* swapRequest *req = swapRequestNew(NULL[>!cold<],SWAP_DEL,0,ctx,data,NULL,NULL,NULL,NULL,NULL); */
       /* req->notify_cb = mockNotifyCallback; */
       /* req->notify_pd = NULL; */
       /* executeSwapRequest(req); */
       /* test_assert(req->errcode == 0); */
       /* finishSwapRequest(req); */
       /* test_assert(req->errcode == 0); */
       /* test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL); */
       /* test_assert(!wholeKeyRocksDataExists(db,key1)); */
       /* test_assert(!wholeKeyRocksMetaExists(db,key1)); */
   /* } */

   /* TEST("exec: swap-in.del") { */
       /* incrRefCount(val1); */
       /* dbAdd(db,key1,val1); */

       /* [> swap out hot key1 <] */
       /* swapData *out_data = createWholeKeySwapData(db,key1,val1,NULL); */
       /* swapRequest *out_req = swapRequestNew(NULL[>!cold<],SWAP_OUT,0,ctx,out_data,NULL,NULL,NULL,NULL,NULL); */
       /* out_req->notify_cb = mockNotifyCallback; */
       /* out_req->notify_pd = NULL; */
       /* processSwapRequest(out_req); */
       /* test_assert(out_req->errcode == 0); */
       /* finishSwapRequest(out_req); */
       /* test_assert(out_req->errcode == 0); */
       /* test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL); */
       /* test_assert(wholeKeyRocksMetaExists(db,key1)); */
       /* test_assert(wholeKeyRocksDataExists(db,key1)); */

       /* [> In.del cold key1 <] */
       /* swapData *in_del_data = createSwapData(db,key1,NULL); */
       /* key1_req->cmd_intention = SWAP_IN; */
       /* key1_req->cmd_intention_flags = SWAP_IN_DEL; */
       /* swapRequest *in_del_req = swapRequestNew(key1_req,-1,-1,ctx,in_del_data,NULL,NULL,NULL,NULL,NULL); */
       /* in_del_req->notify_cb = mockNotifyCallback; */
       /* in_del_req->notify_pd = NULL; */
       /* processSwapRequest(in_del_req); */
       /* test_assert(in_del_req->errcode == 0); */
       /* finishSwapRequest(in_del_req); */
       /* test_assert(in_del_req->errcode == 0); */
       /* test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) != NULL); */
       /* test_assert(!wholeKeyRocksMetaExists(db,key1)); */
       /* test_assert(!wholeKeyRocksDataExists(db,key1)); */
   /* } */

   /* return error; */
/* } */

#endif

