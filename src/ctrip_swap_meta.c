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
    swapScanSession *session;
} metaScanDataCtxScan;

static inline int parseScanCursor(robj *o, unsigned long *cursor) {
    char *eptr;
    errno = 0;
    *cursor = strtoul(o->ptr, &eptr, 10);
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
        return -1;
    return 0;
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
    metaScanDataCtxScan *scanctx = datactx->extend;
    swapScanSessionUnbind(scanctx->session, result->nextseek);
    result->nextseek = NULL; /* moved */
}

metaScanDataCtxType scanMetaScanDataCtxType = {
    .swapAna = metaScanDataCtxScanSwapAna,
    .swapIn = metaScanDataCtxScanSwapIn,
    .freeExtend = NULL,
};

/* SCAN cursor [MATCH pattern] [COUNT count] [TYPE type] */
int setupMetaScanDataCtx4Scan(metaScanDataCtx *datactx, client *c) {
    int i, j, reason = 0;
    unsigned long outer_cursor;
    swapScanSession *session;

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

    session = swapScanSessionsBind(server.swap_scan_sessions,
            outer_cursor, &reason);
    if (session == NULL) return reason;

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

    metaScanDataCtxScan *scanctx = zmalloc(sizeof(metaScanDataCtxScan));
    scanctx->session = session;
    if (session->nextseek) datactx->seek = sdsdup(session->nextseek);
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

int SwapAnaAction(swapData *data, int intention, void *datactx_, int *action) {
    UNUSED(data), UNUSED(datactx_);
    switch (intention) {
        case SWAP_IN:
            *action = ROCKS_ITERATE;
            break;
        default:
            *action = ROCKS_NOP;
            return SWAP_ERR_DATA_UNEXPECTED_INTENTION;
    }
    return 0;
}

int metaScanEncodeRange(struct swapData *data, int intention, void *datactx_, int *limit,
        uint32_t *flags, int *pcf, sds *start, sds *end) {
    metaScanDataCtx *datactx = datactx_;
    serverAssert(SWAP_IN == intention);
    *pcf = META_CF;
    *flags |= ROCKS_ITERATE_CONTINUOUSLY_SEEK;
    *start = rocksEncodeMetaKey(data->db,datactx->seek);
    *end = NULL;
    *limit = datactx->limit;
    return 0;
}

int metaScanDecodeData(swapData *data, int num, int *cfs, sds *rawkeys,
        sds *rawvals, void **pdecoded) {
    int i, retval = 0;
    metaScanResult *result = metaScanResultCreate();
    sds nextseek_rawkey = data->nextseek;

    /* last entry in rawkeys is nextseek, NULL if iterate EOF. */
    if (nextseek_rawkey) {
        const char *nextseek;
        size_t seeklen;
        rocksDecodeMetaKey(nextseek_rawkey,sdslen(nextseek_rawkey),NULL,
                &nextseek,&seeklen);
        metaScanResultSetNextSeek(result, sdsnewlen(nextseek,seeklen));
        sdsfree(data->nextseek);
        data->nextseek = NULL, nextseek_rawkey = NULL;
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
                &object_type,&expire,NULL,NULL,NULL)) {
            retval = SWAP_ERR_DATA_DECODE_FAIL;
            break;
        }

        metaScanResultAppend(result,object_type,sdsnewlen(key,keylen),expire);
    }

    if (pdecoded) *pdecoded = (robj*)result;

    return retval;
}

void *metaScanCreateOrMergeObject(swapData *data, void *decoded, void *datactx) {
    UNUSED(data), UNUSED(datactx);
    return decoded;
}

int metaScanSwapIn(swapData *data, void *result_, void *datactx_) {
    metaScanDataCtx *datactx = datactx_;
    metaScanResult *result = result_;
    client *c = datactx->c;
    UNUSED(data);
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
    .swapAnaAction = SwapAnaAction,
    .encodeKeys = NULL,
    .encodeData = NULL,
    .encodeRange = metaScanEncodeRange,
    .decodeData = metaScanDecodeData,
    .swapIn = metaScanSwapIn,
    .swapOut = NULL,
    .swapDel = NULL,
    .createOrMergeObject = metaScanCreateOrMergeObject,
    .cleanObject = NULL,
    .beforeCall = NULL,
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

/* make session ready to assign again (session_id not changed). */
void swapScanSessionReset(swapScanSession *session) {
    session->last_active = 0;
    if (session->nextseek) {
        sdsfree(session->nextseek);
        session->nextseek = NULL;
    }
    session->binded = 0;
    swapScanSessionZeroNextCursor(session);
}

swapScanSessions *swapScanSessionsCreate(int bits) {
    serverAssert(bits > 0);
    size_t capacity = 1 << bits;
    swapScanSessions *sessions = zcalloc(sizeof(struct swapScanSessions));
    sessions->free = listCreate();
    sessions->assigned = raxNew();
    swapScanSession *session_array = zcalloc(capacity*sizeof(swapScanSession));
    for (size_t i = 0; i < capacity; i++) {
        swapScanSession *session = session_array + i;
        session->session_id = i;
        listAddNodeTail(sessions->free, session);
    }
    sessions->array = session_array;
    return sessions;
}

void swapScanSessionsRelease(swapScanSessions *sessions) {
    if (sessions == NULL) return;
    listRelease(sessions->free);
    raxFreeWithCallback(sessions->assigned, NULL);
    zfree(sessions->array);
}

static inline int swapScanSessionExpired(swapScanSession *session) {
    return server.mstime - session->last_active >
        server.swap_scan_session_max_idle_seconds * 1000;
}

static inline uint64_t sessionId2RaxKey(unsigned long session_id) {
    return htonu64(session_id);
}

swapScanSession *swapScanSessionsAssign(swapScanSessions *sessions) {
    uint64_t id;
    swapScanSession *session = NULL;

    if (listLength(sessions->free)) {
        /* Assign free session if exists. */
        listNode *ln = listFirst(sessions->free);
        session = listNodeValue(ln);
        listDelNode(sessions->free, ln);
        id = sessionId2RaxKey(session->session_id);
        raxInsert(sessions->assigned,(unsigned char*)&id,sizeof(id),session,NULL);
        // TODO remove serverLog(LL_WARNING, "[xxx] insert %lu => %ld", id, session->session_id);
    } else {
        /* Try assign the least active, assign fails if session is not
         * idled long enough. */
        raxIterator ri;
        raxStart(&ri,sessions->assigned);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            swapScanSession *s = ri.data;

            if (s->binded) continue;

            if (session == NULL) {
                session = s;
                continue;
            }

            if (s->last_active < session->last_active) {
                session = s;
            }
        }
        raxStop(&ri);

        if (!swapScanSessionExpired(session)) session = NULL;
    }

    if (session) {
        swapScanSessionReset(session);
        session->last_active = server.mstime;
        sessions->stat.assigned_succeded++;
    } else {
        sessions->stat.assigned_failed++;
    }

    return session;
}

void swapScanSessionUnassign(swapScanSessions *sessions, swapScanSession *session) {
    void *session_;
    uint64_t id = sessionId2RaxKey(session->session_id);

    // TODO remove serverLog(LL_WARNING, "[xxx] remove %lu => %ld", id, session->session_id);

    if (raxRemove(sessions->assigned, (unsigned char *)&id, sizeof(id),
                &session_)) {
        serverAssert(session == session_);
        listAddNodeTail(sessions->free, session);
    }
}

swapScanSession *swapScanSessionsFind(swapScanSessions *sessions,
        unsigned long outer_cursor) {
    uint64_t id = sessionId2RaxKey(cursorGetSessionId(outer_cursor));

    swapScanSession *session = raxFind(sessions->assigned,
            (unsigned char*)&id, sizeof(id));
    if (session == raxNotFound) session = NULL;

    // TODO remove serverLog(LL_WARNING, "[xxx] find %lu => %lu", id, (session ? session->session_id : 999));

    return session;
}

swapScanSession *swapScanSessionsBind(swapScanSessions *sessions,
        unsigned long outer_cursor, int *reason) {
    swapScanSession *session;

    /* session not found: invalid cursor, can't scan cold keys from
     * arbitary cursor. */
    session = swapScanSessionsFind(server.swap_scan_sessions, outer_cursor);
    if (session == NULL) {
        if (reason) *reason = SWAP_ERR_METASCAN_SESSION_UNASSIGNED;
        goto fail;
    }

    serverAssert(cursorGetSessionId(outer_cursor) == session->session_id);

    /* session inprogress: can't scan concurrently */
    if (session->binded) {
        if (reason) *reason = SWAP_ERR_METASCAN_SESSION_INPROGRESS;
        goto fail;
    }

    /* cursor not continuos: must use cursor return previously. */
    if (session->nextcursor != cursorOuterToInternal(outer_cursor)) {
        if (reason) *reason = SWAP_ERR_METASCAN_SESSION_SEQUNMATCH;
        goto fail;
    }

    session->last_active = server.mstime;
    session->binded = 1;
    sessions->stat.bind_succeded++;

    return session;

fail:
    sessions->stat.bind_failed++;
    return NULL;
}

void swapScanSessionUnbind(swapScanSession *session, MOVE sds nextseek) {
    if (session->nextseek) {
        sdsfree(session->nextseek);
        session->nextseek = NULL;
    }
    session->nextseek = nextseek;

    if (nextseek) {
        swapScanSessionIncrNextCursor(session);
    } else {
        swapScanSessionZeroNextCursor(session);
    }

    session->binded = 0;
    session->last_active = server.mstime;
}

sds genSwapScanSessionStatString(sds info) {
    size_t assigned = raxSize(server.swap_scan_sessions->assigned);
    size_t free = listLength(server.swap_scan_sessions->free);
    info = sdscatprintf(info,
            "swap_scan_session_assigned:%lu\r\n"
            "swap_scan_session_free:%lu\r\n",
            assigned, free);
    return info;
}

sds catSwapScanSessionInfoString(sds o, swapScanSession *session) {
    char *nextseek = session->nextseek ? session->nextseek : "nil";
    unsigned long long next_outcursor = cursorInternalToOuter(1, session->nextcursor);
    return sdscatfmt(o,
            "session_id=%U nextseek=%s nextcursor=%U next_outcursor=%U binded=%i last_active=%U",
            (unsigned long long)session->session_id,
            nextseek,
            (unsigned long long)session->nextcursor,
            (unsigned long long)next_outcursor,
            session->binded,
            (unsigned long long)session->last_active);
}

sds getAllSwapScanSessionsInfoString(long long outer_cursor) {
    swapScanSessions *sessions = server.swap_scan_sessions;
    unsigned long session_id = cursorGetSessionId(outer_cursor);

    sds o = sdsnewlen(SDS_NOINIT,100*raxSize(sessions->assigned));
    sdsclear(o);

    if (cursorIsHot(outer_cursor)) return o;

    raxIterator ri;
    raxStart(&ri,sessions->assigned);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        swapScanSession *s = ri.data;
        if (outer_cursor == -1 || session_id == s->session_id) {
            o = catSwapScanSessionInfoString(o, s);
            o = sdscatlen(o,"\n",1);
        }
    }
    raxStop(&ri);

    return o;
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

        server.swap_scan_session_bits = 7;
        server.swap_scan_session_max_idle_seconds = 60;
        server.swap_scan_sessions = swapScanSessionsCreate(server.swap_scan_session_bits);
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
        int action, numkeys, retval, i, cf;
        uint32_t flags;
        int onumkeys, *ocfs;
        sds *orawkeys, *orawvals;
        sds start, end;
        metaScanDataCtx *datactx;
        void *decoded;
        swapData *data;
        metaScanResult *result;
        swapScanSession *session;

        data = createSwapData(db,NULL,NULL);
        session = swapScanSessionsAssign(server.swap_scan_sessions);
        test_assert(session->session_id == 0);
        rewriteResetClientCommandCString(c,6,"SCAN","1","COUNT","3","MATCH","*");
        retval = swapDataSetupMetaScan(data,SWAP_METASCAN_SCAN,c,(void**)&datactx);
        test_assert(retval == 0);
        test_assert(datactx->limit == 3);
        swapDataSwapAnaAction(data,SWAP_IN,datactx,&action);
        swapDataEncodeRange(data,SWAP_IN,datactx,&numkeys, &flags, &cf, &start, &end);
        test_assert(action == ROCKS_ITERATE);
        test_assert(numkeys == 3);
        test_assert(cf == META_CF);
        sdsfree(start);

        onumkeys = numkeys;
        sds lastkey = sdsfromlonglong(onumkeys);
        orawkeys = zmalloc((onumkeys) * sizeof(sds));
        orawvals = zmalloc((onumkeys) * sizeof(sds));
        ocfs = zmalloc(onumkeys * sizeof(int));
        for (i = 0; i < onumkeys; i++) {
            ocfs[i] = META_CF;
            sds key = sdsfromlonglong(i);
            orawkeys[i] = rocksEncodeMetaKey(db,key);
            orawvals[i] = rocksEncodeMetaVal(OBJ_HASH,-1,0,NULL);
            sdsfree(key);
        }
        data->nextseek = rocksEncodeMetaKey(db,lastkey);
        retval = swapDataDecodeData(data,onumkeys,ocfs,orawkeys,orawvals,&decoded);
        test_assert(retval == 0);
        result = (metaScanResult*)decoded;
        test_assert(result->num == onumkeys);
        test_assert(result->metas[0].expire == -1);
        test_assert(result->metas[0].object_type == OBJ_HASH);
        test_assert(!strcmp(result->metas[0].key,"0"));
        test_assert(!sdscmp(result->nextseek,lastkey));
        for (i = 0; i < onumkeys; i++) {
            if (orawkeys[i]) sdsfree(orawkeys[i]);
            if (orawvals[i]) sdsfree(orawvals[i]);
        }
        sdsfree(data->nextseek);
        data->nextseek = NULL;
        zfree(ocfs);
        zfree(orawkeys);
        zfree(orawvals);

        swapDataCreateOrMergeObject(data,decoded,datactx);

        retval = swapDataSwapIn(data,result,datactx);
        test_assert(retval == 0);

        test_assert(session->nextcursor == 0x80);
        test_assert(!sdscmp(session->nextseek,lastkey));
        test_assert((void*)c->swap_metas == (void*)decoded);

        /* finish session */
        result = metaScanResultCreate();
        swapDataSwapIn(data,result,datactx);
        test_assert(session->nextcursor == 0);
        test_assert(session->nextseek == NULL);
        swapScanSessionUnassign(server.swap_scan_sessions, session);

        swapDataFree(data,datactx);
        freeClient(c);
        sdsfree(lastkey);
    }

    TEST("metascan - scan session cursor manipulate") {
        swapScanSession session_, *session = &session_;

        test_assert(cursorGetSessionId(0x10) == 0x8);
        test_assert(cursorGetSessionId(0xF010) == 0x8);
        test_assert(cursorGetSessionSeq(0x10) == 0);
        test_assert(cursorGetSessionSeq(0xF010) == 0xF0);

        memset(session, 0, sizeof(swapScanSession));
        session->session_id = 0x04;
        session->nextcursor = 0xF04;
        test_assert(swapScanSessionGetNextCursor(session) == 0xF04);
        swapScanSessionIncrNextCursor(session);
        test_assert(swapScanSessionGetNextCursor(session) == 0xF84);
        swapScanSessionZeroNextCursor(session);
        test_assert(swapScanSessionGetNextCursor(session) == 0x04);

        sds foo = sdsnew("foo"), bar = sdsnew("bar");
        swapScanSessionUnbind(session, foo);
        test_assert(sdscmp(session->nextseek, foo) == 0);
        test_assert(swapScanSessionGetNextCursor(session) == 0x84);
        swapScanSessionUnbind(session, bar);
        test_assert(swapScanSessionGetNextCursor(session) == 0x104);
        test_assert(sdscmp(session->nextseek, bar) == 0);
        swapScanSessionUnbind(session, NULL);
        test_assert(swapScanSessionGetNextCursor(session) == 0x04);
        test_assert(session->nextseek == NULL);

        swapScanSession *s0, *s0a;
        int SESSIONS_COUNT = 128, reason;
        swapScanSession *sessions[SESSIONS_COUNT];

        s0 = swapScanSessionsBind(server.swap_scan_sessions, 1, &reason);
        test_assert(s0 == NULL && reason == SWAP_ERR_METASCAN_SESSION_UNASSIGNED);

        for (int i = 0; i < SESSIONS_COUNT; i++) {
            sessions[i] = swapScanSessionsAssign(server.swap_scan_sessions);
            test_assert(sessions[i] != NULL);
            test_assert(sessions[i]->nextseek == NULL);
            test_assert(sessions[i]->nextcursor == sessions[i]->session_id);
        }

        test_assert(swapScanSessionsAssign(server.swap_scan_sessions) == NULL);

        s0 = swapScanSessionsBind(server.swap_scan_sessions, 1, &reason);
        test_assert(s0 != NULL);
        test_assert(s0->binded == 1);
        test_assert(s0->nextcursor == 0);

        s0a = swapScanSessionsBind(server.swap_scan_sessions, 1, &reason);
        test_assert(s0a == NULL && reason == SWAP_ERR_METASCAN_SESSION_INPROGRESS);

        swapScanSessionUnbind(s0, sdsnew("hello"));
        test_assert(!s0->binded && s0->nextcursor == 0x80 && !strcmp(s0->nextseek, "hello"));

        s0a = swapScanSessionsBind(server.swap_scan_sessions, 1, &reason);
        test_assert(s0a == NULL && reason == SWAP_ERR_METASCAN_SESSION_SEQUNMATCH);

        s0a = swapScanSessionsBind(server.swap_scan_sessions, 0x101, &reason);
        test_assert(s0a != NULL && s0 == s0a);
        swapScanSessionUnbind(s0a, sdsnew("world"));
        test_assert(!s0a->binded && s0a->nextcursor == 0x100 && !strcmp(s0a->nextseek, "world"));

        for (int i = 1; i < SESSIONS_COUNT; i++) {
            server.swap_scan_sessions->array[i].last_active = server.mstime+1000;
        }

        server.mstime += 61*1000; /* session can assign again if idled too long. */
        s0 = swapScanSessionsAssign(server.swap_scan_sessions);
        test_assert(s0 != NULL && s0->session_id == 0);
        test_assert(s0->nextseek == NULL);
        test_assert(s0->nextcursor == 0);

        swapScanSessionUnassign(server.swap_scan_sessions, s0);

        test_assert(listLength(server.swap_scan_sessions->free) == 1);
        test_assert(raxSize(server.swap_scan_sessions->assigned) == 127);
    }

    return error;
}

#endif

