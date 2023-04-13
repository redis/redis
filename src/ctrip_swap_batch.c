/* Copyright (c) 2023, ctrip.com * All rights reserved.
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

#define SWAP_REQUEST_MEMORY_OVERHEAD (sizeof(swapRequest)+sizeof(swapCtx)+ \
                                      sizeof(wholeKeySwapData)/*typical*/+ \
                                      sizeof(lock)+sizeof(swapRequestBatch)/SWAP_BATCH_DEFAULT_SIZE)


/* swapExecBatch: batch of requests with the same SWAP intention and action. */
void swapExecBatchInit(swapExecBatch *exec_batch) {
    exec_batch->reqs = exec_batch->req_buf;
    exec_batch->capacity = SWAP_BATCH_DEFAULT_SIZE;
    exec_batch->count = 0;
    exec_batch->intention = SWAP_UNSET;
    exec_batch->action = ROCKS_UNSET;
}

void swapExecBatchDeinit(swapExecBatch *exec_batch) {
    if (exec_batch == NULL) return;
    if (exec_batch->reqs != exec_batch->req_buf) {
        zfree(exec_batch->reqs);
        exec_batch->reqs = NULL;
    }
}

void swapExecBatchAppend(swapExecBatch *exec_batch, swapRequest *req) {
    if (exec_batch->count == exec_batch->capacity) {
        exec_batch->capacity = exec_batch->capacity < SWAP_BATCH_LINEAR_SIZE ? exec_batch->capacity*2 : exec_batch->capacity + SWAP_BATCH_LINEAR_SIZE;
        serverAssert(exec_batch->capacity > exec_batch->count);
        if (exec_batch->reqs == exec_batch->req_buf) {
            exec_batch->reqs = zmalloc(sizeof(swapRequest*)*exec_batch->capacity);
            memcpy(exec_batch->reqs,exec_batch->req_buf,sizeof(swapRequest*)*exec_batch->count);
        } else {
            exec_batch->reqs = zrealloc(exec_batch->reqs,sizeof(swapRequest*)*exec_batch->capacity);
        }
    }
    exec_batch->reqs[exec_batch->count++] = req;
}

static inline void swapExecBatchCtxExecuteIfNeeded(swapExecBatch *exec_ctx) {
    if (!swapExecBatchCtxEmpty(exec_ctx)) {
        /* exec batch and ctx are identical */
        swapExecBatch *exec_batch = exec_ctx;
        swapExecBatchExecute(exec_batch);
    }
}

void swapExecBatchCtxStart(swapExecBatch *exec_ctx) {
    UNUSED(exec_ctx);
}

static inline int swapIntentionInOutDel(int intention) {
    return intention == SWAP_IN || intention == SWAP_OUT ||
            intention == SWAP_DEL;
}

void swapExecBatchCtxFeed(swapExecBatch *exec_ctx, swapRequest *req) {
    int req_action;
    serverAssert(req->intention != SWAP_UNSET);

    if (swapIntentionInOutDel(req->intention)) {
        swapDataSwapAnaAction(req->data,req->intention,req->datactx,
                &req_action);
    } else {
        req_action = ROCKS_NOP;
    }

    /* execute before append req if intention or action switched */
    if ((req->intention != exec_ctx->intention ||
            req_action != exec_ctx->action)) {
        swapExecBatchCtxExecuteIfNeeded(exec_ctx);
        swapExecBatchCtxReset(exec_ctx,req->intention,req_action);
    }

    swapExecBatchAppend(exec_ctx,req);

    if (!swapIntentionInOutDel(req->intention)) {
        swapExecBatchCtxExecuteIfNeeded(exec_ctx);
        swapExecBatchCtxReset(exec_ctx,SWAP_UNSET,ROCKS_UNSET);
    }
}

void swapExecBatchCtxEnd(swapExecBatch *exec_batch) {
    swapExecBatchCtxExecuteIfNeeded(exec_batch);
    swapExecBatchCtxReset(exec_batch,SWAP_UNSET,ROCKS_UNSET);
}

/* swapRequestBatch: batch of requests that submitted together, those requests
 * does not depend on each other to proceed or unlock.
 * Although these requests have same cmd intentions, their swap intention
 * might differ because swapAna might result in different intention. */
swapRequestBatch *swapRequestBatchNew() {
    swapRequestBatch *reqs = zmalloc(sizeof(swapRequestBatch));
    reqs->reqs = reqs->req_buf;
    reqs->capacity = SWAP_BATCH_DEFAULT_SIZE;
    reqs->count = 0;
    reqs->swap_queue_timer = 0;
    reqs->notify_queue_timer = 0;
    return reqs;
}

void swapRequestBatchFree(swapRequestBatch *reqs) {
    if (reqs == NULL) return;
    for (size_t i = 0; i < reqs->count; i++) {
        swapRequestFree(reqs->reqs[i]);
        reqs->reqs[i] = NULL;
    }
    if (reqs->reqs != reqs->req_buf) {
        zfree(reqs->reqs);
        reqs->reqs = NULL;
    }
    zfree(reqs);
}

static inline int swapRequestBatchEmpty(swapRequestBatch *reqs) {
    return reqs->count == 0;
}

void swapRequestBatchAppend(swapRequestBatch *reqs, swapRequest *req) {
    if (reqs->count == reqs->capacity) {
        reqs->capacity = reqs->capacity < SWAP_BATCH_LINEAR_SIZE ? reqs->capacity*2 : reqs->capacity + SWAP_BATCH_LINEAR_SIZE;
        serverAssert(reqs->capacity > reqs->count);
        if (reqs->reqs == reqs->req_buf) {
            reqs->reqs = zmalloc(sizeof(swapRequest*)*reqs->capacity);
            memcpy(reqs->reqs,reqs->req_buf,reqs->count*sizeof(swapRequest*));
        } else {
            reqs->reqs = zrealloc(reqs->reqs,sizeof(swapRequest*)*reqs->capacity);
        }
    }
    reqs->reqs[reqs->count++] = req;
}

void swapRequestBatchCallback(swapRequestBatch *reqs) {
    size_t swap_memory = 0;

    if (reqs->notify_queue_timer) {
        metricDebugInfo(SWAP_DEBUG_NOTIFY_QUEUE_WAIT, elapsedUs(reqs->notify_queue_timer));
    }

    for (size_t i = 0; i < reqs->count; i++) {
        swapRequest *req = reqs->reqs[i];
        swap_memory += req->swap_memory;

        if (!swapRequestGetError(req))
            swapRequestMerge(req);

        if (req->trace) swapTraceCallback(req->trace);
        req->finish_cb(req->data,req->finish_pd,swapRequestGetError(req));
    }

    atomicDecr(server.swap_inprogress_batch,1);
    atomicDecr(server.swap_inprogress_count,reqs->count);
    atomicDecr(server.swap_inprogress_memory,swap_memory);
}

void swapRequestBatchDispatched(swapRequestBatch *reqs) {
    size_t swap_memory = 0;

    if (server.swap_debug_trace_latency) elapsedStart(&reqs->swap_queue_timer);

    for (size_t i = 0; i < reqs->count; i++) {
        swapRequest *req = reqs->reqs[i];
        if (req->trace) swapTraceDispatch(req->trace);
        req->swap_memory += SWAP_REQUEST_MEMORY_OVERHEAD;
        swap_memory += req->swap_memory;
    }

    atomicIncr(server.swap_inprogress_batch,1);
    atomicIncr(server.swap_inprogress_count,reqs->count);
    atomicIncr(server.swap_inprogress_memory,swap_memory);
}

void swapRequestBatchProcessStart(swapRequestBatch *reqs) {
    if (reqs->swap_queue_timer) {
        metricDebugInfo(SWAP_DEBUG_SWAP_QUEUE_WAIT,elapsedUs(reqs->swap_queue_timer));
    }
    for (size_t i = 0; i < reqs->count; i++) {
        swapRequest *req = reqs->reqs[i];
        if (req->trace) swapTraceProcess(req->trace);
    }
}

void swapRequestBatchProcessEnd(swapRequestBatch *reqs) {
    if (server.swap_debug_trace_latency) elapsedStart(&reqs->notify_queue_timer);
    for (size_t i = 0; i < reqs->count; i++) {
        swapRequest *req = reqs->reqs[i];
        if (req->trace) swapTraceNotify(req->trace,req->intention);
    }
    reqs->notify_cb(reqs, reqs->notify_pd);
}

void swapRequestBatchExecute(swapRequestBatch *reqs) {
    swapExecBatch exec_ctx;

    swapExecBatchCtxInit(&exec_ctx);
    swapExecBatchCtxStart(&exec_ctx);
    for (size_t i = 0; i < reqs->count; i++) {
        swapRequest *req = reqs->reqs[i];
        if (!swapRequestGetError(req) && req->intention != SWAP_NOP) {
            swapExecBatchCtxFeed(&exec_ctx,req);
        }
    }
    swapExecBatchCtxEnd(&exec_ctx);
    swapExecBatchCtxDeinit(&exec_ctx);
}

void swapRequestBatchPreprocess(swapRequestBatch *reqs) {
    swapExecBatch meta_batch;

    swapExecBatchInit(&meta_batch);

    for (size_t i = 0; i < reqs->count; i++) {
        swapRequest *req = reqs->reqs[i];
        if (swapRequestIsMetaType(req)) {
            swapExecBatchAppend(&meta_batch,req);
        }
    }

    if (!swapExecBatchEmpty(&meta_batch)) {
        swapExecBatchPreprocess(&meta_batch);
    }

    swapExecBatchDeinit(&meta_batch);
}

void swapRequestBatchProcess(swapRequestBatch *reqs) {
    swapRequestBatchProcessStart(reqs);
    swapRequestBatchPreprocess(reqs);
    swapRequestBatchExecute(reqs);
    swapRequestBatchProcessEnd(reqs);
}

/* swapBatchCtx: currently acummulated requests about to submit in batch. */
static void swapBatchCtxStatInit(swapBatchCtxStat *batch_stat) {
    batch_stat->submit_batch_count = 0;
    batch_stat->submit_request_count = 0;
    for (int i = 0 ; i < SWAP_BATCH_FLUSH_TYPES; i++) {
        batch_stat->submit_batch_flush[i] = 0;
    }
}

void trackSwapBatchInstantaneousMetrics() {
    swapBatchCtxStat *batch_stat = &server.swap_batch_ctx->stat;
    trackInstantaneousMetric(batch_stat->stats_metric_idx_request,
            batch_stat->submit_request_count);
    trackInstantaneousMetric(batch_stat->stats_metric_idx_batch,
            batch_stat->submit_batch_count);
    batch_stat->stats_metric_idx_request =
        SWAP_BATCH_STATS_METRIC_OFFSET+SWAP_BATCH_STATS_METRIC_SUBMIT_REQUEST;
    batch_stat->stats_metric_idx_batch =
        SWAP_BATCH_STATS_METRIC_OFFSET+SWAP_BATCH_STATS_METRIC_SUBMIT_BATCH;
}

void resetSwapBatchInstantaneousMetrics(void) {
    swapBatchCtxStat *batch_stat = &server.swap_batch_ctx->stat;
    batch_stat->submit_request_count = 0;
    batch_stat->submit_batch_count = 0;
    for (int i = 0; i < SWAP_BATCH_FLUSH_TYPES; i++) {
        batch_stat->submit_batch_flush[i] = 0;
    }
}

sds genSwapBatchInfoString(sds info) {
    swapBatchCtxStat *batch_stat = &server.swap_batch_ctx->stat;
    long long type_count, request_count = batch_stat->submit_request_count,
           batch_count = batch_stat->submit_batch_count;
    long long batch_ps, request_ps;
    double request_pb;

    request_ps = getInstantaneousMetric(batch_stat->stats_metric_idx_request);
    batch_ps = getInstantaneousMetric(batch_stat->stats_metric_idx_batch);
    request_pb = batch_ps == 0 ? 0 : (double)request_ps/batch_ps;

    info = sdscatprintf(info,
            "swap_submit_request_count:%lld\r\n"
            "swap_submit_batch_count:%lld\r\n"
            "swap_submit_instantaneous_request_ps:%lld\r\n"
            "swap_submit_instantaneous_batch_ps:%lld\r\n"
            "swap_submit_instantaneous_request_pb:%.2f\r\n",
            request_count,batch_count,request_ps,batch_ps,request_pb);

    info = sdscatprintf(info,"swap_submit_batch_type:");
    for (int i = 0; i < SWAP_BATCH_FLUSH_TYPES; i++) {
        type_count = server.swap_batch_ctx->stat.submit_batch_flush[i];
        if (i == 0) {
            info = sdscatprintf(info,"%s=%lld",swapBatchFlushTypeName(i),type_count);
        } else {
            info = sdscatprintf(info,",%s=%lld",swapBatchFlushTypeName(i),type_count);
        }
    }
    info = sdscatprintf(info,"\r\n");

    return info;
}


swapBatchCtx *swapBatchCtxNew() {
    swapBatchCtx *batch_ctx = zmalloc(sizeof(swapBatchCtx));
    swapBatchCtxStatInit(&batch_ctx->stat);
    batch_ctx->batch = swapRequestBatchNew();
    batch_ctx->thread_idx = -1;
    batch_ctx->cmd_intention = SWAP_UNSET;
    return batch_ctx;
}

void swapBatchCtxFree(swapBatchCtx *batch_ctx) {
    if (batch_ctx == NULL) return;
    if (batch_ctx->batch != NULL) {
        swapRequestBatchFree(batch_ctx->batch);
        batch_ctx->batch = NULL;
    }
    zfree(batch_ctx);
}

static inline swapRequestBatch *swapBatchCtxShift(swapBatchCtx *batch_ctx) {
    serverAssert(batch_ctx->batch != NULL);
    swapRequestBatch *reqs = batch_ctx->batch;
    batch_ctx->batch = swapRequestBatchNew();
    return reqs;
}

size_t swapBatchCtxFlush(swapBatchCtx *batch_ctx, int reason) {
    if (swapRequestBatchEmpty(batch_ctx->batch)) return 0;
    int thread_idx = batch_ctx->thread_idx;
    swapRequestBatch *reqs = swapBatchCtxShift(batch_ctx);
    size_t reqs_count = reqs->count;
    batch_ctx->stat.submit_batch_count++;
    batch_ctx->stat.submit_request_count+=reqs_count;
    batch_ctx->stat.submit_batch_flush[reason]++;
    submitSwapRequestBatch(SWAP_MODE_ASYNC,reqs,thread_idx);
    return reqs_count;
}

static
inline int swapBatchCtxExceedsLimit(swapBatchCtx *batch_ctx) {
    int exceeded = 0;
    swapBatchLimitsConfig *limit;

    serverAssert(swapIntentionInOutDel(batch_ctx->cmd_intention));

    limit = server.swap_batch_limits+batch_ctx->cmd_intention;
    if (limit->count > 0 && batch_ctx->batch->count >= (size_t)limit->count) {
        exceeded = 1;
    }

    /* TODO account for mem as well. */
    return exceeded;
}

void swapBatchCtxFeed(swapBatchCtx *batch_ctx, int flush,
        swapRequest *req, int thread_idx) {
    int cmd_intention;

    if (req->intention == SWAP_UNSET) {
        cmd_intention = req->key_request->cmd_intention;
    } else {
        cmd_intention = req->intention;
    }

    /* flush before handling req if req is dispatched to another thread. */
    if (batch_ctx->thread_idx != thread_idx) {
        swapBatchCtxFlush(batch_ctx,SWAP_BATCH_FLUSH_THREAD_SWITCH);
    } else if (batch_ctx->cmd_intention != cmd_intention) {
        swapBatchCtxFlush(batch_ctx,SWAP_BATCH_FLUSH_INTENT_SWITCH);
    } else {
        /* no need to flush beforehand */
    }

    batch_ctx->thread_idx = thread_idx;
    batch_ctx->cmd_intention = cmd_intention;

    swapRequestBatchAppend(batch_ctx->batch,req);

    /* flush after handling req if flush hint set. */
    /* execute after append req if exceeded swap-batch-limit */
    if (flush) {
        swapBatchCtxFlush(batch_ctx,SWAP_BATCH_FLUSH_FORCE_FLUSH);
    } else if (!swapIntentionInOutDel(batch_ctx->cmd_intention)) {
        swapBatchCtxFlush(batch_ctx,SWAP_BATCH_FLUSH_UTILS_TYPE);
    } else if (swapBatchCtxExceedsLimit(batch_ctx)) {
        swapBatchCtxFlush(batch_ctx,SWAP_BATCH_FLUSH_REACH_LIMIT);
    } else {
        /* no need to flush afterwards */
    }
}

#ifdef REDIS_TEST

void mockNotifyCallback(swapRequestBatch *reqs, void *pd);
void initServerConfig(void);
int swapBatchTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    server.hz = 10;
    robj *key1 = createStringObject("key1",4);
    robj *val1 = createStringObject("val1",4);
    initTestRedisDb();
    monotonicInit();
    redisDb *db = server.db;

    TEST("batch: init") {
        server.hz = 10;
        initTestRedisDb();
        monotonicInit();
        initServerConfig();
        if (!server.rocks) rocksInit();
        initStatsSwap();
        if (!server.swap_lock) swapLockCreate();
        if (!server.swap_batch_ctx) server.swap_batch_ctx = swapBatchCtxNew();
    }

    TEST("batch: exec batch") {
        swapExecBatch _exec_batch, *exec_batch = &_exec_batch;
        swapRequest *req;
        req = swapDataRequestNew(SWAP_IN,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
        swapExecBatchInit(exec_batch);
        swapExecBatchAppend(exec_batch,req);
        test_assert(exec_batch->count == 1);
        swapExecBatchCtxReset(exec_batch,SWAP_UNSET,ROCKS_UNSET);
        test_assert(exec_batch->count == 0);
        for (int i = 0; i <= SWAP_BATCH_DEFAULT_SIZE; i++) {
            swapExecBatchAppend(exec_batch,req);
        }
        test_assert(exec_batch->count == SWAP_BATCH_DEFAULT_SIZE+1);
        test_assert(exec_batch->capacity > exec_batch->count);
        swapExecBatchDeinit(exec_batch);
        swapRequestFree(req);
    }

    TEST("batch: exec batch ctx") {
        swapExecBatchCtx _exec_ctx, *exec_ctx = &_exec_ctx;
        swapRequest *utils_req = swapDataRequestNew(SWAP_UTILS,COMPACT_RANGE_TASK,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
        swapData *data = createWholeKeySwapData(db,key1,val1,NULL);
        swapRequest *out_req = swapDataRequestNew(SWAP_OUT,0,NULL,data,NULL,NULL,NULL,NULL,NULL);

        resetStatsSwap();

        swapExecBatchCtxInit(exec_ctx);

        swapExecBatchCtxStart(exec_ctx);
        swapExecBatchCtxEnd(exec_ctx);

        swapExecBatchCtxStart(exec_ctx);

        /* out_req buffered (not executed) */
        swapExecBatchCtxFeed(exec_ctx, out_req);
        test_assert(exec_ctx->intention == SWAP_OUT);
        test_assert(exec_ctx->action == ROCKS_PUT);
        test_assert(exec_ctx->count == 1);
        test_assert(server.ror_stats->swap_stats[SWAP_IN].batch == 0);
        test_assert(server.ror_stats->swap_stats[SWAP_IN].count == 0);

        swapExecBatchCtxFeed(exec_ctx, utils_req);
        /* out_req executed, utils_req executed */
        test_assert(exec_ctx->intention == SWAP_UNSET);
        test_assert(exec_ctx->action == ROCKS_UNSET);
        test_assert(exec_ctx->count == 0);
        test_assert(server.ror_stats->swap_stats[SWAP_UTILS].batch == 1);
        test_assert(server.ror_stats->swap_stats[SWAP_UTILS].count == 1);
        test_assert(server.ror_stats->rio_stats[ROCKS_PUT].batch == 1);
        test_assert(server.ror_stats->rio_stats[ROCKS_PUT].count == 1);

        swapExecBatchCtxFeed(exec_ctx,out_req);
        /* out_req buffered */
        test_assert(exec_ctx->intention == SWAP_OUT);
        test_assert(exec_ctx->action == ROCKS_PUT);
        test_assert(exec_ctx->count == 1);
        test_assert(server.ror_stats->rio_stats[ROCKS_PUT].batch == 1);
        test_assert(server.ror_stats->rio_stats[ROCKS_PUT].count == 1);

        swapExecBatchCtxEnd(exec_ctx);
        /* out_req executed, all request flushed */
        test_assert(server.ror_stats->rio_stats[ROCKS_PUT].batch == 2);
        test_assert(server.ror_stats->rio_stats[ROCKS_PUT].count == 2);
        test_assert(exec_ctx->intention == SWAP_UNSET);
        test_assert(exec_ctx->action == ROCKS_UNSET);
        test_assert(exec_ctx->count == 0);

        swapExecBatchCtxDeinit(exec_ctx);
        swapRequestFree(out_req);
        swapRequestFree(utils_req);
        swapDataFree(data,NULL);
    }

    TEST("batch: request batch") {
        swapData *data;
        swapRequest *out_req1, *out_req2, *utils_req;
        swapRequestBatch *reqs;

        resetStatsSwap();

        data = createWholeKeySwapData(db,key1,val1,NULL);
        reqs = swapRequestBatchNew();
        reqs->notify_cb = mockNotifyCallback;
        reqs->notify_pd = NULL;
        out_req1 = swapDataRequestNew(SWAP_OUT,0,NULL,data,NULL,NULL,NULL,NULL,NULL);
        out_req2 = swapDataRequestNew(SWAP_OUT,0,NULL,data,NULL,NULL,NULL,NULL,NULL);
        utils_req = swapDataRequestNew(SWAP_UTILS,COMPACT_RANGE_TASK,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
        swapRequestBatchAppend(reqs,utils_req);
        swapRequestBatchAppend(reqs,out_req1);
        swapRequestBatchAppend(reqs,out_req2);
        swapRequestBatchProcess(reqs);
        swapRequestBatchFree(reqs);
        test_assert(server.ror_stats->swap_stats[SWAP_OUT].batch == 1);
        test_assert(server.ror_stats->swap_stats[SWAP_OUT].count == 2);
        test_assert(server.ror_stats->swap_stats[SWAP_UTILS].batch == 1);
        test_assert(server.ror_stats->swap_stats[SWAP_UTILS].count == 1);
        test_assert(server.ror_stats->rio_stats[ROCKS_PUT].batch == 1);
        test_assert(server.ror_stats->rio_stats[ROCKS_PUT].count == 2);

        swapDataFree(data,NULL);
    }

    TEST("batch: request batch ctx") {
        swapData *data;
        swapBatchCtx *batch_ctx = swapBatchCtxNew();
        swapRequest *out_req1, *out_req2, *utils_req;

        swapThreadsInit();
        server.el = aeCreateEventLoop(server.maxclients+CONFIG_FDSET_INCR);
        asyncCompleteQueueInit();

        /* flush empty ctx => nop */
        swapBatchCtxFlush(batch_ctx,SWAP_BATCH_FLUSH_BEFORE_SLEEP);
        test_assert(batch_ctx->stat.submit_batch_count == 0);
        test_assert(batch_ctx->stat.submit_request_count == 0);

        data = createWholeKeySwapData(db,key1,val1,NULL);
        out_req1 = swapDataRequestNew(SWAP_OUT,0,NULL,data,NULL,NULL,NULL,NULL,NULL);
        out_req2 = swapDataRequestNew(SWAP_OUT,0,NULL,data,NULL,NULL,NULL,NULL,NULL);
        utils_req = swapDataRequestNew(SWAP_UTILS,GET_ROCKSDB_STATS_TASK,NULL,NULL,NULL,NULL,NULL,NULL,NULL);

        swapBatchCtxFeed(batch_ctx,0,utils_req,-1);
        test_assert(batch_ctx->stat.submit_batch_count == 1);

        /* switch intention triggers flush before append */
        swapBatchCtxFeed(batch_ctx,0,out_req1,-1);
        test_assert(batch_ctx->stat.submit_batch_count == 1);
        test_assert(batch_ctx->stat.submit_request_count == 1);

        /* force flush triggers flush after append */
        swapBatchCtxFeed(batch_ctx,1,out_req2,-1);
        test_assert(batch_ctx->stat.submit_batch_count == 2);
        test_assert(batch_ctx->stat.submit_request_count == 3);

        /* exceeds swap batch limit triggers flush after append. */
        for (int i = 0; i < SWAP_BATCH_DEFAULT_SIZE; i++) {
            test_assert(batch_ctx->stat.submit_batch_count == 2);
            swapBatchCtxFeed(batch_ctx,0,out_req2,-1);
        }
        test_assert(batch_ctx->stat.submit_batch_count == 3);
        test_assert(batch_ctx->stat.submit_request_count == 3+SWAP_BATCH_DEFAULT_SIZE);

        swapBatchCtxFree(batch_ctx);
    }

    return error;
}

#endif
