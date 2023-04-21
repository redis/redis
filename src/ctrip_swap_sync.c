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

/* --- Parallel sync rocks io (pipe)--- */
int parallelSyncInit(int parallel) {
    int i;
    parallelSync *ps = zmalloc(sizeof(parallelSync));

    ps->parallel = parallel;
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
        e->reqs = NULL;

        listAddNodeTail(ps->entries, e);
    }
    server.parallel_sync = ps;
    return C_OK;

err:
    listRelease(ps->entries);
    return C_ERR;
}

void parallelSyncDeinit() {
    parallelSync *ps = server.parallel_sync;
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
    server.parallel_sync = NULL;
}

static int parallelSwapProcess(swapEntry *e) {
    if (e->inprogress) {
        char c;
        if (read(e->pipe_read_fd, &c, 1) != 1) {
            serverLog(LL_WARNING, "wait swap entry failed: %s",
                    strerror(errno));
            return C_ERR;
        }
        serverAssert(c == 'x');
        swapRequestBatchCallback(e->reqs);
        swapRequestBatchFree(e->reqs);
        e->reqs = NULL;
        e->inprogress = 0;
    }
    return C_OK;
}

void parallelSyncSwapNotifyCallback(swapRequestBatch *reqs, void *pd) {
    swapEntry *e = pd;
    UNUSED(reqs);
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
}

/* Submit one swap (task). swap will start and finish in submit order. */
int parallelSyncSwapRequestBatchSubmit(swapRequestBatch *reqs, int idx) {
    listNode *ln;
    swapEntry *e;
    parallelSync *ps = server.parallel_sync;
    /* wait and handle previous swap */
    if (!(ln = listFirst(ps->entries))) return C_ERR;
    e = listNodeValue(ln);
    if (parallelSwapProcess(e)) return C_ERR;
    listRotateHeadToTail(ps->entries);
    /* submit */
    reqs->notify_cb = parallelSyncSwapNotifyCallback;
    reqs->notify_pd = e;
    e->reqs = reqs;
    e->inprogress = 1;
    swapThreadsDispatch(reqs,idx);
    return C_OK;
}

int parallelSyncDrain() {
    listIter li;
    listNode *ln;

    listRewind(server.parallel_sync->entries, &li);
    while((ln = listNext(&li))) {
        swapEntry *e = listNodeValue(ln);
        if ((parallelSwapProcess(e)))
            return C_ERR;
    }

    return C_OK;
}

