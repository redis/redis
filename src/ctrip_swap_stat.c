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

/* ----------------------------- statistics ------------------------------ */
/* Estimate memory used for one swap action, server will slow down event
 * processing if swap consumed too much memory(i.e. server is generating
 * io requests faster than rocksdb can handle). */
#define SWAP_MEM_ESTMIATED_ZMALLOC_OVERHEAD   512
#define SWAP_MEM_INFLIGHT_BASE (                                    \
        /* db.evict store scs */                                    \
        sizeof(moduleValue) + sizeof(robj) + sizeof(dictEntry) +    \
        sizeof(swapClient) + sizeof(swappingClients) +              \
        sizeof(rocksPrivData) +                                     \
        sizeof(RIO) +                                               \
        /* link in scs, pending_rios, processing_rios */            \
        (sizeof(list) + sizeof(listNode))*3 )

void updateStatsSwapStart(int type, sds rawkey, sds rawval) {
    serverAssert(type < SWAP_TYPES);
    size_t rawkey_bytes = rawkey == NULL ? 0 : sdslen(rawkey);
    size_t rawval_bytes = rawval == NULL ? 0 : sdslen(rawval);
    server.swap_stats[type].started++;
    server.swap_stats[type].last_start_time = server.mstime;
    server.swap_stats[type].started_rawkey_bytes += rawkey_bytes;
    server.swap_stats[type].started_rawval_bytes += rawval_bytes;
}

void updateStatsSwapFinish(int type, sds rawkey, sds rawval) {
    serverAssert(type < SWAP_TYPES);
    size_t rawkey_bytes = rawkey == NULL ? 0 : sdslen(rawkey);
    size_t rawval_bytes = rawval == NULL ? 0 : sdslen(rawval);
    server.swap_stats[type].finished++;
    server.swap_stats[type].last_finish_time = server.mstime;
    server.swap_stats[type].finished_rawkey_bytes += rawkey_bytes;
    server.swap_stats[type].finished_rawval_bytes += rawval_bytes;
}


/* ----------------------------- ratelimit ------------------------------ */
/* sleep 100us~100ms if current swap memory is (slowdown, stop). */
#define SWAP_RATELIMIT_DELAY_SLOW 1
#define SWAP_RATELIMIT_DELAY_STOP 10

int swapRateLimitState() {
    if (server.swap_memory < server.swap_memory_slowdown) {
        return SWAP_RL_NO;
    } else if (server.swap_memory < server.swap_memory_stop) {
        return SWAP_RL_SLOW;
    } else {
        return SWAP_RL_STOP;
    }
    return SWAP_RL_NO;
}

int swapRateLimit(client *c) {
    float pct;
    int delay;

    switch(swapRateLimitState()) {
    case SWAP_RL_NO:
        delay = 0;
        break;
    case SWAP_RL_SLOW:
        pct = ((float)server.swap_memory - server.swap_memory_slowdown) / ((float)server.swap_memory_stop - server.swap_memory_slowdown);
        delay = (int)(SWAP_RATELIMIT_DELAY_SLOW + pct*(SWAP_RATELIMIT_DELAY_STOP - SWAP_RATELIMIT_DELAY_SLOW));
        break;
    case SWAP_RL_STOP:
        delay = SWAP_RATELIMIT_DELAY_STOP;
        break;
    default:
        delay = 0;
        break;
    }

    if (delay > 0) {
        if (c) c->swap_rl_until = server.mstime + delay;
        serverLog(LL_VERBOSE, "[ratelimit] client(%ld) swap_memory(%ld) delay(%d)ms",
                c ? (int64_t)c->id:-2, server.swap_memory, delay);
    } else {
        if (c) c->swap_rl_until = 0;
    }
    
    return delay;
}

int swapRateLimited(client *c) {
    return c->swap_rl_until >= server.mstime;
}

