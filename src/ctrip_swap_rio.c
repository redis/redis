/* Copyright (c) 2021, ctrip.com * All rights reserved.
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

/* --- RIO --- */
RIO *RIONewWB(rocksdb_writebatch_t *wb) {
    RIO *rio = zcalloc(sizeof(RIO));
    rio->action = ROCKS_WRITE;
    rio->req.wb = wb;
    return rio;
}

RIO *RIONewKV(int action, sds rawkey, sds rawval) {
    RIO *rio = zcalloc(sizeof(RIO));
    rio->action = action;
    rio->req.kv.rawkey = rawkey;
    rio->req.kv.rawval = rawval;
    return rio;
}

void RIOFree(RIO *req) {
    zfree(req);
}

/* --- Rocks rio thread --- */
void rocksIOSubmit(uint32_t dist, RIO *req, RIOFinished cb, void *pd) {
    serverAssert(server.rocks->threads_num >= 0);
    RIOThread *rt = &server.rocks->threads[dist % server.rocks->threads_num];
    req->rio.cb = cb;
    req->rio.pd = pd;
    pthread_mutex_lock(&rt->lock);
    listAddNodeTail(rt->pending_rios, req);
    pthread_cond_signal(&rt->cond);
    pthread_mutex_unlock(&rt->lock);
}

static int doRIORead(RIO *rio) {
    size_t vallen;
    char *err = NULL, *val;

    serverAssert(rio->action == ROCKS_GET);

    val = rocksdb_get(server.rocks->rocksdb, server.rocks->rocksdb_ropts,
            rio->req.kv.rawkey, sdslen(rio->req.kv.rawkey), &vallen, &err);
    if (err != NULL) {
        rio->rsp.err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb get failed: %s", err);
        return -1;
    }
    rio->rsp.kv.rawkey = rio->req.kv.rawkey;
    rio->rsp.kv.rawval = sdsnewlen(val, vallen);
    zlibc_free(val);
    return 0;
}

static int doRIOPut(RIO *rio) {
    char *err = NULL;
    serverAssert(rio->action == ROCKS_PUT);

    rocksdb_put(server.rocks->rocksdb, server.rocks->rocksdb_wopts,
            rio->req.kv.rawkey, sdslen(rio->req.kv.rawkey),
            rio->req.kv.rawval, sdslen(rio->req.kv.rawval), &err);
    if (err != NULL) {
        rio->rsp.err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb write failed: %s", err);
        return -1;
    }
    return 0;
}

static int doRIODel(RIO *rio) {
    char *err = NULL;
    serverAssert(rio->action == ROCKS_DEL);
    rocksdb_delete(server.rocks->rocksdb, server.rocks->rocksdb_wopts,
            rio->req.kv.rawkey, sdslen(rio->req.kv.rawkey), &err);
    if (err != NULL) {
        rio->rsp.err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb del failed: %s", err);
        return -1;
    }
    return 0;
}

static int doRIOWrite(RIO *rio) {
    char *err = NULL;
    serverAssert(rio->action == ROCKS_WRITE);
    rocksdb_write(server.rocks->rocksdb, server.rocks->rocksdb_wopts,
            rio->req.wb, &err);
    if (err != NULL) {
        rio->rsp.err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb del failed: %s", err);
        return -1;
    }
    return 0;
}

static int doRIO(RIO *rio) {
    int ret;
    if (server.debug_rio_latency) usleep(server.debug_rio_latency*1000);

    switch (rio->action) {
    case ROCKS_GET:
        ret = doRIORead(rio);
        break;
    case ROCKS_PUT:
        ret = doRIOPut(rio);
        break;
    case ROCKS_DEL:
        ret = doRIODel(rio);
        break;
    case ROCKS_WRITE:
        ret = doRIOWrite(rio);
        break;
    default:
        serverPanic("[rocks] Unknown io action: %d", rio->action);
        return -1;
    }
    
    rio->rio.cb(rio, rio->rio.pd);
    return ret;
}

void *RIOThreadMain (void *arg) {
    char thdname[16];
    RIOThread *thread = arg;

    snprintf(thdname, sizeof(thdname), "rocks_thd_%d", thread->id);
    redis_set_thread_title(thdname);

    while (1) {
        listIter li;
        listNode *ln;
        list *processing_rios = listCreate();

        pthread_mutex_lock(&thread->lock);
        while (listLength(thread->pending_rios) == 0)
            pthread_cond_wait(&thread->cond, &thread->lock);

        listRewind(thread->pending_rios, &li);
        while ((ln = listNext(&li))) {
            RIO *rio = listNodeValue(ln);
            listAddNodeHead(processing_rios, rio);
            listDelNode(thread->pending_rios, ln);
        }
        pthread_mutex_unlock(&thread->lock);

        listRewind(processing_rios, &li);
        atomic_store(&thread->is_running_rio, 1);
        while ((ln = listNext(&li))) {
            doRIO(listNodeValue(ln));
        }
        atomic_store(&thread->is_running_rio, 0);
        listRelease(processing_rios);
    }

    return NULL;
}

int rocksInitThreads(rocks *rocks) {
    int i, nthd = RIO_THREADS_DEFAULT;

    if (nthd > RIO_THREADS_MAX) nthd = RIO_THREADS_MAX;
    rocks->threads_num = nthd;

    for (i = 0; i < nthd; i++) {
        RIOThread *thread = &rocks->threads[i];

        thread->id = i;
        thread->pending_rios = listCreate();
        atomic_store(&thread->is_running_rio, 0);
        pthread_mutex_init(&thread->lock, NULL);
        pthread_cond_init(&thread->cond, NULL);
        if (pthread_create(&thread->thread_id, NULL, RIOThreadMain, thread)) {
            serverLog(LL_WARNING, "Fatal: create rocks IO threads failed.");
            return -1;
        }
    }

    return 0;
}

void rocksDeinitThreads(rocks *rocks) {
    int i, err;

    for (i = 0; i < rocks->threads_num; i++) {
        RIOThread *thread = &rocks->threads[i];
        listRelease(thread->pending_rios);
        if (thread->thread_id == pthread_self()) continue;
        if (thread->thread_id && pthread_cancel(thread->thread_id) == 0) {
            if ((err = pthread_join(thread->thread_id, NULL)) != 0) {
                serverLog(LL_WARNING, "rocks thread #%d can't be joined: %s",
                        i, strerror(err));
            } else {
                serverLog(LL_WARNING, "rocks thread #%d terminated.", i);
            }
        }
    }
}

int rocksThreadsDrained(rocks *rocks) {
    RIOThread *rt;
    int drained = 1, i;
    for (i = 0; i < rocks->threads_num; i++) {
        rt = &server.rocks->threads[i];

        pthread_mutex_lock(&rt->lock);
        if (listLength(rt->pending_rios) || atomic_load(&rt->is_running_rio)) drained = 0;
        pthread_mutex_unlock(&rt->lock);
    }
    return drained;
}

/* --- Async rocks io --- */
int asyncCompleteQueueProcess(asyncCompleteQueue *cq) {
    int processed;
    listIter li;
    listNode *ln;
    list *processing_rios = listCreate();

    pthread_mutex_lock(&cq->lock);
    listRewind(cq->complete_queue, &li);
    while ((ln = listNext(&li))) {
        listAddNodeTail(processing_rios, listNodeValue(ln));
        listDelNode(cq->complete_queue, ln);
    }
    pthread_mutex_unlock(&cq->lock);

    listRewind(processing_rios, &li);
    while ((ln = listNext(&li))) {
        RIO *rio = listNodeValue(ln);
        rocksAsyncCallback async_rio_finished = (rocksAsyncCallback)rio->svr.cb;
        serverAssert(rio->svr.cb);
        async_rio_finished(rio->action, rio->rsp.kv.rawkey,
                rio->rsp.kv.rawval, rio->rsp.err, rio->svr.pd);
        RIOFree(rio);
    }

    processed = listLength(processing_rios);
    listRelease(processing_rios);
    return processed;
}

/* read before unlink clients so that main thread won't miss notify event:
 * rocksb thread: 1. link req; 2. send notify byte;
 * main thread: 1. read notify bytes; 2. unlink req;
 * if main thread read less notify bytes than unlink clients num (e.g. rockdb
 * thread link more clients when , main thread would still be triggered because
 * epoll LT-triggering mode. */
void asyncCompleteQueueHanlder(aeEventLoop *el, int fd, void *privdata, int mask) {
    char notify_recv_buf[ASYNC_COMPLETE_QUEUE_NOTIFY_READ_MAX];

    UNUSED(el);
    UNUSED(mask);

    int nread = read(fd, notify_recv_buf, sizeof(notify_recv_buf));
    if (nread == 0) {
        serverLog(LL_WARNING, "[rocks] notify recv fd closed.");
    } else if (nread < 0) {
        serverLog(LL_WARNING, "[rocks] read notify failed: %s",
                strerror(errno));
    }

    asyncCompleteQueueProcess(privdata); /* privdata is cq */
}

int asyncCompleteQueueInit(rocks *rocks) {
    int fds[2];
    char anetErr[ANET_ERR_LEN];
    asyncCompleteQueue *cq = &rocks->CQ;

    if (pipe(fds)) {
        perror("Can't create notify pipe");
        return -1;
    }

    cq->notify_recv_fd = fds[0];
    cq->notify_send_fd = fds[1];

    pthread_mutex_init(&cq->lock, NULL);

    cq->complete_queue = listCreate();

    if (anetNonBlock(anetErr, cq->notify_recv_fd) != ANET_OK) {
        serverLog(LL_WARNING,
                "Fatal: set notify_recv_fd non-blocking failed: %s",
                anetErr);
        return -1;
    }

    if (anetNonBlock(anetErr, cq->notify_send_fd) != ANET_OK) {
        serverLog(LL_WARNING,
                "Fatal: set notify_recv_fd non-blocking failed: %s",
                anetErr);
        return -1;
    }

    if (aeCreateFileEvent(server.el, cq->notify_recv_fd,
                AE_READABLE, asyncCompleteQueueHanlder, cq) == AE_ERR) {
        serverLog(LL_WARNING,"Fatal: create notify recv event failed: %s",
                strerror(errno));
        return -1;
    }

    return 0;
}

void asyncCompleteQueueDeinit(asyncCompleteQueue *cq) {
    close(cq->notify_recv_fd);
    close(cq->notify_send_fd);
    pthread_mutex_destroy(&cq->lock);
    listRelease(cq->complete_queue);
}

void asyncRIOFinished(RIO *rio, void *pd) {
    UNUSED(pd);
    asyncCompleteQueueAppend(&server.rocks->CQ, rio);
}

int asyncCompleteQueueAppend(asyncCompleteQueue *cq, RIO *rio) {
    pthread_mutex_lock(&cq->lock);
    listAddNodeTail(cq->complete_queue, rio);
    pthread_mutex_unlock(&cq->lock);
    if (write(cq->notify_send_fd, "x", 1) < 1 && errno != EAGAIN) {
        static mstime_t prev_log;
        if (server.mstime - prev_log >= 1000) {
            prev_log = server.mstime;
            serverLog(LL_NOTICE, "[rocks] notify rio finish failed: %s",
                    strerror(errno));
        }
        return -1;
    }
    return 0;
}

void rocksAsyncSubmit(uint32_t dist, int action, sds rawkey, sds rawval,
        rocksAsyncCallback cb, void *pd) {
    RIO *rio = RIONewKV(action, rawkey, rawval);
    rio->svr.cb = (voidfuncptr)cb;
    rio->svr.pd = pd;
    rocksIOSubmit(dist, rio, asyncRIOFinished, NULL);
}

void rocksAsyncPut(uint32_t dist, sds rawkey, sds rawval,
        rocksAsyncCallback cb, void *pd) {
    rocksAsyncSubmit(dist, ROCKS_PUT, rawkey, rawval, cb, pd);
}

void rocksAsyncGet(uint32_t dist, sds rawkey,
        rocksAsyncCallback cb, void *pd) {
    rocksAsyncSubmit(dist, ROCKS_GET, rawkey, NULL, cb, pd);
}

void rocksAsyncDel(uint32_t dist, sds rawkey,
        rocksAsyncCallback cb, void *pd) {
    rocksAsyncSubmit(dist, ROCKS_DEL, rawkey, NULL, cb, pd);
}

static int asyncCompleteQueueDrained(rocks *rocks) {
    int drained = 1;

    if (!rocksThreadsDrained(rocks)) return 0;
    pthread_mutex_lock(&rocks->CQ.lock);
    if (listLength(rocks->CQ.complete_queue)) drained = 0;
    pthread_mutex_unlock(&rocks->CQ.lock);
    return drained;
}

int asyncCompleteQueueDrain(rocks *rocks, mstime_t time_limit) {
    int result = 0;
    mstime_t start = mstime();
    while (!asyncCompleteQueueDrained(rocks)) {
        asyncCompleteQueueProcess(&rocks->CQ);

        if (time_limit >= 0 && mstime() - start > time_limit) {
            result = -1;
            break;
        }
    }

    serverLog(LL_NOTICE,
            "[rocks] drain IO %s: elapsed (%lldms) limit (%lldms)",
            result == 0 ? "ok":"failed", mstime() - start, time_limit);

    return result;
}

/* --- Parallel sync rocks io (pipe)--- */
parallelSwap *parallelSwapNew(int parallel, int mode) {
    int i;
    parallelSwap *ps = zmalloc(sizeof(parallelSwap));

    ps->parallel = parallel;
    ps->mode = mode;
    ps->entries = listCreate();

    for (i = 0; i < parallel; i++) {
        int fds[2];
        swapEntry *e;

        if (pipe(fds)) {
            serverLog(LL_WARNING, "create future pipe failed: %s",
                    strerror(errno));
            goto err;
        }

        e = zmalloc(sizeof(swapEntry));
        e->inprogress = 0;
        e->pipe_read_fd = fds[0];
        e->pipe_write_fd = fds[1];
        e->pd = NULL;

        listAddNodeTail(ps->entries, e);
    }
    return ps;

err:
    listRelease(ps->entries);
    return NULL;
}

void parallelSwapFree(parallelSwap *ps) {
    listNode *ln;
    while ((ln = listFirst(ps->entries))) {
        swapEntry *e = listNodeValue(ln);
        close(e->pipe_read_fd);
        close(e->pipe_write_fd);
        zfree(e);
        listDelNode(ps->entries, ln);
    }
    listRelease(ps->entries);
    zfree(ps);
}

static int parallelSwapProcess(swapEntry *e) {
    if (e->inprogress) {
        char c;
        if (read(e->pipe_read_fd, &c, 1) != 1) {
            serverLog(LL_WARNING, "wait swap entry failed: %s",
                    strerror(errno));
            return C_ERR;
        }
        e->inprogress = 0;
        e->cb(e->rsp.action, e->rsp.rawkey, e->rsp.rawval,
                e->rsp.err, e->pd);
    }
    return C_OK;
}

static uint32_t parallelSwapNextDist(parallelSwap *ps) {
    static uint32_t dist = 0;
    switch(ps->mode) {
    case PARALLEL_SWAP_MODE_CONST: return 0;
    case PARALLEL_SWAP_MODE_ROUND_ROBIN: return dist++;
    case PARALLEL_SWAP_MODE_BATCH: return dist++ / PARALLEL_SWAP_MODE_BATCH_SIZE;
    default: return 0;
    }
}

void parallelSwapRIOFinished(RIO *rio, void *pd) {
    swapEntry *e = pd;
    /* Reap result */
    e->rsp.action = rio->action;
    e->rsp.rawkey = rio->rsp.kv.rawkey;
    e->rsp.rawval = rio->rsp.kv.rawval;
    e->rsp.err = rio->rsp.err;
    /* Notify svr to progress */
    if (write(e->pipe_write_fd, "x", 1) < 1 && errno != EAGAIN) {
        static mstime_t prev_log;
        if (server.mstime - prev_log >= 1000) {
            prev_log = server.mstime;
            serverLog(LL_NOTICE,
                    "[rocks] notify rio finish failed: %s",
                    strerror(errno));
        }
    }
    RIOFree(rio);
}

/* Submit one swap (task). swap will start and finish in submit order. */
static int parallelSwapSubmit(parallelSwap *ps, RIO *rio, parallelSwapCallback cb, void *pd) {
    listNode *ln;
    swapEntry *e;
    uint32_t dist = parallelSwapNextDist(ps);
    /* wait and handle previous swap */
    if (!(ln = listFirst(ps->entries))) return C_ERR;
    e = listNodeValue(ln);
    if (parallelSwapProcess(e)) return C_ERR;
    listRotateHeadToTail(ps->entries);
    /* load new swap */
    e->cb = cb;
    e->pd = pd;
    e->inprogress = 1;
    /* submit */
    rocksIOSubmit(dist, rio, parallelSwapRIOFinished, e);
    return C_OK;
}

/* Submit one swap (task). swap will start and finish in submit order. */
int parallelSwapSubmitKV(parallelSwap *ps, int action, sds rawkey, sds rawval, parallelSwapCallback cb, void *pd) {
    RIO *rio = RIONewKV(action, rawkey, rawval);
    rio->svr.cb = (voidfuncptr)cb;
    rio->svr.pd = pd;
    return parallelSwapSubmit(ps, rio, cb, pd);
}

int parallelSwapSubmitWB(parallelSwap *ps, rocksdb_writebatch_t *wb, parallelSwapCallback cb, void *pd) {
    RIO *rio = RIONewWB(wb);
    rio->svr.cb = (voidfuncptr)cb;
    rio->svr.pd = pd;
    return parallelSwapSubmit(ps, rio, cb, pd);
}

/* utility functions */
int parallelSwapGet(parallelSwap *ps, sds rawkey, parallelSwapCallback cb, void *pd) {
    return parallelSwapSubmitKV(ps, SWAP_GET, rawkey, NULL, cb, pd);
}

int parallelSwapPut(parallelSwap *ps, sds rawkey, sds rawval, parallelSwapCallback cb, void *pd) {
    return parallelSwapSubmitKV(ps, SWAP_PUT, rawkey, rawval, cb, pd);
}

int parallelSwapDel(parallelSwap *ps, sds rawkey, parallelSwapCallback cb, void *pd) {
    return parallelSwapSubmitKV(ps, SWAP_DEL, rawkey, NULL, cb, pd);
}

int parallelSwapWrite(parallelSwap *ps, rocksdb_writebatch_t *wb, parallelSwapCallback cb, void *pd) {
    return parallelSwapSubmitWB(ps, wb, cb, pd);
}

int parallelSwapDrain(parallelSwap *ps) {
    listIter li;
    listNode *ln;

    listRewind(ps->entries, &li);
    while((ln = listNext(&li))) {
        swapEntry *e = listNodeValue(ln);
        if ((parallelSwapProcess(e)))
            return C_ERR;
    }

    return C_OK;
}
