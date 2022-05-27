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

#define EXEC_OK      0
#define EXEC_FAIL    -1

#define RIO_SCAN_NUMKEYS_ALLOC_INIT 16
#define RIO_SCAN_NUMKEYS_ALLOC_LINER 4096

/* --- RIO --- */
void RIOInitGet(RIO *rio, sds rawkey) {
    rio->action = ROCKS_GET;
    rio->get.rawkey = rawkey;
    rio->err = NULL;
}

void RIOInitPut(RIO *rio, sds rawkey, sds rawval) {
    rio->action = ROCKS_PUT;
    rio->put.rawkey = rawkey;
    rio->put.rawval = rawval;
    rio->err = NULL;
}

void RIOInitDel(RIO *rio, sds rawkey) {
    rio->action = ROCKS_DEL;
    rio->del.rawkey = rawkey;
    rio->err = NULL;
}

void RIOInitWrite(RIO *rio, rocksdb_writebatch_t *wb) {
    rio->action = ROCKS_WRITE;
    rio->write.wb = wb;
    rio->err = NULL;
}

void RIOInitMultiGet(RIO *rio, int numkeys, sds *rawkeys) {
    rio->action = ROCKS_MULTIGET;
    rio->multiget.numkeys = numkeys;
    rio->multiget.rawkeys = rawkeys;
    rio->err = NULL;
}

void RIOInitScan(RIO *rio, sds prefix) {
    rio->action = ROCKS_SCAN;
    rio->scan.prefix = prefix;
    rio->err = NULL;
}

void RIODeinit(RIO *rio) {
    int i;

    switch (rio->action) {
    case  ROCKS_GET:
        sdsfree(rio->get.rawkey);
        sdsfree(rio->get.rawval);
        break;
    case  ROCKS_PUT:
        sdsfree(rio->put.rawkey);
        sdsfree(rio->put.rawval);
        break;
    case  ROCKS_DEL:
        sdsfree(rio->del.rawkey);
        break;
    case  ROCKS_MULTIGET:
        for (i = 0; i < rio->multiget.numkeys; i++) {
            if (rio->multiget.rawkeys) sdsfree(rio->multiget.rawkeys[i]);
            if (rio->multiget.rawvals) sdsfree(rio->multiget.rawvals[i]);
        }
        zfree(rio->multiget.rawkeys);
        zfree(rio->multiget.rawvals);
        break;
    case  ROCKS_SCAN:
        sdsfree(rio->scan.prefix);
        for (i = 0; i < rio->scan.numkeys; i++) {
            if (rio->scan.rawkeys) sdsfree(rio->scan.rawkeys[i]);
            if (rio->scan.rawvals) sdsfree(rio->scan.rawvals[i]);
        }
        zfree(rio->scan.rawkeys);
        zfree(rio->scan.rawvals);
        break;
    case  ROCKS_WRITE:
        rocksdb_writebatch_destroy(rio->write.wb);
        break;
    default:
        break;
    }
}

static int doRIOGet(RIO *rio) {
    size_t vallen;
    char *err = NULL, *val;

    val = rocksdb_get(server.rocks->db, server.rocks->ropts,
            rio->get.rawkey, sdslen(rio->get.rawkey), &vallen, &err);
    if (err != NULL) {
        rio->err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb get failed: %s", err);
        return -1;
    }
    rio->get.rawval = sdsnewlen(val, vallen);
    zlibc_free(val);
    return 0;
}

static int doRIOPut(RIO *rio) {
    char *err = NULL;
    rocksdb_put(server.rocks->db, server.rocks->wopts,
            rio->put.rawkey, sdslen(rio->put.rawkey),
            rio->put.rawval, sdslen(rio->put.rawval), &err);
    if (err != NULL) {
        rio->err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb write failed: %s", err);
        return -1;
    }
    return 0;
}

static int doRIODel(RIO *rio) {
    char *err = NULL;
    rocksdb_delete(server.rocks->db, server.rocks->wopts,
            rio->del.rawkey, sdslen(rio->del.rawkey), &err);
    if (err != NULL) {
        rio->err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb del failed: %s", err);
        return -1;
    }
    return 0;
}

static int doRIOWrite(RIO *rio) {
    char *err = NULL;
    rocksdb_write(server.rocks->db, server.rocks->wopts,
            rio->write.wb, &err);
    if (err != NULL) {
        rio->err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb del failed: %s", err);
        return -1;
    }
    return 0;
}

static int doRIOMultiGet(RIO *rio) {
    char *err = NULL;
    int ret = 0, i;
    char **keys_list = zmalloc(rio->multiget.numkeys*sizeof(char*));
    char **values_list = zmalloc(rio->multiget.numkeys*sizeof(char*));
    size_t *keys_list_sizes = zmalloc(rio->multiget.numkeys*sizeof(size_t));
    size_t *values_list_sizes = zmalloc(rio->multiget.numkeys*sizeof(size_t));

    for (i = 0; i < rio->multiget.numkeys; i++) {
        keys_list[i] = rio->multiget.rawkeys[i];
        keys_list_sizes[i] = sdslen(rio->multiget.rawkeys[i]);
    }

    rocksdb_multi_get(server.rocks->db, server.rocks->ropts,
            rio->multiget.numkeys,
            (const char**)keys_list, (const size_t*)keys_list_sizes,
            values_list, values_list_sizes, &err);
    if (err != NULL) {
        rio->err = err;
        serverLog(LL_WARNING,"[rocks] do rocksdb del failed: %s", err);
        ret = -1;
        goto end;
    }

end:
    zfree(keys_list);
    zfree(values_list);
    zfree(keys_list_sizes);
    zfree(values_list_sizes);
    return ret;
}

static int doRIOScan(RIO *rio) {
    int ret = 0;
    char *err = NULL;
    rocksdb_iterator_t *iter = NULL;
    sds prefix = rio->scan.prefix;
    size_t numkeys = 0, numalloc = 8;
    sds *rawkeys = zmalloc(numalloc*sizeof(sds));
    sds *rawvals = zmalloc(numalloc*sizeof(sds));

    iter = rocksdb_create_iterator(server.rocks->db, server.rocks->ropts);
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
        rawkeys[numkeys] = sdsnewlen(rawkey, klen);
        rawvals[numkeys] = sdsnewlen(rawval, vlen);
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

static int doRIO(RIO *rio) {
    int ret;
    if (server.debug_rio_latency) usleep(server.debug_rio_latency*1000);

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
    default:
        serverPanic("[rocks] Unknown io action: %d", rio->action);
        return -1;
    }
    return ret;
}

static void doNotify(swapRequest *req) {
    req->notify_cb(req, req->notify_pd);
}

static int executeSwapDelRequest(swapRequest *req) {
    int i, numkeys, retval = EXEC_OK, action;
    sds *rawkeys = NULL;
    RIO _rio = {0}, *rio = &_rio;
    rocksdb_writebatch_t *wb;
    swapData *data = req->data;

    if (swapDataEncodeKeys(data,req->intention,&action,&numkeys,&rawkeys)) {
        retval = EXEC_FAIL;
        goto end;
    }

    if (numkeys == 0) goto end;

    if (action == ROCKS_WRITE) {
        wb = rocksdb_writebatch_create();
        for (i = 0; i < numkeys; i++) {
            rocksdb_writebatch_delete(wb, rawkeys[i], sdslen(rawkeys[i]));
        }
        RIOInitWrite(rio,wb);
    } else if (action == ROCKS_DEL) {
        serverAssert(numkeys == 1 && rawkeys);
        RIOInitDel(rio,rawkeys[0]);
        zfree(rawkeys), rawkeys = NULL;
    } else {
        retval = EXEC_FAIL;
        goto end;
    }

    if (doRIO(rio)) {
        retval = EXEC_FAIL;
        goto end;
    }

    if (swapDataCleanObject(data,req->datactx)) {
        retval = EXEC_FAIL;
        goto end;
    }

    doNotify(req);

end:
    RIODeinit(rio);

    return retval;
}

static int executeSwapOutRequest(swapRequest *req) {
    int i, numkeys, retval = EXEC_OK, action;
    sds *rawkeys = NULL, *rawvals = NULL;
    RIO _rio = {0}, *rio = &_rio;
    rocksdb_writebatch_t *wb = NULL;
    swapData *data = req->data;

    if (swapDataEncodeData(data,req->intention,&action,&numkeys,
                &rawkeys,&rawvals,req->datactx)) {
        retval = EXEC_FAIL;
        goto end;
    }

    if (numkeys <= 0) goto end;

    if (action == ROCKS_PUT) {
        serverAssert(numkeys == 1);
        RIOInitPut(rio,rawkeys[0],rawvals[0]);
        zfree(rawkeys), rawkeys = NULL;
        zfree(rawvals), rawvals = NULL;
    } else if (action == ROCKS_WRITE) {
        wb = rocksdb_writebatch_create();
        for (i = 0; i < numkeys; i++) {
            rocksdb_writebatch_put(wb, rawkeys[i], sdslen(rawkeys[i]),
                    rawvals[i], sdslen(rawvals[i]));
        }
        RIOInitWrite(rio, wb);
    } else {
        retval = EXEC_FAIL;
        goto end;
    }

    if (doRIO(rio)) {
        retval = EXEC_FAIL;
        goto end;
    }

    if (swapDataCleanObject(data,req->datactx)) {
        retval = EXEC_FAIL;
        goto end;
    }

    doNotify(req);

end:
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
    return retval;
}

static int executeSwapInRequest(swapRequest *req) {
    robj *decoded;
    int numkeys, retval = EXEC_OK, action;
    sds *rawkeys = NULL;
    RIO _rio = {0}, *rio = &_rio;
    swapData *data = req->data;

    if (swapDataEncodeKeys(data,req->intention,&action,&numkeys,&rawkeys)) {
        retval = EXEC_FAIL;
        goto end;
    }

    if (numkeys <= 0) return retval;

    if (action == ROCKS_MULTIGET) {
        RIOInitMultiGet(rio, numkeys, rawkeys);
        if (doRIO(rio)) {
            retval = EXEC_FAIL;
            goto end;
        }
        if (swapDataDecodeData(data,rio->multiget.numkeys,
                    rio->multiget.rawkeys,rio->multiget.rawvals,&decoded)) {
            retval = EXEC_FAIL;
            goto end;
        }
    } else if (action == ROCKS_GET) {
        serverAssert(numkeys == 1);
        RIOInitGet(rio, rawkeys[0]);
        if (doRIO(rio)) {
            retval = EXEC_FAIL;
            goto end;
        }
        if (swapDataDecodeData(data,1,&rio->get.rawkey,
                    &rio->get.rawval,&decoded)) {
            retval = EXEC_FAIL;
            goto end;
        }
        zfree(rawkeys);
    } else if (action == ROCKS_SCAN) {
        serverAssert(numkeys == 1);
        RIOInitScan(rio, rawkeys[0]);
        if (doRIO(rio)) {
            retval = EXEC_FAIL;
            goto end;
        }
        if (swapDataDecodeData(data,rio->scan.numkeys,rio->scan.rawkeys,
                    rio->scan.rawvals,&decoded)) {
            retval = EXEC_FAIL;
            goto end;
        }
        zfree(rawkeys);
    } else {
        retval = EXEC_FAIL;
        goto end;
    }

    req->result = swapDataCreateOrMergeObject(data,decoded,req->datactx);
    doNotify(req);

end:
    RIODeinit(rio);
    return retval;
}

int executeSwapRequest(swapRequest *req) {
    switch (req->intention) {
    case SWAP_IN: return executeSwapInRequest(req);
    case SWAP_OUT: return executeSwapOutRequest(req);
    case SWAP_DEL: return executeSwapDelRequest(req);
    default: return EXEC_FAIL;
    }
}

/* Called by async-complete-queue or parallel-sync in server thread
 * to swap in/out/del data */
int finishSwapRequest(swapRequest *req) {
    switch(req->intention) {
    case SWAP_IN: return swapDataSwapIn(req->data,req->result,req->datactx);
    case SWAP_OUT: return swapDataSwapOut(req->data,req->datactx);
    case SWAP_DEL: return swapDataSwapDel(req->data,req->datactx);
    default: return -1;
    }
}

void submitSwapRequest(int mode, int intention, swapData* data, void *datactx,
        swapRequestFinishedCallback cb, void *pd) {
    swapRequest *req = swapRequestNew(intention,data,datactx,cb,pd);
    if (mode == SWAP_MODE_ASYNC) {
        asyncSwapRequestSubmit(req);
    } else {
        parallelSyncSwapRequestSubmit(req);
    }
}

swapRequest *swapRequestNew(int intention, swapData *data, void *datactx,
        swapRequestFinishedCallback cb, void *pd) {
    swapRequest *req = zcalloc(sizeof(swapRequest));
    req->intention = intention;
    req->data = data;
    req->datactx = datactx;
    req->result = NULL;
    req->finish_cb = cb;
    req->finish_pd = pd;
    return req;
}

void swapRequestFree(swapRequest *req) {
    if (req->result) decrRefCount(req->result);
    zfree(req);
}
