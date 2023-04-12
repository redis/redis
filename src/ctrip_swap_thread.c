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
            swapRequestBatch *reqs = listNodeValue(ln);
            swapRequestBatchProcess(reqs);
        }

        atomicSetWithSync(thread->is_running_rio, 0);
        listRelease(processing_reqs);
    }

    return NULL;
}

int swapThreadsInit() {
    int i;
    server.swap_defer_thread_idx = server.swap_threads_num;
    server.swap_util_thread_idx = server.swap_threads_num + 1;
    server.total_swap_threads_num = server.swap_threads_num + 2;
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

void swapThreadsDispatch(swapRequestBatch *reqs, int idx) {
    if (idx == -1) {
        idx = swapThreadsDistNext() % server.swap_threads_num;
    } else {
        serverAssert(idx < server.total_swap_threads_num);
    }
    swapRequestBatchDispatched(reqs);
    swapThread *t = server.swap_threads+idx;
    pthread_mutex_lock(&t->lock);
    listAddNodeTail(t->pending_reqs,reqs);
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
    for(int i = 0; i < EXCLUSIVE_TASK_COUNT;i++) {
        manager->stats[i].stat = ROCKSDB_UTILS_TASK_DONE;
    }
    return manager;
}
int isUtilTaskExclusive(int type) {
    return type < EXCLUSIVE_TASK_COUNT;
}
int isRunningUtilTask(rocksdbUtilTaskManager* manager, int type) {
    serverAssert(type < EXCLUSIVE_TASK_COUNT);
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

void checkpointDirPipeWriteHandler(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask) {
    UNUSED(eventLoop),UNUSED(mask);
    checkpointDirPipeWritePayload *pd = clientData;
    ssize_t nwritten;
    if (server.child_pid != pd->waiting_child) {
        serverLog(LL_WARNING, "[rocks] waiting child exit, skip checkpoint dir write");
        goto end;
    }

    ssize_t total = sdslen(pd->data);
    while (1) {
        nwritten = write(fd, pd->data, total - pd->written);
        if (nwritten < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            serverLog(LL_WARNING, "[rocks] write checkpoint dir fail: %s", strerror(errno));
            if (server.child_type == CHILD_TYPE_RDB) killRDBChild();
            else if (server.child_type == CHILD_TYPE_AOF) killAppendOnlyChild();
            goto end;
        }

        pd->written += nwritten;
        if (pd->written == total) {
            serverLog(LL_NOTICE, "[rocks] write checkpoint dir done, %s.", pd->data);
            goto end;
        }
    }

    end:
    aeDeleteFileEvent(server.el, fd, AE_WRITABLE);
    close(fd);
    sdsfree(pd->data);
    zfree(pd);
}

void createCheckpointDone(swapData *data, void *_pd, int errcode) {
    UNUSED(data), UNUSED(errcode);
    rocksdbCreateCheckpointPayload *pd = _pd;
    rocks *rocks = server.rocks;
    if (NULL != rocks->checkpoint) {
        serverLog(LL_WARNING, "[rocks] release old checkpoint.");
        rocksReleaseCheckpoint();
    }
    if (NULL != pd->checkpoint) {
        serverLog(LL_NOTICE, "[rocks] create checkpoint %s.", pd->checkpoint_dir);
        rocks->checkpoint = pd->checkpoint;
        rocks->checkpoint_dir = pd->checkpoint_dir;
    }

    if (pd->waiting_child && server.child_pid == pd->waiting_child) {
        if (NULL == pd->checkpoint_dir) {
            /* create checkpoint fail, send empty str. */
            close(pd->checkpoint_dir_pipe_writing);
        } else {
            sds write_data = sdsdup(pd->checkpoint_dir);
            checkpointDirPipeWritePayload *write_payload = zcalloc(sizeof(checkpointDirPipeWritePayload));
            write_payload->data = write_data;
            write_payload->waiting_child = pd->waiting_child;
            if (aeCreateFileEvent(server.el, pd->checkpoint_dir_pipe_writing,
                                  AE_WRITABLE, checkpointDirPipeWriteHandler,write_payload) == AE_ERR) {
                serverPanic("Unrecoverable error creating checkpoint_dir_pipe_writing file event.");
            }
        }
        /* parent process release snapshot so that rocksdb can continue compacting. */
        /* child process still maintain snapshot copy. */
        rocksReleaseSnapshot();
    }

    zfree(pd);
}

int submitUtilTask(int type, void* pd, sds* error) {
    swapRequest *req = NULL;

    if (isUtilTaskExclusive(type)) {
        if (isRunningUtilTask(server.util_task_manager, type)) {
            if(error != NULL) *error = sdsnew("task running");
            return 0;
        }
        server.util_task_manager->stats[type].stat = ROCKSDB_UTILS_TASK_DOING;
    }

    switch (type) {
        case COMPACT_RANGE_TASK:
            req = swapDataRequestNew(SWAP_UTILS,COMPACT_RANGE_TASK,NULL,NULL,
                    NULL,NULL,compactRangeDone,pd,NULL);
            submitSwapRequest(SWAP_MODE_ASYNC,req,server.swap_util_thread_idx);
            break;
        case GET_ROCKSDB_STATS_TASK:
            req = swapDataRequestNew(SWAP_UTILS,GET_ROCKSDB_STATS_TASK,NULL,
                    NULL,NULL,NULL,getRocksdbStatsDone,pd,NULL);
            submitSwapRequest(SWAP_MODE_ASYNC,req,server.swap_util_thread_idx);
            break;
        case CREATE_CHECKPOINT:
            req = swapDataRequestNew(SWAP_UTILS,CREATE_CHECKPOINT,NULL,NULL,
                    NULL,NULL,createCheckpointDone,pd,NULL);
            submitSwapRequest(SWAP_MODE_ASYNC,req,server.swap_util_thread_idx);
            break;
        default:
            if (error != NULL)
                *error = sdscatprintf(sdsempty(),"unknown util type %d.",type);
            return 0;
    }
    return 1;
}

sds genSwapThreadInfoString(sds info) {
    size_t thread_depth = 0, async_depth;

    pthread_mutex_lock(&server.CQ->lock);
    async_depth = listLength(server.CQ->complete_queue);
    pthread_mutex_unlock(&server.CQ->lock);

    for (int i = 0; i < server.swap_threads_num; i++) {
        swapThread *thread = server.swap_threads+i;
        pthread_mutex_lock(&thread->lock);
        thread_depth += listLength(thread->pending_reqs);
        pthread_mutex_unlock(&thread->lock);
    }
    thread_depth /= server.swap_threads_num;

    info = sdscatprintf(info,
            "swap_thread_queue_depth:%lu\r\n"
            "swap_async_queue_depth:%lu\r\n",
            thread_depth, async_depth);

    return info;
}
