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

void swapRequestUpdateStatsSwapInNotFound(swapRequest *req) {
    /* key confirmed not exists, no need to execute swap request. */
    serverAssert(!swapDataAlreadySetup(req->data));
    if (isSwapHitStatKeyRequest(req->key_request)) {
        atomicIncr(server.swap_hit_stats->stat_swapin_not_found_cachemiss_count,1);
    }
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

/* Called by async-complete-queue or parallel-sync in server thread
 * to swap in/out/del data */
void swapRequestMerge(swapRequest *req) {
    DEBUG_MSGS_APPEND(req->msgs,"exec-finish","intention=%s",
            swapIntentionName(req->intention));
    int retval = 0, del_skip = 0, swap_out_completely = 0;
    swapData *data = req->data;
    void *datactx = req->datactx;

    serverAssert(!swapRequestGetError(req));

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
    case SWAP_UTILS:
        retval = 0;
        break;
    default:
        serverPanic("merge: unexpected request intention");
        retval = SWAP_ERR_DATA_FIN_FAIL;
        break;
    }

    if (retval) swapRequestSetError(req, retval);
}

static
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

/* Note that, to keep rio count align with req, an empty rio will be append
 * to rios. */
static
void swapExecBatchPrepareRIOBatch(swapExecBatch *exec_batch, RIOBatch *rios) {
    for (size_t i = 0; i < exec_batch->count; i++) {
        int errcode, numkeys, *cfs = NULL;
        sds *rawkeys = NULL, *rawvals = NULL;
        swapRequest *req = NULL;
        RIO *rio = NULL;
        int cf, limit;
        uint32_t flags = 0;
        sds start = NULL, end = NULL;

        req = exec_batch->reqs[i];
        rio = RIOBatchAlloc(rios);

        switch (exec_batch->action) {
        case ROCKS_GET:
            if ((errcode = swapDataEncodeKeys(req->data,req->intention,
                            req->datactx,&numkeys,&cfs,&rawkeys))) {
                swapRequestSetError(req,errcode);
                serverAssert(numkeys == 0);
            }
            RIOInitGet(rio,numkeys,cfs,rawkeys);
            break;
        case ROCKS_ITERATE:
            if ((errcode = swapDataEncodeRange(req->data,req->intention,
                            req->datactx,&limit,&flags,&cf,&start,&end))) {
                swapRequestSetError(req,errcode);
                serverAssert(start == NULL);
            }
            RIOInitIterate(rio,cf,flags,start,end,limit);
            break;
        case ROCKS_PUT:
            if ((errcode = swapDataEncodeData(req->data,req->intention,
                            req->datactx,&numkeys,&cfs,&rawkeys,&rawvals))) {
                swapRequestSetError(req,errcode);
            }
            RIOInitPut(rio,numkeys,cfs,rawkeys,rawvals);
            break;
        case ROCKS_DEL:
            if ((errcode = swapDataEncodeKeys(req->data,req->intention,
                            req->datactx,&numkeys,&cfs,&rawkeys))) {
                swapRequestSetError(req,errcode);
            }
            RIOInitDel(rio,numkeys,cfs,rawkeys);
            break;
        default:
            serverPanic("exec: unexepcted action when prepare");
            swapRequestSetError(req,SWAP_ERR_EXEC_UNEXPECTED_ACTION);
            break;
        }
    }
}

static
void swapExecBatchDoRIOBatch(swapExecBatch *exec_batch, RIOBatch *rios) {
    RIOBatchDo(rios);
    for (size_t i = 0; i < exec_batch->count; i++) {
        int errcode = 0;
        swapRequest *req = exec_batch->reqs[i];
        RIO *rio = rios->rios+i;
        if (!swapRequestGetError(req) && ((errcode = RIOGetError(rio)))) {
            swapRequestSetError(req,errcode);
        }
    }
}

static void swapRequestInIntentionDelEncodeKeys(swapRequest *req, RIO *rio,
        int merged_is_hot, int *pnumkeys, int **pcfs, sds **prawkeys) {
    int data_numkeys = 0, numkeys = 0;
    sds meta_rawkey = NULL, *data_rawkeys = NULL, *rawkeys = NULL;
    int *data_cfs = NULL, *tmpcfs = NULL, *cfs = NULL;

    serverAssert(rio->action == ROCKS_GET || rio->action == ROCKS_ITERATE);

    /* There is no need to delete subkey if meta gets deleted,
     * subkeys will be deleted by compaction filter (except for
     * string type, which is not deleted by compaction filter). */

    if (merged_is_hot) {
        meta_rawkey = swapDataEncodeMetaKey(req->data);
    }

    if (!merged_is_hot || req->data->object_type == OBJ_STRING) {
        int *rio_cfs = NULL, rio_numkeys = 0;
        sds *rio_rawkeys = NULL, *rio_rawvals = NULL;

        if (rio->action == ROCKS_GET) {
            rio_numkeys = rio->get.numkeys;
            rio_cfs = rio->get.cfs;
            rio_rawkeys = rio->get.rawkeys;
            rio_rawvals = rio->get.rawvals;
        } else { /* ROCKS_ITERATE */
            tmpcfs = zmalloc(sizeof(int)*rio->iterate.numkeys);
            for (int i = 0; i < rio->iterate.numkeys; i++)
                tmpcfs[i] = rio->iterate.cf;
            rio_numkeys = rio->iterate.numkeys;
            rio_cfs = tmpcfs;
            rio_rawkeys = rio->iterate.rawkeys;
            rio_rawvals = rio->iterate.rawvals;
        }

        if (req->data->type->rocksDel) {
            int data_action;
            req->data->type->rocksDel(req->data,req->datactx,
                    rio->action,rio_numkeys,rio_cfs,rio_rawkeys,rio_rawvals,
                    &data_action,&data_numkeys,&data_cfs,&data_rawkeys);
        } else {
            data_numkeys = rio_numkeys;
            data_cfs = zmalloc((rio_numkeys)*sizeof(int));
            data_rawkeys = zmalloc((rio_numkeys)*sizeof(sds));
            for (int i = 0; i < rio_numkeys; i++) {
                data_cfs[i] = rio_cfs[i];
                data_rawkeys[i] = sdsdup(rio_rawkeys[i]);
            }
        }
    }

    if (meta_rawkey && data_rawkeys) {
        /* string */
        numkeys = data_numkeys+1;

        cfs = zmalloc(numkeys*sizeof(int));
        memcpy(cfs,data_cfs,data_numkeys*sizeof(int));
        cfs[data_numkeys] = META_CF;
        zfree(data_cfs);
        data_cfs = NULL;

        rawkeys = zmalloc(numkeys*sizeof(sds));
        memcpy(rawkeys,data_rawkeys,data_numkeys*sizeof(sds));
        rawkeys[data_numkeys] = meta_rawkey;
        zfree(data_rawkeys);
        data_rawkeys = NULL;
        meta_rawkey = NULL;
    } else if (data_rawkeys) {
        /* hash/set/zset/list merged not hot. */
        numkeys = data_numkeys;
        cfs = data_cfs;
        rawkeys = data_rawkeys;
    } else if (meta_rawkey) {
        /* hash/set/zset/list merged is hot.*/
        numkeys = 1;
        cfs = zmalloc(sizeof(int));
        cfs[0] = META_CF;
        rawkeys = zmalloc(sizeof(sds));
        rawkeys[0] = meta_rawkey;
        meta_rawkey = NULL;
    } else {
        numkeys = 0;
        cfs = NULL;
        rawkeys = NULL;
    }

    *pnumkeys = numkeys;
    *pcfs = cfs;
    *prawkeys = rawkeys;

    if (tmpcfs) {
        zfree(tmpcfs);
        tmpcfs = NULL;
    }
}

static void swapExecBatchExecuteIntentionDel(swapExecBatch *exec_batch,
        RIOBatch *rios) {
    int errcode, *merged_is_hots = NULL;
    RIOBatch _aux_rios = {0}, *aux_rios = &_aux_rios;
    RIO *aux_rio;

    merged_is_hots = zcalloc(sizeof(int)*exec_batch->count);
    RIOBatchInit(aux_rios,ROCKS_DEL);

    for (size_t i = 0; i < exec_batch->count; i++) {
        int is_hot, aux_numkeys, *aux_cfs;
        sds *aux_rawkeys;
        RIO *rio = rios->rios+i;
        swapRequest *req = exec_batch->reqs[i];
        if (!(req->intention_flags & SWAP_EXEC_IN_DEL)) continue;
        if (swapRequestGetError(req)) continue;

        is_hot = swapDataMergedIsHot(req->data,req->result,req->datactx);
        merged_is_hots[i] = is_hot;

        aux_rio = RIOBatchAlloc(aux_rios);
        swapRequestInIntentionDelEncodeKeys(req,rio,is_hot,
                &aux_numkeys,&aux_cfs,&aux_rawkeys);
        RIOInitDel(aux_rio,aux_numkeys,aux_cfs,aux_rawkeys);
    }

    RIOBatchDo(aux_rios);

    size_t aux_idx = 0;
    for (size_t i = 0; i < exec_batch->count; i++) {
        swapRequest *req = exec_batch->reqs[i];
        if (!(req->intention_flags & SWAP_EXEC_IN_DEL)) continue;
        if (swapRequestGetError(req)) continue;

        aux_rio = aux_rios->rios + aux_idx++;
        if ((errcode = RIOGetError(aux_rio))) {
            swapRequestSetError(req, errcode);
            continue;
        }

        if (merged_is_hots[i]) {
            req->data->del_meta = 1;
            req->data->persistence_deleted = 1;
        }
    }

    RIOBatchDeinit(aux_rios);
    if (merged_is_hots) {
        zfree(merged_is_hots);
        merged_is_hots = NULL;
    }
}

void swapExecBatchExecuteIn(swapExecBatch *exec_batch) {
    RIOBatch _rios = {0}, *rios = &_rios;
    int errcode, action = exec_batch->action;
    void *decoded;

    serverAssert(action == ROCKS_GET || action == ROCKS_ITERATE);

    RIOBatchInit(rios,action);
    swapExecBatchPrepareRIOBatch(exec_batch,rios);
    swapExecBatchDoRIOBatch(exec_batch,rios);

    for (size_t i = 0; i < exec_batch->count; i++) {
        swapRequest *req = exec_batch->reqs[i];
        RIO *rio = rios->rios+i;
        if (swapRequestGetError(req)) continue;

        if (action == ROCKS_GET) {
            if ((errcode = swapDataDecodeData(req->data,rio->get.numkeys,
                            rio->get.cfs,rio->get.rawkeys,rio->get.rawvals,
                            &decoded))) {
                swapRequestSetError(req,errcode);
                continue;
            }
        } else { /* ROCKS_ITERATE */
            int *tmpcfs = zmalloc(sizeof(int)*rio->iterate.numkeys);

            for (int i = 0; i < rio->iterate.numkeys; i++)
                tmpcfs[i] = rio->iterate.cf;

            if (rio->iterate.nextseek) {
                req->data->nextseek = rio->iterate.nextseek;
                rio->iterate.nextseek = NULL;
            }

            if ((errcode = swapDataDecodeData(req->data,rio->iterate.numkeys,
                            tmpcfs,rio->iterate.rawkeys,rio->iterate.rawvals,
                            &decoded))) {
                swapRequestSetError(req,errcode);
                zfree(tmpcfs);
                continue;
            }
            zfree(tmpcfs);
        }

        req->result = swapDataCreateOrMergeObject(req->data,decoded,req->datactx);
    }

    swapExecBatchExecuteIntentionDel(exec_batch,rios);
    swapExecBatchUpdateStatsRIOBatch(exec_batch,rios);
    RIOBatchDeinit(rios);
}

static void swapExecBatchExecuteDoOutMeta(swapExecBatch *exec_batch) {
    int errcode, *meta_cfs = NULL, num_metas = 0;
    RIO _meta_rio = {0}, *meta_rio = &_meta_rio;
    sds *meta_rawkeys = NULL, *meta_rawvals = NULL;
    size_t count = exec_batch->count;

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
}

void swapExecBatchExecuteOut(swapExecBatch *exec_batch) {
    RIOBatch _rios = {0}, *rios = &_rios;
    serverAssert(exec_batch->action == ROCKS_PUT);
    RIOBatchInit(rios,ROCKS_PUT);
    swapExecBatchPrepareRIOBatch(exec_batch,rios);
    swapExecBatchDoRIOBatch(exec_batch, rios);
    for (size_t i = 0; i < exec_batch->count; i++) {
        int errcode = 0;
        swapRequest *req = exec_batch->reqs[i];
        if (swapRequestGetError(req)) continue;
        if ((errcode = swapDataCleanObject(req->data,req->datactx))) {
            swapRequestSetError(req,errcode);
            continue;
        }
    }
    swapExecBatchExecuteDoOutMeta(exec_batch);
    swapExecBatchUpdateStatsRIOBatch(exec_batch,rios);
    RIOBatchDeinit(rios);
}

static void swapExecBatchExecuteDoDelMeta(swapExecBatch *exec_batch) {
    int errcode, *meta_cfs, num_metas = 0;
    sds *meta_rawkeys = NULL;
    RIO _meta_rio = {0}, *meta_rio = &_meta_rio;
    size_t count = exec_batch->count;

    meta_cfs = zmalloc(sizeof(int)*count);
    meta_rawkeys = zmalloc(sizeof(sds)*count);
    for (size_t i = 0; i < count; i++) {
        swapRequest *req = exec_batch->reqs[i];
        meta_cfs[num_metas] = META_CF;
        meta_rawkeys[num_metas] = swapDataEncodeMetaKey(req->data);
        num_metas++;
    }

    RIOInitDel(meta_rio,num_metas,meta_cfs,meta_rawkeys);
    RIODo(meta_rio);
    if ((errcode = RIOGetError(meta_rio))) {
        swapExecBatchSetError(exec_batch,errcode);
    }
    RIODeinit(meta_rio);
}

void swapExecBatchExecuteDel(swapExecBatch *exec_batch) {
    RIOBatch _rios, *rios = &_rios;
    serverAssert(exec_batch->action == ROCKS_DEL);
    RIOBatchInit(rios,ROCKS_DEL);
    swapExecBatchPrepareRIOBatch(exec_batch,rios);
    swapExecBatchDoRIOBatch(exec_batch, rios);
    swapExecBatchExecuteDoDelMeta(exec_batch);
    swapExecBatchUpdateStatsRIOBatch(exec_batch,rios);
    RIOBatchDeinit(rios);
}

void swapExecBatchExecuteUtils(swapExecBatch *exec_batch) {
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
    size_t swap_memory = 0;
    int intention = exec_batch->intention;
    const long duration = elapsedUs(exec_batch->swap_timer);

    for (size_t i = 0; i < exec_batch->count; i++) {
        swapRequest *req = exec_batch->reqs[i];
        swap_memory += req->swap_memory;
    }

    atomicIncr(server.ror_stats->swap_stats[intention].batch,1);
    atomicIncr(server.ror_stats->swap_stats[intention].count,exec_batch->count);
    atomicIncr(server.ror_stats->swap_stats[intention].time,duration);
    atomicIncr(server.ror_stats->swap_stats[intention].memory,swap_memory);
}

void swapExecBatchExecute(swapExecBatch *exec_batch) {
    serverAssert(exec_batch->intention != SWAP_NOP);
    serverAssert(!swapExecBatchGetError(exec_batch));

    swapExecBatchExecuteStart(exec_batch);

    switch (exec_batch->intention) {
    case SWAP_IN:
        swapExecBatchExecuteIn(exec_batch);
        break;
    case SWAP_OUT:
        swapExecBatchExecuteOut(exec_batch);
        if (server.swap_debug_swapout_notify_delay_micro)
            usleep(server.swap_debug_swapout_notify_delay_micro);
        break;
    case SWAP_DEL:
        swapExecBatchExecuteDel(exec_batch);
        break;
    case SWAP_UTILS:
        swapExecBatchExecuteUtils(exec_batch);
        break;
    default:
        swapExecBatchSetError(exec_batch,SWAP_ERR_EXEC_FAIL);
        serverLog(LL_WARNING,
                "unexpected execute batch intention(%d) action(%d)\n",
                exec_batch->intention, exec_batch->action);
        break;
    }

    swapExecBatchExecuteEnd(exec_batch);
}

void swapExecBatchPreprocess(swapExecBatch *meta_batch) {
    swapRequest *req;
    int errcode, intention;
    uint32_t intention_flags;
    RIO _rio = {0}, *rio = &_rio;
    int *cfs = zmalloc(meta_batch->count*sizeof(int));
    sds *rawkeys = zmalloc(meta_batch->count*sizeof(sds));

    for (size_t i = 0; i < meta_batch->count; i++) {
        req = meta_batch->reqs[i];
        cfs[i] = META_CF;
        rawkeys[i] = swapDataEncodeMetaKey(req->data);
    }

    RIOInitGet(rio,meta_batch->count,cfs,rawkeys);
    RIODo(rio);
    if ((errcode = RIOGetError(rio))) {
        swapExecBatchSetError(meta_batch,errcode);
        goto end;
    }

    for (size_t i = 0; i < meta_batch->count; i++) {
        req = meta_batch->reqs[i];
        sds rawval = rio->get.rawvals[i];
        if (rawval == NULL) {
            /* No swap needed if meta not found. */
            swapRequestSetIntention(req,SWAP_NOP,0);
            swapRequestUpdateStatsSwapInNotFound(req);
            continue;
        }

        if ((errcode = swapDataDecodeAndSetupMeta(req->data,rawval,
                        &req->datactx))) {
            swapRequestSetError(req,errcode);
            continue;
        }

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
void mockNotifyCallback(swapRequestBatch *reqs, void *pd) {
    UNUSED(reqs),UNUSED(pd);
}

int wholeKeyRocksDataExists(redisDb *db, robj *key) {
    size_t vlen;
    char *err = NULL, *rawval = NULL;
    sds rawkey = rocksEncodeDataKey(db,key->ptr,0,NULL);
    rawval = rocksdb_get_cf(server.rocks->db, server.rocks->ropts,swapGetCF(DATA_CF),rawkey,sdslen(rawkey),&vlen,&err);
    serverAssert(err == NULL);
    zlibc_free(rawval);
    sdsfree(rawkey);
    return rawval != NULL;
}

int wholeKeyRocksMetaExists(redisDb *db, robj *key) {
    size_t vlen;
    char *err = NULL, *rawval = NULL;
    sds rawkey = rocksEncodeMetaKey(db,key->ptr);
    rawval = rocksdb_get_cf(server.rocks->db, server.rocks->ropts,swapGetCF(META_CF),rawkey,sdslen(rawkey),&vlen,&err);
    serverAssert(err == NULL);
    zlibc_free(rawval);
    sdsfree(rawkey);
    return rawval != NULL;
}

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

int swapExecTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    server.hz = 10;
    robj *key1 = createStringObject("key1",4);
    robj *val1 = createStringObject("val1",4), *val;
    initTestRedisDb();
    monotonicInit();
    redisDb *db = server.db;
    long long EXPIRE = 3000000000LL * 1000;

    keyRequest key1_req_, *key1_req = &key1_req_;
    key1_req->level = REQUEST_LEVEL_KEY;
    key1_req->b.num_subkeys = 0;
    key1_req->key = createStringObject("key1",4);
    key1_req->b.subkeys = NULL;
    swapCtx *ctx = swapCtxCreate(NULL,key1_req,NULL,NULL);

    TEST("exec: init") {
        initServerConfig4Test();
        incrRefCount(val1);
        dbAdd(db,key1,val1);
        setExpire(NULL,db,key1,EXPIRE);
        if (!server.rocks) rocksInit();
        initStatsSwap();
    }

    TEST("exec: swap-out hot string") {
        val = lookupKey(db,key1,LOOKUP_NOTOUCH);
        test_assert(val != NULL);
        test_assert(getExpire(db,key1) == EXPIRE);
        swapData *data = createWholeKeySwapDataWithExpire(db,key1,val,EXPIRE,NULL);
        swapRequest *req = swapRequestNew(NULL/*!cold*/,SWAP_OUT,0,ctx,data,NULL,NULL,NULL,NULL,NULL);
        swapRequestBatch *reqs = swapRequestBatchNew();
        reqs->notify_cb = mockNotifyCallback;
        reqs->notify_pd = NULL;
        swapRequestBatchAppend(reqs,req);
        swapRequestBatchProcess(reqs);
        serverAssert(swapRequestGetError(req) == 0);
        swapRequestMerge(req);
        test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
        test_assert(getExpire(db,key1) == -1);
        test_assert(wholeKeyRocksDataExists(db,key1));
        test_assert(wholeKeyRocksMetaExists(db,key1));
        swapRequestBatchFree(reqs);
        swapDataFree(data,NULL);
    }

    TEST("exec: swap-in cold string") {
        /* rely on val1 swap out to rocksdb by previous case */
        swapData *data = createSwapData(db,key1,NULL);
        key1_req->cmd_intention = SWAP_IN;
        key1_req->cmd_intention_flags = 0;
        swapRequest *req = swapRequestNew(key1_req,-1,-1,ctx,data,NULL,NULL,NULL,NULL,NULL);
        swapRequestBatch *reqs = swapRequestBatchNew();
        reqs->notify_cb = mockNotifyCallback;
        reqs->notify_pd = NULL;
        swapRequestBatchAppend(reqs,req);
        swapRequestBatchProcess(reqs);
        test_assert(swapRequestGetError(req) == 0);
        swapRequestMerge(req);
        test_assert((val = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL && !objectIsDirty(val));
        test_assert(sdscmp(val->ptr, val1->ptr) == 0);
        test_assert(getExpire(db,key1) == EXPIRE);
        test_assert(wholeKeyRocksDataExists(db,key1));
        test_assert(wholeKeyRocksMetaExists(db,key1));
        swapRequestBatchFree(reqs);
        swapDataFree(data,NULL);
    }

    TEST("exec: swap-del hot string") {
        /* rely on val1 swapped in by previous case */
        val = lookupKey(db,key1,LOOKUP_NOTOUCH);
        swapData *data = createWholeKeySwapData(db,key1,val,NULL);
        swapRequest *req = swapRequestNew(NULL/*!cold*/,SWAP_DEL,0,ctx,data,NULL,NULL,NULL,NULL,NULL);
        swapRequestBatch *reqs = swapRequestBatchNew();
        reqs->notify_cb = mockNotifyCallback;
        reqs->notify_pd = NULL;
        swapRequestBatchAppend(reqs,req);
        swapRequestBatchProcess(reqs);
        swapRequestMerge(req);
        test_assert(swapRequestGetError(req) == 0);
        test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
        test_assert(!wholeKeyRocksDataExists(db,key1));
        test_assert(!wholeKeyRocksMetaExists(db,key1));
        swapRequestBatchFree(reqs);
        swapDataFree(data,NULL);
    }

    TEST("exec: swap-in.del") {
        incrRefCount(val1);
        dbAdd(db,key1,val1);

        /* swap out hot key1 */
        swapData *out_data = createWholeKeySwapData(db,key1,val1,NULL);
        swapRequest *out_req = swapRequestNew(NULL/*!cold*/,SWAP_OUT,0,ctx,out_data,NULL,NULL,NULL,NULL,NULL);
        swapRequestBatch *reqs = swapRequestBatchNew();
        reqs->notify_cb = mockNotifyCallback;
        reqs->notify_pd = NULL;
        swapRequestBatchAppend(reqs, out_req);
        swapRequestBatchProcess(reqs);
        swapRequestMerge(out_req);
        test_assert(swapRequestGetError(out_req) == 0);
        test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
        test_assert(wholeKeyRocksMetaExists(db,key1));
        test_assert(wholeKeyRocksDataExists(db,key1));
        swapRequestBatchFree(reqs);
        swapDataFree(out_data,NULL);

        /* In.del cold key1 */
        swapData *in_del_data = createSwapData(db,key1,NULL);
        key1_req->cmd_intention = SWAP_IN;
        key1_req->cmd_intention_flags = SWAP_IN_DEL;
        swapRequest *in_del_req = swapRequestNew(key1_req,-1,-1,ctx,in_del_data,NULL,NULL,NULL,NULL,NULL);
        swapRequestBatch *reqs2 = swapRequestBatchNew();
        reqs2->notify_cb = mockNotifyCallback;
        reqs2->notify_pd = NULL;
        swapRequestBatchAppend(reqs2, in_del_req);
        swapRequestBatchProcess(reqs2);
        test_assert(swapRequestGetError(in_del_req) == 0);
        swapRequestMerge(in_del_req);
        test_assert(swapRequestGetError(in_del_req) == 0);
        test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) != NULL);
        test_assert(!wholeKeyRocksMetaExists(db,key1));
        test_assert(!wholeKeyRocksDataExists(db,key1));

        swapRequestBatchFree(reqs2);
        swapDataFree(in_del_data,NULL);
    }

    swapCtxSetSwapData(ctx,NULL,NULL);
    swapCtxFree(ctx);
    decrRefCount(key1);
    decrRefCount(val1);

    return error;
}

#endif

