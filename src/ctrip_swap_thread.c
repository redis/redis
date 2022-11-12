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
    listIter li;
    listNode *ln;
    list *processing_reqs;
    while (1) {
        pthread_mutex_lock(&thread->lock);
        while (listLength(thread->pending_reqs) == 0)
            pthread_cond_wait(&thread->cond, &thread->lock);

        listRewind(thread->pending_reqs, &li);
        processing_reqs = listCreate();
        while ((ln = listNext(&li))) {
            swapRequest *req = listNodeValue(ln);
            listAddNodeHead(processing_reqs, req);
            listDelNode(thread->pending_reqs, ln);
        }
        pthread_mutex_unlock(&thread->lock);

        listRewind(processing_reqs, &li);
        atomicSetWithSync(thread->is_running_rio, 1);
        while ((ln = listNext(&li))) {
            swapRequest *req = listNodeValue(ln);
            if (req->swap_queue_timer) {
                metricDebugInfo(SWAP_DEBUG_SWAP_QUEUE_WAIT, elapsedUs(req->swap_queue_timer));
            }
            processSwapRequest(req);
        }

        atomicSetWithSync(thread->is_running_rio, 0);
        listRelease(processing_reqs);
    }

    return NULL;
}

int swapThreadsInit() {
    int i;
    server.total_swap_threads_num = server.swap_threads_num + 1;
    server.swap_threads = zcalloc(sizeof(swapThread)*server.total_swap_threads_num);
    for (i = 0; i < server.total_swap_threads_num; i++) {
        swapThread *thread = server.swap_threads+i;
        thread->id = i;
        thread->pending_reqs = listCreate();
        atomicSetWithSync(thread->is_running_rio, 0);
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
    for (i = 0; i < server.total_swap_threads_num; i++) {
        swapThread *thread = server.swap_threads+i;
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

static inline int swapThreadsDistNext() {
    static int dist;
    dist++;
    if (dist < 0) dist = 0;
    return dist;
}

void swapThreadsDispatch(swapRequest *req, int idx) {
    if (idx == -1) {
        idx = swapThreadsDistNext() % server.swap_threads_num;
    } else {
        serverAssert(idx <= server.swap_threads_num);
    }
    if (server.swap_debug) elapsedStart(&req->swap_queue_timer);
    swapThread *t = server.swap_threads+idx;
    pthread_mutex_lock(&t->lock);
    listAddNodeTail(t->pending_reqs,req);
    pthread_cond_signal(&t->cond);
    pthread_mutex_unlock(&t->lock);
}

int swapThreadsDrained() {
    swapThread *rt;
    int drained = 1, i;
    for (i = 0; i < server.total_swap_threads_num; i++) {
        rt = server.swap_threads+i;

        pthread_mutex_lock(&rt->lock);
        unsigned long count = 0;
        atomicGetWithSync(rt->is_running_rio, count);
        if (listLength(rt->pending_reqs) || count) drained = 0;
        pthread_mutex_unlock(&rt->lock);
    }
    return drained;
}

// utils task
#define ROCKSDB_UTILS_TASK_DONE 0
#define ROCKSDB_UTILS_TASK_DOING 1

rocksdbUtilTaskManager* createRocksdbUtilTaskManager() {
    rocksdbUtilTaskManager * manager = zmalloc(sizeof(rocksdbUtilTaskManager));
    for(int i = 0; i < TASK_COUNT;i++) {
        manager->stats[i].stat = ROCKSDB_UTILS_TASK_DONE;
    }
    return manager;
}
int isRunningUtilTask(rocksdbUtilTaskManager* manager, int type) {
    serverAssert(type < TASK_COUNT);
    return manager->stats[type].stat == ROCKSDB_UTILS_TASK_DOING;
}

void compactRangeDone(swapData *data, void *pd, int errcode){
    UNUSED(data),UNUSED(pd),UNUSED(errcode);
    server.util_task_manager->stats[COMPACT_RANGE_TASK].stat = ROCKSDB_UTILS_TASK_DONE;
}

void getRocksdbStatsDone(swapData *data, void *pd, int errcode) {
    UNUSED(data),UNUSED(pd),UNUSED(errcode);
    if (pd != NULL) {
        if (server.rocks->rocksdb_stats_cache != NULL)  {
            for(int i = 0; i < CF_COUNT; i++) {
                zlibc_free(server.rocks->rocksdb_stats_cache[i]);
            }
        }
        zfree(server.rocks->rocksdb_stats_cache);
        server.rocks->rocksdb_stats_cache = pd;
    }
    server.util_task_manager->stats[GET_ROCKSDB_STATS_TASK].stat = ROCKSDB_UTILS_TASK_DONE;
}

int submitUtilTask(int type, void* pd, sds* error) {
    if (isRunningUtilTask(server.util_task_manager, type)) {
        if(error != NULL) *error = sdsnew("task running");
        return 0;
    }
    serverAssert(type < TASK_COUNT);
    server.util_task_manager->stats[type].stat = ROCKSDB_UTILS_TASK_DOING;
    switch (type) {
        case COMPACT_RANGE_TASK:
            submitSwapDataRequest(SWAP_MODE_ASYNC,ROCKSDB_UTILS,0,NULL,
                    NULL,NULL,compactRangeDone,pd,NULL,server.swap_threads_num);
            break;
        case GET_ROCKSDB_STATS_TASK:
            submitSwapDataRequest(SWAP_MODE_ASYNC, ROCKSDB_UTILS,1,NULL,
                    NULL,NULL,getRocksdbStatsDone,pd,NULL,server.swap_threads_num);
            break;
        default:
            break;
    }
    return 1;
}

