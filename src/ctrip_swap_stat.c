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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/times.h>

/* ----------------------------- statistics ------------------------------ */
/* Estimate memory used for one swap action, server will slow down event
 * processing if swap consumed too much memory(i.e. server is generating
 * io requests faster than rocksdb can handle). */

/* swap stats */
#define SWAP_REQUEST_MEMORY_OVERHEAD (sizeof(swapRequest)+sizeof(swapCtx)+ \
                                      sizeof(wholeKeySwapData)/*typical*/+ \
                                      sizeof(lock))

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
    default:
        break;
    }

    return memory;
}

void initStatsSwap() {
    int i, metric_offset;
    server.ror_stats = zmalloc(sizeof(rorStat));
    server.ror_stats->swap_stats = zmalloc(SWAP_TYPES*sizeof(swapStat));
    for (i = 0; i < SWAP_TYPES; i++) {
        metric_offset = SWAP_SWAP_STATS_METRIC_OFFSET + i*SWAP_STAT_METRIC_SIZE;
        server.ror_stats->swap_stats[i].name = swapIntentionName(i);
        server.ror_stats->swap_stats[i].count = 0;
        server.ror_stats->swap_stats[i].memory = 0;
        server.ror_stats->swap_stats[i].time = 0;
        server.ror_stats->swap_stats[i].stats_metric_idx_count = metric_offset+SWAP_STAT_METRIC_COUNT;
        server.ror_stats->swap_stats[i].stats_metric_idx_memory = metric_offset+SWAP_STAT_METRIC_MEMORY;
        server.ror_stats->swap_stats[i].stats_metric_idx_time = metric_offset+SWAP_STAT_METRIC_TIME;
    }
    server.ror_stats->rio_stats = zmalloc(ROCKS_TYPES*sizeof(swapStat));
    for (i = 0; i < ROCKS_TYPES; i++) {
        metric_offset = SWAP_RIO_STATS_METRIC_OFFSET + i*SWAP_STAT_METRIC_SIZE;
        server.ror_stats->rio_stats[i].name = rocksActionName(i);
        server.ror_stats->rio_stats[i].count = 0;
        server.ror_stats->rio_stats[i].memory = 0;
        server.ror_stats->rio_stats[i].time = 0;
        server.ror_stats->rio_stats[i].stats_metric_idx_count = metric_offset+SWAP_STAT_METRIC_COUNT;
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

    swapCpuUsageInit(&server.thread_cpu_usage);
}

static double getUptime(void) {
    FILE *file = fopen("/proc/uptime", "r");
    if (!file) {
        serverLog(LL_WARNING, "Error opening file /proc/uptime : %s (error: %d)", strerror(errno), errno);
        return -1;
    }

    double uptime;
    int matched = fscanf(file, "%lf", &uptime);
    fclose(file);

    if (matched != 1) {
        serverLog(LL_WARNING, "Error reading file /proc/uptime : %s (error: %d)", strerror(errno), errno);
        return -1;
    }

    return uptime;
}

static double getThreadTicks(int pid, int tid) {
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/proc/%d/task/%d/stat", pid, tid);

    FILE *file = fopen(filepath, "r");
    if (!file) {
        serverLog(LL_WARNING, "Error opening file /proc/%d/task/%d/stat : %s (error: %d)", pid, tid, strerror(errno), errno);
        return -1;
    }

    double utime, stime;
    int matched = fscanf(file, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %lf %lf",
                         &utime, &stime);
    fclose(file);

    if (matched != 2) {
        serverLog(LL_WARNING, "Error reading file /proc/%d/task/%d/stat : %s (error: %d)", pid, tid, strerror(errno), errno);
        return -1;
    }

    return utime + stime;
}

static void getThreadTids(int redis_pid, int *tid_array, const char *prefix, int array_length) {
    char task_dir[256];
    snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", redis_pid);

    DIR *dir = opendir(task_dir);
    if (!dir) {
        serverLog(LL_WARNING, "Error opening file /proc/%d/task : %s (error: %d)", redis_pid, strerror(errno), errno);
        return -1;
    }

    if(array_length == -1){
        return dirfd(dir);
    }

    struct dirent *entry;
    int num = 0;
    while ((entry = readdir(dir)) != NULL) {
        if(num >= array_length){
            break;
        }
        if (entry->d_type != DT_DIR || entry->d_name[0] == '.'|| entry->d_name[0] == '..')
            continue;

        int tid = atoi(entry->d_name);
        if (tid == 0)
            continue;
        char stat_path[256];
        snprintf(stat_path, sizeof(stat_path), "%s/%d/stat", task_dir, tid);

        FILE *file = fopen(stat_path, "r");
        if (!file) {
            serverLog(LL_WARNING, "Error opening file %s/%d/stat : %s (error: %d)", task_dir, tid, strerror(errno), errno);
            continue;
        }

        int parsed_tid;
        char comm[256];
        if (fscanf(file, "%d %255s", &parsed_tid, comm) != 2) {
            serverLog(LL_WARNING, "Error reading file %s/%d/stat : %s (error: %d)", task_dir, tid, strerror(errno), errno);
            fclose(file);
            continue;
        }
        fclose(file);

        if (tid_array != NULL && strncmp(comm, prefix, strlen(prefix)) == 0) {
            tid_array[num++] = tid;
        }
    }

    closedir(dir);
    return;
}

static int *filter_tids(int *total_tids, int num, int *tids1, int num1, int *tids2, int num2) {
    int n = num - num1 - num2;
    int *result = zmalloc(n * sizeof(int));
    int result_idx = 0;

    for (int i = 0; i < num; i++) {
        int tid = total_tids[i];
        int found = 0;

        for (int j = 0; j < num1 && !found; j++) {
            if (tid == tids1[j]) {
                found = 1;
            }
        }

        for (int j = 0; j < num2 && !found; j++) {
            if (tid == tids2[j]) {
                found = 1;
            }
        }

        if (!found) {
            result[result_idx++] = tid;
        }
    }

    return result;
}

static int count_task_subdirs(int pid) {
    DIR *dir;
    struct dirent *entry;
    int subdir_count = 0;
    char dir_path[256];

    snprintf(dir_path, sizeof(dir_path), "/proc/%d/task", pid);

    dir = opendir(dir_path);
    if (dir == NULL) {
        serverLog(LL_WARNING, "Error opening file /proc/%d/task : %s (error: %d)", pid, strerror(errno), errno);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                subdir_count++;
            }
        }
    }

    closedir(dir);

    return subdir_count;
}

void swapCpuUsageInit(redisThreadCpuUsage *cpuUsage) {
    cpuUsage->pid = getpid();
    cpuUsage->hertz = sysconf(_SC_CLK_TCK);
    cpuUsage->uptime_save = getUptime();

    cpuUsage->swap_thread_ticks_save = zmalloc(server.total_swap_threads_num * sizeof(double));
    cpuUsage->swap_tids = zmalloc(server.total_swap_threads_num * sizeof(int));

    getThreadTids(cpuUsage->pid, cpuUsage->main_tid, "(redis-server", 1);
    cpuUsage->main_thread_ticks_save = getThreadTicks(cpuUsage->pid, cpuUsage->main_tid[0]);

    getThreadTids(cpuUsage->pid, cpuUsage->swap_tids, "(swap", server.total_swap_threads_num);
    for (int i = 0; i < server.total_swap_threads_num; i++) {
        cpuUsage->swap_thread_ticks_save[i] = getThreadTicks(cpuUsage->pid, cpuUsage->swap_tids[i]);
    }
}

static void otherCpuUsageInit(redisThreadCpuUsage *cpuUsage){
    cpuUsage->other_tids_num = count_task_subdirs(cpuUsage->pid);
    if(cpuUsage->other_tids_num == 0){
        cpuUsage->other_tids_num = -1;
        return;
    }
    int total_tids[cpuUsage->other_tids_num];
    getThreadTids(cpuUsage->pid, total_tids, "(", cpuUsage->other_tids_num);
    cpuUsage->other_tids = filter_tids(total_tids,cpuUsage->other_tids_num,cpuUsage->main_tid,1,cpuUsage->swap_tids,server.total_swap_threads_num);
    int other_num = cpuUsage->other_tids_num - 1 - server.total_swap_threads_num;
    cpuUsage->other_tids_num = other_num;
    cpuUsage->other_thread_ticks_save = zmalloc(other_num * sizeof(double));
    for (int i = 0; i < cpuUsage->other_tids_num; i++){
        cpuUsage->other_thread_ticks_save[i] = getThreadTicks(cpuUsage->pid, cpuUsage->other_tids[i]);
    }
    return;
}

void redisThreadCpuUsageUpdate(redisThreadCpuUsage *cpuUsage) {
    if(cpuUsage->other_tids_num == 0)
        otherCpuUsageInit(cpuUsage);
    double time_cur = 0;
    double temp = 0;
    time_cur = getUptime();
    double hertz_multiplier = cpuUsage->hertz * (time_cur - cpuUsage->uptime_save);
    cpuUsage->uptime_save = time_cur;

    time_cur = getThreadTicks(cpuUsage->pid, cpuUsage->main_tid[0]);
    cpuUsage->main_thread_cpu_usage = (time_cur - cpuUsage->main_thread_ticks_save) / hertz_multiplier;
    cpuUsage->main_thread_ticks_save = time_cur;

    for (int i = 0; i < server.total_swap_threads_num; i++) {
        time_cur = getThreadTicks(cpuUsage->pid, cpuUsage->swap_tids[i]);
        temp += (time_cur - cpuUsage->swap_thread_ticks_save[i]) / hertz_multiplier;
        cpuUsage->swap_thread_ticks_save[i] = time_cur;
    }
    cpuUsage->swap_threads_cpu_usage = temp;

    if(cpuUsage->other_tids_num == -1)
        return;

    temp = 0;
    for (int i = 0; i < cpuUsage->other_tids_num; i++){
        time_cur = getThreadTicks(cpuUsage->pid, cpuUsage->other_tids[i]);
        temp += (time_cur - cpuUsage->other_thread_ticks_save[i]) / hertz_multiplier;
        cpuUsage->other_thread_ticks_save[i] = time_cur;
    }
    cpuUsage->other_threads_total_cpu_usage = temp;
}

sds genRedisThreadCpuUsageInfoString(sds info, redisThreadCpuUsage *cpuUsage){
    info = sdscatprintf(info,
                        "main_thread_cpu_usage:%.2f%%\r\n"
                        "swap_threads_total_cpu_usage:%.2f%%\r\n"
                        "other_threads_total_cpu_usage:%.2f%%\r\n",
                        cpuUsage->main_thread_cpu_usage * 100,
                        cpuUsage->swap_threads_cpu_usage * 100,
                        cpuUsage->other_threads_total_cpu_usage * 100);
    return info;
}

void trackSwapInstantaneousMetrics() {
    int i;
    swapStat *s;
    size_t count, memory, time;
    for (i = 1; i < SWAP_TYPES; i++) {
        s = server.ror_stats->swap_stats + i;
        atomicGet(s->count,count);
        atomicGet(s->memory,memory);
        atomicGet(s->time,time);
        trackInstantaneousMetric(s->stats_metric_idx_count,count);
        trackInstantaneousMetric(s->stats_metric_idx_memory,memory);
        trackInstantaneousMetric(s->stats_metric_idx_time,time);
    }
    for (i = 1; i < ROCKS_TYPES; i++) {
        s = server.ror_stats->rio_stats + i;
        atomicGet(s->count,count);
        atomicGet(s->memory,memory);
        atomicGet(s->time,time);
        trackInstantaneousMetric(s->stats_metric_idx_count,count);
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
    long long ops, total_latency;
    size_t count, memory;
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
        atomicGet(s->memory,memory);
        ops = getInstantaneousMetric(s->stats_metric_idx_count);
        total_latency = getInstantaneousMetric(s->stats_metric_idx_time);
        info = sdscatprintf(info, "swap_%s:count=%ld,memory=%ld,ops=%lld,bps=%lld,latency_po=%lld\r\n",
                s->name,count,memory,
                ops, getInstantaneousMetric(s->stats_metric_idx_memory), ops > 0 ? total_latency/ops : 0);
    }

    for (j = 1; j < ROCKS_TYPES; j++) {
        swapStat *s = &server.ror_stats->rio_stats[j];
        atomicGet(s->count,count);
        atomicGet(s->memory,memory);
        ops = getInstantaneousMetric(s->stats_metric_idx_count);
        total_latency = getInstantaneousMetric(s->stats_metric_idx_time);
        info = sdscatprintf(info,"swap_rio_%s:count=%ld,memory=%ld,ops=%lld,bps=%lld,latency_po=%lld\r\n",
                s->name,count,memory,
                ops, getInstantaneousMetric(s->stats_metric_idx_memory), ops > 0 ? total_latency/ops : 0);
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
        server.ror_stats->swap_stats[i].memory = 0;
    }
    for (i = 0; i < ROCKS_TYPES; i++) {
        server.ror_stats->rio_stats[i].count = 0;
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

void updateStatsSwapStart(swapRequest *req) {
    elapsedStart(&req->swap_timer);
    req->swap_memory += SWAP_REQUEST_MEMORY_OVERHEAD;
    atomicIncr(server.swap_inprogress_count,1);
    atomicIncr(server.swap_inprogress_memory,req->swap_memory);
}

void updateStatsSwapNotify(swapRequest *req) {
    /* req->intention may be negative when key doesn't exist */
    int intention = req->intention < 0 ? req->key_request->cmd_intention : req->intention;
    const long duration = elapsedUs(req->swap_timer);
    atomicIncr(server.ror_stats->swap_stats[intention].count,1);
    atomicIncr(server.ror_stats->swap_stats[intention].memory,req->swap_memory);
    atomicIncr(server.ror_stats->swap_stats[intention].time, duration);
}

void updateStatsSwapFinish(swapRequest *req) {
    atomicDecr(server.swap_inprogress_count,1);
    atomicDecr(server.swap_inprogress_memory,req->swap_memory);
}

void updateStatsSwapDataNotFound(RIO *rio) {
    int notfound = 0;

    if (rio->action == ROCKS_GET && rio->get.rawval == NULL)
        notfound = 1;

    if (rio->action == ROCKS_MULTIGET) {
        int anyfound = 0, i;
        for (i = 0; i < rio->multiget.numkeys; i++) {
            if (rio->multiget.rawvals[i] != NULL) {
                anyfound = 1;
                break;
            }
        }
        if (!anyfound) notfound = 1;
    }

    if (notfound) {
        atomicIncr(server.swap_hit_stats->stat_swapin_data_not_found_count,1);
    }
}

void updateStatsSwapRIO(swapRequest *req, RIO *rio) {
    // TODO: miss rio memory used in swap meta
    size_t rio_memory = estimateRIOSwapMemory(rio);
    req->swap_memory += rio_memory;
    atomicIncr(server.swap_inprogress_memory,rio_memory);
}

void updateStatsSwapRIOFinish(RIO *rio, long duration) {
    int action = rio->action;
    size_t rio_memory = estimateRIOSwapMemory(rio);
    atomicIncr(server.ror_stats->rio_stats[action].memory,rio_memory);
    atomicIncr(server.ror_stats->rio_stats[action].count,1);
    atomicIncr(server.ror_stats->rio_stats[action].time,duration);
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
