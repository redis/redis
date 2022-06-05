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

int typeR2O(char rocks_type);
int doRIO(RIO *rio);

void swapCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"ENCODE-KEY <type:K/H/h/...> <key> [<subkey>]",
"    Encode rocksdb key or subkey.",
"DECODE-KEY <rawkey>",
"    Decode rocksdb rawkey.",
"RIO-GET <rawkey>",
"    Get raw value from rocksdb.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"encode-key") && c->argc >=4) {
        sds typesds = c->argv[2]->ptr;
        char type = typesds[0];
        sds rawkey;
        if ((sdslen(typesds)) != 1) {
            addReplyError(c,"Key type invalid");
            return;
        } else if (type >= 'A' && type <= 'Z' && c->argc == 4) {
            rawkey = rocksEncodeKey(typeR2O(type), c->argv[3]->ptr);
        } else if (type >= 'a' && type <= 'z' && c->argc == 5) {
            rawkey = rocksEncodeSubkey(typeR2O(type), (sds)c->argv[3]->ptr,
                    (sds)c->argv[4]->ptr);
        } else {
            addReply(c, shared.syntaxerr);
            return;
        }
        addReplyBulkSds(c,rawkey);
    } else if (!strcasecmp(c->argv[1]->ptr,"decode-key") && c->argc == 3) {
        sds raw = c->argv[2]->ptr;
        const char *key = NULL, *sub = NULL;
        size_t klen = 0, slen = 0;
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
            if ((type = rocksDecodeSubkey(raw,sdslen(raw),&key,&klen,
                            &sub,&slen)) < 0) {
                addReplyError(c, "Invalid raw key.");
                return;
            }
        } else {
            addReplyError(c, "Invalid raw key.");
            return;
        }

        addReplyArrayLen(c,3);
        addReplyBulkCBuffer(c,raw,1);
        addReplyBulkCBuffer(c,key,klen);
        addReplyBulkCBuffer(c,sub,slen);
    } else if (!strcasecmp(c->argv[1]->ptr,"rio-get") && c->argc == 3) {
        RIO _rio, *rio = &_rio;
        sds rawkey, rawval;
        rawkey = sdsdup(c->argv[2]->ptr);
        RIOInitGet(rio,rawkey);
        doRIO(rio);
        rawval = rio->get.rawval;
        if (rawval == NULL) {
            addReplyNull(c);
        } else {
            addReplyBulkSds(c,sdsdup(rawval));
        }
        RIODeinit(rio);
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

