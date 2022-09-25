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
#include <ctype.h>

void scanMetaInit(scanMeta *meta, int object_type, sds key, long long expire) {
    meta->key = key;
    meta->expire = expire;
    meta->object_type = object_type;
}

void scanMetaDeinit(scanMeta *meta) {
    if (meta->key) sdsfree(meta->key);
    meta->key = NULL;
    meta->expire = -1;
    meta->object_type = -1;
}

void metaScanResultMakeRoom(metaScanResult *result, int num) {
	/* Resize if necessary */
	if (num > result->size) {
		if (result->metas != result->buffer) {
			/* We're not using a static buffer, just (re)alloc */
			result->metas = zrealloc(result->metas,
                    num*sizeof(scanMeta));
		} else {
			/* We are using a static buffer, copy its contents */
			result->metas = zmalloc(num*sizeof(scanMeta));
			if (result->num) {
				memcpy(result->metas,result->buffer,
                        result->num*sizeof(scanMeta));
            }
		}
		result->size = num;
	}
}

metaScanResult *metaScanResultCreate() {
    metaScanResult *result = zcalloc(sizeof(metaScanResult));
    result->metas = result->buffer;
    result->size = DEFAULT_SCANMETA_BUFFER;
    result->num = 0;
    result->nextseek = NULL;
    return result;
}

void metaScanResultSetNextSeek(metaScanResult *result, sds nextseek) {
    result->nextseek = nextseek;
}

void metaScanResultAppend(metaScanResult *result, int object_type, sds key, long long expire) {
    if (result->num == result->size) {
        int newsize = result->size + 
            (result->size > 1024 ? 1024 : result->size);
        metaScanResultMakeRoom(result, newsize);
    }

    scanMeta *meta = &result->metas[result->num++];
    scanMetaInit(meta,object_type,key,expire);
}

void freeScanMetaResult(metaScanResult *result) {
    if (result == NULL) return;
    if (result->nextseek) {
        sdsfree(result->nextseek);
        result->nextseek = NULL;
    }
    for (int i = 0; i < result->num; i++) {
        scanMetaDeinit(&result->metas[i]);
    }
    result->num = 0;
    if (result->metas != result->buffer) {
        zfree(result->metas);
        result->metas = NULL;
    }
    zfree(result);
}

/* MetaScanDataCtx */
void metaScanDataCtxSwapAna(metaScanDataCtx *datactx, int *intention,
        uint32_t *intention_flags) {
    if (datactx->type->swapAna) {
        datactx->type->swapAna(datactx,intention,intention_flags);
    }
}

void metaScanDataCtxSwapIn(metaScanDataCtx *datactx, metaScanResult *result) {
    if (datactx->type->swapIn) {
        datactx->type->swapIn(datactx,result);
    }
}

/* metaScanDataCtx - Scan */
typedef struct metaScanDataCtxScan {
    client *c;
    unsigned long outer_cursor;
    unsigned long cursor;
} metaScanDataCtxScan;

static inline int parseScanCursor(robj *o, unsigned long *cursor) {
    char *eptr;
    errno = 0;
    *cursor = strtoul(o->ptr, &eptr, 10);
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
        return -1;
    return 0;
}

void rewindClientSwapScanCursor(client *c) {
    if (c->swap_scan_nextseek) sdsfree(c->swap_scan_nextseek);
    c->swap_scan_nextcursor = 0;
    c->swap_scan_nextseek = NULL;
}

void metaScanDataCtxScanSwapAna(metaScanDataCtx *datactx,
        int *intention, uint32_t *intention_flags) {
    metaScanDataCtxScan *scanctx = datactx->extend;
    if (scanctx == NULL) { /* Hot cursor */
        *intention = SWAP_NOP;
        *intention_flags = 0;
    } else {
        *intention = SWAP_IN;
        *intention_flags = 0;
    }
}

void metaScanDataCtxScanSwapIn(struct metaScanDataCtx *datactx, metaScanResult *result) {
    client *c = datactx->c;
    metaScanDataCtxScan *scanctx = datactx->extend;

    c->swap_scan_nextcursor = 0;
    if (c->swap_scan_nextseek) {
        sdsfree(c->swap_scan_nextseek);
        c->swap_scan_nextseek = NULL;
    }

    if (result->nextseek) {
        c->swap_scan_nextcursor = scanctx->cursor+1;
        c->swap_scan_nextseek = result->nextseek;
        result->nextseek = NULL;
    }
}

metaScanDataCtxType scanMetaScanDataCtxType = {
    .swapAna = metaScanDataCtxScanSwapAna,
    .swapIn = metaScanDataCtxScanSwapIn,
    .freeExtend = NULL,
};

/* SCAN cursor [MATCH pattern] [COUNT count] [TYPE type] */
int setupMetaScanDataCtx4Scan(metaScanDataCtx *datactx, client *c) {
    int i, j;
    unsigned long outer_cursor, cursor;

    datactx->type = &scanMetaScanDataCtxType;

    /* Not supported yet (maybe encode encode cursor to requestkey). */
    if (c->argc < 2 || c->argv[1] == NULL) {
        return SWAP_ERR_METASCAN_UNSUPPORTED_IN_MULTI;
    }

    /* No swap needed if cursor is invalid or hot. */
    if (parseScanCursor(c->argv[1],&outer_cursor) ||
            cursorIsHot(outer_cursor)) {
        datactx->extend = NULL;
        return 0;
    }

    cursor = cursorOuterToInternal(outer_cursor);
    if (cursor != c->swap_scan_nextcursor) {
        return SWAP_ERR_METASCAN_CURSOR_INVALID;
    }

    datactx->limit = 10;
    for (i = 2; i < c->argc; i+=2) {
        j = c->argc - i;
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            long long value;
            if (getLongLongFromObject(c->argv[i+1],&value) == C_OK) {
                datactx->limit = value;
                break;
            }
        }
    }

    if (c->swap_scan_nextseek) datactx->seek = sdsdup(c->swap_scan_nextseek);

    metaScanDataCtxScan *scanctx = zmalloc(sizeof(metaScanDataCtxScan));
    scanctx->outer_cursor = outer_cursor;
    scanctx->cursor = cursor;

    datactx->extend = scanctx;

    return 0;
}

/* metaScanDataCtx - Randomkey */
#define METASCAN_RANDOMKEY_DEFAULT_LIMIT 16

typedef struct metaScanDataCtxRandomkey {
  redisDb *db;
} metaScanDataCtxRandomkey;

void metaScanDataCtxRandomkeySwapAna(metaScanDataCtx *datactx,
        int *intention, uint32_t *intention_flags) {
    UNUSED(datactx);
    *intention = SWAP_IN;
    *intention_flags = 0;
}

void metaScanDataCtxRandomkeySwapIn(struct metaScanDataCtx *datactx,
        metaScanResult *result) {
    metaScanDataCtxRandomkey *randomkeyctx = datactx->extend;
    redisDb *db = randomkeyctx->db;

    if (db->randomkey_nextseek) {
        sdsfree(db->randomkey_nextseek);
        db->randomkey_nextseek = NULL;
    }

    if (result->nextseek) {
        db->randomkey_nextseek = result->nextseek;
        result->nextseek = NULL;
    }
}

metaScanDataCtxType randomkeyMetaScanDataCtxType = {
    .swapAna = metaScanDataCtxRandomkeySwapAna,
    .swapIn = metaScanDataCtxRandomkeySwapIn,
    .freeExtend = NULL,
};

int setupMetaScanDataCtx4Randomkey(metaScanDataCtx *datactx, client *c) {
    metaScanDataCtxRandomkey *randomkeyctx;
    redisDb *db = c->db;
    datactx->type = &randomkeyMetaScanDataCtxType;
    datactx->limit = METASCAN_RANDOMKEY_DEFAULT_LIMIT;
    if (db->randomkey_nextseek)
        datactx->seek = sdsdup(db->randomkey_nextseek);
    else
        datactx->seek = NULL;
    randomkeyctx = zmalloc(sizeof(metaScanDataCtxRandomkey));
    randomkeyctx->db = c->db;
    datactx->extend = randomkeyctx;
    return 0;
}

robj *metaScanResultRandomKey(redisDb *db, metaScanResult *result) {
    scanMeta *meta;
    int i, count;
    robj *keyobj = NULL;

    sds candidates[METASCAN_RANDOMKEY_DEFAULT_LIMIT] = {NULL};

    if (result == NULL || result->num == 0) return NULL;

    for (i = 0, count = 0;
            i < result->num && count < METASCAN_RANDOMKEY_DEFAULT_LIMIT;
            i++) {
        meta = result->metas + i;
        if (!scanMetaExpireIfNeeded(db,meta)) {
            candidates[count++] = meta->key;
        }
    }

    if (count > 0) {
        sds selected = candidates[rand() % count];
        keyobj = createStringObject(selected, sdslen(selected));
    }

    return keyobj;
}

/* metaScanDataCtx - ScanExpire */
typedef struct metaScanDataCtxScanExpire {
    scanExpire *scan_expire;
} metaScanDataCtxScanExpire;

void metaScanDataCtxScanExpireSwapAna(metaScanDataCtx *datactx,
        int *intention, uint32_t *intention_flags) {
    UNUSED(datactx);
    *intention = SWAP_IN;
    *intention_flags = 0;
}

void metaScanDataCtxScanExpireSwapIn(struct metaScanDataCtx *datactx,
        metaScanResult *result) {
    metaScanDataCtxScanExpire *expirectx = datactx->extend;
    scanExpire *scan_expire = expirectx->scan_expire;
    if (scan_expire->nextseek) {
        sdsfree(scan_expire->nextseek);
        scan_expire->nextseek = NULL;
    }

    if (result->nextseek) {
        scan_expire->nextseek = result->nextseek;
        result->nextseek = NULL;
    }
}

metaScanDataCtxType expireMetaScanDataCtxType = {
    .swapAna = metaScanDataCtxScanExpireSwapAna,
    .swapIn = metaScanDataCtxScanExpireSwapIn,
    .freeExtend = NULL,
};

int setupMetaScanDataCtx4ScanExpire(metaScanDataCtx *datactx, client *c) {
    metaScanDataCtxScanExpire *expirectx;
    scanExpire *scan_expire = c->db->scan_expire;
    datactx->type = &expireMetaScanDataCtxType;
    datactx->limit = scan_expire->limit;
    if (scan_expire->nextseek)
        datactx->seek = sdsdup(scan_expire->nextseek);
    else 
        datactx->seek = NULL;
    expirectx = zmalloc(sizeof(metaScanDataCtxScanExpire));
    expirectx->scan_expire = scan_expire;
    datactx->extend = expirectx;
    return 0;
}

/* MetaScan */
int metaScanSwapAna(swapData *data, struct keyRequest *req,
        int *intention, uint32_t *intention_flags, void *datactx_) {
    UNUSED(data),UNUSED(req);
    metaScanDataCtx *datactx = datactx_;
    metaScanDataCtxSwapAna(datactx,intention,intention_flags);
    return 0;
}

int metaScanEncodeKeys(swapData *data, int intention, void *datactx_,
        int *action, int *numkeys, int **pcfs, sds **prawkeys) {
    int *cfs = NULL, retval = C_OK;
    sds *rawkeys = NULL;
    metaScanDataCtx *datactx = datactx_;

    switch (intention) {
    case SWAP_IN:
        cfs = zmalloc(sizeof(int));
        rawkeys = zmalloc(sizeof(sds));
        cfs[0] = META_CF;
        rawkeys[0] = rocksEncodeMetaKey(data->db,datactx->seek);
        *numkeys = datactx->limit;
        *pcfs = cfs;
        *prawkeys = rawkeys;
        *action = ROCKS_ITERATE;
        break;
    default:
        *action = 0;
        *numkeys = 0;
        *pcfs = NULL;
        *prawkeys = NULL;
        retval = SWAP_ERR_DATA_UNEXPECTED_INTENTION;
        break;
    }
    return retval;
}

int metaScanDecodeData(swapData *data, int num, int *cfs, sds *rawkeys,
        sds *rawvals, robj **pdecoded) {
    int i, retval = 0;
    metaScanResult *result = metaScanResultCreate();
    sds nextseek_rawkey = rawkeys[num];

    UNUSED(data);
    serverAssert(rawvals && rawvals[num] == NULL);

    /* last entry in rawkeys is nextseek, NULL if iterate EOF. */
    if (nextseek_rawkey) {
        const char *nextseek;
        size_t seeklen;
        rocksDecodeMetaKey(nextseek_rawkey,sdslen(nextseek_rawkey),NULL,
                &nextseek,&seeklen);
        metaScanResultSetNextSeek(result, sdsnewlen(nextseek,seeklen));
    }

    for (i = 0; i < num; i++) {
        const char *key;
        size_t keylen;
        long long expire;
        int object_type;

        serverAssert(cfs[i] == META_CF);
        if (rocksDecodeMetaKey(rawkeys[i],sdslen(rawkeys[i]),
                NULL,&key,&keylen)) {
            retval = SWAP_ERR_DATA_DECODE_FAIL;
            break;
        }
        if (rocksDecodeMetaVal(rawvals[i],sdslen(rawvals[i]),
                &object_type,&expire,NULL,NULL)) {
            retval = SWAP_ERR_DATA_DECODE_FAIL;
            break;
        }

        metaScanResultAppend(result,object_type,sdsnewlen(key,keylen),expire);
    }

    if (pdecoded) *pdecoded = (robj*)result;

    return retval;
}

robj *metaScanCreateOrMergeObject(swapData *data, robj *decoded, void *datactx_) {
    metaScanDataCtx *datactx = datactx_;
    UNUSED(data);
    /* metaScanResult is not robj, exec will decrRefcount if returned. so
     * we move metaScanResult to datactx instead. */
    serverAssert(datactx->result == NULL);
    datactx->result = (metaScanResult*)decoded;
    return NULL;
}

int metaScanSwapIn(swapData *data, robj *result_, void *datactx_) {
    metaScanDataCtx *datactx = datactx_;
    metaScanResult *result = datactx->result;
    client *c = datactx->c;
    UNUSED(data), UNUSED(result_);
    if (c->swap_metas) freeScanMetaResult(c->swap_metas);
    c->swap_metas = result;
    metaScanDataCtxSwapIn(datactx,result);
    return 0;
}

void freeMetaScanSwapData(swapData *data, void *datactx_) {
    UNUSED(data);
    metaScanDataCtx *datactx = datactx_;
    if (datactx == NULL) return;
    if (datactx->extend) {
        if (datactx->type->freeExtend)
            datactx->type->freeExtend(datactx->extend);
        else
            zfree(datactx->extend);
        datactx->extend = NULL;
    }
    if (datactx->seek) {
        sdsfree(datactx->seek);
        datactx->seek = NULL;
    }
    zfree(datactx);
}

swapDataType metaScanSwapDataType = {
    .name = "metascan",
    .swapAna = metaScanSwapAna,
    .encodeKeys = metaScanEncodeKeys,
    .encodeData = NULL,
    .decodeData = metaScanDecodeData,
    .swapIn = metaScanSwapIn,
    .swapOut = NULL,
    .swapDel = NULL,
    .createOrMergeObject = metaScanCreateOrMergeObject,
    .cleanObject = NULL,
    .free = freeMetaScanSwapData,
};


#define METASCAN_DEFAULT_LIMIT 16

int swapDataSetupMetaScan(swapData *data, uint32_t intention_flags,
        client *c, void **pdatactx) {
    int retval;

    data->type = &metaScanSwapDataType;
    data->expire = -1;
    /* use shared object to mock that metascan is a hot swapin, so
     * db.cold_keys wouldn't be wrongly updated by exec.  */
    data->key = shared.redacted;
    data->value = shared.redacted;

    metaScanDataCtx *datactx = NULL;
    datactx = zmalloc(sizeof(metaScanDataCtx));
    datactx->c = c;
    datactx->limit = METASCAN_DEFAULT_LIMIT;
    datactx->seek = NULL;
    datactx->result = NULL;
    datactx->extend = NULL;

    if (c == NULL) {
        return SWAP_ERR_SETUP_FAIL;
    } else if (intention_flags & SWAP_METASCAN_SCAN) {
        retval = setupMetaScanDataCtx4Scan(datactx,c);
    } else if (intention_flags & SWAP_METASCAN_RANDOMKEY) {
        retval = setupMetaScanDataCtx4Randomkey(datactx,c);
    } else if (intention_flags & SWAP_METASCAN_EXPIRE) {
        retval = setupMetaScanDataCtx4ScanExpire(datactx,c);
    } else {
        retval = SWAP_ERR_SETUP_FAIL;
    }

    *pdatactx = datactx;
    return retval;
}

#ifdef REDIS_TEST

void initServerConfig(void);
void rewriteResetClientCommandCString(client *c, int argc, ...);
int metaScanTest(int argc, char *argv[], int accurate) {
    client *c;
    int error = 0;
    redisDb *db;

    UNUSED(argc), UNUSED(argv), UNUSED(accurate);

    TEST("metascan - init") {
        initServerConfig();
        ACLInit();
        server.hz = 10;
        server.repl_swapping_clients = listCreate();
        server.repl_worker_clients_free = listCreate();
        server.repl_worker_clients_used = listCreate();
        initTestRedisDb();
        c = createClient(NULL);
        selectDb(c,0);
        db = server.db+0;
    }

    TEST("metascan - scanmeta & result") {
        metaScanResult *result = metaScanResultCreate();
        int limit = 2*DEFAULT_SCANMETA_BUFFER;
        for (int i = 0; i < limit; i++) {
            metaScanResultAppend(result, OBJ_STRING, sdsnew("foo"), -1);
        }
        test_assert(result->buffer != result->metas);
        metaScanResultSetNextSeek(result,sdsnew("bar"));
        freeScanMetaResult(result);
    }

    TEST("metascan - scan") {
        int action, numkeys, *cfs, retval, i;
        int onumkeys, *ocfs;
        sds *rawkeys, *orawkeys, *orawvals;
        metaScanDataCtx *datactx;
        metaScanDataCtxScan *scanctx;
        robj *decoded;
        swapData *data;
        metaScanResult *result;

        data = createSwapData(db,NULL,NULL);
        c->swap_scan_nextcursor = 0;
        c->swap_scan_nextseek = sdsnew("nextseek");
        rewriteResetClientCommandCString(c,6,"SCAN","1","COUNT","3","MATCH","*");
        retval = swapDataSetupMetaScan(data,SWAP_METASCAN_SCAN,c,(void**)&datactx);
        scanctx = datactx->extend;
        test_assert(retval == 0);
        test_assert(datactx->limit == 3);
        test_assert(scanctx->cursor == 0);

        swapDataEncodeKeys(data,SWAP_IN,datactx,&action,&numkeys,&cfs,&rawkeys);
        test_assert(action == ROCKS_ITERATE);
        test_assert(numkeys == 3);
        test_assert(cfs[0] == META_CF);
        sdsfree(rawkeys[0]);
        zfree(cfs);
        zfree(rawkeys);

        onumkeys = numkeys;
        sds lastkey = sdsfromlonglong(onumkeys);
        orawkeys = zmalloc((onumkeys+1) * sizeof(sds));
        orawvals = zmalloc((onumkeys+1) * sizeof(sds));
        ocfs = zmalloc(onumkeys * sizeof(int));
        for (i = 0; i < onumkeys; i++) {
            ocfs[i] = META_CF;
            sds key = sdsfromlonglong(i);
            orawkeys[i] = rocksEncodeMetaKey(db,key);
            orawvals[i] = rocksEncodeMetaVal(OBJ_HASH,-1,NULL);
            sdsfree(key);
        }
        orawkeys[onumkeys] = rocksEncodeMetaKey(db,lastkey);
        orawvals[onumkeys] = NULL;
        retval = swapDataDecodeData(data,onumkeys,ocfs,orawkeys,orawvals,&decoded);
        test_assert(retval == 0);
        result = (metaScanResult*)decoded;
        test_assert(result->num == onumkeys);
        test_assert(result->metas[0].expire == -1);
        test_assert(result->metas[0].object_type == OBJ_HASH);
        test_assert(!strcmp(result->metas[0].key,"0"));
        test_assert(!sdscmp(result->nextseek,lastkey));
        for (i = 0; i <= onumkeys; i++) {
            if (orawkeys[i]) sdsfree(orawkeys[i]);
            if (orawvals[i]) sdsfree(orawvals[i]);
        }
        zfree(ocfs);
        zfree(orawkeys);
        zfree(orawvals);

        swapDataCreateOrMergeObject(data,decoded,datactx);
        test_assert((void*)datactx->result == (void*)decoded);

        retval = swapDataSwapIn(data,(robj*)result,datactx);
        test_assert(retval == 0);
        test_assert(c->swap_scan_nextcursor == 1);
        test_assert(!sdscmp(c->swap_scan_nextseek,lastkey));
        test_assert((void*)c->swap_metas == (void*)decoded);

        swapDataFree(data,datactx);
        freeClient(c);
        sdsfree(lastkey);
    }

    return error;
}

#endif

