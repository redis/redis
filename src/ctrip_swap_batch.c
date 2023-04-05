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

/* swapExecBatch: batch of requests with the same swap intention and action. */
void swapExecBatchInit(swapExecBatch *exec_batch) {
    exec_batch->reqs = exec_batch->req_buf;
    exec_batch->capacity = SWAP_BATCH_DEFAULT_SIZE;
    exec_batch->count = 0;
    exec_batch->action = SWAP_UNSET;
    exec_batch->intention = 0;
}

void swapExecBatchDeinit(swapExecBatch *exec_batch) {
    if (exec_batch == NULL) return;
    if (exec_batch->reqs != exec_batch->req_buf) {
        zfree(exec_batch->reqs);
        exec_batch->reqs = NULL;
    }
}

void swapExecBatchReset(swapExecBatch *exec_batch, int intention, int action) {
    exec_batch->count = 0;
    exec_batch->intention = intention;
    exec_batch->action = action;
}

static inline int swapExecBatchEmpty(swapExecBatch *exec_batch) {
    return exec_batch->count == 0;
}

static inline void swapExecBatchExecuteIfNeeded(swapExecBatch *exec_batch) {
    if (!swapExecBatchEmpty(exec_batch)) {
        swapExecBatchExecute(exec_batch);
    }
}

void swapExecBatchStart(swapExecBatch *exec_batch) {
    UNUSED(exec_batch);
}

void swapExecBatchEnd(swapExecBatch *exec_batch) {
    swapExecBatchExecuteIfNeeded(exec_batch);
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

void swapExecBatchFeed(swapExecBatch *exec_batch, swapRequest *req) {
    int req_action;
    serverAssert(req->intention != SWAP_UNSET);

    if (req->intention == SWAP_IN || req->intention == SWAP_OUT ||
            req->intention == SWAP_DEL) {
        swapDataSwapAnaAction(req->data,req->intention,req->datactx,
                &req_action);
    } else {
        req_action = ROCKS_NOP;
    }

    if ((req->intention != exec_batch->intention ||
            req_action != exec_batch->action)) {
        swapExecBatchExecuteIfNeeded(exec_batch);

        swapExecBatchReset(exec_batch,req->intention,req_action);
    }

    swapExecBatchAppend(exec_batch,req);
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
    for (size_t i = 0; i < reqs->count; i++) {
        swapRequest *req = reqs->reqs[i];
        if (req->trace) swapTraceDispatch(req->trace);
    }
}

void swapRequestBatchStart(swapRequestBatch *reqs) {
    for (size_t i = 0; i < reqs->count; i++) {
        swapRequest *req = reqs->reqs[i];
        swapRequestUpdateStatsStart(req);
        if (req->trace) swapTraceProcess(req->trace);
    }
}

void swapRequestBatchEnd(swapRequestBatch *reqs) {
    reqs->notify_cb(reqs, reqs->notify_pd);
    UNUSED(reqs);
}

void swapRequestBatchExecute(swapRequestBatch *reqs) {
    swapExecBatch exec_batch;

    swapExecBatchInit(&exec_batch);
    swapExecBatchStart(&exec_batch);
    for (size_t i = 0; i < reqs->count; i++) {
        swapRequest *req = reqs->reqs[i];
        if (!swapRequestGetError(req) && req->intention != SWAP_NOP) {
            swapExecBatchFeed(&exec_batch,req);
        }
    }
    swapExecBatchEnd(&exec_batch);
    swapExecBatchDeinit(&exec_batch);
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
    swapRequestBatchStart(reqs);
    swapRequestBatchPreprocess(reqs);
    swapRequestBatchExecute(reqs);
    swapRequestBatchEnd(reqs);
}

/* swapBatchCtx: currently acummulated requests. */

void swapBatchStatInit(swapBatchStat *batch_stat) {
    atomicSet(batch_stat->batch_count,0);
    atomicSet(batch_stat->request_count,0);
}

swapBatchCtx *swapBatchCtxNew() {
    swapBatchCtx *batch_ctx = zmalloc(sizeof(swapBatchCtx));
    swapBatchStatInit(&batch_ctx->stat);
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
#endif

