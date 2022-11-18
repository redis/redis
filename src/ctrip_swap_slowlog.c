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
#include "slowlog.h"

void swapSlowlogCommand(client *c) {
    if (server.swap_mode == SWAP_MODE_MEMORY) {
        slowlogCommand(c);
        return;
    }

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"GET [<count>]",
"    Return top <count> entries from the slowlog (default: 10). Entries are",
"    made of:",
"    id, timestamp, time in microseconds, swap cnt, swap time in microseconds, arguments array, ",
"    client IP and port, client nasme,",
"    swap debug traces(need open swap-debug-trace-latency before).",
NULL
        };
        addReplyHelp(c, help);
    } else if ((c->argc == 2 || c->argc == 3) && !strcasecmp(c->argv[1]->ptr,"get")) {
        long count = 10, sent = 0;
        long lock, dispatch, process, notify;
        listIter li;
        void *totentries;
        listNode *ln;
        slowlogEntry *se;

        if (c->argc == 3 &&
        getLongFromObjectOrReply(c,c->argv[2],&count,NULL) != C_OK)
            return;

        listRewind(server.slowlog,&li);
        totentries = addReplyDeferredLen(c);
        while(count-- && (ln = listNext(&li))) {
            int j;

            se = ln->value;
            addReplyArrayLen(c,se->traces ? 10:8);
            addReplyLongLong(c,se->id);
            addReplyLongLong(c,se->time);
            addReplyLongLong(c,se->duration);
            addReplyLongLong(c,se->swap_cnt);
            addReplyLongLong(c,se->swap_duration);
            addReplyArrayLen(c,se->argc);
            for (j = 0; j < se->argc; j++)
                addReplyBulk(c,se->argv[j]);
            addReplyBulkCBuffer(c,se->peerid,sdslen(se->peerid));
            addReplyBulkCBuffer(c,se->cname,sdslen(se->cname));
            if (se->traces) {
                addReplyBulkCString(c, "swap traces:");
                addReplyArrayLen(c, se->trace_cnt);
                for (int i = 0; i < se->trace_cnt; i++) {
                    swapTrace *trace = se->traces + i;
                    if (trace->swap_process_time) {
                        lock = trace->swap_dispatch_time - trace->swap_lock_time;
                        dispatch = trace->swap_process_time - trace->swap_dispatch_time;
                        process = trace->swap_notify_time - trace->swap_process_time;
                        notify = trace->swap_callback_time - trace->swap_notify_time;
                        addReplyStatusFormat(c, "%s:lock=%lu,dispatch=%lu,process:%lu,notify:%lu",
                            swapIntentionName(trace->intention), lock, dispatch, process, notify);
                    } else {
                        /* no swap */
                        lock = trace->swap_dispatch_time - trace->swap_lock_time;
                        addReplyStatusFormat(c, "%s:lock=%lu,dispatch=-1,process:-1,notify:-1",
                            swapIntentionName(trace->intention), lock);
                    }
                }
            }
            sent++;
        }
        setDeferredArrayLen(c,totentries,sent);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

swapCmdTrace *createSwapCmdTrace() {
    swapCmdTrace *cmd = zcalloc(sizeof(swapCmdTrace));
    return cmd;
}

void initSwapTraces(swapCmdTrace *swap_cmd, int swap_cnt) {
    serverAssert(NULL == swap_cmd->swap_traces && 0 == swap_cmd->swap_cnt);
    swap_cmd->swap_cnt = swap_cnt;
    swap_cmd->swap_traces = zcalloc(swap_cnt * sizeof(swapTrace));
}

inline void swapCmdSwapSubmitted(swapCmdTrace *swap_cmd) {
    swap_cmd->swap_submitted_time = getMonotonicUs();
}

inline void swapTraceLock(swapTrace *trace) {
    trace->swap_lock_time = getMonotonicUs();
}

inline void swapTraceDispatch(swapTrace *trace) {
    trace->swap_dispatch_time = getMonotonicUs();
}

inline void swapTraceProcess(swapTrace *trace) {
    trace->swap_process_time = getMonotonicUs();
}

inline void swapTraceNotify(swapTrace *trace, int intention) {
    trace->intention = intention;
    trace->swap_notify_time = getMonotonicUs();
}

inline void swapTraceCallback(swapTrace *trace) {
    trace->swap_callback_time = getMonotonicUs();
}

void swapCmdSwapFinished(swapCmdTrace *swap_cmd) {
    swap_cmd->finished_swap_cnt++;
    if (swap_cmd->finished_swap_cnt == swap_cmd->swap_cnt) {
        swap_cmd->swap_finished_time = getMonotonicUs();
    }
}

void swapCmdTraceFree(swapCmdTrace *trace) {
    if (trace->swap_traces) zfree(trace->swap_traces);
    zfree(trace);
}

void attachSwapTracesToSlowlog(void *ptr, swapCmdTrace *swap_cmd) {
    slowlogEntry *se = (slowlogEntry*) ptr;
    se->swap_cnt = swap_cmd->swap_cnt;
    se->swap_duration = swap_cmd->swap_finished_time - swap_cmd->swap_submitted_time;
    if (swap_cmd->swap_traces && swap_cmd->swap_cnt) {
        if (swap_cmd->swap_cnt > SLOWLOG_ENTRY_MAX_TRACE) {
            se->trace_cnt = SLOWLOG_ENTRY_MAX_TRACE;
            se->traces = zmalloc(SLOWLOG_ENTRY_MAX_TRACE*sizeof(swapTrace));
            memcpy(se->traces, swap_cmd->swap_traces, SLOWLOG_ENTRY_MAX_TRACE*sizeof(swapTrace));
        } else {
            /* move traces from swapCmdTrace to slowlog */
            se->trace_cnt = swap_cmd->swap_cnt;
            se->traces = swap_cmd->swap_traces;
            swap_cmd->swap_traces = NULL;
        }
    }
}
