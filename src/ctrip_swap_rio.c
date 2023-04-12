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

#define RIO_ITERATE_NUMKEYS_ALLOC_INIT 8
#define RIO_ITERATE_NUMKEYS_ALLOC_LINER 4096
#define RIO_ESTIMATE_PAYLOAD_SAMPLE 8

#define MIN(a,b) ((a) > (b) ? (b): (a))

static inline void RIOInitGeneric(RIO *rio, int action, int numkeys,
        int *cfs, sds *rawkeys, sds *rawvals) {
    rio->action = action;
    rio->generic.numkeys = numkeys;
    rio->generic.cfs = cfs;
    rio->generic.rawkeys = rawkeys;
    rio->generic.rawvals = rawvals;
    rio->generic.notfound = 0;
    rio->err = NULL;
    rio->errcode = 0;
}

void RIOInitGet(RIO *rio, int numkeys, int *cfs, sds *rawkeys) {
    RIOInitGeneric(rio,ROCKS_GET,numkeys,cfs,rawkeys,NULL);
}

void RIOInitPut(RIO *rio, int numkeys, int *cfs, sds *rawkeys, sds *rawvals) {
    RIOInitGeneric(rio,ROCKS_PUT,numkeys,cfs,rawkeys,rawvals);
}

void RIOInitDel(RIO *rio, int numkeys, int *cfs, sds *rawkeys) {
    RIOInitGeneric(rio,ROCKS_DEL,numkeys,cfs,rawkeys,NULL);
}

void RIOInitIterate(RIO *rio, int cf, uint32_t flags, sds start, sds end, size_t limit) {
    rio->action = ROCKS_ITERATE;
    rio->iterate.cf = cf;
    rio->iterate.flags = flags;
    rio->iterate.start = start;
    rio->iterate.end = end;
    rio->iterate.limit = limit;
    rio->iterate.numkeys = 0;
    rio->iterate.rawkeys = NULL;
    rio->iterate.rawvals = NULL;
    rio->iterate.nextseek = NULL;
    rio->err = NULL;
    rio->errcode = 0;
}

void RIODeinit(RIO *rio) {
    int i;

    if (rio->err) {
        sdsfree(rio->err);
        rio->err = NULL;
    }
    switch (rio->action) {
    case  ROCKS_GET:
    case  ROCKS_PUT:
    case  ROCKS_DEL:
        for (i = 0; i < rio->generic.numkeys; i++) {
            if (rio->generic.rawkeys) sdsfree(rio->generic.rawkeys[i]);
            if (rio->generic.rawvals) sdsfree(rio->generic.rawvals[i]);
        }
        zfree(rio->generic.cfs);
        rio->generic.cfs = NULL;
        zfree(rio->generic.rawkeys);
        rio->generic.rawkeys = NULL;
        zfree(rio->generic.rawvals);
        rio->generic.rawvals = NULL;
        break;
    case ROCKS_ITERATE:
        if (rio->iterate.start) {
            sdsfree(rio->iterate.start);
            rio->iterate.start = NULL;
        }
        if (rio->iterate.end) {
            sdsfree(rio->iterate.end);
            rio->iterate.end = NULL;
        }
        for (i = 0; i < rio->iterate.numkeys; i++) {
            if (rio->iterate.rawkeys) sdsfree(rio->iterate.rawkeys[i]);
            if (rio->iterate.rawvals) sdsfree(rio->iterate.rawvals[i]);
        }
        if (rio->iterate.rawkeys) zfree(rio->iterate.rawkeys);
        rio->iterate.rawkeys = NULL;
        if (rio->iterate.rawvals) zfree(rio->iterate.rawvals);
        rio->iterate.rawvals = NULL;
        if (rio->iterate.nextseek) sdsfree(rio->iterate.nextseek);
        rio->iterate.nextseek = NULL;
        break;
    default:
        break;
    }
}

void RIODoGet(RIO *rio) {
    int i;
    rocksdb_column_family_handle_t **cfs_list;
    char **keys_list = zmalloc(rio->get.numkeys*sizeof(char*));
    char **values_list = zmalloc(rio->get.numkeys*sizeof(char*));
    size_t *keys_list_sizes = zmalloc(rio->get.numkeys*sizeof(size_t));
    size_t *values_list_sizes = zmalloc(rio->get.numkeys*sizeof(size_t));
    char **errs = zmalloc(rio->get.numkeys*sizeof(char*));
    cfs_list = zmalloc(rio->get.numkeys*sizeof(rocksdb_column_family_handle_t*));

    for (i = 0; i < rio->get.numkeys; i++) {
        cfs_list[i] = swapGetCF(rio->get.cfs[i]);
        keys_list[i] = rio->get.rawkeys[i];
        keys_list_sizes[i] = sdslen(rio->get.rawkeys[i]);
    }

    rocksdb_multi_get_cf(server.rocks->db, server.rocks->ropts,
            (const rocksdb_column_family_handle_t *const *)cfs_list,
            rio->get.numkeys,
            (const char**)keys_list, (const size_t*)keys_list_sizes,
            values_list, values_list_sizes, errs);

    rio->get.rawvals = zmalloc(rio->get.numkeys*sizeof(sds));
    for (i = 0; i < rio->get.numkeys; i++) {
        if (values_list[i] == NULL) {
            rio->get.rawvals[i] = NULL;
            rio->get.notfound++;
        } else {
            rio->get.rawvals[i] = sdsnewlen(values_list[i],
                    values_list_sizes[i]);
            zlibc_free(values_list[i]);
        }
        if (errs[i]) {
            if (!RIOGetError(rio)) {
                RIOSetError(rio,SWAP_ERR_RIO_GET_FAIL,sdsnew(errs[i]));
                serverLog(LL_WARNING,"[rocks] do rocksdb get failed: %s",
                        rio->err);
            }
            zlibc_free(errs[i]);
        }
    }

    zfree(cfs_list);
    zfree(keys_list);
    zfree(values_list);
    zfree(keys_list_sizes);
    zfree(values_list_sizes);
    zfree(errs);
}

static void RIODoPut(RIO *rio) {
    char *err = NULL;
    rocksdb_writebatch_t *wb = rocksdb_writebatch_create();

    for (int i = 0; i < rio->put.numkeys; i++) {
        rocksdb_writebatch_put_cf(wb,swapGetCF(rio->put.cfs[i]),
                rio->put.rawkeys[i],sdslen(rio->put.rawkeys[i]),
                rio->put.rawvals[i],sdslen(rio->put.rawvals[i]));
    }

    rocksdb_write(server.rocks->db,server.rocks->wopts,wb,&err);
    if (err != NULL) {
        RIOSetError(rio,SWAP_ERR_RIO_PUT_FAIL,sdsnew(err));
        serverLog(LL_WARNING,"[rocks] do rocksdb put failed: %s",rio->err);
        zlibc_free(err);
    }
    rocksdb_writebatch_destroy(wb);
}

static void RIODoDel(RIO *rio) {
    char *err = NULL;
    rocksdb_writebatch_t *wb = rocksdb_writebatch_create();

    for (int i = 0; i < rio->del.numkeys; i++) {
        rocksdb_writebatch_delete_cf(wb,swapGetCF(rio->del.cfs[i]),
                rio->put.rawkeys[i],sdslen(rio->put.rawkeys[i]));
    }

    rocksdb_write(server.rocks->db,server.rocks->wopts,wb,&err);
    if (err != NULL) {
        RIOSetError(rio,SWAP_ERR_RIO_DEL_FAIL,sdsnew(err));
        serverLog(LL_WARNING,"[rocks] do rocksdb put failed: %s",rio->err);
        zlibc_free(err);
    }
    rocksdb_writebatch_destroy(wb);
}

static void RIODoIterate(RIO *rio) {
    size_t numkeys = 0;
    char *err = NULL;
    rocksdb_iterator_t *iter = NULL;
    sds start = rio->iterate.start;
    sds end = rio->iterate.end;
    size_t limit = rio->iterate.limit;
    rocksdb_readoptions_t *ropts = NULL;

    int reverse = rio->iterate.flags & ROCKS_ITERATE_REVERSE;
    int low_bound_exclude = rio->iterate.flags & ROCKS_ITERATE_LOW_BOUND_EXCLUDE;
    int high_bound_exclude = rio->iterate.flags & ROCKS_ITERATE_HIGH_BOUND_EXCLUDE;
    int next_seek = rio->iterate.flags & ROCKS_ITERATE_CONTINUOUSLY_SEEK;
    int disable_cache = rio->iterate.flags & ROCKS_ITERATE_DISABLE_CACHE;

    size_t numalloc = ROCKS_ITERATE_NO_LIMIT == limit ? RIO_ITERATE_NUMKEYS_ALLOC_INIT : limit;
    numalloc = numalloc > RIO_ITERATE_NUMKEYS_ALLOC_LINER ? RIO_ITERATE_NUMKEYS_ALLOC_LINER : numalloc;
    sds *rawkeys = zmalloc(numalloc*sizeof(sds));
    sds *rawvals = zmalloc(numalloc*sizeof(sds));

    size_t klen, vlen;
    const char *rawkey, *rawval;
    size_t start_len = start ? sdslen(start) : 0, end_len = end ? sdslen(end) : 0;

    if (start == NULL && end == NULL) goto end;

    if (disable_cache) {
        ropts = rocksdb_readoptions_create();
        rocksdb_readoptions_set_verify_checksums(ropts, 0);
        rocksdb_readoptions_set_fill_cache(ropts, 0);
    }
    iter = rocksdb_create_iterator_cf(server.rocks->db,NULL!=ropts?ropts:server.rocks->ropts,swapGetCF(rio->iterate.cf));

    if (reverse) rocksdb_iter_seek_for_prev(iter,end,end_len);
    else rocksdb_iter_seek(iter, start, start_len);
    if (!rocksdb_iter_valid(iter)) goto end;

    if (reverse && high_bound_exclude) {
        rawkey = rocksdb_iter_key(iter, &klen);
        if (end_len == klen && 0 == memcmp(rawkey, end, end_len)) {
            rocksdb_iter_prev(iter);
        }
    } else if (!reverse && low_bound_exclude) {
        rawkey = rocksdb_iter_key(iter, &klen);
        if (start_len == klen && 0 == memcmp(rawkey, start, start_len))
            rocksdb_iter_next(iter);
    }

    sds bound = reverse ? start : end;
    size_t bound_len = reverse ? start_len : end_len;
    int bound_exclude = reverse ? low_bound_exclude : high_bound_exclude;
    while (rocksdb_iter_valid(iter) && (limit == ROCKS_ITERATE_NO_LIMIT || numkeys < limit)) {
        rawkey = rocksdb_iter_key(iter, &klen);
        if (bound) {
            int cmp_result = memcmp(rawkey, bound, MIN(bound_len, klen));
            if (0 == cmp_result) {
                if (bound_len != klen) cmp_result = klen > bound_len;
                else if (bound_exclude) break;
            }
            if ((reverse && cmp_result < 0) || (!reverse && cmp_result > 0)) break;
        }

        rawval = rocksdb_iter_value(iter, &vlen);
        numkeys++;

        if (numkeys > numalloc) {
            if (numalloc >= RIO_ITERATE_NUMKEYS_ALLOC_LINER) {
                numalloc += RIO_ITERATE_NUMKEYS_ALLOC_LINER;
            } else {
                numalloc *= 2;
            }
            rawkeys = zrealloc(rawkeys, numalloc*sizeof(sds));
            rawvals = zrealloc(rawvals, numalloc*sizeof(sds));
        }
        rawkeys[numkeys - 1] = sdsnewlen(rawkey, klen);
        rawvals[numkeys - 1] = sdsnewlen(rawval, vlen);

        if (reverse) rocksdb_iter_prev(iter);
        else rocksdb_iter_next(iter);
    }

    rocksdb_iter_get_error(iter, &err);
    if (err != NULL) {
        RIOSetError(rio,SWAP_ERR_RIO_ITER_FAIL,sdsnew(err));
        serverLog(LL_WARNING,"[rocks] do rocksdb iterate failed: %s", err);
        zlibc_free(err);
    }

    /* save next seek */
    if (next_seek && rocksdb_iter_valid(iter)) {
        rawkey = rocksdb_iter_key(iter, &klen);
        rio->iterate.nextseek = sdsnewlen(rawkey, klen);
    }

    end:
    rio->iterate.numkeys = numkeys;
    rio->iterate.rawkeys = rawkeys;
    rio->iterate.rawvals = rawvals;

    if (iter) rocksdb_iter_destroy(iter);
    if (ropts) rocksdb_readoptions_destroy(ropts);
}

static sds RIODumpGeneric(RIO *rio, sds repr) {
    repr = sdscatfmt(repr, "%s:\n", rocksActionName(rio->action));
    for (int i = 0; i < rio->generic.numkeys; i++) {
        repr = sdscatprintf(repr, "  ([%s] ", swapGetCFName(rio->generic.cfs[i]));
        repr = sdscatrepr(repr,rio->generic.rawkeys[i],sdslen(rio->generic.rawkeys[i]));
        repr = sdscat(repr, ")=>(");
        if (rio->generic.rawvals && rio->generic.rawvals[i]) {
            repr = sdscatrepr(repr,rio->generic.rawvals[i],sdslen(rio->generic.rawvals[i]));
        } else {
            repr = sdscatfmt(repr, "<nil>");
        }
        repr = sdscat(repr,")\n");
    }
    return repr;
}

void RIODump(RIO *rio) {
    sds repr = sdsnew("[RIO] ");
    switch (rio->action) {
    case ROCKS_GET:
    case ROCKS_PUT:
    case ROCKS_DEL:
        repr = RIODumpGeneric(rio,repr);
        break;
    case ROCKS_ITERATE:
        repr = sdscatprintf(repr, "ITERATE [%s]: (flags=%d,limit=%lu",swapGetCFName(rio->iterate.cf),
                            rio->iterate.flags, rio->iterate.limit);
        if (rio->iterate.start) {
            repr = sdscat(repr, ",start=");
            repr = sdscatrepr(repr, rio->iterate.start, sdslen(rio->iterate.start));
        }
        if (rio->iterate.end) {
            repr = sdscat(repr, ",end=");
            repr = sdscatrepr(repr, rio->iterate.end, sdslen(rio->iterate.end));
        }
        repr = sdscat(repr, ")\n");
        for (int i = 0; i < rio->iterate.numkeys; i++) {
            repr = sdscat(repr, "  (");
            repr = sdscatrepr(repr, rio->iterate.rawkeys[i],
                              sdslen(rio->iterate.rawkeys[i]));
            repr = sdscat(repr, ")=>(");
            repr = sdscatrepr(repr, rio->iterate.rawvals[i],
                              sdslen(rio->iterate.rawvals[i]));
            repr = sdscat(repr,")\n");
        }
        if (rio->iterate.nextseek) {
            repr = sdscat(repr, "nextseek=");
            repr = sdscatrepr(repr, rio->iterate.nextseek, sdslen(rio->iterate.nextseek));
            repr = sdscat(repr,"\n");
        }
        break;
    default:
        serverPanic("[rocks] Unknown io action: %d", rio->action);
        break;
    }
    serverLog(LL_NOTICE, "%s", repr);
    sdsfree(repr);
}

static inline int RIOGetCF(RIO *rio) {
    int cf;
    if (rio->action == ROCKS_ITERATE) {
        cf = rio->iterate.cf;
    } else if (rio->generic.numkeys > 0) {
        cf = rio->generic.cfs[0];
    } else {
        cf = META_CF;
    };
    return cf;
}

void RIODo(RIO *rio) {
    monotime io_timer;

    elapsedStart(&io_timer);

    if (server.swap_debug_rio_delay_micro)
        usleep(server.swap_debug_rio_delay_micro);

    if (server.swap_debug_rio_error > 0) {
        server.swap_debug_rio_error--;
        RIOSetError(rio,SWAP_ERR_RIO_FAIL,sdsnew("rio mock error"));
        goto end;
    }

    switch (rio->action) {
    case ROCKS_GET:
        RIODoGet(rio);
        break;
    case ROCKS_PUT:
        RIODoPut(rio);
        break;
    case ROCKS_DEL:
        RIODoDel(rio);
        break;
    case ROCKS_ITERATE:
        RIODoIterate(rio);
        break;
    default:
        serverPanic("[RIO] Unknown io action: %d", rio->action);
    }

#ifdef ROCKS_DEBUG
    RIODump(rio);
#endif

end:
    if (RIOGetCF(rio) != META_CF) {
        RIOUpdateStatsDo(rio, elapsedUs(io_timer));
        RIOUpdateStatsDataNotFound(rio);
    }
}

size_t RIOEstimatePayloadSize(RIO *rio) {
    int i;
    size_t memory = 0;

    switch (rio->action) {
    case ROCKS_GET:
    case ROCKS_PUT:
    case ROCKS_DEL:
        for (i = 0; i < rio->get.numkeys && i < RIO_ESTIMATE_PAYLOAD_SAMPLE; i++) {
            memory += sdsalloc(rio->get.rawkeys[i]);
            if (rio->get.rawvals && rio->get.rawvals[i])
                memory += sdsalloc(rio->get.rawvals[i]);
        }
        if (rio->get.numkeys > RIO_ESTIMATE_PAYLOAD_SAMPLE) {
            memory = memory*rio->get.numkeys/RIO_ESTIMATE_PAYLOAD_SAMPLE;
        }

        break;
    case ROCKS_ITERATE:
        for (i = 0; i < rio->iterate.numkeys && i < RIO_ESTIMATE_PAYLOAD_SAMPLE; i++) {
            memory += sdsalloc(rio->iterate.rawkeys[i]);
            if (rio->iterate.rawvals && rio->iterate.rawvals[i])
                memory += sdsalloc(rio->iterate.rawvals[i]);
        }
        if (rio->get.numkeys > RIO_ESTIMATE_PAYLOAD_SAMPLE) {
            memory = memory*rio->get.numkeys/RIO_ESTIMATE_PAYLOAD_SAMPLE;
        }
        break;
    default:
        break;
    }

    return memory;
}

void RIOUpdateStatsDo(RIO *rio, long duration) {
    int action = rio->action;
    size_t payload_size = RIOEstimatePayloadSize(rio);
    atomicIncr(server.ror_stats->rio_stats[action].memory,payload_size);
    atomicIncr(server.ror_stats->rio_stats[action].count,1);
    atomicIncr(server.ror_stats->rio_stats[action].batch,1);
    atomicIncr(server.ror_stats->rio_stats[action].time,duration);
}

void RIOUpdateStatsDataNotFound(RIO *rio) {
    if (rio->action == ROCKS_GET && rio->get.notfound) {
        atomicIncr(server.swap_hit_stats->stat_swapin_data_not_found_count,
                rio->get.notfound);
    }
}


void RIOBatchInit(RIOBatch *rios, int action) {
    rios->rios = rios->rio_buf;
    rios->capacity = SWAP_BATCH_DEFAULT_SIZE;
    rios->count = 0;
    rios->action = action;
}

void RIOBatchDeinit(RIOBatch *rios) {
    if (rios == NULL) return;
    for (size_t i = 0; i < rios->count; i++) {
        RIO *rio = rios->rios+i;
        RIODeinit(rio);
    }
    rios->count = 0;
    if (rios->rios != rios->rio_buf) {
        zfree(rios->rios);
        rios->rios = NULL;
    }
}

/* Note that Alloc may invalidate previous RIO pointer. */
RIO *RIOBatchAlloc(RIOBatch *rios) {
    if (rios->count == rios->capacity) {
        rios->capacity = rios->capacity < SWAP_BATCH_LINEAR_SIZE ? rios->capacity*2 : rios->capacity + SWAP_BATCH_LINEAR_SIZE;
        serverAssert(rios->capacity > rios->count);
        if (rios->rios == rios->rio_buf) {
            rios->rios = zmalloc(sizeof(RIO)*rios->capacity);
            memcpy(rios->rios,rios->rio_buf,sizeof(RIO)*rios->count);
        } else {
            rios->rios = zrealloc(rios->rios,sizeof(RIO)*rios->capacity);
        }
    }
    return rios->rios + rios->count++;
}

void RIOBatchDoGet(RIOBatch *rios) {
    RIO *rio;
    size_t count = 0, x;

    serverAssert(rios->action == ROCKS_GET);
    for (size_t i = 0; i < rios->count; i++) {
        count += rios->rios[i].get.numkeys;
    }

    rocksdb_column_family_handle_t **cfs_list;
    char **keys_list = zmalloc(count*sizeof(char*));
    char **values_list = zmalloc(count*sizeof(char*));
    size_t *keys_list_sizes = zmalloc(count*sizeof(size_t));
    size_t *values_list_sizes = zmalloc(count*sizeof(size_t));
    char **errs = zmalloc(count*sizeof(char*));
    cfs_list = zmalloc(count*sizeof(rocksdb_column_family_handle_t*));

    x = 0;
    for (size_t i = 0; i < rios->count; i++) {
        rio = rios->rios+i;
        serverAssert(rio->action == rios->action);
        for (int j = 0; j < rio->get.numkeys; j++) {
            cfs_list[x] = swapGetCF(rio->get.cfs[j]);
            keys_list[x] = rio->get.rawkeys[j];
            keys_list_sizes[x] = sdslen(rio->get.rawkeys[j]);
            x++;
        }
    }
    serverAssert(x == count);

    rocksdb_multi_get_cf(server.rocks->db, server.rocks->ropts,
            (const rocksdb_column_family_handle_t *const *)cfs_list,count,
            (const char**)keys_list, (const size_t*)keys_list_sizes,
            values_list, values_list_sizes, errs);

    x = 0;
    for (size_t i = 0; i < rios->count; i++) {
        rio = rios->rios+i;
        rio->get.rawvals = zmalloc(rio->get.numkeys*sizeof(sds));
        for (int j = 0; j < rio->get.numkeys; j++) {
            if (values_list[x] == NULL) {
                rio->get.rawvals[j] = NULL;
                rio->get.notfound++;
            } else {
                rio->get.rawvals[j] = sdsnewlen(values_list[x],
                        values_list_sizes[x]);
                zlibc_free(values_list[x]);
            }
            if (errs[x]) {
                if (!RIOGetError(rio)) {
                    RIOSetError(rio,SWAP_ERR_RIO_GET_FAIL,sdsnew(errs[x]));
                    serverLog(LL_WARNING,"[rocks] do batch rocksdb get failed: %s",
                            rio->err);
                }
                zlibc_free(errs[x]);
            }
            x++;
        }
    }
    serverAssert(x == count);

    zfree(cfs_list);
    zfree(keys_list);
    zfree(values_list);
    zfree(keys_list_sizes);
    zfree(values_list_sizes);
    zfree(errs);
}

static void RIOBatchSetError(RIOBatch *rios, int errcode, const char *err) {
    for (size_t i = 0; i < rios->count; i++) {
        RIO *rio = rios->rios+i;
        RIOSetError(rio,errcode,sdsnew(err));
    }
}

void RIOBatchDoPut(RIOBatch *rios) {
    char *err = NULL;
    rocksdb_writebatch_t *wb = rocksdb_writebatch_create();

    serverAssert(rios->action == ROCKS_PUT);

    for (size_t i = 0; i < rios->count; i++) {
        RIO *rio = rios->rios+i;
        serverAssert(rio->action == rios->action);
        for (int j = 0; j < rio->put.numkeys; j++) {
            rocksdb_writebatch_put_cf(wb,swapGetCF(rio->put.cfs[j]),
                    rio->put.rawkeys[j],sdslen(rio->put.rawkeys[j]),
                    rio->put.rawvals[j],sdslen(rio->put.rawvals[j]));
        }
    }

    rocksdb_write(server.rocks->db,server.rocks->wopts,wb,&err);
    if (err != NULL) {
        RIOBatchSetError(rios,SWAP_ERR_RIO_PUT_FAIL,err);
        serverLog(LL_WARNING,"[rocks] do rocksdb batch put failed: %s",err);
        zlibc_free(err);
    }

    rocksdb_writebatch_destroy(wb);
}

void RIOBatchDoDel(RIOBatch *rios) {
    char *err = NULL;
    rocksdb_writebatch_t *wb = rocksdb_writebatch_create();

    serverAssert(rios->action == ROCKS_DEL);

    for (size_t i = 0; i < rios->count; i++) {
        RIO *rio = rios->rios+i;
        serverAssert(rio->action == rios->action);
        for (int j = 0; j < rio->del.numkeys; j++) {
            rocksdb_writebatch_delete_cf(wb,swapGetCF(rio->del.cfs[j]),
                    rio->del.rawkeys[j],sdslen(rio->del.rawkeys[j]));
        }
    }

    rocksdb_write(server.rocks->db,server.rocks->wopts,wb,&err);
    if (err != NULL) {
        RIOBatchSetError(rios,SWAP_ERR_RIO_DEL_FAIL,err);
        serverLog(LL_WARNING,"[rocks] do rocksdb batch del failed: %s",err);
        zlibc_free(err);
    }

    rocksdb_writebatch_destroy(wb);
}

void RIOBatchDump(RIOBatch *rios) {
    serverLog(LL_NOTICE, "[RIOBatch] action=%s,count=%ld ===",
            rocksActionName(rios->action),rios->count);
    for (size_t i = 0; i < rios->count; i++) {
        RIODump(rios->rios+i);
    }
}

static void RIOBatchDoIndividually(RIOBatch *rios) {
    for (size_t i = 0; i < rios->count; i++) {
        RIO *rio = rios->rios+i;
        serverAssert(rio->action == rios->action);
        RIODo(rios->rios+i);
    }
}

/* GET -- multiget; PUT/DEL -- write; ITERATE -- cant batch; */
void RIOBatchDo(RIOBatch *rios) {
    monotime io_timer;

    /* Fallback to RIODo for actions that cant batch */
    if (rios->action == ROCKS_ITERATE) {
        RIOBatchDoIndividually(rios);
        return;
    }

    elapsedStart(&io_timer);

    if (server.swap_debug_rio_delay_micro)
        usleep(server.swap_debug_rio_delay_micro);

    if (server.swap_debug_rio_error > 0) {
        server.swap_debug_rio_error--;
        RIOBatchSetError(rios,SWAP_ERR_RIO_FAIL,sdsnew("rio mock error"));
        goto end;
    }

    switch (rios->action) {
    case ROCKS_GET:
        RIOBatchDoGet(rios);
        break;
    case ROCKS_PUT:
        RIOBatchDoPut(rios);
        break;
    case ROCKS_DEL:
        RIOBatchDoDel(rios);
        break;
    default:
        serverPanic("[RIOBatch] Unknown io action %d", rios->action);
        break;
    }
#ifdef ROCKS_DEBUG
    RIOBatchDump(rios);
#endif

end:
    RIOBatchUpdateStatsDo(rios, elapsedUs(io_timer));
    RIOBatchUpdateStatsDataNotFound(rios);
}

void RIOBatchUpdateStatsDo(RIOBatch *rios, long duration) {
    int action = rios->action;
    size_t payload_size = 0;
    size_t count = 0;
    for (size_t i = 0; i < rios->count; i++) {
        RIO *rio = rios->rios+i;
        int cf = RIOGetCF(rio);
        if (cf != META_CF) {
            payload_size += RIOEstimatePayloadSize(rio);
            count++;
        }
    }
    atomicIncr(server.ror_stats->rio_stats[action].memory,payload_size);
    atomicIncr(server.ror_stats->rio_stats[action].count,count);
    atomicIncr(server.ror_stats->rio_stats[action].batch,1);
    atomicIncr(server.ror_stats->rio_stats[action].time,duration);
}

void RIOBatchUpdateStatsDataNotFound(RIOBatch *rios) {
    int notfound = 0;
    for (size_t i = 0; i < rios->count; i++) {
        RIO *rio = rios->rios+i;
        int cf = RIOGetCF(rio);
        if (cf != META_CF) notfound += rio->get.notfound;
    }
    if (notfound) {
        atomicIncr(server.swap_hit_stats->stat_swapin_data_not_found_count,
                notfound);
    }
}

#ifdef REDIS_TEST

sds *genSdsArray(int count, ...) {
    va_list valist;
    sds *result = zmalloc(count*sizeof(sds));
    va_start(valist,count);
    for (int i = 0; i < count; i++) {
        sds raw = va_arg(valist, sds);
        result[i] = sdsdup(raw);
    }
    va_end(valist);
    return result;
}

int *genIntArray(int count, ...) {
    va_list valist;
    int *cfs = zmalloc(count*sizeof(sds));
    va_start(valist,count);
    for (int i = 0; i < count; i++) {
        int cf = va_arg(valist, int);
        cfs[i] = cf;
    }
    va_end(valist);
    return cfs;
}

#define getStatsRIO(action,stats_type) (server.ror_stats->rio_stats[action].stats_type)
#define getStatsDataNotFound() (server.swap_hit_stats->stat_swapin_data_not_found_count)

void resetRIOStats() {
    for (int action = 0; action < ROCKS_TYPES; action++) {
        atomicSet(server.ror_stats->rio_stats[action].memory,0);
        atomicSet(server.ror_stats->rio_stats[action].count,0);
        atomicSet(server.ror_stats->rio_stats[action].batch,0);
        atomicSet(server.ror_stats->rio_stats[action].time,0);
    }
    atomicSet(server.swap_hit_stats->stat_swapin_data_not_found_count,0);
}

void initServerConfig(void);
int swapRIOTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);
    int error = 0;

    TEST("RIO: init") {
        server.hz = 10;
        initTestRedisDb();
        monotonicInit();
        initServerConfig();
        if (!server.rocks) rocksInit();
        initStatsSwap();
    }

    TEST("RIO: get/del/put") {
        RIO _rio, *rio = &_rio;
        sds foo = sdsnew("foo"), bar = sdsnew("bar"), miss = sdsnew("miss");
        sds *rawkeys, *rawvals;
        int *cfs;
        size_t foo_size = sdsalloc(foo), bar_size = sdsalloc(bar),
               miss_size = sdsalloc(miss);

        resetRIOStats();

        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,foo);
        rawvals = genSdsArray(1,bar);
        RIOInitPut(rio,1,cfs,rawkeys,rawvals);
        RIODo(rio);
        test_assert(getStatsRIO(ROCKS_PUT,count) == 1);
        test_assert(getStatsRIO(ROCKS_PUT,batch) == 1);
        test_assert(getStatsRIO(ROCKS_PUT,memory) == foo_size+bar_size);
        RIODeinit(rio);

        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,miss);
        RIOInitGet(rio,1,cfs,rawkeys);
        RIODo(rio);
        test_assert(rio->get.rawvals[0] == NULL);
        test_assert(getStatsRIO(ROCKS_GET,count) == 1);
        test_assert(getStatsRIO(ROCKS_GET,batch) == 1);
        test_assert(getStatsRIO(ROCKS_GET,memory) == miss_size);
        test_assert(getStatsDataNotFound() == 1);
        RIODeinit(rio);

        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,foo);
        RIOInitGet(rio,1,cfs,rawkeys);
        RIODo(rio);
        test_assert(sdscmp(rio->get.rawvals[0],bar) == 0);
        test_assert(getStatsRIO(ROCKS_GET,count) == 2);
        test_assert(getStatsRIO(ROCKS_GET,batch) == 2);
        test_assert(getStatsRIO(ROCKS_GET,memory) == miss_size+foo_size+bar_size);
        test_assert(getStatsDataNotFound() == 1);
        RIODeinit(rio);

        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,foo);
        RIOInitDel(rio,1,cfs,rawkeys);
        RIODo(rio);
        test_assert(getStatsRIO(ROCKS_DEL,count) == 1);
        test_assert(getStatsRIO(ROCKS_DEL,batch) == 1);
        test_assert(getStatsRIO(ROCKS_DEL,memory) == foo_size);
        RIODeinit(rio);

        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,foo);
        RIOInitGet(rio,1,cfs,rawkeys);
        RIODo(rio);
        test_assert(rio->get.rawvals[0] == NULL);
        test_assert(getStatsRIO(ROCKS_GET,count) == 3);
        test_assert(getStatsRIO(ROCKS_GET,batch) == 3);
        test_assert(getStatsRIO(ROCKS_GET,memory) == miss_size+foo_size*2+bar_size);
        test_assert(getStatsDataNotFound() == 2);
        RIODeinit(rio);

        sdsfree(foo), sdsfree(bar), sdsfree(miss);
    }

    TEST("RIO: batch get/del/put") {
        RIOBatch _rios, *rios = &_rios;
        RIO *rio;
        sds foo = sdsnew("foo"), bar = sdsnew("bar"), miss = sdsnew("miss"),
            hello = sdsnew("hello"), world = sdsnew("world");
        sds *rawkeys, *rawvals;
        int *cfs;
        size_t foo_size = sdsalloc(foo), bar_size = sdsalloc(bar),
               miss_size = sdsalloc(miss), hello_size = sdsalloc(hello),
                   world_size = sdsalloc(world), expected_size;

        resetRIOStats();

        /* put : (foo:bar),(hello:world)
         * get : (foo),(hello),(keymiss) => (bar),(wrold),(<nil>)
         * del : (foo),(bar),(keymiss)
         * get : (foo),(bar),(keymiss) => (<nil>),(<nil>),(<nil>)
         */

        RIOBatchInit(rios,ROCKS_PUT);
        rio = RIOBatchAlloc(rios);
        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,foo);
        rawvals = genSdsArray(1,bar);
        RIOInitPut(rio,1,cfs,rawkeys,rawvals);
        rio = RIOBatchAlloc(rios);
        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,hello);
        rawvals = genSdsArray(1,world);
        RIOInitPut(rio,1,cfs,rawkeys,rawvals);
        RIOBatchDo(rios);
        test_assert(getStatsRIO(ROCKS_PUT,batch) == 1);
        test_assert(getStatsRIO(ROCKS_PUT,count) == 2);
        test_assert(getStatsRIO(ROCKS_PUT,memory) == foo_size+bar_size+hello_size+world_size);
        RIOBatchDeinit(rios);

        RIOBatchInit(rios,ROCKS_GET);
        rio = RIOBatchAlloc(rios);
        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,foo);
        RIOInitGet(rio,1,cfs,rawkeys);
        rio = RIOBatchAlloc(rios);
        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,hello);
        RIOInitGet(rio,1,cfs,rawkeys);
        rio = RIOBatchAlloc(rios);
        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,miss);
        RIOInitGet(rio,1,cfs,rawkeys);
        RIOBatchDo(rios);
        test_assert(getStatsRIO(ROCKS_GET,batch) == 1);
        test_assert(getStatsRIO(ROCKS_GET,count) == 3);
        expected_size = foo_size+bar_size+hello_size+world_size+miss_size;
        test_assert(getStatsRIO(ROCKS_GET,memory) == expected_size);
        test_assert(getStatsDataNotFound() == 1);
        RIOBatchDeinit(rios);

        RIOBatchInit(rios,ROCKS_DEL);
        rio = RIOBatchAlloc(rios);
        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,foo);
        RIOInitDel(rio,1,cfs,rawkeys);
        rio = RIOBatchAlloc(rios);
        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,hello);
        RIOInitDel(rio,1,cfs,rawkeys);
        rio = RIOBatchAlloc(rios);
        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,miss);
        RIOInitDel(rio,1,cfs,rawkeys);
        RIOBatchDo(rios);
        test_assert(getStatsRIO(ROCKS_DEL,batch) == 1);
        test_assert(getStatsRIO(ROCKS_DEL,count) == 3);
        test_assert(getStatsRIO(ROCKS_DEL,memory) == foo_size+hello_size+miss_size);
        RIOBatchDeinit(rios);

        RIOBatchInit(rios,ROCKS_GET);
        rio = RIOBatchAlloc(rios);
        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,foo);
        RIOInitGet(rio,1,cfs,rawkeys);
        rio = RIOBatchAlloc(rios);
        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,hello);
        RIOInitGet(rio,1,cfs,rawkeys);
        rio = RIOBatchAlloc(rios);
        cfs = genIntArray(1,DATA_CF);
        rawkeys = genSdsArray(1,miss);
        RIOInitGet(rio,1,cfs,rawkeys);
        RIOBatchDo(rios);
        test_assert(getStatsRIO(ROCKS_GET,batch) == 2);
        test_assert(getStatsRIO(ROCKS_GET,count) == 6);
        expected_size += foo_size+hello_size+miss_size;
        test_assert(getStatsRIO(ROCKS_GET,memory) == expected_size);
        test_assert(getStatsDataNotFound() == 4);
        RIOBatchDeinit(rios);

        sdsfree(foo), sdsfree(bar), sdsfree(miss);
        sdsfree(hello), sdsfree(world);
    }

    return error;
}
#endif
