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

/* --- Async rocks io --- */
int asyncCompleteQueueProcess(asyncCompleteQueue *cq) {
    int processed;
    listIter li;
    listNode *ln;
    list *processing_reqs = listCreate();

    pthread_mutex_lock(&cq->lock);
    listRewind(cq->complete_queue, &li);
    while ((ln = listNext(&li))) {
        listAddNodeTail(processing_reqs, listNodeValue(ln));
        listDelNode(cq->complete_queue, ln);
    }
    pthread_mutex_unlock(&cq->lock);

    listRewind(processing_reqs, &li);
    while ((ln = listNext(&li))) {
        swapRequest *req = listNodeValue(ln);
        /* currently async mode are only used by cmd swap. */
        finishCmdSwapRequest(req);
        req->finish_cb(req->data, req->finish_pd);
        swapRequestFree(req);
    }

    processed = listLength(processing_reqs);
    listRelease(processing_reqs);
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

    asyncCompleteQueueProcess(privdata);
}

int asyncCompleteQueueInit() {
    int fds[2];
    char anetErr[ANET_ERR_LEN];
    asyncCompleteQueue *cq = &server.rocks->CQ;

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

int asyncSwapRequestNotifyCallback(struct swapRequest *req, void *pd) {
    UNUSED(pd);
    return asyncCompleteQueueAppend(&server.rocks->CQ, req);
}

int asyncCompleteQueueAppend(asyncCompleteQueue *cq, swapRequest *req) {
    pthread_mutex_lock(&cq->lock);
    listAddNodeTail(cq->complete_queue, req);
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

void asyncSwapRequestSubmit(swapRequest *req) {
    req->notify_cb = asyncSwapRequestNotifyCallback;
    req->notify_pd = NULL;
    swapThreadsDispatch(req);
}

static int asyncCompleteQueueDrained() {
    int drained = 1;

    if (!swapThreadsDrained()) return 0;
    pthread_mutex_lock(&server.rocks->CQ.lock);
    if (listLength(server.rocks->CQ.complete_queue)) drained = 0;
    pthread_mutex_unlock(&server.rocks->CQ.lock);

    return drained;
}

int asyncCompleteQueueDrain(mstime_t time_limit) {
    int result = 0;
    mstime_t start = mstime();

    while (!asyncCompleteQueueDrained()) {
        asyncCompleteQueueProcess(&server.rocks->CQ);

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

