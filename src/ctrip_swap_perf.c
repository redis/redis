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

perflogEntry *perflogCreateEntry(const char *identity, const char *perf_report,
        const char *iostats_report, long long duration) {
    perflogEntry *pe = zcalloc(sizeof(*pe));
    strncpy(pe->identity,identity,sizeof(pe->identity)-1);
    pe->id = server.swap_perflog_entry_id++;
    pe->time = time(NULL);
    pe->duration = duration;
    if (perf_report) pe->perf_report = sdsnew(perf_report);
    if (iostats_report) pe->iostats_report = sdsnew(iostats_report);
    return pe;
}

void perflogFreeEntry(void *entry_) {
    perflogEntry *entry = entry_;
    if (entry->perf_report) sdsfree(entry->perf_report);
    if (entry->iostats_report) sdsfree(entry->iostats_report);
    zfree(entry);
}

void perflogInit(void) {
    server.swap_perflog = listCreate();
    server.swap_perflog_entry_id = 0;
    listSetFreeMethod(server.swap_perflog,perflogFreeEntry);
}

void perflogPushEntry(const char *identity, const char *perf_report,
        const char *iostats_report, long long duration) {
        listAddNodeHead(server.swap_perflog,
                        perflogCreateEntry(identity,perf_report,
                            iostats_report,duration));
    while (listLength(server.swap_perflog) > server.swap_perflog_max_len)
        listDelNode(server.swap_perflog,listLast(server.swap_perflog));
}

void perflogReset(void) {
    while (listLength(server.swap_perflog) > 0)
        listDelNode(server.swap_perflog,listLast(server.swap_perflog));
}

#define PERF_LEVEL_kUninitialized 0
#define PERF_LEVEL_kDisable 1
#define PERF_LEVEL_kEnableCount 2
#define PERF_LEVEL_kEnableTimeExceptForMutex 3
#define PERF_LEVEL_kEnableTime 4
#define PERF_LEVEL_kOutOfBounds 5

void perflogSampleDisable(perflogSampleContext *samplectx) {
    samplectx->disabled = 1;
}

/* Perflog disabled if swap-perflog-sample-ration is 0.
 * Note that perf and iostats context uses thread local ptr, must start and
 * end in swap thread. */
void perflogSampleStart(perflogSampleContext *samplectx, swapRequest *req) {
    int x = rand();
    x = x >= 0 ? x : -x;
    if (samplectx->disabled || (x % 100) < server.swap_perflog_sample_ratio) {
        samplectx->started = 1;
        samplectx->req = req;
        rocksdb_set_perf_level(PERF_LEVEL_kEnableTimeExceptForMutex);
        samplectx->perfctx = rocksdb_perfcontext_create();
        rocksdb_perfcontext_reset(samplectx->perfctx);
        elapsedStart(&samplectx->timer);
    } else {
        samplectx->started = 0;
    }
}

void perflogSampleEnd(perflogSampleContext *samplectx) {
    uint64_t duration;
    char *perf_report = NULL, *iostats_report = NULL;
    char *cmdname = "<no-cmd>";
    sds keyname = NULL;
    char identity[PERLOG_IDENTITY_MAX] = {0};
    swapRequest *req = samplectx->req;

    if (!samplectx->started) return;

    rocksdb_set_perf_level(PERF_LEVEL_kDisable);

    duration = elapsedUs(samplectx->timer);
    if (duration < server.swap_perflog_log_slower_than) {
        rocksdb_perfcontext_destroy(samplectx->perfctx);
        return;
    }

    if (req->swapCtx && req->swapCtx->c) {
        client *c = req->swapCtx->c;
        if (c->cmd) {
            cmdname = c->cmd->name;
        } else if (c->lastcmd) {
            cmdname = c->lastcmd->name;
        }
    }

    if (req->data && req->data->key)
        keyname = req->data->key->ptr;

    snprintf(identity,sizeof(identity)-1,"[%s.%d] [%s]: %s", 
            swapIntentionName(req->intention),req->intention_flags,
            cmdname, keyname ? keyname : "NULL");

    perf_report = rocksdb_perfcontext_report(samplectx->perfctx,0);

    perflogPushEntry(identity,perf_report,iostats_report,duration);

    if (perf_report) zlibc_free(perf_report);
    if (iostats_report) zlibc_free(iostats_report);

    rocksdb_perfcontext_destroy(samplectx->perfctx);
}

void perflogCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"GET [<count>]",
"    Return top <count> entries from the perflog (default: 10). Entries are",
"    made of:",
"    id, timestamp, time(microseconds), identity, perf_report, iostats_report",
"LEN",
"    Return the length of the perflog.",
"RESET",
"    Reset the perflog.",
NULL
        };
        addReplyHelp(c, help);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"reset")) {
        perflogReset();
        addReply(c,shared.ok);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"len")) {
        addReplyLongLong(c,listLength(server.swap_perflog));
    } else if ((c->argc == 2 || c->argc == 3) &&
               !strcasecmp(c->argv[1]->ptr,"get"))
    {
        long count = 10, sent = 0;
        listIter li;
        void *totentries;
        listNode *ln;
        perflogEntry *pe;

        if (c->argc == 3 &&
            getLongFromObjectOrReply(c,c->argv[2],&count,NULL) != C_OK)
            return;

        listRewind(server.swap_perflog,&li);
        totentries = addReplyDeferredLen(c);
        while(count-- && (ln = listNext(&li))) {
            pe = ln->value;
            addReplyArrayLen(c,6);
            addReplyLongLong(c,pe->id);
            addReplyLongLong(c,pe->time);
            addReplyLongLong(c,pe->duration);
            addReplyBulkCString(c,pe->identity);
            addReplyBulkCString(c,pe->perf_report);
            addReplyBulkCString(c,pe->iostats_report);
            sent++;
        }
        setDeferredArrayLen(c,totentries,sent);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

