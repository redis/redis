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

void *swapThreadMain (void *arg) {
    char thdname[16];
    swapThread *thread = arg;

    snprintf(thdname, sizeof(thdname), "swap_thd_%d", thread->id);
    redis_set_thread_title(thdname);

    while (1) {
        listIter li;
        listNode *ln;
        list *processing_reqs = listCreate();

        pthread_mutex_lock(&thread->lock);
        while (listLength(thread->pending_reqs) == 0)
            pthread_cond_wait(&thread->cond, &thread->lock);

        listRewind(thread->pending_reqs, &li);
        while ((ln = listNext(&li))) {
            swapRequest *req = listNodeValue(ln);
            listAddNodeHead(processing_reqs, req);
            listDelNode(thread->pending_reqs, ln);
        }
        pthread_mutex_unlock(&thread->lock);

        listRewind(processing_reqs, &li);
        atomic_store(&thread->is_running_rio, 1);
        while ((ln = listNext(&li))) {
            executeSwapRequest(listNodeValue(ln));
        }

        atomic_store(&thread->is_running_rio, 0);
        listRelease(processing_reqs);
    }

    return NULL;
}

int swapThreadsInit() {
    int i, nthd = SWAP_THREADS_DEFAULT;

    if (nthd > SWAP_THREADS_MAX) nthd = SWAP_THREADS_MAX;
    server.rocks->threads_num = nthd;

    for (i = 0; i < nthd; i++) {
        swapThread *thread = &server.rocks->threads[i];

        thread->id = i;
        thread->pending_reqs = listCreate();
        atomic_store(&thread->is_running_rio, 0);
        pthread_mutex_init(&thread->lock, NULL);
        pthread_cond_init(&thread->cond, NULL);
        if (pthread_create(&thread->thread_id, NULL, swapThreadMain, thread)) {
            serverLog(LL_WARNING, "Fatal: create swap threads failed.");
            return -1;
        }
    }

    return 0;
}

void swapThreadsDeinit() {
    int i, err;

    for (i = 0; i < server.rocks->threads_num; i++) {
        swapThread *thread = &server.rocks->threads[i];
        listRelease(thread->pending_reqs);
        if (thread->thread_id == pthread_self()) continue;
        if (thread->thread_id && pthread_cancel(thread->thread_id) == 0) {
            if ((err = pthread_join(thread->thread_id, NULL)) != 0) {
                serverLog(LL_WARNING, "swap thread #%d can't be joined: %s",
                        i, strerror(err));
            } else {
                serverLog(LL_WARNING, "swap thread #%d terminated.", i);
            }
        }
    }
}

static int swapThreadsDistNext() {
    static int dist;
    dist++;
    if (dist < 0) dist = 0;
    return dist;
}

void swapThreadsDispatch(swapRequest *req) {
    int idx = swapThreadsDistNext() % server.rocks->threads_num;
    swapThread *t = server.rocks->threads + idx;
    pthread_mutex_lock(&t->lock);
    listAddNodeTail(t->pending_reqs, req);
    pthread_cond_signal(&t->cond);
    pthread_mutex_unlock(&t->lock);
}

int swapThreadsDrained() {
    swapThread *rt;
    int drained = 1, i;
    for (i = 0; i < server.rocks->threads_num; i++) {
        rt = &server.rocks->threads[i];

        pthread_mutex_lock(&rt->lock);
        if (listLength(rt->pending_reqs) || atomic_load(&rt->is_running_rio)) drained = 0;
        pthread_mutex_unlock(&rt->lock);
    }
    return drained;
}

