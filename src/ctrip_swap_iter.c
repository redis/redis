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

static inline void iterResultInit(iterResult *result,
        int cf, unsigned char rdbtype,
        MOVE sds rawkey, MOVE sds rawval) {
    result->cf = cf;
    result->rdbtype = rdbtype;
    result->rawkey = rawkey;
    result->rawval = rawval;
#ifdef SWAP_DEBUG
    sds rawkeyrepr = sdscatrepr(sdsempty(),rawkey,sdslen(rawkey));
    sds rawvalrepr = sdscatrepr(sdsempty(),rawval,sdslen(rawval));
    serverLog(LL_WARNING, "iterated: cf=%d, rawkey=%s, rawval=%s",
            cf, rawkeyrepr, rawvalrepr);
    sdsfree(rawkeyrepr);
    sdsfree(rawvalrepr);
#endif
}

#define ITER_RATE_LIMIT_INTERVAL_MS 100

static inline int rocksdbIterValid(rocksdb_iterator_t *iter, sds endkey) {
    const char *rawkey;
    size_t rklen, len;
    if (!rocksdb_iter_valid(iter)) return 0;
    rawkey = rocksdb_iter_key(iter,&rklen);
    len = rklen < sdslen(endkey) ? rklen : sdslen(endkey);
    return memcmp(endkey,rawkey,len) > 0;
}

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
            int curidx, meta_valid, data_valid, cf = -1;
            const char *meta_rawkey = NULL, *meta_rawval = NULL,
                  *data_rawkey = NULL, *data_rawval = NULL;
            size_t meta_rklen, meta_rvlen, data_rklen, data_rvlen;

            meta_valid = rocksdbIterValid(it->meta_iter,it->meta_endkey);
            data_valid = rocksdbIterValid(it->data_iter,it->data_endkey);
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
                cf = META_CF;
            } else if (!meta_valid && data_valid) {
                cf = DATA_CF;
            } else {
                int ret = memcmp(meta_rawkey,data_rawkey,MIN(meta_rklen,data_rklen));
                if (ret < 0) {
                    cf = META_CF;
                } else if (ret > 0) {
                    cf = DATA_CF;
                } else {
                    if (meta_rklen <= data_rklen) {
                        cf = META_CF;
                    } else {
                        cf = DATA_CF;
                    }
                }
            }

            if (cf == META_CF) {
                iterResultInit(cur,cf,-1,sdsnewlen(meta_rawkey,meta_rklen),
                        sdsnewlen(meta_rawval,meta_rvlen));
                meta_itered++;
                accumulated_memory += meta_rklen+meta_rvlen;
                rocksdb_iter_next(it->meta_iter);
            } else { /* DATA_CF */
                unsigned char rdbtype;
                if (data_rvlen == 0) {
                    // set type data only save empty str in rocksdb
                    rdbtype = 0;
                } else {
                    rdbtype = data_rawval[0];
                    data_rawval++, data_rvlen--;
                }
                iterResultInit(cur,cf,rdbtype,sdsnewlen(data_rawkey,data_rklen),
                        sdsnewlen(data_rawval,data_rvlen));
                data_itered++;
                accumulated_memory += data_rklen+data_rvlen;
                rocksdb_iter_next(it->data_iter);
            }

            signal = ((data_itered + meta_itered) & (ITER_NOTIFY_BATCH-1)) ? 0 : 1;
            rocksIterNotifyReady(it, signal);

            if (server.swap_repl_max_rocksdb_read_bps && signal &&
                    mstime() - last_ratelimit_time >
                    ITER_RATE_LIMIT_INTERVAL_MS) {
                mstime_t minimal_timespan = accumulated_memory*1000/server.swap_repl_max_rocksdb_read_bps;
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
    int error, i;
    rocksdb_iterator_t *data_iter = NULL, *meta_iter = NULL;
    rocksIter *it = zcalloc(sizeof(rocksIter));
    sds meta_start_key = NULL, data_start_key = NULL;

    it->rocks = rocks;
    it->db = db;
    it->checkpoint_db = NULL;

    if (rocks->snapshot) rocksdb_readoptions_set_snapshot(rocks->ropts, rocks->snapshot);
    if (rocks->checkpoint_dir != NULL) {
        serverLog(LL_WARNING, "[rocks] create iter from checkpoint %s.", rocks->checkpoint_dir);
        rocksdb_options_t* cf_opts[CF_COUNT];
        for (i = 0; i < CF_COUNT; i++) {
            /* disable cf cache since cache is useless for iterator */
            cf_opts[i] = rocksdb_options_create_copy(server.rocks->cf_opts[i]);
            rocksdb_block_based_table_options_t* block_opt = rocksdb_block_based_options_create();
            rocksdb_block_based_options_set_no_block_cache(block_opt, 1);
            rocksdb_options_set_block_based_table_factory(cf_opts[i], block_opt);
            rocksdb_block_based_options_destroy(block_opt);
        }

        char *errs[CF_COUNT] = {NULL};
        rocksdb_t* checkpoint_db = rocksdb_open_column_families(rocks->db_opts,
                rocks->checkpoint_dir, CF_COUNT, swap_cf_names,
                (const rocksdb_options_t *const *)cf_opts,
                it->cf_handles, errs);
        for (i = 0; i < CF_COUNT; i++) rocksdb_options_destroy(cf_opts[i]);

        if (errs[0] || errs[1] || errs[2]) {
            serverLog(LL_WARNING,
                    "[rocks] rocksdb open db fail, dir:%s, default_cf=%s, meta_cf=%s, score_cf=%s",
                    rocks->checkpoint_dir, errs[0], errs[1], errs[2]);
            goto err;
        }
        it->checkpoint_db = checkpoint_db;
        data_iter = rocksdb_create_iterator_cf(it->checkpoint_db, rocks->ropts,
                it->cf_handles[DATA_CF]);
        meta_iter = rocksdb_create_iterator_cf(it->checkpoint_db, rocks->ropts,
                it->cf_handles[META_CF]);
    } else {
        serverLog(LL_WARNING, "[rocks] create iter from rocksdb.");
        data_iter = rocksdb_create_iterator_cf(rocks->db, rocks->ropts,
                rocks->cf_handles[DATA_CF]);
        meta_iter = rocksdb_create_iterator_cf(rocks->db, rocks->ropts,
                rocks->cf_handles[META_CF]);
    }

    if (data_iter == NULL || meta_iter == NULL) {
        serverLog(LL_WARNING, "Create rocksdb iterator failed.");
        goto err;
    }

    data_start_key = rocksEncodeDbRangeStartKey(db->id);
    meta_start_key = rocksEncodeDbRangeStartKey(db->id);
    rocksdb_iter_seek(data_iter,data_start_key,sdslen(data_start_key));
    rocksdb_iter_seek(meta_iter,meta_start_key,sdslen(meta_start_key));
    sdsfree(data_start_key);
    sdsfree(meta_start_key);

    it->data_iter = data_iter;
    it->meta_iter = meta_iter;
    it->data_endkey = rocksEncodeDbRangeEndKey(db->id);
    it->meta_endkey = rocksEncodeDbRangeEndKey(db->id);

    it->buffered_cq = bufferedIterCompleteQueueNew(ITER_BUFFER_CAPACITY_DEFAULT);

    if ((error = pthread_create(&it->io_thread, NULL, rocksIterIOThreadMain, it))) {
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
void rocksIterCfKeyTypeValue(rocksIter *it, int *cf, sds *rawkey, unsigned char *rdbtype, sds *rawval) {
    int idx;
    iterResult *cur;
    bufferedIterCompleteQueue *cq = it->buffered_cq;
    idx = cq->processed_count % cq->buffer_capacity;
    cur = it->buffered_cq->buffered+idx;
    if (cf) *cf = cur->cf;
    if (rdbtype) *rdbtype = cur->rdbtype;
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
    int err, i;

    if (it == NULL) return;

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

    if (it->buffered_cq) {
        bufferedIterCompleteQueueFree(it->buffered_cq);
        it->buffered_cq = NULL;
    }

    for (i = 0; i < CF_COUNT; i++) {
        if (it->cf_handles[i]) {
            rocksdb_column_family_handle_destroy(it->cf_handles[i]);
        }
    }

    if (it->data_iter) {
        rocksdb_iter_destroy(it->data_iter);
        it->data_iter = NULL;
    }

    if (it->meta_iter) {
        rocksdb_iter_destroy(it->meta_iter);
        it->meta_iter = NULL;
    }

    if (it->data_endkey) {
        sdsfree(it->data_endkey);
        it->data_endkey = NULL;
    }

    if (it->meta_endkey) {
        sdsfree(it->meta_endkey);
        it->meta_endkey = NULL;
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

#ifdef REDIS_TEST

void dbg_rocksdb_put_cf(
    rocksdb_t* db, const rocksdb_writeoptions_t* options,
    rocksdb_column_family_handle_t* column_family, const char* key,
    size_t keylen, const char* val, size_t vallen, char** errptr) {

    sds rawkeyrepr = sdscatrepr(sdsempty(),key,keylen);
    sds rawvalrepr = sdscatrepr(sdsempty(),val,vallen);
    serverLog(LL_WARNING, "save: rawkey=%s, rawva=%s", rawkeyrepr, rawvalrepr);
    sdsfree(rawkeyrepr);
    sdsfree(rawvalrepr);

    rocksdb_put_cf(db,options,column_family,key,keylen,val,vallen,errptr);
}

#define PUT_META(redisdb,object_type,key_,expire) do {      \
    char *err = NULL;                                       \
    sds keysds = rocksEncodeMetaKey(redisdb, key_);         \
    sds valsds = rocksEncodeMetaVal(object_type,expire,0,NULL);\
    rocksdb_put_cf(server.rocks->db,server.rocks->wopts,    \
            server.rocks->cf_handles[META_CF],              \
            keysds,sdslen(keysds),valsds,sdslen(valsds),&err);\
    serverAssert(err == NULL);                              \
    sdsfree(keysds), sdsfree(valsds);                       \
} while (0)

#define PUT_DATA(redisdb,key_,subkey_,val_) do {           \
    char *err = NULL;                                       \
    sds keysds = rocksEncodeDataKey(redisdb,key_,0,subkey_);\
    sds valsds = rocksEncodeValRdb(val_);                   \
    rocksdb_put_cf(server.rocks->db,server.rocks->wopts,    \
            server.rocks->cf_handles[DATA_CF],              \
            keysds,sdslen(keysds),valsds,sdslen(valsds),&err);\
    serverAssert(err == NULL);                              \
    sdsfree(keysds), sdsfree(valsds);                       \
} while (0)

int doRocksdbFlush();
void initServerConfig(void);
int rdbSaveRocksIterDecode(rocksIter *it, decodedResult *decoded, rdbSaveRocksStats *stats);

void prepareDataForDb(redisDb *db) {
    sds ha = sdsnew("ha"), h = sdsnew("h"), field_a = sdsnew("a");
    robj *val = createStringObject("val", 4);

    PUT_META(db,OBJ_STRING,ha,-1);
    PUT_DATA(db,ha,NULL,val);
    PUT_META(db,OBJ_HASH,h,-1);
    PUT_DATA(db,h,field_a,val);

    sdsfree(ha), sdsfree(h), sdsfree(field_a);
    decrRefCount(val);
}

void validateRocksIterForDb(redisDb *db) {
    decodedResult decoded_ = {0}, *decoded = &decoded_;
    rocksIter *it = rocksCreateIter(server.rocks,db);
    rdbSaveRocksStats stats_ = {0}, *stats = &stats_;
    sds ha = sdsnew("ha"), h = sdsnew("h"), field_a = sdsnew("a");

    serverAssert(rocksIterSeekToFirst(it));
    rdbSaveRocksIterDecode(it,decoded,stats);
    serverAssert(decoded->cf == META_CF);
    serverAssert(!sdscmp(decoded->key,h));
    decodedResultDeinit(decoded);

    serverAssert(rocksIterNext(it));
    rdbSaveRocksIterDecode(it,decoded,stats);
    serverAssert(decoded->cf == DATA_CF);
    serverAssert(!sdscmp(decoded->key, h));
    decodedResultDeinit(decoded);

    serverAssert(rocksIterNext(it));
    rdbSaveRocksIterDecode(it,decoded,stats);
    serverAssert(decoded->cf == META_CF);
    serverAssert(!sdscmp(decoded->key, ha));
    decodedResultDeinit(decoded);

    serverAssert(rocksIterNext(it));
    rdbSaveRocksIterDecode(it,decoded,stats);
    serverAssert(decoded->cf == DATA_CF);
    serverAssert(!sdscmp(decoded->key, ha));
    decodedResultDeinit(decoded);

    serverAssert(!rocksIterNext(it));

    rocksReleaseIter(it);
    sdsfree(ha), sdsfree(h), sdsfree(field_a);
}

int swapIterTest(int argc, char *argv[], int accurate) {
    UNUSED(argc), UNUSED(argv), UNUSED(accurate);

    int error = 0;
    server.hz = 10;

    initTestRedisDb();
    redisDb *db = server.db, *db1 = server.db+1;

    TEST("iter: init") {
        initServerConfig();
        if (!server.rocks) rocksInit();
    }

    TEST("iter: basic") {
        prepareDataForDb(db);
        doRocksdbFlush();
        validateRocksIterForDb(db);
    }

    TEST("iter: multi db") {
        prepareDataForDb(db);
        prepareDataForDb(db1);
        doRocksdbFlush();
        validateRocksIterForDb(db);
        validateRocksIterForDb(db1);
    }

    return error;
}


#endif

