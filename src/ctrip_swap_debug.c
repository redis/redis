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

int doRIO(RIO *rio);

static sds debugRioGet(int cf, sds rawkey) {
    sds rawval;
    RIO _rio, *rio = &_rio;
    RIOInitGet(rio,cf,rawkey);
    doRIO(rio);
    rawval = rio->get.rawval;
    return rawval;
}

static sds getSwapObjectInfo(robj *o) {
    if (o) {
        return sdscatprintf(sdsempty(),
                "at=%p,refcount=%d,type=%s,encoding=%s,dirty=%d,"
                "lru=%d,lru_seconds_idle=%llu",
                (void*)o,o->refcount,strObjectType(o->type),
                strEncoding(o->encoding),o->dirty,o->lru,
                estimateObjectIdleTime(o)/1000);
    } else {
        return sdsnew("<nil>");
    }
}

static sds getSwapMetaInfo(int object_type, long long expire, objectMeta *m) {
    if (object_type == -1) return sdsnew("<nil>");
    sds info = sdscatprintf(sdsempty(),"object_type=%d,expire=%lld",
            object_type,expire);
    if (m) {
        sds omdump = dumpObjectMeta(m);
        info = sdscatprintf(info, ",at=%p,%s",(void*)m,omdump);
        sdsfree(omdump);
    } else {
        info = sdscatprintf(info, ",at=<nil>");
    }
    return info;
}

static int getCfOrReply(client *c, robj *cf) {
    if (!strcasecmp(cf->ptr,"meta")) {
        return META_CF;
    } else if (!strcasecmp(cf->ptr, "data")) {
        return DATA_CF;
    } else {
        addReplyError(c,"invalid cf");
        return -1;
    }
}

void swapCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"OBJECT <key>",
"    Show info about `key` and assosiated value.",
"ENCODE-META-KEY <key>",
"    Encode meta key.",
"DECODE-META-KEY <rawkey>",
"    Decode meta key.",
"ENCODE-DATA-KEY <key>",
"    Encode data key.",
"DECODE-DATA-KEY <rawkey>",
"    Decode data key.",
"RIO-GET meta|data <rawkey> <rawkey> ...",
"    Get raw value from rocksdb.",
"RIO-SCAN meta|data <prefix>",
"    Scan rocksdb with prefix.",
"RIO-ERROR <count>",
"    Make next count rio return error.",
"RESET-STATS",
"    Reset swap stats.",
"COMPACT",
"   COMPACT rocksdb",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"object") && c->argc == 3) {
        redisDb *db = c->db;
        robj *key = c->argv[2];
        robj *value = lookupKey(db,key,LOOKUP_NOTOUCH);
        objectMeta *hot_meta = lookupMeta(db,key), *cold_meta = NULL;
        long long hot_expire = getExpire(db,key), cold_expire = -1;
        sds meta_rawkey = NULL, meta_rawval = NULL;
        int hot_object_type = hot_meta ? hot_meta->object_type : -1;
        int cold_object_type = -1;

        meta_rawkey = rocksEncodeMetaKey(db,key->ptr);
        meta_rawval = debugRioGet(META_CF,meta_rawkey);
        if (meta_rawval) {
            const char *extend;
            size_t extlen;
            rocksDecodeMetaVal(meta_rawval,sdslen(meta_rawval),
                    &cold_object_type,&cold_expire,&extend,&extlen);
            if (extend) {
                buildObjectMeta(cold_object_type,extend,extlen,&cold_meta);
            }
        }

        if (!value && !hot_meta && !meta_rawval) {
            addReplyErrorObject(c,shared.nokeyerr);
            if (meta_rawkey) sdsfree(meta_rawkey);
            if (meta_rawval) sdsfree(meta_rawval);
            return;
        }

        sds value_info = getSwapObjectInfo(value);
        sds hot_meta_info = getSwapMetaInfo(hot_object_type,hot_expire,hot_meta);
        sds cold_meta_info = getSwapMetaInfo(cold_object_type,cold_expire,cold_meta);
        sds info = sdscatprintf(sdsempty(),
                "value: %s\nhot_meta: %s\ncold_meta: %s\n",
                value_info,hot_meta_info,cold_meta_info);
        addReplyVerbatim(c,info,sdslen(info),"txt");
        sdsfree(value_info);
        sdsfree(hot_meta_info);
        sdsfree(cold_meta_info);
        sdsfree(info);
        if (meta_rawkey) sdsfree(meta_rawkey);
        if (meta_rawval) sdsfree(meta_rawval);
        if (cold_meta) freeObjectMeta(cold_meta);
    } else if (!strcasecmp(c->argv[1]->ptr,"encode-meta-key") && c->argc == 3) {
        addReplyBulkSds(c,rocksEncodeMetaKey(c->db,c->argv[2]->ptr));
    } else if (!strcasecmp(c->argv[1]->ptr,"decode-meta-key") && c->argc == 3) {
        int dbid, retval;
        sds rawkey = c->argv[2]->ptr;
        const char *key;
        size_t keylen;

        retval = rocksDecodeMetaKey(rawkey,sdslen(rawkey),&dbid,&key,&keylen);
        if (retval) {
            addReplyError(c,"invalid meta key");
        } else {
            addReplyArrayLen(c,2);
            addReplyBulkLongLong(c,dbid);
            addReplyBulkCBuffer(c,key,keylen);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"encode-data-key") && c->argc == 4) {
        addReplyBulkSds(c,rocksEncodeDataKey(c->db,c->argv[2]->ptr,c->argv[3]->ptr));
    } else if (!strcasecmp(c->argv[1]->ptr,"decode-data-key") && c->argc == 3) {
        int dbid, retval;
        sds rawkey = c->argv[2]->ptr;
        const char *key, *subkey;
        size_t keylen, sublen;

        retval = rocksDecodeDataKey(rawkey,sdslen(rawkey),
                &dbid,&key,&keylen,&subkey,&sublen);
        if (retval) {
            addReplyError(c,"invalid data key");
        } else {
            addReplyArrayLen(c,3);
            addReplyBulkLongLong(c,dbid);
            addReplyBulkCBuffer(c,key,keylen);
            addReplyBulkCBuffer(c,subkey,sublen);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"rio-get") && c->argc >= 4) {
        int cf;
        addReplyArrayLen(c, c->argc-3);
        if ((cf = getCfOrReply(c,c->argv[2])) < 0) return;
        for (int i = 3; i < c->argc; i++) {
            sds rawval = debugRioGet(cf,c->argv[i]->ptr);
            if (rawval == NULL) {
                addReplyNull(c);
            } else {
                addReplyBulkSds(c,sdsdup(rawval));
            }
            sdsfree(rawval);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"rio-scan") && c->argc == 4) {
        int cf;
        RIO _rio, *rio = &_rio;
        if ((cf = getCfOrReply(c,c->argv[2])) < 0) return;
        sds prefix = sdsdup(c->argv[3]->ptr);
        RIOInitScan(rio,cf,prefix);
        doRIO(rio);
        addReplyArrayLen(c,rio->scan.numkeys);
        for (int i = 0; i < rio->scan.numkeys; i++) {
            sds repr = sdsempty();
            repr = sdscatsds(repr, rio->scan.rawkeys[i]);
            repr = sdscat(repr, "=>");
            repr = sdscatsds(repr, rio->scan.rawvals[i]);
            addReplyBulkSds(c,repr);
        }
        RIODeinit(rio);
    } else if (!strcasecmp(c->argv[1]->ptr,"debug-rio-error") && c->argc == 3) {
        long long count;
        if (getLongLongFromObjectOrReply(c,c->argv[2],&count,NULL))
            return;
        if (count > INT_MAX || count < 0) {
            addReplyError(c,"debug-rio-error count invalid");
        } else {
            server.debug_rio_error = (int)count;
            addReply(c,shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"reset-stats") && c->argc == 2) {
        resetStatsSwap();
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"compact") && c->argc == 2) {
        sds error = NULL;
        if (submitUtilTask(COMPACT_RANGE_TASK, NULL, &error)) {
            addReply(c,shared.ok);
        } else {
            addReplyErrorSds(c,error);
        }
    } else {
        addReplySubcommandSyntaxError(c);
        return;
    }
}

#ifdef SWAP_DEBUG

void swapDebugMsgsInit(swapDebugMsgs *msgs, char *identity) {
    snprintf(msgs->identity,MAX_MSG,"[%s]",identity);
}

void swapDebugMsgsAppend(swapDebugMsgs *msgs, char *step, char *fmt, ...) {
    va_list ap;
    char *name = msgs->steps[msgs->index].name;
    char *info = msgs->steps[msgs->index].info;
    strncpy(name,step,MAX_MSG-1);
    va_start(ap,fmt);
    vsnprintf(info,MAX_MSG,fmt,ap);
    va_end(ap);
    msgs->index++;
}

void swapDebugMsgsDump(swapDebugMsgs *msgs) {
    serverLog(LL_NOTICE,"=== %s ===", msgs->identity);
    for (int i = 0; i < msgs->index; i++) {
        char *name = msgs->steps[i].name;
        char *info = msgs->steps[i].info;
        serverLog(LL_NOTICE,"%2d %25s : %s",i,name,info);
    }
}

#endif
