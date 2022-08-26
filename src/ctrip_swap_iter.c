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

/* TODO currently rocks iter could scan obselete data that are already swapped
 * in, althouth it will be filtered when search db.dict, still it wastes io
 * and cpu cycle. delete those data in customized filter for those obseletes. */

static int rocksIterWaitReady(rocksIter* it) {
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    pthread_mutex_lock(&cq->buffer_lock);
    while (1) {
        /* iterResult ready */
        if (cq->processed_count < cq->buffered_count)
            break;
        /* iter finished */
        if (cq->iter_finished) {
            pthread_mutex_unlock(&cq->buffer_lock);
            return 0;
        }
        /* wait io thread */
        pthread_cond_wait(&cq->ready_cond, &cq->buffer_lock);
    }
    pthread_mutex_unlock(&cq->buffer_lock);
    return 1;
}

static void rocksIterNotifyReady(rocksIter* it, int signal) {
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    pthread_mutex_lock(&cq->buffer_lock);
    cq->buffered_count++;
    if (signal) pthread_cond_signal(&cq->ready_cond);
    pthread_mutex_unlock(&cq->buffer_lock);
}

static void rocksIterNotifyFinshed(rocksIter* it) {
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    pthread_mutex_lock(&cq->buffer_lock);
    cq->iter_finished = 1;
    pthread_cond_signal(&cq->ready_cond);
    pthread_mutex_unlock(&cq->buffer_lock);
}

static int rocksIterWaitVacant(rocksIter *it) {
    int64_t slots, occupied;
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    /* wait untill there are vacant slots in buffer. */
    pthread_mutex_lock(&cq->buffer_lock);
    while (1) {
        occupied = cq->buffered_count - cq->processed_count;
        slots = cq->buffer_capacity - occupied;
        if (slots < 0) {
            serverPanic("CQ slots is negative.");
        } else if (slots == 0) {
            pthread_cond_wait(&cq->vacant_cond, &cq->buffer_lock);
        } else {
            break;
        }
    }
    pthread_mutex_unlock(&cq->buffer_lock);
    return slots;
}

static void rocksIterNotifyVacant(rocksIter* it) {
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    pthread_mutex_lock(&cq->buffer_lock);
    cq->processed_count++;
    pthread_cond_signal(&cq->vacant_cond);
    pthread_mutex_unlock(&cq->buffer_lock);
}


#define MIN(a,b) ((a) < (b) ? (a) : (b))

#define SELECT_META do {    \
    cf = META_CF;           \
    rdbtype = 0;            \
    rawkey = meta_rawkey;   \
    rklen = meta_rvlen;     \
    rawval = meta_rawval;   \
    rvlen = meta_rvlen;     \
    meta_itered++;          \
    rocksdb_iter_next(it->meta_iter);   \
} while (0)

#define SELECT_DATA do {    \
    cf = DATA_CF;           \
    rdbtype = rawval[0];    \
    rawkey = data_rawkey;   \
    rklen = data_rvlen;     \
    rawval = data_rawval;   \
    rvlen = data_rvlen;     \
    rawval++;               \
    rvlen--;                \
    data_itered++;          \
    rocksdb_iter_next(it->data_iter);   \
} while (0)

#define ITER_RATE_LIMIT_INTERVAL_MS 100
void *rocksIterIOThreadMain(void *arg) {
    rocksIter *it = arg;
    size_t meta_itered = 0, data_itered = 0, accumulated_memory = 0;
    mstime_t last_ratelimit_time = mstime();
    int signal;
    bufferedIterCompleteQueue *cq = it->buffered_cq;

    redis_set_thread_title("rocks_iter");

    while (!cq->iter_finished) {
        int64_t slots = rocksIterWaitVacant(it);

        /* there are only one producer, slots will decrease only by current
         * thread, we can produce multiple iterResult in one loop. */
        while (slots--) {
            iterResult *cur;
            int curidx, meta_valid, data_valid, cf;
            unsigned char rdbtype;
            const char *meta_rawkey, *meta_rawval, *data_rawkey, *data_rawval;
            size_t meta_rklen, meta_rvlen, data_rklen, data_rvlen;
            const char  *rawkey, *rawval;
            size_t rklen, rvlen;

            meta_valid = rocksdb_iter_valid(it->meta_iter);
            data_valid = rocksdb_iter_valid(it->data_iter);
            if (!meta_valid && !data_valid) {
                rocksIterNotifyFinshed(it);
                serverLog(LL_WARNING,
                        "Rocks iter thread iterated meta=%ld data=%ld.",
                        meta_itered, data_itered);
                break;
            }

            curidx = cq->buffered_count % cq->buffer_capacity;
            cur = cq->buffered + curidx;

            if (meta_valid) {
                meta_rawkey = rocksdb_iter_key(it->meta_iter,&meta_rklen);
                meta_rawval = rocksdb_iter_value(it->meta_iter,&meta_rvlen);
            }

            if (data_valid) {
                data_rawkey = rocksdb_iter_key(it->data_iter,&data_rklen);
                data_rawval = rocksdb_iter_value(it->data_iter,&data_rvlen);
            }

            if (meta_valid && !data_valid) {
                SELECT_META;
            } else if (!meta_valid && data_valid) {
                SELECT_DATA;
            } else {
                int ret = memcmp(meta_rawkey,data_rawkey,MIN(meta_rklen,data_rklen));
                if (ret < 0) {
                    SELECT_META;
                } else if (ret > 0) {
                    SELECT_DATA;
                } else {
                    if (meta_rklen <= data_rklen) {
                        SELECT_META;
                    } else {
                        SELECT_DATA;
                    }
                }
            }

            accumulated_memory += rklen+rvlen;

#ifdef SWAP_DEBUG
            sds rawkeyrepr = sdscatrepr(sdsempty(),rawkey,rklen);
            sds rawvalrepr = sdscatrepr(sdsempty(),rawval,rvlen);
            serverLog(LL_WARNING, "iterated: rawkey=%s, rawva=%s", rawkeyrepr, rawvalrepr);
            sdsfree(rawkeyrepr);
            sdsfree(rawvalrepr);
#endif
            cur->cf = cf;
            cur->type = rdbtype;
            cur->rawkey = sdsnewlen(rawkey, rklen);
            cur->rawval = sdsnewlen(rawval, rvlen);
            signal = ((data_itered + meta_itered) & (ITER_NOTIFY_BATCH-1)) ? 0 : 1;
            rocksIterNotifyReady(it, signal);

            if (server.swap_max_iter_rate && signal &&
                    mstime() - last_ratelimit_time >
                    ITER_RATE_LIMIT_INTERVAL_MS) {
                mstime_t minimal_timespan = accumulated_memory*1000/server.swap_max_iter_rate;
                mstime_t elapsed_timespan = mstime() - last_ratelimit_time;
                mstime_t sleep_timespan = minimal_timespan - elapsed_timespan;
                if (sleep_timespan > 0) {
                    usleep(sleep_timespan*1000);
                    serverLog(LL_DEBUG, "Rocks iter thread sleep %lld ms: "
                            "memory=%lu, elapsed=%lld, minimal=%lld",
                            sleep_timespan, accumulated_memory,
                            elapsed_timespan, minimal_timespan);
                }
                last_ratelimit_time = mstime();
                accumulated_memory = 0;
            }
        }
    }

    pthread_mutex_lock(&it->io_thread_exit_mutex);
    it->io_thread_exited = 1;
    pthread_mutex_unlock(&it->io_thread_exit_mutex);

    serverLog(LL_WARNING, "Rocks iter thread exit.");

    return NULL;
}

bufferedIterCompleteQueue *bufferedIterCompleteQueueNew(int capacity) {
    int i;
    bufferedIterCompleteQueue* buffered_cq;

    buffered_cq = zmalloc(sizeof(bufferedIterCompleteQueue));
    memset(buffered_cq, 0 ,sizeof(*buffered_cq));
    buffered_cq->buffer_capacity = capacity;

    buffered_cq->buffered = zmalloc(capacity*sizeof(iterResult));
    for (i = 0; i < capacity; i++) {
        iterResult *iter_result = buffered_cq->buffered+i;
        iter_result->rawkey = NULL;
        iter_result->rawval = NULL;
    }

    pthread_mutex_init(&buffered_cq->buffer_lock, NULL);
    pthread_cond_init(&buffered_cq->ready_cond, NULL);
    pthread_mutex_init(&buffered_cq->buffer_lock, NULL);
    pthread_cond_init(&buffered_cq->vacant_cond, NULL);
    return buffered_cq;
}

void bufferedIterCompleteQueueFree(bufferedIterCompleteQueue *buffered_cq) {
    if (buffered_cq == NULL) return;
    for (int i = 0; i < buffered_cq->buffer_capacity;i++) {
        iterResult *res = buffered_cq->buffered+i;
        if (res->rawkey) {
            sdsfree(res->rawkey);
            res->rawkey = NULL;
        }
        if (res->rawval) {
            sdsfree(res->rawval);
            res->rawval = NULL;
        }
    }
    zfree(buffered_cq->buffered);
    pthread_mutex_destroy(&buffered_cq->buffer_lock);
    pthread_cond_destroy(&buffered_cq->ready_cond);
    pthread_mutex_destroy(&buffered_cq->buffer_lock);
    pthread_cond_destroy(&buffered_cq->vacant_cond);
    zfree(buffered_cq);
}

rocksIter *rocksCreateIter(rocks *rocks, redisDb *db) {
    int error;
    const char *default_cf_name = "default";
    const char *meta_cf_name = "meta";
    const char * cf_names[] = {default_cf_name, meta_cf_name};
    rocksdb_iterator_t *data_iter = NULL, *meta_iter = NULL;
    rocksIter *it = zcalloc(sizeof(rocksIter));
    rocksdb_column_family_handle_t *cf_handles[2];
    sds meta_start_key = NULL, data_start_key = NULL;

    it->rocks = rocks;
    it->db = db;
    it->checkpoint_db = NULL;

    if (rocks->checkpoint != NULL) {
        char *errs[2] = {NULL};
        rocksdb_options_t *cf_opts[] = {rocks->data_cf_opts, rocks->meta_cf_opts};
        rocksdb_t* db = rocksdb_open_column_families(rocks->db_opts,
                rocks->checkpoint_dir, 2, cf_names,
                (const rocksdb_options_t *const *)cf_opts,cf_handles,errs);
        if (errs[0] != NULL || errs[1] != NULL) {
            serverLog(LL_WARNING,
                    "[rocks]rocksdb open db fail, dir:%s, default_cf=%s, score_cf=%s",
                    rocks->checkpoint_dir, errs[0], errs[1]);
            goto err;
        }
        it->data_cf = cf_handles[0];
        it->meta_cf = cf_handles[1];
        it->checkpoint_db = db;
    }

    data_iter = rocksdb_create_iterator_cf(it->checkpoint_db, rocks->ropts, it->data_cf);
    meta_iter = rocksdb_create_iterator_cf(it->checkpoint_db, rocks->ropts, it->meta_cf);
    if (data_iter == NULL || meta_iter == NULL) {
        serverLog(LL_WARNING, "Create rocksdb iterator failed.");
        goto err;
    }

    data_start_key = rocksEncodeDataKey(db,NULL,NULL);
    meta_start_key = rocksEncodeMetaKey(db,NULL);
    rocksdb_iter_seek(data_iter,data_start_key,sdslen(data_start_key));
    rocksdb_iter_seek(meta_iter,meta_start_key,sdslen(meta_start_key));
    sdsfree(data_start_key);
    sdsfree(meta_start_key);

    it->data_iter = data_iter;
    it->meta_iter = meta_iter;

    it->buffered_cq = bufferedIterCompleteQueueNew(ITER_BUFFER_CAPACITY_DEFAULT);

    pthread_mutex_init(&it->io_thread_exit_mutex, NULL);
    it->io_thread_exited = 0;
    if ((error = pthread_create(&it->io_thread, NULL, rocksIterIOThreadMain, it))) {
        it->io_thread_exited = 1;
        serverLog(LL_WARNING, "Create rocksdb iterator thread failed: %s.", strerror(error));
        goto err;
    }

    return it;

err:
    rocksReleaseIter(it);
    return NULL;
}

int rocksIterSeekToFirst(rocksIter *it) {
    return rocksIterWaitReady(it);
}

/* rocks iter rawkey, rawval moved. */
void rocksIterCfKeyTypeValue(rocksIter *it, int *cf, sds *rawkey, unsigned char *type, sds *rawval) {
    int idx;
    iterResult *cur;
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    idx = cq->processed_count % cq->buffer_capacity;
    cur = it->buffered_cq->buffered+idx;
    if (cf) *cf = cur->cf;
    if (type) *type = cur->type;
    if (rawkey) {
        *rawkey = cur->rawkey;
        cur->rawkey = NULL;
    }
    if (rawval) {
        *rawval = cur->rawval;
        cur->rawval = NULL;
    }
}

/* Will block untill at least one result is ready.
 * note that rawkey and rawval are owned by rocksIter. */
int rocksIterNext(rocksIter *it) {
    int idx;
    iterResult *cur;
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    idx = cq->processed_count % cq->buffer_capacity;
    cur = it->buffered_cq->buffered+idx;
    /* clear previous state */
    if (cur->rawkey) {
        sdsfree(cur->rawkey);
        cur->rawkey = NULL;
    }

    if (cur->rawval) {
        sdsfree(cur->rawval);
        cur->rawval = NULL;
    }
    rocksIterNotifyVacant(it);
    return rocksIterWaitReady(it);
}

void rocksReleaseIter(rocksIter *it) {
    int err;

    if (it == NULL) return;

    pthread_mutex_lock(&it->io_thread_exit_mutex);
    if (!it->io_thread_exited) {
        if (it->io_thread) {
            if (pthread_cancel(it->io_thread) == 0) {
                if ((err = pthread_join(it->io_thread, NULL)) != 0) {
                    serverLog(LL_WARNING, "Iter io thread can't be joined: %s",
                            strerror(err));
                } else {
                    serverLog(LL_WARNING, "Iter io thread terminated.");
                }
            }
            it->io_thread = 0;
        }
        it->io_thread_exited = 1;
    }
    pthread_mutex_unlock(&it->io_thread_exit_mutex);

    pthread_mutex_destroy(&it->io_thread_exit_mutex);

    if (it->buffered_cq) {
        bufferedIterCompleteQueueFree(it->buffered_cq);
        it->buffered_cq = NULL;
    }

    if (it->data_iter) {
        rocksdb_iter_destroy(it->data_iter);
        it->data_iter = NULL;
    }

    if (it->meta_iter) {
        rocksdb_iter_destroy(it->meta_iter);
        it->meta_iter = NULL;
    }

    if (it->checkpoint_db != NULL) {
        rocksdb_close(it->checkpoint_db);
        it->checkpoint_db = NULL;
    }
    zfree(it);
}

void rocksIterGetError(rocksIter *it, char **perror) {
    char *error;
    rocksdb_iter_get_error(it->data_iter, &error);
    if (error == NULL) rocksdb_iter_get_error(it->meta_iter, &error);
    if (perror) *perror = error;
}

