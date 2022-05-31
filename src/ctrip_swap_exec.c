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

    rio->multiget.rawvals = zmalloc(rio->multiget.numkeys*sizeof(sds));
    for (i = 0; i < rio->multiget.numkeys; i++) {
        rio->multiget.rawvals[i] = sdsnewlen(values_list[i],
                values_list_sizes[i]);
        zlibc_free(values_list[i]);
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

    iter = rocksdb_create_iterator(server.rocks->db,server.rocks->ropts);
    rocksdb_iter_seek_to_first(iter);
    //rocksdb_iter_seek(iter,"raw",4); //FIXME rocksdb iter not working if seek

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

void dumpRIO(RIO *rio) {
    sds repr = sdsnew("[ROCKS] ");
    switch (rio->action) {
    case ROCKS_GET:
        repr = sdscat(repr, "GET rawkey=");
        repr = sdscatrepr(repr, rio->get.rawkey, sdslen(rio->get.rawkey));
        repr = sdscat(repr, ", rawval=");
        repr = sdscatrepr(repr, rio->get.rawval, sdslen(rio->get.rawval));
        break;
    case ROCKS_PUT:
        repr = sdscat(repr, "PUT rawkey=");
        repr = sdscatrepr(repr, rio->get.rawkey, sdslen(rio->get.rawkey));
        repr = sdscat(repr, ", rawval=");
        repr = sdscatrepr(repr, rio->get.rawval, sdslen(rio->get.rawval));
        break;
    case ROCKS_DEL:
        repr = sdscat(repr, "DEL ");
        repr = sdscatrepr(repr, rio->get.rawkey, sdslen(rio->get.rawkey));
        break;
    case ROCKS_WRITE:
        repr = sdscat(repr, "WRITE ");
        break;
    case ROCKS_MULTIGET:
        repr = sdscat(repr, "MULTIGET ");
        break;
    case ROCKS_SCAN:
        repr = sdscat(repr, "MULTIGET ");
        break;
    default:
        serverPanic("[rocks] Unknown io action: %d", rio->action);
        break;
    }
    serverLog(LL_NOTICE, "%s", repr);
    sdsfree(repr);
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

#ifdef ROCKS_DEBUG
    dumpRIO(rio);
#endif

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
    DEBUG_MSGS_APPEND(req->msgs,"execswap-del-encodekeys",
            "action=%s, numkeys=%d", rocksActionName(action), numkeys);

    if (numkeys == 0) goto end;

    if (action == ROCKS_WRITE) {
        wb = rocksdb_writebatch_create();
        for (i = 0; i < numkeys; i++) {
            rocksdb_writebatch_delete(wb, rawkeys[i], sdslen(rawkeys[i]));
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-del-write","numkeys=%d.",numkeys);
        RIOInitWrite(rio,wb);
    } else if (action == ROCKS_DEL) {
        serverAssert(numkeys == 1 && rawkeys);
        DEBUG_MSGS_APPEND(req->msgs,"execswap-del-del","rawkey=%s",rawkeys[0]);
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
    DEBUG_MSGS_APPEND(req->msgs,"execswap-del-cleanobject", "ok");

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
    DEBUG_MSGS_APPEND(req->msgs,"execswap-out-encodedata","action=%s, numkeys=%d",
            rocksActionName(action), numkeys);

    if (numkeys <= 0) goto end;

    if (action == ROCKS_PUT) {
        serverAssert(numkeys == 1);

#ifdef SWAP_DEBUG
        sds rawval_repr = sdscatrepr(sdsempty(), rawvals[0], sdslen(rawvals[0]));
        DEBUG_MSGS_APPEND(req->msgs,"execswap-out-put","rawkey=%s,rawval=%s",
                rawkeys[0], rawval_repr);
        sdsfree(rawval_repr);
#endif

        RIOInitPut(rio,rawkeys[0],rawvals[0]);
        zfree(rawkeys), rawkeys = NULL;
        zfree(rawvals), rawvals = NULL;
    } else if (action == ROCKS_WRITE) {
        wb = rocksdb_writebatch_create();
        for (i = 0; i < numkeys; i++) {
            rocksdb_writebatch_put(wb,rawkeys[i],sdslen(rawkeys[i]),
                    rawvals[i], sdslen(rawvals[i]));
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-out-write","numkeys=%d",numkeys);
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
    DEBUG_MSGS_APPEND(req->msgs,"execswap-out-cleanobject","ok");

    doNotify(req);

end:
    DEBUG_MSGS_APPEND(req->msgs,"execswap-out-end","retval=%d",retval);

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
    DEBUG_MSGS_APPEND(req->msgs,"execswap-in-encodekeys","action=%s, numkeys=%d",
            rocksActionName(action),numkeys);

    if (numkeys <= 0) goto end;

    if (action == ROCKS_MULTIGET) {
        RIOInitMultiGet(rio,numkeys,rawkeys);
        if (doRIO(rio)) {
            retval = EXEC_FAIL;
            goto end;
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-multiget","numkeys=%d,rio=ok",numkeys);
        if (swapDataDecodeData(data,rio->multiget.numkeys,
                    rio->multiget.rawkeys,rio->multiget.rawvals,&decoded)) {
            retval = EXEC_FAIL;
            goto end;
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-decodedata","decoded=%p",(void*)decoded);
    } else if (action == ROCKS_GET) {
        serverAssert(numkeys == 1);
        RIOInitGet(rio, rawkeys[0]);
        if (doRIO(rio)) {
            retval = EXEC_FAIL;
            goto end;
        }

#ifdef SWAP_DEBUG
        sds rawval_repr = sdscatrepr(sdsempty(),rio->get.rawval,
                sdslen(rio->get.rawval));
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-get","rawkey=%s,rawval=%s",rawkeys[0],rawval_repr);
        sdsfree(rawval_repr);
#endif

        if (swapDataDecodeData(data,1,&rio->get.rawkey,
                    &rio->get.rawval,&decoded)) {
            retval = EXEC_FAIL;
            goto end;
        }
        zfree(rawkeys);
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-decodedata","decoded=%p",(void*)decoded);
    } else if (action == ROCKS_SCAN) {
        serverAssert(numkeys == 1);
        RIOInitScan(rio, rawkeys[0]);
        if (doRIO(rio)) {
            retval = EXEC_FAIL;
            goto end;
        }
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-scan","prefix=%s,rio=ok",rawkeys[0]);
        if (swapDataDecodeData(data,rio->scan.numkeys,rio->scan.rawkeys,
                    rio->scan.rawvals,&decoded)) {
            retval = EXEC_FAIL;
            goto end;
        }
        zfree(rawkeys);
        DEBUG_MSGS_APPEND(req->msgs,"execswap-in-decodedata", "decoded=%p",(void*)decoded);
    } else {
        retval = EXEC_FAIL;
        goto end;
    }

    req->result = swapDataCreateOrMergeObject(data,decoded,req->datactx);
    DEBUG_MSGS_APPEND(req->msgs,"execswap-in-createormerge","result=%p",(void*)req->result);

    doNotify(req);

end:
    DEBUG_MSGS_APPEND(req->msgs,"execswap-in-end","retval=%d",retval);
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
    DEBUG_MSGS_APPEND(req->msgs,"execswap-finish","intention=%s",
            swapIntentionName(req->intention));
    switch(req->intention) {
    case SWAP_IN: return swapDataSwapIn(req->data,req->result,req->datactx);
    case SWAP_OUT: return swapDataSwapOut(req->data,req->datactx);
    case SWAP_DEL: return swapDataSwapDel(req->data,req->datactx);
    default: return -1;
    }
}

void submitSwapRequest(int mode, int intention, swapData* data, void *datactx,
        swapRequestFinishedCallback cb, void *pd, void *msgs) {
    swapRequest *req = swapRequestNew(intention,data,datactx,cb,pd,msgs);
    if (mode == SWAP_MODE_ASYNC) {
        asyncSwapRequestSubmit(req);
    } else {
        parallelSyncSwapRequestSubmit(req);
    }
}

swapRequest *swapRequestNew(int intention, swapData *data, void *datactx,
        swapRequestFinishedCallback cb, void *pd, void *msgs) {
    swapRequest *req = zcalloc(sizeof(swapRequest));
    UNUSED(msgs);
    req->intention = intention;
    req->data = data;
    req->datactx = datactx;
    req->result = NULL;
    req->finish_cb = cb;
    req->finish_pd = pd;
#ifdef SWAP_DEBUG
    req->msgs = msgs;
#endif
    return req;
}

void swapRequestFree(swapRequest *req) {
    if (req->result) decrRefCount(req->result);
    zfree(req);
}

#ifdef REDIS_TEST

int mockNotifyCallback(swapRequest *req, void *pd) {
    UNUSED(req),UNUSED(pd);
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

    TEST("exec: init") {
        initServerConfig();
        dbAdd(db,key1,val1);
        rocksInit();
    }

   TEST("exec: rio") {
       rocksdb_writebatch_t *wb;
       RIO _rio, *rio = &_rio;

       RIOInitPut(rio,rawkey1,rawval1);
       test_assert(doRIO(rio) == C_OK);

       RIOInitGet(rio,rawkey1);
       test_assert(doRIO(rio) == C_OK);
       test_assert(sdscmp(rio->get.rawval, rawval1) == 0);

       RIOInitDel(rio,rawkey1);
       test_assert(doRIO(rio) == C_OK);

       wb = rocksdb_writebatch_create();
       rocksdb_writebatch_put(wb,rawkey1,sdslen(rawkey1),rawval1,sdslen(rawval1));
       rocksdb_writebatch_put(wb,rawkey2,sdslen(rawkey2),rawval2,sdslen(rawval2));
       RIOInitWrite(rio,wb);
       test_assert(doRIO(rio) == C_OK);

       sds *rawkeys = zmalloc(sizeof(sds)*2);
       rawkeys[0] = rawkey1;
       rawkeys[1] = rawkey2;
       RIOInitMultiGet(rio,2,rawkeys);
       test_assert(doRIO(rio) == C_OK);
       test_assert(rio->multiget.numkeys == 2);
       test_assert(sdscmp(rio->multiget.rawvals[0],rawval1) == 0);
       test_assert(sdscmp(rio->multiget.rawvals[1],rawval2) == 0);

       RIOInitScan(rio,prefix);
       test_assert(doRIO(rio) == C_OK);
       test_assert(rio->scan.numkeys == 2);
       test_assert(sdscmp(rio->scan.rawvals[0],rawval1) == 0);
       test_assert(sdscmp(rio->scan.rawvals[1],rawval2) == 0);
   } 

   TEST("exec: swap-out") {
       val1 = lookupKey(db,key1,LOOKUP_NOTOUCH);
       swapData *data = createWholeKeySwapData(db,key1,val1,NULL,NULL);
       swapRequest *req = swapRequestNew(SWAP_OUT,data,NULL,NULL,NULL,NULL);
       req->notify_cb = mockNotifyCallback;
       req->notify_pd = NULL;
       test_assert(executeSwapRequest(req) == 0);
       test_assert(finishSwapRequest(req) == 0);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
       test_assert(lookupEvictKey(db,key1) != NULL);
   }

   TEST("exec: swap-in") {
       robj *evict1 = lookupEvictKey(db,key1);
       swapData *data = createWholeKeySwapData(db,key1,NULL,evict1,NULL);
       swapRequest *req = swapRequestNew(SWAP_IN,data,NULL,NULL,NULL,NULL);
       req->notify_cb = mockNotifyCallback;
       req->notify_pd = NULL;
       test_assert(executeSwapRequest(req) == 0);
       test_assert(finishSwapRequest(req) == 0);
       test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) != NULL);
       test_assert(lookupEvictKey(db,key1) == NULL);
   } 

   TEST("exec: swap-del") {
       val1 = lookupKey(db,key1,LOOKUP_NOTOUCH);
       swapData *data = createWholeKeySwapData(db,key1,val1,NULL,NULL);
       swapRequest *req = swapRequestNew(SWAP_DEL,data,NULL,NULL,NULL,NULL);
       req->notify_cb = mockNotifyCallback;
       req->notify_pd = NULL;
       test_assert(executeSwapRequest(req) == 0);
       test_assert(finishSwapRequest(req) == 0);
       // FIXME test_assert(lookupKey(db,key1,LOOKUP_NOTOUCH) == NULL);
       test_assert(lookupEvictKey(db,key1) == NULL);
   }

   return 0;
}

#endif

