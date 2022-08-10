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

static sds getSwapObjectInfo(robj *o) {
    if (o) {
        return sdscatprintf(sdsempty(),
                "at=%p,refcount=%d,type=%s,encoding=%s,big=%d,dirty=%d,"
                "lru=%d,lru_seconds_idle=%llu",
                (void*)o,o->refcount,strObjectType(o->type),
                strEncoding(o->encoding),o->big,o->dirty,o->lru,
                estimateObjectIdleTime(o)/1000);
    } else {
        return sdsnew("<nil>");
    }
}

static sds getSwapMetaInfo(objectMeta *m) {
    if (m) {
        return sdscatprintf(sdsempty(),
                "at=%p,len=%ld,version=%lu",
                (void*)m, m->len, m->version);
    } else {
        return sdsnew("<nil>");
    }
}

void swapCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"OBJECT <key>",
"    Show swap info about `key` and assosiated value.",
"ENCODE-KEY <type:K/H/h/...> <key>|<version key subkey>",
"    Encode rocksdb key or subkey.",
"DECODE-KEY <rawkey>",
"    Decode rocksdb rawkey.",
"RIO-GET <rawkey> <rawkey> ...",
"    Get raw value from rocksdb.",
"RIO-SCAN <prefix>",
"    Scan rocksdb with prefix.",
"RESET-STATS",
"    Reset swap stats.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"object") && c->argc == 3) {
        redisDb *db = c->db;
        robj *key = c->argv[2];
        robj *value = lookupKey(db,key,LOOKUP_NOTOUCH);
        robj *evict = lookupEvictKey(db,key);
        objectMeta *meta = lookupMeta(db,key);
        if (!value && !evict) {
            addReplyErrorObject(c,shared.nokeyerr);
            return;
        }
        sds value_info = getSwapObjectInfo(value);
        sds evict_info = getSwapObjectInfo(evict);
        sds meta_info = getSwapMetaInfo(meta);
        sds info = sdscatprintf(sdsempty(),
                "value: %s\nevict: %s\nmeta: %s",
                value_info,evict_info,meta_info);
        addReplyVerbatim(c,info,sdslen(info),"txt");
        sdsfree(value_info);
        sdsfree(evict_info);
        sdsfree(meta_info);
        sdsfree(info);
    } else if (!strcasecmp(c->argv[1]->ptr,"encode-key") && c->argc >= 4) {
        sds typesds = c->argv[2]->ptr;
        char type = typesds[0];
        long long version;
        sds rawkey;
        if ((sdslen(typesds)) != 1) {
            addReplyError(c,"Key type invalid");
            return;
        } else if (type >= 'A' && type <= 'Z' && c->argc == 4) {
            rawkey = rocksEncodeKey(type, c->argv[3]->ptr);
        } else if (type >= 'a' && type <= 'z' && c->argc == 6 &&
                isSdsRepresentableAsLongLong(c->argv[3]->ptr,&version) == C_OK) {
            rawkey = rocksEncodeSubkey(type, (uint64_t)version,
                    (sds)c->argv[4]->ptr, (sds)c->argv[5]->ptr);
        } else {
            addReply(c, shared.syntaxerr);
            return;
        }
        addReplyBulkSds(c,rawkey);
    } else if (!strcasecmp(c->argv[1]->ptr,"decode-key") && c->argc == 3) {
        sds raw = c->argv[2]->ptr;
        const char *key = NULL, *sub = NULL;
        size_t klen = 0, slen = 0;
        uint64_t version = 0;
        char type;
        if (sdslen(raw) < 1) {
            addReplyError(c, "Invalid raw key.");
            return;
        } else if (raw[0] >= 'A' && raw[0] <= 'Z') {
            if ((type = rocksDecodeKey(raw,sdslen(raw),&key,&klen)) < 0) {
                addReplyError(c, "Invalid raw key.");
                return;
            }
        } else if (raw[0] >= 'a' && raw[0] <= 'z') {
            if ((type = rocksDecodeSubkey(raw,sdslen(raw),&version,
                            &key,&klen,&sub,&slen)) < 0) {
                addReplyError(c, "Invalid raw key.");
                return;
            }
        } else {
            addReplyError(c, "Invalid raw key.");
            return;
        }

        addReplyArrayLen(c,4);
        addReplyBulkCBuffer(c,raw,1);
        addReplyBulkLongLong(c,version);
        addReplyBulkCBuffer(c,key,klen);
        addReplyBulkCBuffer(c,sub,slen);
    } else if (!strcasecmp(c->argv[1]->ptr,"rio-get") && c->argc >= 3) {
        RIO _rio, *rio = &_rio;
        sds rawkey, rawval;

        addReplyArrayLen(c, c->argc-2);
        for (int i = 2; i < c->argc; i++) {
            rawkey = sdsdup(c->argv[i]->ptr);
            RIOInitGet(rio,rawkey);
            doRIO(rio);
            rawval = rio->get.rawval;
            if (rawval == NULL) {
                addReplyNull(c);
            } else {
                addReplyBulkSds(c,sdsdup(rawval));
            }
            RIODeinit(rio);
        }
    } else if (!strcasecmp(c->argv[1]->ptr,"rio-scan") && c->argc == 3) {
        RIO _rio, *rio = &_rio;
        sds prefix = sdsdup(c->argv[2]->ptr);
        RIOInitScan(rio,prefix);
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
    } else if (!strcasecmp(c->argv[1]->ptr,"reset-stats") && c->argc == 2) {
        resetStatsSwap();
        addReply(c,shared.ok);
    } else {
        addReplySubcommandSyntaxError(c);
        return;
    }
}

int getKeyRequestsSwap(struct redisCommand *cmd, robj **argv, int argc,
        struct getKeyRequestsResult *result) {
    UNUSED(cmd);
    if (!strcasecmp(argv[1]->ptr,"object") && argc == 3) {
        getKeyRequestsPrepareResult(result,result->num+1);
        incrRefCount(argv[2]);
        //TODO 确认intention
        getKeyRequestsAppendResult(result,REQUEST_LEVEL_KEY,argv[2],0,NULL,
                cmd->intention,cmd->intention_flags,KEYREQUESTS_DBID);
    }
    return 0;
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

