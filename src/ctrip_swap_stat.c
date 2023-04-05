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
void initStatsSwap() {
    int i, metric_offset;
    server.ror_stats = zmalloc(sizeof(rorStat));
    server.ror_stats->swap_stats = zmalloc(SWAP_TYPES*sizeof(swapStat));
    for (i = 0; i < SWAP_TYPES; i++) {
        metric_offset = SWAP_SWAP_STATS_METRIC_OFFSET + i*SWAP_STAT_METRIC_SIZE;
        server.ror_stats->swap_stats[i].name = swapIntentionName(i);
        server.ror_stats->swap_stats[i].count = 0;
        server.ror_stats->swap_stats[i].batch = 0;
        server.ror_stats->swap_stats[i].memory = 0;
        server.ror_stats->swap_stats[i].time = 0;
        server.ror_stats->swap_stats[i].stats_metric_idx_count = metric_offset+SWAP_STAT_METRIC_COUNT;
        server.ror_stats->swap_stats[i].stats_metric_idx_batch = metric_offset+SWAP_STAT_METRIC_BATCH;
        server.ror_stats->swap_stats[i].stats_metric_idx_memory = metric_offset+SWAP_STAT_METRIC_MEMORY;
        server.ror_stats->swap_stats[i].stats_metric_idx_time = metric_offset+SWAP_STAT_METRIC_TIME;
    }
    server.ror_stats->rio_stats = zmalloc(ROCKS_TYPES*sizeof(swapStat));
    for (i = 0; i < ROCKS_TYPES; i++) {
        metric_offset = SWAP_RIO_STATS_METRIC_OFFSET + i*SWAP_STAT_METRIC_SIZE;
        server.ror_stats->rio_stats[i].name = rocksActionName(i);
        server.ror_stats->rio_stats[i].count = 0;
        server.ror_stats->rio_stats[i].batch = 0;
        server.ror_stats->rio_stats[i].memory = 0;
        server.ror_stats->rio_stats[i].time = 0;
        server.ror_stats->rio_stats[i].stats_metric_idx_count = metric_offset+SWAP_STAT_METRIC_COUNT;
        server.ror_stats->rio_stats[i].stats_metric_idx_batch = metric_offset+SWAP_STAT_METRIC_BATCH;
        server.ror_stats->rio_stats[i].stats_metric_idx_memory = metric_offset+SWAP_STAT_METRIC_MEMORY;
        server.ror_stats->rio_stats[i].stats_metric_idx_time = metric_offset+SWAP_STAT_METRIC_TIME;
    }
    server.ror_stats->compaction_filter_stats = zmalloc(sizeof(compactionFilterStat) * CF_COUNT);
    for (i = 0; i < CF_COUNT; i++) {
        metric_offset = SWAP_COMPACTION_FILTER_STATS_METRIC_OFFSET + i * COMPACTION_FILTER_METRIC_SIZE;
        server.ror_stats->compaction_filter_stats[i].name = swap_cf_names[i];
        server.ror_stats->compaction_filter_stats[i].filt_count = 0;
        server.ror_stats->compaction_filter_stats[i].scan_count = 0;
        server.ror_stats->compaction_filter_stats[i].stats_metric_idx_filte = metric_offset+COMPACTION_FILTER_METRIC_filt_count;
        server.ror_stats->compaction_filter_stats[i].stats_metric_idx_scan = metric_offset+COMPACTION_FILTER_METRIC_SCAN_COUNT;
    }
    server.swap_debug_info = zmalloc(SWAP_DEBUG_INFO_TYPE*sizeof(swapDebugInfo));
    for (i = 0; i < SWAP_DEBUG_INFO_TYPE; i++) {
        metric_offset = SWAP_DEBUG_STATS_METRIC_OFFSET + i*SWAP_DEBUG_SIZE;
        server.swap_debug_info[i].name = swapDebugName(i);
        server.swap_debug_info[i].count = 0;
        server.swap_debug_info[i].value = 0;
        server.swap_debug_info[i].metric_idx_count = metric_offset+SWAP_DEBUG_COUNT;
        server.swap_debug_info[i].metric_idx_value = metric_offset+SWAP_DEBUG_VALUE;
    }

    server.swap_hit_stats = zcalloc(sizeof(swapHitStat));
}

void trackSwapInstantaneousMetrics() {
    int i;
    swapStat *s;
    size_t count, batch, memory, time;
    for (i = 1; i < SWAP_TYPES; i++) {
        s = server.ror_stats->swap_stats + i;
        atomicGet(s->count,count);
        atomicGet(s->batch,batch);
        atomicGet(s->memory,memory);
        atomicGet(s->time,time);
        trackInstantaneousMetric(s->stats_metric_idx_count,count);
        trackInstantaneousMetric(s->stats_metric_idx_batch,batch);
        trackInstantaneousMetric(s->stats_metric_idx_memory,memory);
        trackInstantaneousMetric(s->stats_metric_idx_time,time);
    }
    for (i = 1; i < ROCKS_TYPES; i++) {
        s = server.ror_stats->rio_stats + i;
        atomicGet(s->count,count);
        atomicGet(s->batch,batch);
        atomicGet(s->memory,memory);
        atomicGet(s->time,time);
        trackInstantaneousMetric(s->stats_metric_idx_count,count);
        trackInstantaneousMetric(s->stats_metric_idx_batch,batch);
        trackInstantaneousMetric(s->stats_metric_idx_memory,memory);
        trackInstantaneousMetric(s->stats_metric_idx_time,time);
    }
    long long filt_count, scan_count;
    compactionFilterStat* cfs;
    for (i = 0; i < CF_COUNT; i++) {
        cfs = server.ror_stats->compaction_filter_stats + i;
        atomicGet(cfs->filt_count,filt_count);
        trackInstantaneousMetric(cfs->stats_metric_idx_filte,filt_count);
        atomicGet(cfs->scan_count,scan_count);
        trackInstantaneousMetric(cfs->stats_metric_idx_scan,scan_count);
    }
    if (server.swap_debug_trace_latency) {
        swapDebugInfo *d;
        size_t cnt, val;
        for (i = 0; i < SWAP_DEBUG_INFO_TYPE; i++) {
            d = server.swap_debug_info + i;
            atomicGet(d->count, cnt);
            atomicGet(d->value, val);
            trackInstantaneousMetric(d->metric_idx_count,cnt);
            trackInstantaneousMetric(d->metric_idx_value,val);
        }
    }
    trackSwapLockInstantaneousMetrics();
}

sds genSwapInfoString(sds info) {
    info = genSwapStorageInfoString(info);
    info = genSwapHitInfoString(info);
    info = genSwapExecInfoString(info);
    info = genSwapLockInfoString(info);
    info = genSwapReplInfoString(info);
    info = genSwapThreadInfoString(info);
    info = genSwapScanSessionStatString(info);
    info = genSwapUnblockInfoString(info);
    return info;
}

sds genSwapExecInfoString(sds info) {
    int j;
    long long ops, batch_ps, total_latency;
    size_t count, batch, memory;

    info = sdscatprintf(info,
            "swap_request_count:%lld\r\n"
            "swap_batch_count:%lld\r\n",
            server.swap_batch_ctx->stat.request_count,
            server.swap_batch_ctx->stat.batch_count);

    info = sdscatprintf(info,
            "swap_inprogress_count:%ld\r\n"
            "swap_inprogress_memory:%ld\r\n"
            "swap_inprogress_evict_count:%d\r\n",
            server.swap_inprogress_count,
            server.swap_inprogress_memory,
            server.swap_evict_inprogress_count);

    for (j = 1; j < SWAP_TYPES; j++) {
        swapStat *s = &server.ror_stats->swap_stats[j];
        atomicGet(s->count,count);
        atomicGet(s->batch,batch);
        atomicGet(s->memory,memory);
        ops = getInstantaneousMetric(s->stats_metric_idx_count);
        batch_ps = getInstantaneousMetric(s->stats_metric_idx_count);
        total_latency = getInstantaneousMetric(s->stats_metric_idx_time);
        info = sdscatprintf(info,
                "swap_%s:count=%ld,batch=%ld,memory=%ld,ops=%lld,batch_ps=%lld,bps=%lld,latency_po=%lld,latency_per_batch=%lld\r\n",
                s->name,count,batch,memory, ops, batch_ps,
                getInstantaneousMetric(s->stats_metric_idx_memory),
                ops > 0 ? total_latency/ops : 0,
                batch_ps > 0 ? total_latency/batch_ps : 0);
    }

    for (j = 1; j < ROCKS_TYPES; j++) {
        swapStat *s = &server.ror_stats->rio_stats[j];
        atomicGet(s->count,count);
        atomicGet(s->batch,batch);
        atomicGet(s->memory,memory);
        ops = getInstantaneousMetric(s->stats_metric_idx_count);
        total_latency = getInstantaneousMetric(s->stats_metric_idx_time);
        info = sdscatprintf(info,"swap_rio_%s:count=%ld,batch=%ld,memory=%ld,ops=%lld,batch_ps=%lld,bps=%lld,latency_po=%lld,latency_per_batch=%lld\r\n",
                s->name,count,batch,memory,ops,batch_ps,
                getInstantaneousMetric(s->stats_metric_idx_memory),
                ops > 0 ? total_latency/ops : 0,
                batch_ps > 0 ? total_latency/batch_ps : 0);
    }

    for (j = 0; j < CF_COUNT; j++) {
        compactionFilterStat *cfs = &server.ror_stats->compaction_filter_stats[j];
        long long filt_count, scan_count;
        atomicGet(cfs->filt_count,filt_count);
        atomicGet(cfs->scan_count,scan_count);
        info = sdscatprintf(info,"swap_compaction_filter_%s:filt_count=%lld,scan_count=%lld,filt_ps=%lld,scan_ps=%lld\r\n",
                cfs->name,filt_count,scan_count,
                getInstantaneousMetric(cfs->stats_metric_idx_filte),
                getInstantaneousMetric(cfs->stats_metric_idx_scan));
    }
    if (server.swap_debug_trace_latency) {
        swapDebugInfo *d;
        long long cnt, val;
        info = sdscatprintf(info, "swap_debug_trace_latency:");
        for (j = 0; j < SWAP_DEBUG_INFO_TYPE; j++) {
            d = &server.swap_debug_info[j];
            cnt = getInstantaneousMetric(d->metric_idx_count);
            val = getInstantaneousMetric(d->metric_idx_value);
            info = sdscatprintf(info, "%s:%lld,", d->name, cnt>0 ? val/cnt : -1);
        }
        info = sdscatprintf(info, "\r\n");
    }
    return info;
}

sds genSwapUnblockInfoString(sds info) {
    info = sdscatprintf(info,
            "swap_dependency_block_version:%lld\r\n"
            "swap_dependency_block_total_count:%lld\r\n"
            "swap_dependency_block_swapping_count:%lld\r\n"
            "swap_dependency_block_retry_count:%lld\r\n",
            server.swap_dependency_block_ctx->version,
            server.swap_dependency_block_ctx->swap_total_count,
            server.swap_dependency_block_ctx->swapping_count,
            server.swap_dependency_block_ctx->swap_retry_count);
    return info;
}

/* Note that swap thread upadates swap stats, reset when there are swapRequest
 * inprogress would result swap_in_progress overflow when swap finishs. */ 
void resetStatsSwap() {
    int i;
    for (i = 0; i < SWAP_TYPES; i++) {
        server.ror_stats->swap_stats[i].count = 0;
        server.ror_stats->swap_stats[i].batch = 0;
        server.ror_stats->swap_stats[i].memory = 0;
    }
    for (i = 0; i < ROCKS_TYPES; i++) {
        server.ror_stats->rio_stats[i].count = 0;
        server.ror_stats->rio_stats[i].batch = 0;
        server.ror_stats->rio_stats[i].memory = 0;
    }
    for (i = 0; i < CF_COUNT; i++) {
        server.ror_stats->compaction_filter_stats[i].filt_count = 0;
        server.ror_stats->compaction_filter_stats[i].scan_count = 0;
    }
    resetSwapLockInstantaneousMetrics();
}

void resetSwapHitStat() {
    atomicSet(server.swap_hit_stats->stat_swapin_attempt_count,0);
    atomicSet(server.swap_hit_stats->stat_swapin_not_found_cachehit_count,0);
    atomicSet(server.swap_hit_stats->stat_swapin_not_found_cachemiss_count,0);
    atomicSet(server.swap_hit_stats->stat_swapin_no_io_count,0);
    atomicSet(server.swap_hit_stats->stat_swapin_data_not_found_count,0);
}

sds genSwapHitInfoString(sds info) {
    double memory_hit_perc = 0, keyspace_hit_perc = 0, notfound_cachehit_perc = 0;
    long long attempt, noio, notfound_cachemiss, notfound_cachehit, notfound, data_notfound;

    atomicGet(server.swap_hit_stats->stat_swapin_attempt_count,attempt);
    atomicGet(server.swap_hit_stats->stat_swapin_no_io_count,noio);
    atomicGet(server.swap_hit_stats->stat_swapin_not_found_cachemiss_count,notfound_cachemiss);
    atomicGet(server.swap_hit_stats->stat_swapin_not_found_cachehit_count,notfound_cachehit);
    atomicGet(server.swap_hit_stats->stat_swapin_data_not_found_count,data_notfound);
    notfound = notfound_cachehit + notfound_cachemiss;

    if (attempt) {
        memory_hit_perc = ((double)noio/attempt)*100;
        keyspace_hit_perc = ((double)(attempt - notfound)/attempt)*100;
    }
    if (notfound) {
        notfound_cachehit_perc = ((double)notfound_cachehit/notfound)*100;
    }

    info = sdscatprintf(info,
            "swap_swapin_attempt_count:%lld\r\n"
            "swap_swapin_not_found_count:%lld\r\n"
            "swap_swapin_no_io_count:%lld\r\n"
            "swap_swapin_memory_hit_perc:%.2f%%\r\n"
            "swap_swapin_keyspace_hit_perc:%.2f%%\r\n"
            "swap_swapin_not_found_cachehit:%lld\r\n"
            "swap_swapin_not_found_cachemiss:%lld\r\n"
            "swap_swapin_not_found_cachehit_perc:%.2f%%\r\n"
            "swap_swapin_data_not_found_count:%lld\r\n",
            attempt,notfound,noio,memory_hit_perc,keyspace_hit_perc,
            notfound_cachehit, notfound_cachemiss, notfound_cachehit_perc,
            data_notfound);
    return info;
}

void metricDebugInfo(int type, long val) {
    atomicIncr(server.swap_debug_info[type].count, 1);
    atomicIncr(server.swap_debug_info[type].value, val);
}

void updateCompactionFiltSuccessCount(int cf) {
    atomicIncr(server.ror_stats->compaction_filter_stats[cf].filt_count, 1);
}

void updateCompactionFiltScanCount(int cf) {
    atomicIncr(server.ror_stats->compaction_filter_stats[cf].scan_count, 1);
}

/* ----------------------------- ratelimit ------------------------------ */
#define SWAP_RATELIMIT_DELAY_SLOW 1
#define SWAP_RATELIMIT_DELAY_STOP 10

int swapRateLimitState() {
    if (server.swap_inprogress_memory <
            server.swap_inprogress_memory_slowdown) {
        return SWAP_RL_NO;
    } else if (server.swap_inprogress_memory <
            server.swap_inprogress_memory_stop) {
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
        pct = ((float)server.swap_inprogress_memory - server.swap_inprogress_memory_slowdown) / ((float)server.swap_inprogress_memory_stop - server.swap_inprogress_memory_slowdown);
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
