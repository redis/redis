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

/* swap stats */
#define SWAP_REQUEST_MEMORY_OVERHEAD (sizeof(swapRequest)+sizeof(swapCtx)+ \
                                      sizeof(wholeKeySwapData)/*typical*/+ \
                                      sizeof(requestListener))

static inline size_t estimateRIOSwapMemory(RIO *rio) {
    size_t memory = 0;
    int i;
    switch (rio->action) {
    case ROCKS_GET:
        memory += sdsalloc(rio->get.rawkey);
        if (rio->get.rawval) memory += sdsalloc(rio->get.rawval);
        break;
    case ROCKS_PUT:
        memory += sdsalloc(rio->put.rawkey);
        memory += sdsalloc(rio->put.rawval);
        break;
    case ROCKS_DEL:
        memory += sdsalloc(rio->del.rawkey);
        break;
    case ROCKS_WRITE:
        rocksdb_writebatch_data(rio->write.wb,&memory);
        break;
    case ROCKS_MULTIGET:
        for (i = 0; i < rio->multiget.numkeys; i++) {
            memory += sdsalloc(rio->multiget.rawkeys[i]);
            if (rio->multiget.rawvals && rio->multiget.rawvals[i])
                memory += sdsalloc(rio->multiget.rawvals[i]);
        }
        break;
    case ROCKS_SCAN:
        memory += sdsalloc(rio->scan.prefix);
        for (i = 0; i < rio->scan.numkeys; i++) {
            memory += sdsalloc(rio->scan.rawkeys[i]);
            memory += sdsalloc(rio->scan.rawvals[i]);
        }
        break;
    default:
        break;
    }

    return memory;
}

void initStatsSwap() {
    int i, metric_offset;
    server.swap_stats = zmalloc(SWAP_TYPES*sizeof(swapStat));
    for (i = 0; i < SWAP_TYPES; i++) {
        metric_offset = SWAP_SWAP_STATS_METRIC_OFFSET + i*SWAP_STAT_METRIC_SIZE;
        server.swap_stats[i].name = swapIntentionName(i);
        server.swap_stats[i].count = 0;
        server.swap_stats[i].memory = 0;
        server.swap_stats[i].stats_metric_idx_count = metric_offset+SWAP_STAT_METRIC_COUNT;
        server.swap_stats[i].stats_metric_idx_memory = metric_offset+SWAP_STAT_METRIC_MEMORY;
    }
    server.rio_stats = zmalloc(ROCKS_TYPES*sizeof(swapStat));
    for (i = 0; i < ROCKS_TYPES; i++) {
        metric_offset = SWAP_RIO_STATS_METRIC_OFFSET + i*SWAP_STAT_METRIC_SIZE;
        server.rio_stats[i].name = rocksActionName(i);
        server.rio_stats[i].count = 0;
        server.rio_stats[i].memory = 0;
        server.rio_stats[i].stats_metric_idx_count = metric_offset+SWAP_STAT_METRIC_COUNT;
        server.rio_stats[i].stats_metric_idx_memory = metric_offset+SWAP_STAT_METRIC_MEMORY;
    }
}

void trackSwapInstantaneousMetrics() {
    int i;
    swapStat *s;
    size_t count, memory;
    for (i = 1; i < SWAP_TYPES; i++) {
        s = server.swap_stats + i;
        atomicGet(s->count,count);
        atomicGet(s->memory,memory);
        trackInstantaneousMetric(s->stats_metric_idx_count,count);
        trackInstantaneousMetric(s->stats_metric_idx_memory,memory);
    }
    for (i = 1; i < ROCKS_TYPES; i++) {
        s = server.rio_stats + i;
        atomicGet(s->count,count);
        atomicGet(s->memory,memory);
        trackInstantaneousMetric(s->stats_metric_idx_count,count);
        trackInstantaneousMetric(s->stats_metric_idx_memory,memory);
    }
}

sds genSwapInfoString(sds info) {
    int j;
    size_t count, memory;
    info = sdscatprintf(info,
            "swap_inprogress_count:%ld\r\n"
            "swap_inprogress_memory:%ld\r\n",
            server.swap_inprogress_count,
            server.swap_inprogress_memory);

    for (j = 1; j < SWAP_TYPES; j++) {
        swapStat *s = &server.swap_stats[j];
        atomicGet(s->count,count);
        atomicGet(s->memory,memory);
        info = sdscatprintf(info, "swap_%s:count=%ld,memory=%ld,ops=%lld,bps=%lld\r\n",
                s->name,count,memory,
                getInstantaneousMetric(s->stats_metric_idx_count),
                getInstantaneousMetric(s->stats_metric_idx_memory));
    }

    for (j = 1; j < ROCKS_TYPES; j++) {
        swapStat *s = &server.rio_stats[j];
        atomicGet(s->count,count);
        atomicGet(s->memory,memory);
        info = sdscatprintf(info,"rio_%s:count=%ld,memory=%ld,ops=%lld,bps=%lld\r\n",
                s->name,count,memory,
                getInstantaneousMetric(s->stats_metric_idx_count),
                getInstantaneousMetric(s->stats_metric_idx_memory));
    }
    return info;
}

/* Note that swap thread upadates swap stats, reset when there are swapRequest
 * inprogress would result swap_in_progress overflow when swap finishs. */ 
void resetStatsSwap() {
    int i;
    for (i = 0; i < SWAP_TYPES; i++) {
        server.swap_stats[i].count = 0;
        server.swap_stats[i].memory = 0;
    }
    for (i = 0; i < ROCKS_TYPES; i++) {
        server.rio_stats[i].count = 0;
        server.rio_stats[i].memory = 0;
    }
}

void updateStatsSwapStart(swapRequest *req) {
    req->swap_memory += SWAP_REQUEST_MEMORY_OVERHEAD;
    atomicIncr(server.swap_inprogress_count,1);
    atomicIncr(server.swap_inprogress_memory,req->swap_memory);
    atomicIncr(server.swap_stats[req->intention].count,1);
    atomicIncr(server.swap_stats[req->intention].memory,req->swap_memory);
}

void updateStatsSwapFinish(swapRequest *req) {
    atomicDecr(server.swap_inprogress_count,1);
    atomicDecr(server.swap_inprogress_memory,req->swap_memory);
}

void updateStatsSwapRIO(swapRequest *req, RIO *rio) {
    int intention = req->intention, action = rio->action;
    size_t rio_memory = estimateRIOSwapMemory(rio);
    req->swap_memory += rio_memory;
    atomicIncr(server.swap_inprogress_memory,rio_memory);
    atomicIncr(server.swap_stats[intention].memory,rio_memory);
    atomicIncr(server.rio_stats[action].count,1);
    atomicIncr(server.rio_stats[action].memory,rio_memory);
}

/* ----------------------------- ratelimit ------------------------------ */
#define SWAP_RATELIMIT_DELAY_SLOW 1
#define SWAP_RATELIMIT_DELAY_STOP 10

int swapRateLimitState() {
    if (server.swap_inprogress_memory < server.swap_memory_slowdown) {
        return SWAP_RL_NO;
    } else if (server.swap_inprogress_memory < server.swap_memory_stop) {
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
        pct = ((float)server.swap_inprogress_memory - server.swap_memory_slowdown) / ((float)server.swap_memory_stop - server.swap_memory_slowdown);
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
        serverLog(LL_VERBOSE, "[ratelimit] client(%ld) swap_inprogress_memory(%ld) delay(%d)ms",
                c ? (int64_t)c->id:-2, server.swap_inprogress_memory, delay);
    } else {
        if (c) c->swap_rl_until = 0;
    }
    
    return delay;
}

int swapRateLimited(client *c) {
    return c->swap_rl_until >= server.mstime;
}

