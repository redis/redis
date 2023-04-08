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

static inline int swapExecBatchCtxExceedBatchLimit(swapExecBatchCtx *exec_ctx) {
    int exceeded = 0;
    swapBatchLimitsConfig *limit;

    serverAssert(swapIntentionInOutDel(exec_ctx->intention));

    limit = server.swap_batch_limits+exec_ctx->intention;
    if (limit->count > 0 && exec_ctx->count >= (size_t)limit->count) {
        exceeded = 1;
    }

    /* TODO account for mem as well. */
    return exceeded;
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

    /* execute after append req if exceeded swap-batch-limit */
    if (!swapIntentionInOutDel(req->intention) ||
            swapExecBatchCtxExceedBatchLimit(exec_ctx)) {
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
    if (reqs->notify_queue_timer) {
        metricDebugInfo(SWAP_DEBUG_NOTIFY_QUEUE_WAIT, elapsedUs(reqs->notify_queue_timer));
    }
    for (size_t i = 0; i < reqs->count; i++) {
       swapRequestCallback(reqs->reqs[i]);
    }
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
    swapExecBatch exec_batch;

    swapExecBatchCtxInit(&exec_batch);
    swapExecBatchCtxStart(&exec_batch);
    for (size_t i = 0; i < reqs->count; i++) {
        swapRequest *req = reqs->reqs[i];
        if (!swapRequestGetError(req) && req->intention != SWAP_NOP) {
            swapExecBatchCtxFeed(&exec_batch,req);
        }
    }
    swapExecBatchCtxEnd(&exec_batch);
    swapExecBatchCtxDeinit(&exec_batch);
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
    atomicSet(batch_stat->batch_count,0);
    atomicSet(batch_stat->request_count,0);
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

size_t swapBatchCtxFlush(swapBatchCtx *batch_ctx) {
    if (swapRequestBatchEmpty(batch_ctx->batch)) return 0;
    int thread_idx = batch_ctx->thread_idx;
    swapRequestBatch *reqs = swapBatchCtxShift(batch_ctx);
    size_t reqs_count = reqs->count;
    atomicIncr(batch_ctx->stat.batch_count,1);
    submitSwapRequestBatch(SWAP_MODE_ASYNC,reqs,thread_idx);
    return reqs_count;
}

void swapBatchCtxFeed(swapBatchCtx *batch_ctx, int force_flush,
        swapRequest *req, int thread_idx) {
    /* flush before handling req if req is dispatched to another thread. */
    if (batch_ctx->thread_idx != thread_idx ||
            batch_ctx->cmd_intention != req->intention) {
        swapBatchCtxFlush(batch_ctx);
    }

    batch_ctx->thread_idx = thread_idx;
    batch_ctx->cmd_intention = req->intention;

    swapRequestBatchAppend(batch_ctx->batch,req);
    atomicIncr(batch_ctx->stat.request_count,1);

    /* flush after handling req if flush hint set. */
    if (force_flush) swapBatchCtxFlush(batch_ctx);
}

#ifdef REDIS_TEST

int swapBatchTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);
    int error = 0;

    TEST("batch: init") {
        server.hz = 10;
        initTestRedisDb();
        monotonicInit();
        //initServerConfig();
        if (!server.rocks) rocksInit();
        initStatsSwap();
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
    }

    TEST("batch: exec batch ctx") {
        swapExecBatchCtx _exec_ctx, *exec_ctx = &_exec_ctx;
        swapRequest *util_req = swapDataRequestNew(SWAP_UTILS,0,NULL,NULL,
                NULL,NULL,NULL,NULL,NULL);
        swapRequest *get_req = swapDataRequestNew(SWAP_IN,0,NULL,NULL,
                NULL,NULL,NULL,NULL,NULL);

        resetStatsSwap();

        swapExecBatchCtxInit(exec_ctx);

        swapExecBatchCtxStart(exec_ctx);
        swapExecBatchCtxEnd(exec_ctx);
        test_assert(server.swap_batch_ctx->stat.batch_count == 0);
        test_assert(server.swap_batch_ctx->stat.request_count == 0);

        swapExecBatchCtxStart(exec_ctx);
        swapExecBatchCtxFeed(exec_ctx, util_req);
        /* util_req buffered (not executed) */
        test_assert(exec_ctx->intention == SWAP_UTILS);
        test_assert(exec_ctx->action == ROCKS_UNSET);
        test_assert(exec_ctx->count == 1);
        test_assert(server.swap_batch_ctx->stat.batch_count == 0);
        test_assert(server.swap_batch_ctx->stat.request_count == 0);

        swapExecBatchCtxFeed(exec_ctx,get_req);
        /* util_req executed, get_req buffered */
        test_assert(server.swap_batch_ctx->stat.batch_count == 1);
        test_assert(server.swap_batch_ctx->stat.request_count == 1);
        test_assert(server.ror_stats->swap_stats[SWAP_UTILS].batch == 1);
        test_assert(server.ror_stats->swap_stats[SWAP_UTILS].count == 1);
        test_assert(exec_ctx->intention == SWAP_IN);
        test_assert(exec_ctx->action == ROCKS_GET);
        test_assert(exec_ctx->count == 1);

        swapExecBatchCtxEnd(exec_ctx);
        /* get_req executed, all request flushed */
        test_assert(server.swap_batch_ctx->stat.batch_count == 2);
        test_assert(server.swap_batch_ctx->stat.request_count == 2);
        test_assert(server.ror_stats->swap_stats[SWAP_IN].batch == 1);
        test_assert(server.ror_stats->swap_stats[SWAP_IN].count == 1);
        test_assert(server.ror_stats->rio_stats[ROCKS_GET].batch == 1);
        test_assert(server.ror_stats->rio_stats[ROCKS_GET].count == 1);
        test_assert(exec_ctx->intention == SWAP_UNSET);
        test_assert(exec_ctx->action == ROCKS_UNSET);
        test_assert(exec_ctx->count == 0);

        swapExecBatchCtxDeinit(exec_ctx);
    }

    TEST("batch: request batch") {
        swapRequest *get_req1, *get_req2, *util_req;
        swapRequestBatch *reqs;

        resetStatsSwap();

        reqs = swapRequestBatchNew();
        get_req1 = swapDataRequestNew(SWAP_IN,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
        get_req2 = swapDataRequestNew(SWAP_IN,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
        util_req = swapDataRequestNew(SWAP_IN,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
        swapRequestBatchAppend(reqs,util_req);
        swapRequestBatchAppend(reqs,get_req1);
        swapRequestBatchAppend(reqs,get_req2);
        swapRequestBatchProcess(reqs);
        swapRequestBatchFree(reqs);

        test_assert(server.swap_batch_ctx->stat.batch_count == 2);
        test_assert(server.swap_batch_ctx->stat.request_count == 3);
        test_assert(server.ror_stats->swap_stats[SWAP_IN].batch == 1);
        test_assert(server.ror_stats->swap_stats[SWAP_IN].count == 2);
        test_assert(server.ror_stats->swap_stats[SWAP_UTILS].batch == 1);
        test_assert(server.ror_stats->swap_stats[SWAP_UTILS].count == 1);
        test_assert(server.ror_stats->rio_stats[ROCKS_GET].batch == 1);
        test_assert(server.ror_stats->rio_stats[ROCKS_GET].count == 2);
    }

    TEST("batch: request batch ctx") {
        swapBatchCtx *batch_ctx = swapBatchCtxNew();
        swapRequest *get_req1, *get_req2, *util_req;

        /* flush empty ctx => nop */
        swapBatchCtxFlush(batch_ctx);
        test_assert(batch_ctx->stat.batch_count == 0);
        test_assert(batch_ctx->stat.request_count == 0);

        util_req = swapDataRequestNew(SWAP_IN,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
        get_req1 = swapDataRequestNew(SWAP_IN,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
        get_req2 = swapDataRequestNew(SWAP_IN,0,NULL,NULL,NULL,NULL,NULL,NULL,NULL);

        swapBatchCtxFeed(batch_ctx,0,util_req,-1);
        test_assert(batch_ctx->stat.batch_count == 0);

        /* switch intention triggers flush before append */
        swapBatchCtxFeed(batch_ctx,0,get_req1,-1);
        test_assert(batch_ctx->stat.batch_count == 1);
        test_assert(batch_ctx->stat.request_count == 1);

        /* force flush triggers flush after append */
        swapBatchCtxFeed(batch_ctx,1,get_req2,-1);
        test_assert(batch_ctx->stat.batch_count == 2);
        test_assert(batch_ctx->stat.request_count == 3);

        /* exceeds swap batch limit triggers flush after append. */
        for (int i = 0; i < SWAP_BATCH_DEFAULT_SIZE; i++) {
            test_assert(batch_ctx->stat.batch_count == 2);
            swapBatchCtxFeed(batch_ctx,1,get_req2,-1);
        }
        test_assert(batch_ctx->stat.batch_count == 3);
        test_assert(batch_ctx->stat.batch_count == 3+SWAP_BATCH_DEFAULT_SIZE);
    }

    return error;
}

#endif
