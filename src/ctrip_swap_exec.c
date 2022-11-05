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
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <math.h>

#define RIO_SCAN_NUMKEYS_ALLOC_INIT 16
#define RIO_SCAN_NUMKEYS_ALLOC_LINER 4096

/* --- RIO --- */
void RIOInitGet(RIO *rio, int cf, sds rawkey) {
    rio->action = ROCKS_GET;
    rio->get.cf = cf;
    rio->get.rawkey = rawkey;
    rio->get.rawval = NULL;
    rio->err = NULL;
}

void RIOInitPut(RIO *rio, int cf, sds rawkey, sds rawval) {
    rio->action = ROCKS_PUT;
    rio->put.cf = cf;
    rio->put.rawkey = rawkey;
    rio->put.rawval = rawval;
    rio->err = NULL;
}

void RIOInitDel(RIO *rio, int cf, sds rawkey) {
    rio->action = ROCKS_DEL;
    rio->del.cf = cf;
    rio->del.rawkey = rawkey;
    rio->err = NULL;
}

void RIOInitWrite(RIO *rio, rocksdb_writebatch_t *wb) {
    rio->action = ROCKS_WRITE;
    rio->write.wb = wb;
    rio->err = NULL;
}

void RIOInitMultiGet(RIO *rio, int numkeys, int *cfs, sds *rawkeys) {
    rio->action = ROCKS_MULTIGET;
    rio->multiget.numkeys = numkeys;
    rio->multiget.cfs = cfs;
    rio->multiget.rawkeys = rawkeys;
    rio->multiget.rawvals = NULL;
    rio->err = NULL;
}

void RIOInitScan(RIO *rio, int cf, sds prefix) {
    rio->action = ROCKS_SCAN;
    rio->scan.cf = cf;
    rio->scan.prefix = prefix;
    rio->scan.rawkeys = NULL;
    rio->scan.rawvals = NULL;
    rio->err = NULL;
}

void RIOInitMultiDeleteRange(RIO* rio, int num, int* cf , sds* rawkeys) {
    rio->action = ROCKS_MULTI_DELETERANGE;
    rio->multidel_range.num = num;
    rio->multidel_range.cf = cf;
    rio->multidel_range.rawkeys = rawkeys;
    rio->err = NULL;

}

void RIOInitIterate(RIO *rio, int cf, sds seek, int limit) {
    rio->action = ROCKS_ITERATE;
    rio->iterate.cf = cf;
    rio->iterate.seek = seek;
    rio->iterate.limit = limit;
    rio->iterate.numkeys = 0;
    rio->iterate.rawkeys = NULL;
    rio->iterate.rawvals = NULL;
    rio->err = NULL;
}

void RIOInitRange(RIO *rio, int cf, sds start, sds end, int reverse, int limit) {
    rio->action = ROCKS_RANGE;
    rio->range.cf = cf;
    rio->range.start = start;
    rio->range.end = end;
    rio->range.reverse = reverse;
    rio->range.limit = limit;
}


void RIODeinit(RIO *rio) {
    int i;

    switch (rio->action) {
    case  ROCKS_GET:
        sdsfree(rio->get.rawkey);
        rio->get.rawkey = NULL;
        sdsfree(rio->get.rawval);
        rio->get.rawval = NULL;
        break;
    case  ROCKS_PUT:
        sdsfree(rio->put.rawkey);
        rio->put.rawkey = NULL;
        sdsfree(rio->put.rawval);
        rio->put.rawval = NULL;
        break;
    case  ROCKS_DEL:
        sdsfree(rio->del.rawkey);
        rio->del.rawkey = NULL;
        break;
    case  ROCKS_MULTIGET:
        for (i = 0; i < rio->multiget.numkeys; i++) {
            if (rio->multiget.rawkeys) sdsfree(rio->multiget.rawkeys[i]);
            if (rio->multiget.rawvals) sdsfree(rio->multiget.rawvals[i]);
        }
        zfree(rio->multiget.cfs);
        rio->multiget.cfs = NULL;
        zfree(rio->multiget.rawkeys);
        rio->multiget.rawkeys = NULL;
        zfree(rio->multiget.rawvals);
        rio->multiget.rawvals = NULL;
        break;
    case  ROCKS_SCAN:
        sdsfree(rio->scan.prefix);
        for (i = 0; i < rio->scan.numkeys; i++) {
            if (rio->scan.rawkeys) sdsfree(rio->scan.rawkeys[i]);
            if (rio->scan.rawvals) sdsfree(rio->scan.rawvals[i]);
        }
        zfree(rio->scan.rawkeys);
        rio->scan.rawkeys = NULL;
        zfree(rio->scan.rawvals);
        rio->scan.rawvals = NULL;
        break;
    case  ROCKS_WRITE:
        rocksdb_writebatch_destroy(rio->write.wb);
        rio->write.wb = NULL;
        break;
    case  ROCKS_MULTI_DELETERANGE:
        for(int i = 0; i < rio->multidel_range.num;i++) {
            sdsfree(rio->multidel_range.rawkeys[i]);
        }
        zfree(rio->multidel_range.rawkeys);
        zfree(rio->multidel_range.cf);
        
        rio->multidel_range.rawkeys = NULL;
        rio->multidel_range.cf = NULL;
        break;
    case  ROCKS_ITERATE:
        sdsfree(rio->iterate.seek);
        rio->iterate.seek = NULL;
        for (i = 0; i <= rio->iterate.numkeys; i++) {
            if (rio->iterate.rawkeys) sdsfree(rio->iterate.rawkeys[i]);
            if (rio->iterate.rawvals) sdsfree(rio->iterate.rawvals[i]);
        }
        zfree(rio->iterate.rawkeys);
        rio->iterate.rawkeys = NULL;
        zfree(rio->iterate.rawvals);
        rio->iterate.rawvals = NULL;
        break;
    default:
        break;
    }
}

static inline rocksdb_column_family_handle_t *rioGetCF(int cf) {
    serverAssert(cf < CF_COUNT);
    return server.rocks->cf_handles[cf];
}

static inline const char *rioGetCFName(int cf) {
    serverAssert(cf < CF_COUNT);
    return swap_cf_names[cf];
}

static int doRIOGet(RIO *rio) {
    size_t vallen;
    char *err = NULL, *val;

    val = rocksdb_get_cf(server.rocks->db, server.rocks->ropts,
            rioGetCF(rio->get.cf),
            rio->get.rawkey, sdslen(rio->get.rawkey), &vallen, &err);
    if (err != NULL) {
        rio->err = sdsnew(err);
        serverLog(LL_WARNING,"[rocks] do rocksdb get failed: %s", err);
        zlibc_free(err);
        return -1;
    }
    if (val == NULL) {
        rio->get.rawval = NULL;
    } else  {
        rio->get.rawval = sdsnewlen(val, vallen);
        zlibc_free(val);
    }

    return 0;
}

static int doRIOPut(RIO *rio) {
    char *err = NULL;
    rocksdb_put_cf(server.rocks->db, server.rocks->wopts,rioGetCF(rio->put.cf),
            rio->put.rawkey, sdslen(rio->put.rawkey),
            rio->put.rawval, sdslen(rio->put.rawval), &err);
    if (err != NULL) {
        rio->err = sdsnew(err);
        serverLog(LL_WARNING,"[rocks] do rocksdb write failed: %s", err);
        zlibc_free(err);
        return -1;
    }
    return 0;
}

static int doRIODel(RIO *rio) {
    char *err = NULL;
    rocksdb_delete_cf(server.rocks->db, server.rocks->wopts,
            rioGetCF(rio->del.cf),
            rio->del.rawkey, sdslen(rio->del.rawkey), &err);
    if (err != NULL) {
        rio->err = sdsnew(err);
        serverLog(LL_WARNING,"[rocks] do rocksdb del failed: %s", err);
        zlibc_free(err);
        return -1;
    }
    return 0;
}

static int doRIOWrite(RIO *rio) {
    char *err = NULL;
    rocksdb_write(server.rocks->db, server.rocks->wopts,
            rio->write.wb, &err);
    if (err != NULL) {
        rio->err = sdsnew(err);
        serverLog(LL_WARNING,"[rocks] do rocksdb write failed: %s", err);
        zlibc_free(err);
        return -1;
    }
    return 0;
}

static int doRIOMultiGet(RIO *rio) {
    int ret = 0, i;
    rocksdb_column_family_handle_t **cfs_list;
    char **keys_list = zmalloc(rio->multiget.numkeys*sizeof(char*));
    char **values_list = zmalloc(rio->multiget.numkeys*sizeof(char*));
    size_t *keys_list_sizes = zmalloc(rio->multiget.numkeys*sizeof(size_t));
    size_t *values_list_sizes = zmalloc(rio->multiget.numkeys*sizeof(size_t));
    char **errs = zmalloc(rio->multiget.numkeys*sizeof(char*));
    cfs_list = zmalloc(rio->multiget.numkeys*sizeof(rocksdb_column_family_handle_t*));

    for (i = 0; i < rio->multiget.numkeys; i++) {
        cfs_list[i] = rioGetCF(rio->multiget.cfs[i]);
        keys_list[i] = rio->multiget.rawkeys[i];
        keys_list_sizes[i] = sdslen(rio->multiget.rawkeys[i]);
    }

    rocksdb_multi_get_cf(server.rocks->db, server.rocks->ropts,
            (const rocksdb_column_family_handle_t *const *)cfs_list,
            rio->multiget.numkeys,
            (const char**)keys_list, (const size_t*)keys_list_sizes,
            values_list, values_list_sizes, errs);

    rio->multiget.rawvals = zmalloc(rio->multiget.numkeys*sizeof(sds));
    for (i = 0; i < rio->multiget.numkeys; i++) {
        if (values_list[i] == NULL) {
            rio->multiget.rawvals[i] = NULL;
        } else {
            rio->multiget.rawvals[i] = sdsnewlen(values_list[i],
                    values_list_sizes[i]);
            zlibc_free(values_list[i]);
        }
        if (errs[i]) {
            if (rio->err == NULL) {
                rio->err = sdsnew(errs[i]);
                serverLog(LL_WARNING,"[rocks] do rocksdb multiget failed: %s",
                        rio->err);
            }
            zlibc_free(errs[i]);
        }
    }

    if (rio->err != NULL) {
        ret = -1;
        goto end;
    }

end:
    zfree(cfs_list);
    zfree(keys_list);
    zfree(values_list);
    zfree(keys_list_sizes);
    zfree(values_list_sizes);
    zfree(errs);
    return ret;
}

/*TODO opt cache iter (LRU) */
static int doRIOScan(RIO *rio) {
    int ret = 0;
    char *err = NULL;
    rocksdb_iterator_t *iter = NULL;
    sds prefix = rio->scan.prefix;
    size_t numkeys = 0, numalloc = 8;
    sds *rawkeys = zmalloc(numalloc*sizeof(sds));
    sds *rawvals = zmalloc(numalloc*sizeof(sds));

    iter = rocksdb_create_iterator_cf(server.rocks->db,server.rocks->ropts,
            rioGetCF(rio->scan.cf));
    rocksdb_iter_seek(iter,prefix,sdslen(prefix));

    while (rocksdb_iter_valid(iter)) {
        size_t klen, vlen;
        const char *rawkey, *rawval;
        rawkey = rocksdb_iter_key(iter, &klen);

        if (klen < sdslen(prefix) || memcmp(rawkey, prefix, sdslen(prefix)))
            break;

        numkeys++;
        /* make room for key/val */
        if (numkeys >= numalloc) {
            if (numalloc >= RIO_SCAN_NUMKEYS_ALLOC_LINER) {
                numalloc += RIO_SCAN_NUMKEYS_ALLOC_LINER;
            } else {
                numalloc *= 2;
            }
            rawkeys = zrealloc(rawkeys, numalloc*sizeof(sds));
            rawvals = zrealloc(rawvals, numalloc*sizeof(sds));
        }

        rawval = rocksdb_iter_value(iter, &vlen);
        rawkeys[numkeys-1] = sdsnewlen(rawkey, klen);
        rawvals[numkeys-1] = sdsnewlen(rawval, vlen);

        rocksdb_iter_next(iter);
    }

    rocksdb_iter_get_error(iter, &err);
    if (err != NULL) {
        rio->err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb scan failed: %s", err);
        ret = -1;
    }
    
    rio->scan.numkeys = numkeys;
    rio->scan.rawkeys = rawkeys;
    rio->scan.rawvals = rawvals;
    rocksdb_iter_destroy(iter);

    return ret;
}

#define min(a,b)  (a > b? b: a)
static int doRIORange(RIO* rio) {
    int ret = 0;
    rocksdb_iterator_t *iter = NULL;
    
    int reverse = rio->range.reverse;
    int minex, maxex;
    char* start, *end;
    size_t start_len, end_len; 
    serverAssert(decodeIntervalSds(rio->range.start, &minex, &start, &start_len) == C_OK);
    serverAssert(decodeIntervalSds(rio->range.end, &maxex, &end, &end_len) == C_OK);
    
    iter = rocksdb_create_iterator_cf(server.rocks->db,server.rocks->ropts,
            rioGetCF(rio->range.cf));
    
    if (reverse) {
        rocksdb_iter_seek(iter, end, end_len);
        if (!rocksdb_iter_valid(iter)) {
            rocksdb_iter_seek_for_prev(iter,end,end_len);
        } else {
            size_t klen;
            const char *rawkey;
            rawkey = rocksdb_iter_key(iter, &klen);
            if (memcmp(rawkey, end, end_len)) {
                rocksdb_iter_seek_for_prev(iter,end,end_len);
            }
        }
    } else {
        rocksdb_iter_seek(iter, start, start_len);
    }
    size_t numkeys = 0, numalloc = 8;
    sds *rawkeys = zmalloc(numalloc*sizeof(sds));
    sds *rawvals = zmalloc(numalloc*sizeof(sds));

    for(;rocksdb_iter_valid(iter); reverse?rocksdb_iter_prev(iter):rocksdb_iter_next(iter)) {
        size_t klen, vlen;
        const char *rawkey, *rawval;
        rawkey = rocksdb_iter_key(iter, &klen);
        int start_cmp_result = memcmp(rawkey, start, min(start_len, klen));
        if (start_cmp_result == 0) {
            if (start_len > klen) {
                start_cmp_result = -1;
            } else if(start_len < klen) {
                start_cmp_result = 1;
            }
        }
        if (start_cmp_result < 0
             || (minex && start_cmp_result == 0)) {
            if (!reverse) {
                continue;
            } else {
                break;
            }  
        }
        int end_cmp_result = memcmp(rawkey, end, min(end_len, klen));
        if (end_cmp_result == 0) {
            if (end_len > klen) {
                end_cmp_result = -1;
            } else if(end_len < klen) {
                end_cmp_result = 1;
            }
        }

        if (end_cmp_result > 0
             || (maxex && end_cmp_result == 0)) {
            if (reverse) {
                continue;
            } else {
                break;
            }
        }
        rawval = rocksdb_iter_value(iter, &vlen);
        numkeys++;
       
        if (numkeys >=  numalloc) {
            if (numalloc >= RIO_SCAN_NUMKEYS_ALLOC_LINER) {
                numalloc += RIO_SCAN_NUMKEYS_ALLOC_LINER;
            } else {
                numalloc *= 2;
            }
            rawkeys = zrealloc(rawkeys, numalloc*sizeof(sds));
            rawvals = zrealloc(rawvals, numalloc*sizeof(sds));
        }
        rawkeys[numkeys - 1] = sdsnewlen(rawkey, klen);
        rawvals[numkeys - 1] = sdsnewlen(rawval, vlen);
        if (rio->range.limit > 0 && rio->range.limit <= numkeys) {
            break;
        }
    }
    rio->range.numkeys = numkeys;
    rio->range.rawkeys = rawkeys;
    rio->range.rawvals = rawvals;
    rocksdb_iter_destroy(iter);
    return ret;
}


static int doRIOMultiDeleteRange(RIO *rio) {
    char *err = NULL;
    for(int i = 0; i < (rio->multidel_range.num/2);i++) {
        rocksdb_delete_range_cf(server.rocks->db, server.rocks->wopts,
            rioGetCF(rio->multidel_range.cf[i*2]),
            rio->multidel_range.rawkeys[i*2], sdslen(rio->multidel_range.rawkeys[i*2]),
            rio->multidel_range.rawkeys[i*2+1], sdslen(rio->multidel_range.rawkeys[i*2+1]), &err);
        if (err != NULL) {
            rio->err = sdsnew(err);
            serverLog(LL_WARNING,"[rocks] do rocksdb delete range failed: %s", err);
            zlibc_free(err);
            return -1;
        }
    }
    return 0;
}

static int doRIOIterate(RIO *rio) {
    int ret = 0;
    char *err = NULL;
    rocksdb_iterator_t *iter;
    sds seek = rio->iterate.seek;
    int numkeys = 0;
    int limit = rio->iterate.limit + 1; /*extra one to save nextseek */
    sds *rawkeys = zmalloc(limit*sizeof(sds));
    sds *rawvals = zmalloc(limit*sizeof(sds));

    iter = rocksdb_create_iterator_cf(server.rocks->db,server.rocks->ropts,
            rioGetCF(rio->iterate.cf));
    rocksdb_iter_seek(iter,seek,sdslen(seek));

    while (rocksdb_iter_valid(iter) && numkeys < rio->iterate.limit) {
        size_t klen, vlen;
        const char *rawkey, *rawval;
        rawkey = rocksdb_iter_key(iter, &klen);
        rawval = rocksdb_iter_value(iter, &vlen);
        rawkeys[numkeys] = sdsnewlen(rawkey, klen);
        rawvals[numkeys] = sdsnewlen(rawval, vlen);
        numkeys++;
        rocksdb_iter_next(iter);
    }

    rocksdb_iter_get_error(iter, &err);
    if (err != NULL) {
        rio->err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb iterate failed: %s", err);
        ret = -1;
    }
    
    rio->iterate.numkeys = numkeys;
    rio->iterate.rawkeys = rawkeys;
    rio->iterate.rawvals = rawvals;
    
    /* save nextseek */
    if (rocksdb_iter_valid(iter)) {
        size_t klen;
        const char *rawkey;
        rawkey = rocksdb_iter_key(iter, &klen);
        rio->iterate.rawkeys[rio->iterate.numkeys] = sdsnewlen(rawkey, klen);
        rio->iterate.rawvals[rio->iterate.numkeys] = NULL;
    } else {
        rio->iterate.rawkeys[rio->iterate.numkeys] = NULL;
        rio->iterate.rawvals[rio->iterate.numkeys] = NULL;
    }

    rocksdb_iter_destroy(iter);

    return ret;
}

void dumpRIO(RIO *rio) {
    sds repr = sdsnew("[ROCKS] ");
    switch (rio->action) {
    case ROCKS_GET:
        repr = sdscatprintf(repr, "GET [%s] rawkey=", rioGetCFName(rio->get.cf));
        repr = sdscatrepr(repr, rio->get.rawkey, sdslen(rio->get.rawkey));
        repr = sdscat(repr, ", rawval=");
        if (rio->get.rawval) {
            repr = sdscatrepr(repr, rio->get.rawval, sdslen(rio->get.rawval));
        } else {
            repr = sdscatfmt(repr, "<nil>");
        }
        break;
    case ROCKS_PUT:
        repr = sdscatprintf(repr, "PUT [%s] rawkey=", rioGetCFName(rio->put.cf));
        repr = sdscatrepr(repr, rio->put.rawkey, sdslen(rio->put.rawkey));
        repr = sdscat(repr, ", rawval=");
        repr = sdscatrepr(repr, rio->put.rawval, sdslen(rio->put.rawval));
        break;
    case ROCKS_DEL:
        repr = sdscatprintf(repr, "DEL [%s] rawkey=", rioGetCFName(rio->del.cf));
        repr = sdscatrepr(repr, rio->del.rawkey, sdslen(rio->del.rawkey));
        break;
    case ROCKS_WRITE:
        repr = sdscat(repr, "WRITE");
        break;
    case ROCKS_MULTIGET:
        repr = sdscat(repr, "MULTIGET:\n");
        for (int i = 0; i < rio->multiget.numkeys; i++) {
            repr = sdscatprintf(repr, "  ([%s] ", rioGetCFName(rio->multiget.cfs[i]));
            repr = sdscatrepr(repr, rio->multiget.rawkeys[i],
                    sdslen(rio->multiget.rawkeys[i]));
            repr = sdscat(repr, ")=>(");
            if (rio->multiget.rawvals[i]) {
                repr = sdscatrepr(repr, rio->multiget.rawvals[i],
                        sdslen(rio->multiget.rawvals[i]));
            } else {
                repr = sdscatfmt(repr, "<nil>");
            }
            repr = sdscat(repr,")\n");
        }
        break;
    case ROCKS_SCAN:
        repr = sdscatprintf(repr, "SCAN [%s]: (", rioGetCFName(rio->scan.cf));
        repr = sdscatrepr(repr, rio->scan.prefix, sdslen(rio->scan.prefix));
        repr = sdscat(repr, ")\n");
        for (int i = 0; i < rio->scan.numkeys; i++) {
            repr = sdscat(repr, "  (");
            repr = sdscatrepr(repr, rio->scan.rawkeys[i],
                    sdslen(rio->scan.rawkeys[i]));
            repr = sdscat(repr, ")=>(");
            repr = sdscatrepr(repr, rio->scan.rawvals[i],
                    sdslen(rio->scan.rawvals[i]));
            repr = sdscat(repr,")\n");
        }
        break;
    case ROCKS_MULTI_DELETERANGE:
        for(int i = 0; i < rio->multidel_range.num/2; i++) {
            repr = sdscat(repr, "DELETERANGE start_key=%s");
            repr = sdscatrepr(repr, rio->multidel_range.rawkeys[i * 2], sdslen(rio->multidel_range.rawkeys[i * 2]));
            repr = sdscat(repr, ", end_key=");
            repr = sdscatrepr(repr, rio->multidel_range.rawkeys[i * 2 + 1], sdslen(rio->multidel_range.rawkeys[i * 2 + 1]));
        }
        break;
    case ROCKS_ITERATE:
        repr = sdscatprintf(repr, "ITERATE [%s]: (", rioGetCFName(rio->iterate.cf));
        repr = sdscatrepr(repr, rio->iterate.seek, sdslen(rio->iterate.seek));
        repr = sdscat(repr, "=>");
        sds nextseek = rio->iterate.rawkeys ? rio->iterate.rawkeys[rio->iterate.numkeys] : NULL;
        if (nextseek)
            repr = sdscatrepr(repr, nextseek, sdslen(nextseek));
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
        break;
    default:
        serverPanic("[rocks] Unknown io action: %d", rio->action);
        break;
    }
    serverLog(LL_NOTICE, "%s", repr);
    sdsfree(repr);
}

int doRIO(RIO *rio) {
    int ret;
    if (server.debug_rio_latency) usleep(server.debug_rio_latency*1000);
    if (server.debug_rio_error > 0) {
        server.debug_rio_error--;
        return SWAP_ERR_EXEC_RIO_FAIL;
    }

    switch (rio->action) {
    case ROCKS_GET:
        ret = doRIOGet(rio);
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
    case ROCKS_MULTIGET:
        ret = doRIOMultiGet(rio);
        break;
    case ROCKS_SCAN:
        ret = doRIOScan(rio);
        break;
    case ROCKS_MULTI_DELETERANGE:
        ret = doRIOMultiDeleteRange(rio);
        break;
    case ROCKS_ITERATE:
        ret = doRIOIterate(rio);
        break;
    case ROCKS_RANGE:
        ret = doRIORange(rio);
        break;
    default:
        serverPanic("[rocks] Unknown io action: %d", rio->action);
        return -1;
    }

#ifdef ROCKS_DEBUG
    dumpRIO(rio);
#endif

    return ret ? SWAP_ERR_EXEC_RIO_FAIL : 0;
}



static void doNotify(swapRequest *req, int errcode) {
    req->errcode = errcode;
    req->notify_cb(req, req->notify_pd);
}

static inline int doSwapDelMeta(swapData *data) {
    int retval;
    RIO rio_, *rio = &rio_;
    sds rawkey = swapDataEncodeMetaKey(data);
    RIOInitDel(rio,META_CF,rawkey);
    retval = doRIO(rio);
    RIODeinit(rio);
    return retval;
}

/* int doAuxDelMeta(swapRequest *req, int *del_meta) { */
    /* swapData *data = req->data; */

    /* if (req->intention == SWAP_DEL) { */
        /* *del_meta = 1; */
    /* } else if (req->intention == SWAP_IN && */
            /* req->intention_flags == SWAP_EXEC_IN_DEL && */
            /* swapDataMergedIsHot(data)) { */
        /* *del_meta = 1; */
    /* } else { */
        /* *del_meta = 0; */
    /* } */

    /* return *del_meta ? doSwapDelMeta(data) : 0; */
/* } */

static void executeSwapDelRequest(swapRequest *req) {
    int i, numkeys, errcode = 0, action;
    int *cfs = NULL;
    sds *rawkeys = NULL;
    RIO _rio = {0}, *rio = &_rio;
    rocksdb_writebatch_t *wb;
    swapData *data = req->data;

    if ((errcode = swapDataEncodeKeys(data,req->intention,req->datactx,
                &action,&numkeys,&cfs,&rawkeys))) {
        goto end;
    }
    DEBUG_MSGS_APPEND(req->msgs,"exec-del-encodekeys",
            "action=%s, numkeys=%d", rocksActionName(action), numkeys);

    if (numkeys > 0) {
        if (action == ROCKS_WRITE) {
            wb = rocksdb_writebatch_create();
            for (i = 0; i < numkeys; i++) {
                rocksdb_writebatch_delete_cf(wb, rioGetCF(cfs[i]),
                        rawkeys[i], sdslen(rawkeys[i]));
            }
            DEBUG_MSGS_APPEND(req->msgs,"exec-write","numkeys=%d.",numkeys);
            RIOInitWrite(rio,wb);
        } else if (action == ROCKS_DEL) {
            serverAssert(numkeys == 1 && rawkeys);
            DEBUG_MSGS_APPEND(req->msgs,"exec-del-del","rawkey=%s",rawkeys[0]);
            RIOInitDel(rio,cfs[0],rawkeys[0]);
            zfree(rawkeys), rawkeys = NULL;
            zfree(cfs), cfs = NULL;
        } else {
            errcode = SWAP_ERR_EXEC_UNEXPECTED_ACTION;
            goto end;
        }

        updateStatsSwapRIO(req, rio);
        if ((errcode = doRIO(rio))) {
            goto end;
        }
    }

    if ((errcode = doSwapDelMeta(data))) {
        goto end;
    }

end:
    doNotify(req,errcode);
    if (cfs) zfree(cfs);
    if (rawkeys) {
        for (i = 0; i < numkeys; i++) {
            sdsfree(rawkeys[i]);
        }
        zfree(rawkeys);
    }
    RIODeinit(rio);
}


/*
    calculate the size of all files in a folder
*/
static long get_dir_size(char *dirname)
{
    DIR *dir;
    struct dirent *ptr;
    long total_size = 0;
    char path[PATH_MAX] = {0};

    dir = opendir(dirname);
    if(dir == NULL)
    {
        serverLog(LL_WARNING,"open dir(%s) failed.", dirname);
        return -1;
    }

    while((ptr=readdir(dir)) != NULL)
    {
        snprintf(path, (size_t)PATH_MAX, "%s/%s", dirname,ptr->d_name);
        struct stat buf;
        if(lstat(path, &buf) < 0) {
            serverLog(LL_WARNING, "path(%s) lstat error", path);
        }
        if(strcmp(ptr->d_name,".") == 0) {
            total_size += buf.st_size;
            continue;
        }
        if(strcmp(ptr->d_name,"..") == 0) {
            continue;
        }
        if (S_ISDIR(buf.st_mode))
        {
            total_size += get_dir_size(path);
            memset(path, 0, sizeof(path));
        } else {
            total_size += buf.st_size;
        }
    }
    closedir(dir);
    return total_size;
}

static void executeCompactRange(swapRequest *req) {
    char dir[ROCKS_DIR_MAX_LEN];
    snprintf(dir, ROCKS_DIR_MAX_LEN, "%s/%d", ROCKS_DATA, server.rocksdb_epoch);
    serverLog(LL_WARNING, "[rocksdb compact range before] dir(%s) size(%ld)", dir, get_dir_size(dir));
    rocksdb_compact_range(server.rocks->db, NULL, 0, NULL, 0);
    serverLog(LL_WARNING, "[rocksdb compact range after] dir(%s) size(%ld)", dir, get_dir_size(dir));
    doNotify(req,0);
}

static void executeGetRocksdbStats(swapRequest* req) {
    req->finish_pd = rocksdb_property_value(server.rocks->db, "rocksdb.stats");
    doNotify(req,0);
}


static void executeRocksdbUtils(swapRequest *req) {
    switch(req->intention_flags) {
        case COMPACT_RANGE_TASK:
            executeCompactRange(req);
			break;
        case GET_ROCKSDB_STATS_TASK:
            executeGetRocksdbStats(req);
			break;
        default:
            doNotify(req,SWAP_ERR_EXEC_UNEXPECTED_UTIL);
			break;
    }
}

static inline int doSwapOutMeta(swapData *data) {
    int retval;
    RIO rio_, *rio = &rio_;
    sds rawkey = swapDataEncodeMetaKey(data);
    sds rawval = swapDataEncodeMetaVal(data);

    RIOInitPut(rio,META_CF,rawkey,rawval);
    retval = doRIO(rio);

    RIODeinit(rio);
    return retval;
}

static void executeSwapOutRequest(swapRequest *req) {
    int i, numkeys, errcode = 0, action, *cfs = NULL;
    sds *rawkeys = NULL, *rawvals = NULL;
    RIO _rio = {0}, *rio = &_rio;
    rocksdb_writebatch_t *wb = NULL;
    swapData *data = req->data;

    if ((errcode = swapDataEncodeData(data,req->intention,req->datactx,
                &action,&numkeys,&cfs,&rawkeys,&rawvals))) {
        goto end;
    }
    DEBUG_MSGS_APPEND(req->msgs,"exec-out-encodedata",
            "action=%s, numkeys=%d", rocksActionName(action), numkeys);

    if (action == ROCKS_PUT) {
        serverAssert(numkeys == 1);

#ifdef SWAP_DEBUG
        sds rawval_repr = sdscatrepr(sdsempty(), rawvals[0], sdslen(rawvals[0]));
        DEBUG_MSGS_APPEND(req->msgs,"exec-out-put","rawkey=%s,rawval=%s",
                rawkeys[0], rawval_repr);
        sdsfree(rawval_repr);
#endif

        RIOInitPut(rio,cfs[0],rawkeys[0],rawvals[0]);
        zfree(cfs), cfs = NULL;
        zfree(rawkeys), rawkeys = NULL;
        zfree(rawvals), rawvals = NULL;
    } else if (action == ROCKS_WRITE) {
        wb = rocksdb_writebatch_create();
        for (i = 0; i < numkeys; i++) {
            rocksdb_writebatch_put_cf(wb,rioGetCF(cfs[i]),
                    rawkeys[i],sdslen(rawkeys[i]),
                    rawvals[i], sdslen(rawvals[i]));
        }
        DEBUG_MSGS_APPEND(req->msgs,"exec-out-write","numkeys=%d",numkeys);
        RIOInitWrite(rio, wb);
    } else {
        errcode = SWAP_ERR_EXEC_UNEXPECTED_ACTION;
        goto end;
    }

    updateStatsSwapRIO(req,rio);
    if ((errcode = doRIO(rio))) {
        goto end;
    }

    if ((errcode = swapDataCleanObject(data,req->datactx))) {
        goto end;
    }
    DEBUG_MSGS_APPEND(req->msgs,"exec-out-cleanobject","ok");

    if (req->intention_flags & SWAP_EXEC_OUT_META) {
        errcode = doSwapOutMeta(data);

#ifdef SWAP_DEBUG
    objectMeta *object_meta = swapDataObjectMeta(data);
    sds dump = dumpObjectMeta(object_meta);
    DEBUG_MSGS_APPEND(req->msgs,"exec-swapoutmeta","%s => %s",
            (sds)data->key->ptr, dump);
    sdsfree(dump);
#endif

        if (errcode) goto end;
    }

end:

    DEBUG_MSGS_APPEND(req->msgs,"exec-out-end","errcode=%d",errcode);
    if (server.debug_swapout_notify_latency) usleep(server.debug_swapout_notify_latency*1000);
    doNotify(req,errcode);
    if (cfs) zfree(cfs);
    if (rawkeys) {
        for (i = 0; i < numkeys; i++) {
            sdsfree(rawkeys[i]);
        }
        zfree(rawkeys);
    }
    if (rawvals) {
        for (i = 0; i < numkeys; i++) {
            sdsfree(rawvals[i]);
        }
        zfree(rawvals);
    }
    RIODeinit(rio);
}


static int doSwapIntentionDel(swapRequest *req, int numkeys, int *cfs, sds *rawkeys) {
    RIO _rio = {0}, *rio = &_rio;
    int i, retval;
    UNUSED(req);

    rocksdb_writebatch_t *wb = rocksdb_writebatch_create();
    for (i = 0; i < numkeys; i++) {
        rocksdb_writebatch_delete_cf(wb,rioGetCF(cfs[i]),rawkeys[i],sdslen(rawkeys[i]));
    }

    RIOInitWrite(rio, wb);
    updateStatsSwapRIO(req,rio);
    retval = doRIO(rio);
    RIODeinit(rio);

    DEBUG_MSGS_APPEND(req->msgs,"exec-in.del","numkeys=%d,retval=%d",
            numkeys, retval);
    return retval;
}

int doAuxDelSubCustom(swapRequest *req, int action, int numkeys, int* cfs,
    sds* rawkeys, sds* rawvals) {

    int errcode = 0;
    int outaction, outnum;
    int* outcfs = NULL;
    sds* outrawkeys = NULL;

    if ((errcode = req->data->type->rocksDel(req->data, req->datactx, action,
                    numkeys, cfs, rawkeys, rawvals,
                    &outaction, &outnum, &outcfs, &outrawkeys))) {
        return errcode;
    }

    switch(outaction) {
    case ROCKS_WRITE:
        if ((errcode = doSwapIntentionDel(req, outnum, outcfs, outrawkeys))) {
            goto end;
        }
        break;
    default:
        goto end;
        break;
    }

end:
    if (outrawkeys) {
        for (int i = 0; i < outnum; i++) {
            sdsfree(outrawkeys[i]);
        }
        zfree(outrawkeys);
    }
    if (outcfs) {
        zfree(outcfs);
    }
    return errcode;
}

int doAuxDelSub(swapRequest *req, int action, int numkeys, int* cfs,
        sds* rawkeys, sds* rawvals) {
    if (req->data->type->rocksDel == NULL) {
        return doSwapIntentionDel(req,numkeys,cfs,rawkeys);
    } else {
        return doAuxDelSubCustom(req,action,numkeys,cfs,rawkeys,rawvals);
    }
}

/* do auxillary delete:
 * - metacf: if (intention is DEL) or (intention is IN.DEL and key turned hot)
 * - datacf: if intention is IN.DEL or corresponding score subkey deleted(zset)
 * - scorecf: if intention is IN.DEL or correspoding data subkey deleted(zset)
 */
int doAuxDel(swapRequest *req, RIO *rio) {
    sds *rawkeys, *rawvals;
    int errcode, numkeys, *cfs, *tmpcfs = NULL, i;

    switch (rio->action) {
    case ROCKS_GET:
        numkeys = 1;
        cfs = &rio->get.cf;
        rawkeys = &rio->get.rawkey;
        rawvals = &rio->get.rawval;
        break;
    case ROCKS_MULTIGET:
        numkeys = rio->multiget.numkeys;
        cfs = rio->multiget.cfs;
        rawkeys = rio->multiget.rawkeys;
        rawvals = rio->multiget.rawvals;
        break;
    case ROCKS_SCAN:
        tmpcfs = zmalloc(sizeof(int)*rio->scan.numkeys);
        for (i = 0; i < rio->scan.numkeys; i++)
            tmpcfs[i] = rio->scan.cf;

        numkeys = rio->scan.numkeys;
        cfs = tmpcfs;
        rawkeys = rio->scan.rawkeys;
        rawvals = rio->scan.rawvals;
        break;
    case ROCKS_RANGE:
        tmpcfs = zmalloc(sizeof(int)*rio->range.numkeys);
        for (i = 0; i < rio->range.numkeys; i++)
            tmpcfs[i] = rio->range.cf;

        numkeys = rio->range.numkeys;
        cfs = tmpcfs;
        rawkeys = rio->range.rawkeys;
        rawvals = rio->range.rawvals;
        break;
    default:
        numkeys = 0;
        break;
    }

    if (numkeys == 0) return 0;

    errcode = doAuxDelSub(req,rio->action,numkeys,cfs,rawkeys,rawvals);

    if (tmpcfs) zfree(tmpcfs);

    return errcode;
}

static void executeSwapInRequest(swapRequest *req) {
    void *decoded;
    int numkeys, errcode, action, *cfs = NULL;
    sds *rawkeys = NULL;
    RIO _rio = {0}, *rio = &_rio;
    swapData *data = req->data;
    if ((errcode = swapDataEncodeKeys(data,req->intention,req->datactx,
                &action,&numkeys,&cfs,&rawkeys))) {
        goto end;
    }
    DEBUG_MSGS_APPEND(req->msgs,"exec-in-encodekeys","action=%s, numkeys=%d",
            rocksActionName(action),numkeys);

    if (action == ROCKS_MULTIGET) {
        RIOInitMultiGet(rio,numkeys,cfs,rawkeys);
        if ((errcode = doRIO(rio))) {
            goto end;
        }
        DEBUG_MSGS_APPEND(req->msgs,"exec-in-multiget",
                "numkeys=%d,rio=ok", numkeys);

        if ((errcode = swapDataDecodeData(data,rio->multiget.numkeys,rio->multiget.cfs,
                    rio->multiget.rawkeys,rio->multiget.rawvals,&decoded))) {
            goto end;
        }
        DEBUG_MSGS_APPEND(req->msgs,"exec-in-decodedata","decoded=%p",(void*)decoded);
    } else if (action == ROCKS_GET) {
        serverAssert(numkeys == 1);
        RIOInitGet(rio, cfs[0], rawkeys[0]);
        if ((errcode = doRIO(rio))) {
            goto end;
        }

#ifdef SWAP_DEBUG
        sds rawval_repr = sdscatrepr(sdsempty(),rio->get.rawval,
                sdslen(rio->get.rawval));
        DEBUG_MSGS_APPEND(req->msgs,"exec-in-get","rawkey=%s,rawval=%s",rawkeys[0],rawval_repr);
        sdsfree(rawval_repr);
#endif

        if ((errcode = swapDataDecodeData(data,1,&rio->get.cf,&rio->get.rawkey,
                    &rio->get.rawval,&decoded))) {
            goto end;
        }
        DEBUG_MSGS_APPEND(req->msgs,"exec-in-decodedata","decoded=%p",(void*)decoded);

        /* rawkeys not moved, only rawkeys[0] moved, free when done. */
        zfree(cfs);
        zfree(rawkeys);
    } else if (action == ROCKS_SCAN) {
        int *tmpcfs, i;
        RIOInitScan(rio,cfs[0],rawkeys[0]);
        if ((errcode = doRIO(rio))) {
            goto end;
        }
        tmpcfs = zmalloc(sizeof(int)*rio->scan.numkeys);
        for (i = 0; i < rio->scan.numkeys; i++) tmpcfs[i] = cfs[0];
        DEBUG_MSGS_APPEND(req->msgs,"exec-in-scan","prefix=%s,rio=ok",rawkeys[0]);
        if ((errcode = swapDataDecodeData(data,rio->scan.numkeys,tmpcfs,rio->scan.rawkeys,
                    rio->scan.rawvals,&decoded))) {
            zfree(tmpcfs);
            goto end;
        }
        zfree(tmpcfs);
        DEBUG_MSGS_APPEND(req->msgs,"exec-in-decodedata", "decoded=%p",(void*)decoded);

        /* rawkeys not moved, only rawkeys[0] moved, free when done. */
        zfree(cfs);
        zfree(rawkeys);
    } else if (action == ROCKS_ITERATE) {
        int *tmpcfs, i;
        RIOInitIterate(rio,cfs[0],rawkeys[0],numkeys);
        if ((errcode = doRIO(rio))) {
            goto end;
        }

        tmpcfs = zmalloc(sizeof(int)*rio->iterate.numkeys);
        for (i = 0; i < rio->iterate.numkeys; i++) tmpcfs[i] = cfs[0];
        DEBUG_MSGS_APPEND(req->msgs,"exec-in-iterate","seek=%s,limit=%d,rio=ok",
                rawkeys[0], numkeys);
        if ((errcode = swapDataDecodeData(data,rio->iterate.numkeys,tmpcfs,rio->iterate.rawkeys,
                    rio->iterate.rawvals,&decoded))) {
            zfree(tmpcfs);
            goto end;
        }
        zfree(tmpcfs);
        DEBUG_MSGS_APPEND(req->msgs,"exec-in-decodedata", "decoded=%p",(void*)decoded);

        /* rawkeys not moved, only rawkeys[0] moved, free when done. */
        zfree(cfs);
        zfree(rawkeys);
    } else if (action == ROCKS_RANGE) {
        int *tmpcfs, i;
        RIOInitRange(rio, cfs[0], rawkeys[0], rawkeys[1], cfs[1], numkeys);
        if ((errcode = doRIO(rio))) {
            goto end;
        }
        tmpcfs = zmalloc(sizeof(int)*rio->range.numkeys);
        for (i = 0; i < rio->range.numkeys; i++) tmpcfs[i] = cfs[0];
        DEBUG_MSGS_APPEND(req->msgs,"exec-in-scan","prefix=%s,rio=ok",rawkeys[0]);
        if ((errcode = swapDataDecodeData(data,rio->range.numkeys,tmpcfs,rio->range.rawkeys,
                    rio->range.rawvals,&decoded))) {
            zfree(tmpcfs);
            goto end;
        }
        zfree(tmpcfs);
        DEBUG_MSGS_APPEND(req->msgs,"exec-in-decodedata", "decoded=%p",(void*)decoded);

        /* rawkeys not moved, only rawkeys[0] moved, free when done. */
        zfree(cfs);
        zfree(rawkeys);
    } else {
        errcode = SWAP_ERR_EXEC_UNEXPECTED_ACTION;
        goto end;
    }

    req->result = swapDataCreateOrMergeObject(data,decoded,req->datactx);
    DEBUG_MSGS_APPEND(req->msgs,"exec-in-createormerge","result=%p",(void*)req->result);

    if (req->intention_flags & SWAP_EXEC_IN_DEL) {
        int is_hot = swapDataMergedIsHot(data,req->result,req->datactx);

        /* There is no need to delete subkey if meta gets deleted,
         * subkeys will be deleted by compaction filter. */

        if (is_hot) {
            req->data->del_meta = 1;
            errcode = doSwapDelMeta(data);
            if (data->object_type != OBJ_STRING) {
                /* String is not versioned */
                goto end;
            }
        }

        if ((errcode = doAuxDel(req,rio))) {
            goto end;
        }
    }

end:
    updateStatsSwapRIO(req,rio);
    DEBUG_MSGS_APPEND(req->msgs,"exec-in-end","errcode=%d",errcode);
    doNotify(req,errcode);
    RIODeinit(rio);
}

static inline int RIOGetNotFound(RIO *rio) {
    serverAssert(rio->action == ROCKS_GET);
    return rio->get.rawval == NULL && rio->err == NULL;
}

static int swapRequestSwapInMeta(swapRequest *req) {
    int retval = 0;
    RIO _rio = {0}, *rio = &_rio;
    sds rawkey;
    swapData *data = req->data;

    rawkey = swapDataEncodeMetaKey(data);
    if (rawkey == NULL) {
        retval = SWAP_ERR_EXEC_FAIL;
        goto end;
    }

    RIOInitGet(rio,META_CF,rawkey);
    if ((retval = doRIO(rio))) {
        goto end;
    }

    if (RIOGetNotFound(rio)) {
        /* No swap needed if meta not found. */
        req->intention = SWAP_NOP;
        DEBUG_MSGS_APPEND(req->msgs,"exec-swapinmeta", "%s: notfound",
                (sds)data->key->ptr);
        goto end;
    }

    if ((retval = swapDataDecodeAndSetupMeta(data,rio->get.rawval,&req->datactx))) {
        goto end;
    }

#ifdef SWAP_DEBUG
    objectMeta *object_meta = swapDataObjectMeta(data);
    sds dump = dumpObjectMeta(object_meta);
    DEBUG_MSGS_APPEND(req->msgs,"exec-swapinmeta", "%s => %s",
            (sds)data->key->ptr,dump);
    sdsfree(dump);
#endif

    swapCtxSetSwapData(req->swapCtx,req->data,req->datactx);

end:
    RIODeinit(rio);
    return retval;
}

static inline int swapRequestIsMetaType(swapRequest *req) {
    return req->key_request != NULL;
}

static void executeSwapRequest(swapRequest *req) {
    serverAssert(req->errcode == 0);

    updateStatsSwapIntention(req);
    if (server.debug_delay_before_exec_swap) usleep(server.debug_delay_before_exec_swap * 1000);
    /* do execute swap */
    switch (req->intention) {
    case SWAP_NOP:
        doNotify(req,req->errcode);
        break;
    case SWAP_IN:
        executeSwapInRequest(req);
        break;
    case SWAP_OUT:
        executeSwapOutRequest(req);
        break;
    case SWAP_DEL: 
        executeSwapDelRequest(req);
        break;
    case ROCKSDB_UTILS:
        executeRocksdbUtils(req);
        break;
    default:
        doNotify(req,SWAP_ERR_EXEC_UNEXPECTED_ACTION);
        break;
    }
}

static inline void swapRequestSetIntention(swapRequest *req, int intention,
        uint32_t intention_flags) {
    req->intention = intention;
    req->intention_flags = intention_flags;
}

void processSwapRequest(swapRequest *req) {
    int errcode = 0, intention;
    uint32_t intention_flags;

    updateStatsSwapStart(req);
    if (!swapRequestIsMetaType(req)) {
         executeSwapRequest(req);
         return;
    }

    if ((errcode = swapRequestSwapInMeta(req))) {
        goto end;
    }

    /* key confirmed not exists, no need to execute swap request. */
    if (!swapDataAlreadySetup(req->data)) {
        goto end;
    }

    if ((errcode = swapDataAna(req->data,req->key_request,
                    &intention,&intention_flags,req->datactx))) {
        goto end;
    }

    swapRequestSetIntention(req,intention,intention_flags);

    /* If intention is nop, req will finish without executeXXX. */
    executeSwapRequest(req);

    return;
    
end:
    doNotify(req,errcode);

    return;
}

void swapDataTurnWarmOrHot(swapData *data) {
    if (data->expire != -1) {
        setExpire(NULL,data->db,data->key,data->expire);
    }
    data->db->cold_keys--;
}

void swapDataTurnCold(swapData *data) {
    if (data->expire != -1) {
        removeExpire(data->db,data->key);
    }
    data->db->cold_keys++;
}

void swapDataTurnDeleted(swapData *data, int del_skip) {
    if (swapDataIsCold(data)) {
        data->db->cold_keys--;
    } else {
        /* rocks-meta already deleted, only need to delete object_meta
         * from keyspace. */
        if (!del_skip && data->expire != -1) {
            removeExpire(data->db,data->key);
        }
    }
}

/* Called by async-complete-queue or parallel-sync in server thread
 * to swap in/out/del data */
void finishSwapRequest(swapRequest *req) {
    DEBUG_MSGS_APPEND(req->msgs,"exec-finish","intention=%s",
            swapIntentionName(req->intention));
    int retval = 0, del_skip = 0;
    if (req->errcode) return;

    swapData *data = req->data;
    void *datactx = req->datactx;

    switch (req->intention) {
    case SWAP_NOP:
        /* No swap for req if meta not found. */
        break;
    case SWAP_IN:
        retval = swapDataSwapIn(data,req->result,datactx);
        if (retval == 0) {
            if (swapDataIsCold(data) && req->result) {
                swapDataTurnWarmOrHot(data);
            }
        }
        break;
    case SWAP_OUT:
        /* exec removes expire if rocks-meta persists, while object_meta
         * is removed by swapdata. note that exec remove expire (satellite
         * dict) before swapout remove key from db.dict. */
        if ((req->intention_flags & SWAP_EXEC_OUT_META) &&
                !swapDataIsCold(data)) {
            swapDataTurnCold(data);
        }
        retval = swapDataSwapOut(data,datactx);
        break;
    case SWAP_DEL:
        del_skip = req->intention_flags & SWAP_FIN_DEL_SKIP;
        swapDataTurnDeleted(data,del_skip);
        retval = swapDataSwapDel(data,datactx,del_skip);
        break;
    case -1: //swap meta
        break;
    default:
        retval = SWAP_ERR_DATA_FIN_FAIL;
    }
    req->errcode = retval;
}

static inline void submitSwapRequest(int mode,swapRequest *req, int idx) {
    if (mode == SWAP_MODE_ASYNC) {
        asyncSwapRequestSubmit(req,idx);
    } else {
        parallelSyncSwapRequestSubmit(req,idx);
    }
}

void submitSwapMetaRequest(int mode,keyRequest *key_request, swapCtx *ctx, swapData* data,
        void *datactx, swapRequestFinishedCallback cb, void *pd, void *msgs, int idx) {
    swapRequest *req = swapRequestNew(key_request,-1,-1,ctx,data,datactx,cb,pd,msgs);
    submitSwapRequest(mode,req,idx);
}

void submitSwapDataRequest(int mode, int intention,
        uint32_t intention_flags, swapCtx *ctx, swapData* data, void *datactx,
        swapRequestFinishedCallback cb, void *pd, void *msgs, int idx) {
    swapRequest *req = swapRequestNew(NULL,intention,intention_flags,ctx,data,datactx,cb,pd,msgs);
    submitSwapRequest(mode,req,idx);
}

swapRequest *swapRequestNew(keyRequest *key_request, int intention,
        uint32_t intention_flags, swapCtx *ctx, swapData *data, void *datactx,
        swapRequestFinishedCallback cb, void *pd, void *msgs) {
    swapRequest *req = zcalloc(sizeof(swapRequest));
    UNUSED(msgs);
    req->key_request = key_request;
    req->intention = intention;
    req->intention_flags = intention_flags;
    req->swapCtx = ctx;
    req->data = data;
    req->datactx = datactx;
    req->result = NULL;
    req->finish_cb = cb;
    req->finish_pd = pd;
    req->swap_memory = 0;
#ifdef SWAP_DEBUG
    req->msgs = msgs;
#endif
    return req;
}

void swapRequestFree(swapRequest *req) {
    zfree(req);
}

#ifdef REDIS_TEST

void mockNotifyCallback(swapRequest *req, void *pd) {
    UNUSED(req),UNUSED(pd);
}

int wholeKeyRocksDataExists(redisDb *db, robj *key) {
    size_t vlen;
    char *err = NULL, *rawval = NULL;
    sds rawkey = rocksEncodeDataKey(db,key->ptr,0,NULL);
    rawval = rocksdb_get_cf(server.rocks->db, server.rocks->ropts,rioGetCF(DATA_CF),rawkey,sdslen(rawkey),&vlen,&err);
    serverAssert(err == NULL);
    zlibc_free(rawval);
    return rawval != NULL;
}

int wholeKeyRocksMetaExists(redisDb *db, robj *key) {
    size_t vlen;
    char *err = NULL, *rawval = NULL;
    sds rawkey = rocksEncodeMetaKey(db,key->ptr);
    rawval = rocksdb_get_cf(server.rocks->db, server.rocks->ropts,rioGetCF(META_CF),rawkey,sdslen(rawkey),&vlen,&err);
    serverAssert(err == NULL);
    zlibc_free(rawval);
    return rawval != NULL;
}

int doRocksdbFlush() {
    int i;
    char *err = NULL;
    rocksdb_flushoptions_t *flushopts = rocksdb_flushoptions_create();
    for (i = 0; i < CF_COUNT; i++) {
        rocksdb_flush_cf(server.rocks->db, flushopts, server.rocks->cf_handles[i], &err);
    }
    serverAssert(err == NULL);
    rocksdb_flushoptions_destroy(flushopts);
    return 0;
}

void initServer(void);
void initServerConfig(void);
void InitServerLast();
int swapExecTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int error = 0;
    server.hz = 10;
    sds rawkey1 = sdsnew("rawkey1"), rawkey2 = sdsnew("rawkey2");
    sds rawval1 = sdsnew("rawval1"), rawval2 = sdsnew("rawval2");
    sds prefix = sdsnew("rawkey");
    robj *key1 = createStringObject("key1",4);
    robj *val1 = createStringObject("val1",4);
    initTestRedisDb();
    redisDb *db = server.db;
    long long EXPIRE = 3000000000LL * 1000;

    keyRequest key1_req_, *key1_req = &key1_req_;
    key1_req->level = REQUEST_LEVEL_KEY;
    key1_req->b.num_subkeys = 0;
    key1_req->key = createStringObject("key1",4);
    key1_req->b.subkeys = NULL;
    swapCtx *ctx = swapCtxCreate(NULL,key1_req,NULL);

    TEST("exec: init") {
        initServerConfig();
        incrRefCount(val1);
        dbAdd(db,key1,val1);
        setExpire(NULL,db,key1,EXPIRE);
        if (!server.rocks) rocksInit();
        else rocksReinit();
        initStatsSwap();
    }

   TEST("exec: rio") {
       rocksdb_writebatch_t *wb;
       RIO _rio, *rio = &_rio;

       RIOInitPut(rio,DATA_CF,rawkey1,rawval1);
       test_assert(doRIO(rio) == C_OK);

       RIOInitGet(rio,DATA_CF,rawkey1);
       test_assert(doRIO(rio) == C_OK);
       test_assert(sdscmp(rio->get.rawval, rawval1) == 0);


       RIOInitDel(rio,DATA_CF,rawkey1);
       test_assert(doRIO(rio) == C_OK);

       wb = rocksdb_writebatch_create();
       rocksdb_writebatch_put_cf(wb,rioGetCF(DATA_CF),rawkey1,sdslen(rawkey1),rawval1,sdslen(rawval1));
       rocksdb_writebatch_put_cf(wb,rioGetCF(DATA_CF),rawkey2,sdslen(rawkey2),rawval2,sdslen(rawval2));
       RIOInitWrite(rio,wb);
       test_assert(doRIO(rio) == C_OK);

       sds *rawkeys = zmalloc(sizeof(sds)*2);
       int *cfs = zmalloc(sizeof(int)*2);
       rawkeys[0] = rawkey1, cfs[0] = DATA_CF;
       rawkeys[1] = rawkey2, cfs[1] = DATA_CF;
       RIOInitMultiGet(rio,2,cfs,rawkeys);
       test_assert(doRIO(rio) == C_OK);
       test_assert(rio->multiget.numkeys == 2);
       test_assert(sdscmp(rio->multiget.rawvals[0],rawval1) == 0);
       test_assert(sdscmp(rio->multiget.rawvals[1],rawval2) == 0);

       RIOInitScan(rio,DATA_CF,prefix);
       test_assert(doRIO(rio) == C_OK);
       test_assert(rio->scan.numkeys == 2);
       test_assert(sdscmp(rio->scan.rawvals[0],rawval1) == 0);
       test_assert(sdscmp(rio->scan.rawvals[1],rawval2) == 0);
   }

   TEST("exec: swap-out hot string") {
       val1 = lookupKey(db,key1,LOOKUP_NOTOUCH);
       test_assert(val1 != NULL);
       test_assert(getExpire(db,key1) == EXPIRE);
       swapData *data = createWholeKeySwapDataWithExpire(db,key1,val1,EXPIRE,NULL);
       swapRequest *req = swapRequestNew(NULL/*!cold*/,SWAP_OUT,SWAP_EXEC_OUT_META,ctx,data,NULL,NULL,NULL,NULL);
       req->notify_cb = mockNotifyCallback;
       req->notify_pd = NULL;
       processSwapRequest(req);
       test_assert(req->errcode == 0);
       finishSwapRequest(req);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
       test_assert(getExpire(db,key1) == -1);
       test_assert(wholeKeyRocksDataExists(db,key1));
       test_assert(wholeKeyRocksMetaExists(db,key1));
   }

   TEST("exec: swap-in cold string") {
       /* rely on data swap out to rocksdb by previous case */
       swapData *data = createSwapData(db,key1,NULL);
       key1_req->cmd_intention = SWAP_IN;
       key1_req->cmd_intention_flags = 0;
       swapRequest *req = swapRequestNew(key1_req,-1,-1,ctx,data,NULL,NULL,NULL,NULL);
       req->notify_cb = mockNotifyCallback;
       req->notify_pd = NULL;
       processSwapRequest(req);
       test_assert(req->errcode == 0);
       finishSwapRequest(req);
       test_assert((val1 = lookupKey(db,key1,LOOKUP_NOTOUCH)) != NULL && !objectIsDirty(val1));
       test_assert(getExpire(db,key1) == EXPIRE);
       test_assert(wholeKeyRocksDataExists(db,key1));
       test_assert(wholeKeyRocksMetaExists(db,key1));
   } 

   TEST("exec: swap-del hot string") {
       /* rely on data swap out to rocksdb by previous case */
       val1 = lookupKey(db,key1,LOOKUP_NOTOUCH);
       swapData *data = createWholeKeySwapData(db,key1,val1,NULL);
       swapRequest *req = swapRequestNew(NULL/*!cold*/,SWAP_DEL,0,ctx,data,NULL,NULL,NULL,NULL);
       req->notify_cb = mockNotifyCallback;
       req->notify_pd = NULL;
       executeSwapRequest(req);
       test_assert(req->errcode == 0);
       finishSwapRequest(req);
       test_assert(req->errcode == 0);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
       test_assert(!wholeKeyRocksDataExists(db,key1));
       test_assert(!wholeKeyRocksMetaExists(db,key1));
   }

   TEST("exec: swap-in.del") {
       incrRefCount(val1);
       dbAdd(db,key1,val1);

       /* swap out hot key1 */
       swapData *out_data = createWholeKeySwapData(db,key1,val1,NULL);
       swapRequest *out_req = swapRequestNew(NULL/*!cold*/,SWAP_OUT,SWAP_EXEC_OUT_META,ctx,out_data,NULL,NULL,NULL,NULL);
       out_req->notify_cb = mockNotifyCallback;
       out_req->notify_pd = NULL;
       processSwapRequest(out_req);
       test_assert(out_req->errcode == 0);
       finishSwapRequest(out_req);
       test_assert(out_req->errcode == 0);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
       test_assert(wholeKeyRocksMetaExists(db,key1));
       test_assert(wholeKeyRocksDataExists(db,key1));

       /* In.del cold key1 */
       swapData *in_del_data = createSwapData(db,key1,NULL);
       key1_req->cmd_intention = SWAP_IN;
       key1_req->cmd_intention_flags = SWAP_IN_DEL;
       swapRequest *in_del_req = swapRequestNew(key1_req,-1,-1,ctx,in_del_data,NULL,NULL,NULL,NULL);
       in_del_req->notify_cb = mockNotifyCallback;
       in_del_req->notify_pd = NULL;
       processSwapRequest(in_del_req);
       test_assert(in_del_req->errcode == 0);
       finishSwapRequest(in_del_req);
       test_assert(in_del_req->errcode == 0);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) != NULL);
       test_assert(!wholeKeyRocksMetaExists(db,key1));
       test_assert(!wholeKeyRocksDataExists(db,key1));
   }

   return error;
}

#endif

