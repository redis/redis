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

#include "server.h"
#include <rocksdb/c.h>
#include <sys/statvfs.h>
#include <stddef.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "release.h"

#define KB 1024
#define MB (1024*1024)

const char *swap_cf_names[CF_COUNT] = {data_cf_name, meta_cf_name, score_cf_name};

int rmdirRecursive(const char *path);

static inline void rocks_init_option_compression(rocksdb_options_t *opts, int compression) {
    if (compression >= 0) {
        rocksdb_options_set_compression(opts, compression);
    } else {
        int level_values[7] = {
            rocksdb_no_compression,
            rocksdb_no_compression,
            rocksdb_snappy_compression,
            rocksdb_snappy_compression,
            rocksdb_snappy_compression,
            rocksdb_snappy_compression,
            rocksdb_snappy_compression,
        };
        rocksdb_options_set_compression_per_level(opts, level_values, 7);
    }
}

int rocksInit() {
    if (server.swap_debug_init_rocksdb_delay)
        usleep(server.swap_debug_init_rocksdb_delay * 1000);
    rocks *rocks = zcalloc(sizeof(struct rocks));
    char *errs[3] = {NULL}, dir[ROCKS_DIR_MAX_LEN];

    rocks->snapshot = NULL;
    rocks->checkpoint = NULL;
    rocks->checkpoint_dir = NULL;
    rocks->rdb_checkpoint_dir = NULL;
    atomicSetWithSync(server.inflight_snapshot, 0);
    rocks->db_opts = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(rocks->db_opts, 1);
    rocksdb_options_set_create_missing_column_families(rocks->db_opts, 1);
    rocksdb_options_optimize_for_point_lookup(rocks->db_opts, 1);

    rocksdb_options_set_min_write_buffer_number_to_merge(rocks->db_opts, 2);
    rocksdb_options_set_level0_file_num_compaction_trigger(rocks->db_opts, 2);
    rocksdb_options_set_max_bytes_for_level_base(rocks->db_opts, 256*MB);
    rocksdb_options_compaction_readahead_size(rocks->db_opts, 2*1024*1024); /* default 0 */

    rocksdb_options_set_max_background_compactions(rocks->db_opts, server.rocksdb_max_background_compactions); /* default 1 */
    rocksdb_options_set_max_background_flushes(rocks->db_opts, server.rocksdb_max_background_flushes); /* default -1 */
    rocksdb_options_set_max_subcompactions(rocks->db_opts, server.rocksdb_max_subcompactions); /* default 1 */
    rocksdb_options_set_max_open_files(rocks->db_opts,server.rocksdb_max_open_files);
    rocksdb_options_set_enable_pipelined_write(rocks->db_opts,server.rocksdb_enable_pipelined_write);

    rocks->ropts = rocksdb_readoptions_create();
    rocksdb_readoptions_set_verify_checksums(rocks->ropts, 0);
    rocksdb_readoptions_set_fill_cache(rocks->ropts, 1);

    rocks->wopts = rocksdb_writeoptions_create();
    rocksdb_writeoptions_disable_WAL(rocks->wopts, 1);

	if (server.rocksdb_ratelimiter_rate_per_sec > 0) {
		rocksdb_ratelimiter_t *ratelimiter = rocksdb_ratelimiter_create(server.rocksdb_ratelimiter_rate_per_sec, 100*1000/*100ms*/, 10);
		rocksdb_options_set_ratelimiter(rocks->db_opts, ratelimiter);
		rocksdb_ratelimiter_destroy(ratelimiter);
	}
    rocksdb_options_set_bytes_per_sync(rocks->db_opts,server.rocksdb_bytes_per_sync);

    struct stat statbuf;
    if (!stat(ROCKS_DATA, &statbuf) && S_ISDIR(statbuf.st_mode)) {
        /* "data.rocks" folder already exists, remove it on start */
        rmdirRecursive(ROCKS_DATA);
    }
    if (mkdir(ROCKS_DATA, 0755)) {
        serverLog(LL_WARNING, "[ROCKS] mkdir %s failed: %s",
                ROCKS_DATA, strerror(errno));
        return -1;
    }

    snprintf(dir, ROCKS_DIR_MAX_LEN, "%s/%d", ROCKS_DATA, server.rocksdb_epoch);
    rocks->filter_meta_ropts = rocksdb_readoptions_create();
    rocksdb_readoptions_set_verify_checksums(rocks->filter_meta_ropts, 0);
    rocksdb_readoptions_set_fill_cache(rocks->filter_meta_ropts, 0);

    /* data cf */
    rocks->cf_opts[DATA_CF] = rocksdb_options_create_copy(rocks->db_opts);
    rocks_init_option_compression(rocks->cf_opts[DATA_CF],server.rocksdb_data_compression);
    rocksdb_options_set_level0_slowdown_writes_trigger(rocks->cf_opts[DATA_CF],server.rocksdb_data_level0_slowdown_writes_trigger);
    rocksdb_options_set_disable_auto_compactions(rocks->cf_opts[DATA_CF],server.rocksdb_data_disable_auto_compactions);
    rocksdb_options_set_max_write_buffer_number(rocks->cf_opts[DATA_CF], server.rocksdb_data_max_write_buffer_number);
    rocksdb_options_set_target_file_size_base(rocks->cf_opts[DATA_CF], server.rocksdb_data_target_file_size_base);
    rocksdb_options_set_write_buffer_size(rocks->cf_opts[DATA_CF],server.rocksdb_data_write_buffer_size);
    rocksdb_options_set_max_bytes_for_level_base(rocks->cf_opts[DATA_CF],server.rocksdb_data_max_bytes_for_level_base);

    rocks->block_opts[DATA_CF] = rocksdb_block_based_options_create();
    rocks->cf_compaction_filters[DATA_CF] = createDataCfCompactionFilter();
    rocksdb_block_based_options_set_block_size(rocks->block_opts[DATA_CF], server.rocksdb_data_block_size);
    rocksdb_block_based_options_set_cache_index_and_filter_blocks(rocks->block_opts[DATA_CF], server.rocksdb_data_cache_index_and_filter_blocks);
    rocksdb_block_based_options_set_filter_policy(rocks->block_opts[DATA_CF], rocksdb_filterpolicy_create_bloom(10));

    rocksdb_cache_t *data_cache = rocksdb_cache_create_lru(server.rocksdb_data_block_cache_size);
    rocksdb_block_based_options_set_block_cache(rocks->block_opts[DATA_CF], data_cache);
    rocksdb_cache_destroy(data_cache);

    rocksdb_options_set_block_based_table_factory(rocks->cf_opts[DATA_CF], rocks->block_opts[DATA_CF]);
    rocksdb_options_set_compaction_filter(rocks->cf_opts[DATA_CF], rocks->cf_compaction_filters[DATA_CF]);

    /* score cf */
    rocks->cf_opts[SCORE_CF] = rocksdb_options_create_copy(rocks->db_opts);
    rocks_init_option_compression(rocks->cf_opts[SCORE_CF],server.rocksdb_data_compression);
    rocksdb_options_set_level0_slowdown_writes_trigger(rocks->cf_opts[SCORE_CF],server.rocksdb_data_level0_slowdown_writes_trigger);
    rocksdb_options_set_disable_auto_compactions(rocks->cf_opts[SCORE_CF],server.rocksdb_data_disable_auto_compactions);
    rocksdb_options_set_max_write_buffer_number(rocks->cf_opts[SCORE_CF], server.rocksdb_data_max_write_buffer_number);
    rocksdb_options_set_target_file_size_base(rocks->cf_opts[SCORE_CF], server.rocksdb_data_target_file_size_base);
    rocksdb_options_set_write_buffer_size(rocks->cf_opts[SCORE_CF],server.rocksdb_data_write_buffer_size);
    rocksdb_options_set_max_bytes_for_level_base(rocks->cf_opts[SCORE_CF],server.rocksdb_data_max_bytes_for_level_base);

    rocks->block_opts[SCORE_CF] = rocksdb_block_based_options_create();
    rocks->cf_compaction_filters[SCORE_CF] = createScoreCfCompactionFilter();
    rocksdb_block_based_options_set_block_size(rocks->block_opts[SCORE_CF], server.rocksdb_data_block_size);
    rocksdb_block_based_options_set_cache_index_and_filter_blocks(rocks->block_opts[SCORE_CF], server.rocksdb_data_cache_index_and_filter_blocks);
    rocksdb_block_based_options_set_filter_policy(rocks->block_opts[SCORE_CF], rocksdb_filterpolicy_create_bloom(10));

    rocksdb_cache_t *score_cache = rocksdb_cache_create_lru(server.rocksdb_data_block_cache_size);
    rocksdb_block_based_options_set_block_cache(rocks->block_opts[SCORE_CF], score_cache);
    rocksdb_cache_destroy(score_cache);

    rocksdb_options_set_block_based_table_factory(rocks->cf_opts[SCORE_CF], rocks->block_opts[SCORE_CF]);
    rocksdb_options_set_compaction_filter(rocks->cf_opts[SCORE_CF], rocks->cf_compaction_filters[SCORE_CF]);

    /* meta cf */
    rocks->cf_opts[META_CF] = rocksdb_options_create_copy(rocks->db_opts);
    rocks_init_option_compression(rocks->cf_opts[META_CF],server.rocksdb_meta_compression);
    rocksdb_options_set_level0_slowdown_writes_trigger(rocks->cf_opts[META_CF],server.rocksdb_meta_level0_slowdown_writes_trigger);
    rocksdb_options_set_disable_auto_compactions(rocks->cf_opts[META_CF],server.rocksdb_meta_disable_auto_compactions);
    rocksdb_options_set_max_write_buffer_number(rocks->cf_opts[META_CF], server.rocksdb_meta_max_write_buffer_number);
    rocksdb_options_set_target_file_size_base(rocks->cf_opts[META_CF], server.rocksdb_meta_target_file_size_base);
    rocksdb_options_set_write_buffer_size(rocks->cf_opts[META_CF],server.rocksdb_meta_write_buffer_size);
    rocksdb_options_set_max_bytes_for_level_base(rocks->cf_opts[META_CF],server.rocksdb_meta_max_bytes_for_level_base);

    rocks->block_opts[META_CF] = rocksdb_block_based_options_create();
    rocks->cf_compaction_filters[META_CF] = createMetaCfCompactionFilter();
    rocksdb_block_based_options_set_block_size(rocks->block_opts[META_CF], server.rocksdb_meta_block_size);
    rocksdb_block_based_options_set_cache_index_and_filter_blocks(rocks->block_opts[META_CF], server.rocksdb_meta_cache_index_and_filter_blocks);
    rocksdb_block_based_options_set_filter_policy(rocks->block_opts[META_CF], rocksdb_filterpolicy_create_bloom(10));

    rocksdb_cache_t *meta_cache = rocksdb_cache_create_lru(server.rocksdb_meta_block_cache_size);
    rocksdb_block_based_options_set_block_cache(rocks->block_opts[META_CF], meta_cache);
    rocksdb_cache_destroy(meta_cache);

    rocksdb_options_set_block_based_table_factory(rocks->cf_opts[META_CF], rocks->block_opts[META_CF]);
    rocksdb_options_set_compaction_filter(rocks->cf_opts[META_CF], rocks->cf_compaction_filters[META_CF]);

    setFilterState(FILTER_STATE_OPEN);
    rocks->db = rocksdb_open_column_families(rocks->db_opts, dir, CF_COUNT,
            swap_cf_names, (const rocksdb_options_t *const *)rocks->cf_opts,
            rocks->cf_handles, errs);
    if (errs[0] != NULL || errs[1] != NULL) {
        serverLog(LL_WARNING, "[ROCKS] rocksdb open failed: default_cf=%s, meta_cf=%s, score_cf=%s", errs[0], errs[1], errs[2]);
        return -1;
    }
    serverLog(LL_NOTICE, "[ROCKS] opened rocks data in (%s).", dir);
    rocks->rocksdb_stats_cache = NULL;
    server.rocks = rocks;
    return 0;
}

void rocksRelease() {
    int i;
    char dir[ROCKS_DIR_MAX_LEN];

    snprintf(dir, ROCKS_DIR_MAX_LEN, "%s/%d", ROCKS_DATA, server.rocksdb_epoch);
    rocks *rocks = server.rocks;
    serverLog(LL_NOTICE, "[ROCKS] releasing rocksdb in (%s).",dir);
    setFilterState(FILTER_STATE_CLOSE);
    for (i = 0; i < CF_COUNT; i++)
        rocksdb_block_based_options_destroy(rocks->block_opts[i]);
    for (i = 0; i < CF_COUNT; i++)
        rocksdb_options_destroy(rocks->cf_opts[i]);
    for (i = 0; i < CF_COUNT; i++)
        rocksdb_column_family_handle_destroy(rocks->cf_handles[i]);
    if (rocks->rocksdb_stats_cache != NULL) {
        for (i = 0; i < CF_COUNT; i++)
            zlibc_free(rocks->rocksdb_stats_cache[i]);
        zfree(rocks->rocksdb_stats_cache);
    }
    
    rocksdb_options_destroy(rocks->db_opts);
    rocksdb_writeoptions_destroy(rocks->wopts);
    rocksdb_readoptions_destroy(rocks->ropts);
    rocksdb_readoptions_destroy(rocks->filter_meta_ropts);
    rocksdb_close(rocks->db);
    for (i = 0; i < CF_COUNT; i++) {
        if (rocks->cf_compaction_filters[i]) rocksdb_compactionfilter_destroy(rocks->cf_compaction_filters[i]);
    }
    zfree(rocks);
    server.rocks = NULL;
}

void rocksReleaseCheckpoint() {
    rocks *rocks = server.rocks; 
    char* err = NULL;
    if (rocks->checkpoint != NULL) {
        serverLog(LL_NOTICE, "[rocks] releasing checkpoint in (%s).", rocks->checkpoint_dir);
        rocksdb_checkpoint_object_destroy(rocks->checkpoint);
        rocks->checkpoint = NULL;
        rocksdb_destroy_db(rocks->db_opts, rocks->checkpoint_dir, &err);
        if (err != NULL) {
            serverLog(LL_WARNING, "[rocks] destory db fail: %s", rocks->checkpoint_dir);
        }
        sdsfree(rocks->checkpoint_dir);
        rocks->checkpoint_dir = NULL;
    }
}

void rocksReleaseSnapshot() {
    rocks *rocks = server.rocks;
    if (NULL != rocks->snapshot) {
        serverLog(LL_WARNING, "[rocks] release snapshot.");
        rocksdb_release_snapshot(rocks->db, rocks->snapshot);
        rocks->snapshot = NULL;
        atomicDecr(server.inflight_snapshot, 1);
    }
}

int rocksCreateSnapshot() {
    rocks *rocks = server.rocks;
    if (NULL != rocks->snapshot) {
        rocksReleaseSnapshot();
    }

    serverLog(LL_NOTICE, "[rocks] create snapshot.");
    rocks->snapshot = rocksdb_create_snapshot(rocks->db);
    atomicIncr(server.inflight_snapshot, 1);
    return C_OK;
}

int readCheckpointDirFromPipe(int pipe) {
    ssize_t nread = 0, pos = 0;
    char checkpoint_dir[ROCKS_DIR_MAX_LEN] = {0};
    while ((nread = read(pipe, checkpoint_dir + pos, sizeof(checkpoint_dir) - pos)) > 0) {
        pos += nread;
    }

    if (nread < 0) {
        serverLog(LL_WARNING, "[rocks] read checkpoint dir from pipe fail: %s", strerror(errno));
        return C_ERR;
    } else if (0 == strlen(checkpoint_dir)) {
        serverLog(LL_WARNING, "[rocks] read checkpoint dir from pipe empty.");
        return C_ERR;
    } else {
        server.rocks->rdb_checkpoint_dir = sdsnew(checkpoint_dir);
        return C_OK;
    }
}

int rmdirRecursive(const char *path) {
	struct dirent *p;
	DIR *d = opendir(path);
	size_t path_len = strlen(path);
	int r = 0;

	if (d == NULL) return -1;

	while (!r && (p=readdir(d))) {
		int r2 = -1;
		char *buf;
		size_t len;
		struct stat statbuf;

		/* Skip the names "." and ".." as we don't want to recurse on them. */
		if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, ".."))
			continue;

		len = path_len + strlen(p->d_name) + 2; 
		buf = zmalloc(len);

		snprintf(buf, len, "%s/%s", path, p->d_name);
		if (!stat(buf, &statbuf)) {
			if (S_ISDIR(statbuf.st_mode))
				r2 = rmdirRecursive(buf);
			else
				r2 = unlink(buf);
		}

		zfree(buf);
		r = r2;
	}
	closedir(d);

	if (!r) r = rmdir(path);

	return r;
}

int rocksFlushDB(int dbid) {
    int startdb, enddb, retval = 0, i;
    sds startkey = NULL, endkey = NULL;
    char *err = NULL;

    serverAssert(dbid >= -1 && dbid < server.dbnum);

    asyncCompleteQueueDrain(-1);

    if (dbid == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        startdb = enddb = dbid;
    }

    startkey = rocksEncodeDbRangeStartKey(startdb);
    endkey = rocksEncodeDbRangeEndKey(enddb);

    for (i = 0; i < CF_COUNT; i++) {
        rocksdb_delete_range_cf(server.rocks->db,server.rocks->wopts,
                server.rocks->cf_handles[i],startkey,sdslen(startkey),
                endkey,sdslen(endkey), &err);
        if (err != NULL) {
            retval = -1;
            serverLog(LL_WARNING,
                    "[ROCKS] flush db(%d) by delete_range fail:%s",dbid,err);
        }
    }
    serverLog(LL_WARNING, "[ROCKS] flushdb %d by delete_range [%s, %s): %s.",
        dbid, startkey, endkey, retval == 0 ? "ok": "fail");

    if (startkey) sdsfree(startkey);
    if (endkey) sdsfree(endkey);

    return retval;
}

static int parseCfNames(const char *cfnames,
        rocksdb_column_family_handle_t *handles[CF_COUNT],
        const char *names[CF_COUNT+1]) {
    int i = 0, ret = 0;
    char *ptr, *saveptr, *dupnames = NULL;

    if (cfnames == NULL || strlen(cfnames) == 0) {
        memcpy(handles,server.rocks->cf_handles,sizeof(void*)*CF_COUNT);
        if (names) memcpy(names,swap_cf_names,sizeof(void*)*CF_COUNT);
        goto end;
    }

    dupnames = sdsnew(cfnames);
    for (ptr = strtok_r(dupnames,", ",&saveptr);
            ptr != NULL && i < CF_COUNT;
            ptr = strtok_r(NULL,", ",&saveptr)) {
        if (!strcasecmp(ptr,data_cf_name)) {
            handles[i] = server.rocks->cf_handles[DATA_CF];
            if (names) names[i] = data_cf_name;
        } else if (!strcasecmp(ptr,meta_cf_name)) {
            handles[i] = server.rocks->cf_handles[META_CF];
            if (names) names[i] = meta_cf_name;
        } else if (!strcasecmp(ptr,score_cf_name)) {
            handles[i] = server.rocks->cf_handles[SCORE_CF];
            if (names) names[i] = score_cf_name;
        } else {
            ret = -1;
            goto end;
        }

        i++;
    }

end:
    if (dupnames) sdsfree(dupnames);
    return ret;
}

int rocksdbPropertyInt(const char *cfnames, const char *propname,
        uint64_t *out_val) {
    int ret = 0, i = 0;
    uint64_t sum = 0, val = 0;
    rocksdb_column_family_handle_t *handle, *cfhandles[CF_COUNT+1] = {NULL};

    if ((ret = parseCfNames(cfnames,cfhandles,NULL)))
        return ret;

    while ((handle = cfhandles[i++])) {
        if ((ret = rocksdb_property_int_cf(server.rocks->db,handle,
                        propname,&val))) {
            break;
        }
        sum += val;
    }
    *out_val = sum;

    return ret;
}

sds rocksdbPropertyValue(const char *cfnames, const char *propname) {
    int ret = 0, i = 0;
    sds result = NULL;
    char *tmp;
    const char *names[CF_COUNT+1] = {NULL};
    rocksdb_column_family_handle_t *handle, *handles[CF_COUNT+1] = {NULL};

    if ((ret = parseCfNames(cfnames,handles,names))) {
        goto end;
    }

    result = sdsempty();
    while ((handle = handles[i])) {
        if ((tmp = rocksdb_property_value_cf(server.rocks->db,handle,
                        propname))) {
            result = sdscat(result,"[");
            result = sdscat(result,names[i]);
            result = sdscat(result,"]:");
            result = sdscat(result,tmp);
            result = sdscat(result,"\r\n");
            zlibc_free(tmp);
        }
        i++;
    }

end:
    return result;
}

struct rocksdbMemOverhead *rocksGetMemoryOverhead() {
    rocksdbMemOverhead *mh;
    size_t total = 0, mem;

    if (server.rocks->db == NULL)
        return NULL;

    mh = zmalloc(sizeof(struct rocksdbMemOverhead));

    if (!rocksdbPropertyInt(NULL, "rocksdb.cur-size-all-mem-tables", &mem)) {
        mh->memtable = mem;
        total += mem;
    } else {
        mh->memtable = -1;
    }

    if (!rocksdbPropertyInt(NULL, "rocksdb.block-cache-usage", &mem)) {
        mh->block_cache = mem;
        total += mem;
    } else {
        mh->block_cache = -1;
    }

    if (!rocksdbPropertyInt(NULL, "rocksdb.estimate-table-readers-mem", &mem)) {
        mh->index_and_filter = mem;
        total += mem;
    } else {
        mh->index_and_filter = -1;
    }

    if (!rocksdbPropertyInt(NULL, "rocksdb.block-cache-pinned-usage", &mem)) {
        mh->pinned_blocks = mem;
        total += mem;
    } else {
        mh->pinned_blocks = -1;
    }

    mh->total = total;
    return mh;
}

void rocksFreeMemoryOverhead(struct rocksdbMemOverhead *mh) {
    if (mh) zfree(mh);
}


char* nextUnSpace(char* start, int size) {
    if (start == NULL) return NULL;
    int index = 0;
    while(index < size) {
        if(strncmp(start + index, " ", 1) != 0) {
            return start + index;
        } 
        index++;
    }
    return NULL;
}

char* nextSpace(char* start, int n) {
    if (start == NULL) return NULL;
    char* result = start;
    while (n > 0) {
        n--;
        result = strstr(result, " ");
        if (result == NULL) return NULL;
        if(n != 0) result = result + 1;
    }
    return result;    
}

#define readNextSds($v)  do {                           \
    start = nextUnSpace(end, line_end - end);\
    end = nextSpace(start, 1);\
    if (start != NULL && end != NULL) { \
        $v = sdsnewlen(start, end - start);                         \
    }\
} while (0) 

#define default(a, b) (a == NULL? b: a)

sds compactLevelInfo(sds info, int level , char* rocksdb_stats) {
    sds totalFiles = NULL;
    sds compacting_files = NULL;
    double size = 0;
    sds score = NULL;
    sds read = NULL;
    sds rn = NULL;
    sds rnp1 = NULL;
    sds write = NULL;
    sds wnew = NULL;
    sds moved = NULL;
    sds w_amp = NULL;
    sds rd = NULL;
    sds wr = NULL;
    sds comp_sec = NULL;
    sds comp_merge_cpu = NULL;
    sds comp_cnt = NULL;
    sds avg_sec = NULL;
    sds keyin = NULL;
    sds keydrop = NULL;
    /**
     * @brief 
     * @example
     *      Level    Files   Size     Score Read(GB)  Rn(GB) Rnp1(GB) Write(GB) Wnew(GB) Moved(GB) W-Amp Rd(MB/s) Wr(MB/s) Comp(sec) CompMergeCPU(sec) Comp(cnt) Avg(sec) KeyIn KeyDrop Rblob(GB) Wblob(GB)
            ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
            L0      0/0    0.00 KB   0.0     36.0     0.0     36.0     110.0     74.0       0.0   1.5     53.8    164.6    684.42            665.60       904    0.757     19M    73K       0.0       0.0
     * 
     */
    if (rocksdb_stats == NULL) {
        goto end;
    }
    char find_buf[256];
    sprintf(find_buf, "  L%d", level);
    char* start = strstr(rocksdb_stats, find_buf);
    if (start == NULL) {
        goto end;
    }
    char* end = start + strlen(find_buf);
    char* line_end = strstr(end, "\n");
    

    //Files
    start = nextUnSpace(end, line_end - end);
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        char* split_index = strstr(start, "/");
        if((end - split_index) > 0) {
            totalFiles = sdsnewlen(start, split_index - start);
            compacting_files = sdsnewlen(split_index + 1, end - split_index - 1);
        }
    }
    //Size
    start = nextUnSpace(end, line_end - end);
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        string2d(start, end - start, &size);
    }

    start = end + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        // unit GB
        if(strncmp(start, "B", 1) == 0) {
            size = size / (1024 * 1024 * 1024);
        } if (strncmp(start, "KB", 2) == 0) {
            size = size / (1024 * 1024);
        } else if (strncmp(start, "MB", 2) == 0) {
            size = size / 1024;
        } else if (strncmp(start, "GB", 2) == 0) {

        }
    }

    //Score
    readNextSds(score);
    //Read(GB)  
    readNextSds(read);
    //Rn(GB) 
    readNextSds(rn);
    //Rnp1(GB)
    readNextSds(rnp1);
    //Write(GB) 
    readNextSds(write);
    //Wnew(GB) 
    readNextSds(wnew);
    //Moved(GB) 
    readNextSds(moved);
    //W-Amp 
    readNextSds(w_amp);
    //Rd(MB/s) 
    readNextSds(rd);
    //Wr(MB/s) 
    readNextSds(wr);
    //Comp(sec) 
    readNextSds(comp_sec);
    //CompMergeCPU(sec) 
    readNextSds(comp_merge_cpu);
    //Comp(cnt) 
    readNextSds(comp_cnt);
    //Avg(sec) 
    readNextSds(avg_sec);
    //KeyIn 
    readNextSds(keyin);
    //KeyDrop 
    readNextSds(keydrop);
    //Rblob(GB) Wblob(GB)


    end:
    info = sdscatprintf(info,
        "# Rocksdb.L%d\r\n"
        "TotalFiles:%s\r\n"
        "CompactingFiles:%s\r\n"
        "Size(GB):%.2f\r\n"
        "Score:%s\r\n"
        "Read(GB):%s\r\n"
        "Rn(GB):%s\r\n"
        "Rnp1(GB):%s\r\n"
        "Write(GB):%s\r\n"
        "Wnew(GB):%s\r\n"
        "Moved(GB):%s\r\n"
        "W-Amp:%s\r\n"
        "Rd(MB/s):%s\r\n"
        "Wr(MB/s):%s\r\n"
        "Comp(sec):%s\r\n"
        "CompMergeCPU(sec):%s\r\n"
        "Comp(cnt):%s\r\n"
        "Avg(sec):%s\r\n"
        "KeyIn(K):%s\r\n"
        "KeyDrop(K):%s\r\n",
        level,
        default(totalFiles, "0"),
        default(compacting_files, "0"),
        size,
        default(score,"0"),
        default(read, "0"),
        default(rn, "0"),
        default(rnp1, "0"),
        default(write, "0"),
        default(wnew, "0"),
        default(moved, "0"),
        default(w_amp, "0"),
        default(rd, "0"),
        default(wr, "0"),
        default(comp_sec, "0"),
        default(comp_merge_cpu, "0"),
        default(comp_cnt, "0"),
        default(avg_sec, "0"),
        default(keyin, "0"),
        default(keydrop, "0"));
    sdsfree(totalFiles);
    sdsfree(compacting_files);
    sdsfree(score);
    sdsfree(read);
    sdsfree(rn);
    sdsfree(rnp1);
    sdsfree(write);
    sdsfree(wnew);
    sdsfree(moved);
    sdsfree(w_amp);
    sdsfree(rd);
    sdsfree(wr);
    sdsfree(comp_sec);
    sdsfree(comp_merge_cpu);
    sdsfree(comp_cnt);
    sdsfree(avg_sec);
    sdsfree(keyin);
    sdsfree(keydrop);
    return info;
}

sds compactLevelsInfo(sds info, char* rocksdb_stats) {
    for(int i = 0; i < 2; i++) {
        info = compactLevelInfo(info, i, rocksdb_stats);
    }
    return info;
}



double str2k(char* str, int size) {
    //G -> k
    char* end;
    double result = 0.0;
    end = strstr(str, "G");
    if ( end != NULL && (end - str) < size) {
        if (string2d(str, end - str, &result) == 1) {
            result *= 1000000;
            return result;
        }  
    }

    //M -> k 
    end = strstr(str, "M");
    if ( end != NULL && (end - str) < size) {
        if (string2d(str, end - str, &result) == 1) {
            result *= 1000;
            return result;
        }
    }
    // k 
    end = strstr(str, "K");
    if ( end != NULL && (end - str) < size) {
        if (string2d(str, end - str, &result) == 1) {
            return result;
        }
    }
    //
    if (string2d(str, size, &result) == 1) {
        return result/1000;
    }
    return -1.0;
}

sds rocksdbStatsInfo(sds info, char* type, char* rocksdb_stats) {
    double writes_num_k = 0;
    double writes_keys_k = 0;
    double writes_commit_group = 0;
    sds writes_per_commit_group = NULL;
    sds writes_ingest_size = NULL;
    sds writes_ingest_size_unit = NULL;
    sds writes_ingest_speed = NULL;

    double wal_writes_k = 0;
    sds wal_syncs = NULL;
    sds wal_writes_per_sync = NULL;
    sds wal_writen_size_unit = NULL;
    sds wal_writen_size = NULL;
    sds wal_writen_speed = NULL;

    sds stall_time = NULL;
    sds stall_percent = NULL;
    //updateCaseFirst
    char Type[256] = {0};
    memcpy(&Type, type, strlen(type));
    Type[0] = Type[0] - 32;
    Type[strlen(type)] = '\0';
    
    
    /**
     * @brief writes
     * @example 
            Cumulative writes: 285M writes, 556M keys, 283M commit groups, 1.0 writes per commit group, ingest: 83.45 GB, 0.29 MB/s
     * 
     */

    //find writes line frist address
    if (rocksdb_stats == NULL) {
        goto end;
    }
    char find_buf[512];
    snprintf(find_buf, sizeof(find_buf)-1, "%s writes: ", Type);
    char* start = strstr(rocksdb_stats, find_buf);
    if (start == NULL) {
        goto end;
    }

    //writes num
    start = start + strlen(find_buf);
    char* end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        writes_num_k = str2k(start, end - start);
    }
    //writes keys
    start = nextSpace(end + 1, 1) + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        writes_keys_k = str2k(start, end - start);
    }
    //writes commit groups
    start = nextSpace(end + 1, 1) + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        writes_commit_group = str2k(start, end - start);
    }
    //writes per commit group
    start = nextSpace(end + 1, 2) + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        writes_per_commit_group = sdsnewlen(start, end - start);
    }
    //writes ingest size
    start = nextSpace(end + 1, 5) + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        writes_ingest_size = sdsnewlen(start, end - start);
    }
    start = end + 1;
    end = nextSpace(start, 1);
    //1 is ','
    if (start != NULL && end != NULL) {
        writes_ingest_size_unit = sdsnewlen(start, end - start - 1);
    }
    //writes ingest speed
    start = end + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        writes_ingest_speed = sdsnewlen(start, end - start);
    }
    
    /**
     * @brief wal
     * @example 
     *      Cumulative WAL: 0 writes, 0 syncs, 0.00 writes per sync, written: 0.00 GB, 0.00 MB/s
     */
    //find writes line frist address
    snprintf(find_buf, sizeof(find_buf)-1, "%s WAL: ", Type);
    start = strstr(rocksdb_stats, find_buf);
    if (start != NULL) {
        start = start + strlen(find_buf);
    } 
    
    //wal_writes
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        wal_writes_k = str2k(start, end - start);
    }

    //wal syncs
    start = nextSpace(end + 1, 1) + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        wal_syncs = sdsnewlen(start, end - start);
    }

    //wal writes per sync
    start = nextSpace(end + 1, 1) + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        wal_writes_per_sync = sdsnewlen(start, end - start);
    }
    //wal writeen 
    start = nextSpace(end + 1, 4) + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        wal_writen_size = sdsnewlen(start, end - start);
    }
    start = end + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        //1 is ','
        wal_writen_size_unit = sdsnewlen(start, end - start - 1);
    }
    //wal writeen speed 
    start = end + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        wal_writen_speed = sdsnewlen(start, end - start);
    }
    /**
     * @brief stall
     * @example
     *      Cumulative stall: 00:00:0.000 H:M:S, 0.0 percent
     */
    //find writes line frist address
    snprintf(find_buf, sizeof(find_buf)-1, "%s stall: ", Type);
    start = strstr(rocksdb_stats, find_buf);
    if (start != NULL) {
        start = start + strlen(find_buf);
    }

    //stall time 
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        stall_time = sdsnewlen(start, end - start);
    }
    //stall percent
    start = nextSpace(end + 1, 1) + 1;
    end = nextSpace(start, 1);
    if (start != NULL && end != NULL) {
        stall_percent = sdsnewlen(start, end - start);
    }
end:
    info = sdscatprintf(info, 
        "# Rocksdb.%s\r\n"
        "%s_writes_num(K):%.3f\r\n"
        "%s_writes_keys(K):%.3f\r\n"
        "%s_writes_commit_group(K):%.3f\r\n"
        "%s_writes_per_commit_group:%s\r\n"
        "%s_writes_ingest_size(%s):%s\r\n"
        "%s_writes_ingest_speed(MB/s):%s\r\n"
        "%s_wal_writes(K):%.3f\r\n"
        "%s_wal_syncs:%s\r\n"
        "%s_wal_writes_per_sync:%s\r\n"
        "%s_wal_writen_size(%s):%s\r\n"
        "%s_wal_writen_speed(MB/s):%s\r\n"
        "%s_stall_time:%s\r\n"
        "%s_stall_percent:%s\r\n",
        Type,
        type, writes_num_k,
        type, writes_keys_k,
        type, writes_commit_group,
        type, writes_per_commit_group,
        type, writes_ingest_size_unit, writes_ingest_size,
        type, writes_ingest_speed,
        type, wal_writes_k,
        type, wal_syncs,
        type, wal_writes_per_sync,
        type, wal_writen_size_unit, wal_writen_size,
        type, wal_writen_speed,
        type, stall_time,
        type, stall_percent
    ); 
    sdsfree(writes_per_commit_group);
    sdsfree(writes_ingest_size_unit);
    sdsfree(writes_ingest_size);
    sdsfree(writes_ingest_speed);
    sdsfree(wal_syncs);
    sdsfree(wal_writes_per_sync);
    sdsfree(wal_writen_size_unit);
    sdsfree(wal_writen_size);
    sdsfree(wal_writen_speed);
    sdsfree(stall_time);
    sdsfree(stall_percent);
    return info;
}


sds cumulativeInfo(sds info, char* rocksdb_stats) {
    return rocksdbStatsInfo(info, "cumulative", rocksdb_stats);
}

sds intervalInfo(sds info, char* rocksdb_stats) {
    return rocksdbStatsInfo(info, "interval", rocksdb_stats);
}


static uint64_t rocksdbUsedDbSize(rocksdb_t *db) {
    char *err = NULL;
    uint64_t used_db_size = 0, total_used_db_size = 0;
    const char *begin_key = "\x0", *end_key = "\xff";
    const size_t begin_key_len = 1, end_key_len = 1;

    for (int i = 0; i < CF_COUNT; i++) {
        rocksdb_column_family_handle_t *handle = server.rocks->cf_handles[i];
        if (handle == NULL) continue;
        rocksdb_approximate_sizes_cf(db,handle,1,&begin_key,&begin_key_len,
                &end_key,&end_key_len,&used_db_size,&err);
        if (err != NULL) {
            serverLog(LL_WARNING, "rocksdb_approximate_sizes_cf failed: %s",err);
            continue;
        }
        total_used_db_size += used_db_size;
    }

    return total_used_db_size;
}

sds genSwapStorageInfoString(sds info) {
	size_t swap_used_db_size = 0, swap_max_db_size = 0,
           swap_disk_capacity = 0, swap_used_disk_size = 0;
	float swap_used_db_percent = 0, swap_used_disk_percent = 0;
	rocksdb_t *db = server.rocks->db;
	struct statvfs stat;

	if (db) {
        swap_used_db_size = rocksdbUsedDbSize(db);
		swap_max_db_size = server.swap_max_db_size;
		if (swap_max_db_size) swap_used_db_percent = (float)(swap_used_db_size) * 100/swap_max_db_size;
	}

	if (statvfs(ROCKS_DATA, &stat) == 0) {
		swap_disk_capacity = stat.f_blocks * stat.f_frsize;
		swap_used_disk_size = (stat.f_blocks - stat.f_bavail) * stat.f_frsize;
		if (swap_disk_capacity) swap_used_disk_percent = (float)swap_used_disk_size * 100 / swap_disk_capacity;
	}

	info = sdscatprintf(info,
			"swap_used_db_size:%lu\r\n"
			"swap_max_db_size:%lu\r\n"
			"swap_used_db_percent:%0.2f%%\r\n"
			"swap_used_disk_size:%lu\r\n"
			"swap_disk_capacity:%lu\r\n"
			"swap_used_disk_percent:%0.2f%%\r\n"
            "swap_error_count:%ld\r\n",
			swap_used_db_size,
			swap_max_db_size,
			swap_used_db_percent,
			swap_used_disk_size,
			swap_disk_capacity,
			swap_used_disk_percent,
            server.swap_error_count);

    return info;
}

sds genRocksdbInfoString(sds info) {
	size_t sequence = 0;
	rocksdb_t *db = server.rocks->db;

	if (db) sequence = rocksdb_get_latest_sequence_number(db);
	info = sdscatprintf(info,"rocksdb_sequence:%lu\r\n",sequence);

    char* rocksdb_stats = server.rocks->rocksdb_stats_cache? server.rocks->rocksdb_stats_cache[DATA_CF]: NULL;
    info = compactLevelsInfo(info, rocksdb_stats);
    info = cumulativeInfo(info, rocksdb_stats);
    info = intervalInfo(info, rocksdb_stats);

	return info;
}

sds infoCfStats(int cf, sds info) {
    if (server.rocks->rocksdb_stats_cache) {
        info = sdscatfmt(info, "=================== %s rocksdb.stats ===================\n", swap_cf_names[cf]);
        info = sdscat(info, server.rocks->rocksdb_stats_cache[cf]);
    }
    return info;
}

sds genRocksdbStatsString(sds section, sds info) {
    if (sdslen(section) == rocksdb_stats_section_len) {
        info = infoCfStats(DATA_CF, info);
        return info;
    }
    int count = 0;
    sds* section_splits = sdssplitlen(section + rocksdb_stats_section_len + 1, sdslen(section) - rocksdb_stats_section_len - 1, ".", 1, &count); 
    if (count == 0) {
        info = infoCfStats(DATA_CF, info);
    }
    int handled_cf[CF_COUNT] = {0,0,0};
    
    for(int i = 0; i < count; i++) {
        sds type = section_splits[i];
        int handled = 0;
        for (int j = 0; j < CF_COUNT; j++) {
            if(strcasecmp(type, swap_cf_names[j]) == 0) {
                if (handled_cf[j]) {continue;}
                info = infoCfStats(j, info);
                handled_cf[j] = 1;
                handled = 1;
            }
        }
        if (handled == 0) {
            //default
            if (handled_cf[DATA_CF]) {continue;}
            info = infoCfStats(DATA_CF, info);
            handled_cf[DATA_CF] = 1;
        }
    }
    sdsfreesplitres(section_splits, count);
    return info;
}

#define ROCKSDB_DISK_USED_UPDATE_PERIOD 60
#define ROCKSDB_DISK_HEALTH_DETECT_PERIOD 1

void rocksCron() {
    static long long rocks_cron_loops = 0;
    char path[ROCKS_DIR_MAX_LEN] = {0};

    if (rocks_cron_loops % ROCKSDB_DISK_USED_UPDATE_PERIOD == 0) {
        uint64_t property_int = 0;
        if (!rocksdb_property_int(server.rocks->db,
                    "rocksdb.total-sst-files-size", &property_int)) {
            server.rocksdb_disk_used = property_int;
        }
        if (server.swap_max_db_size && server.rocksdb_disk_used > server.swap_max_db_size) {
            serverLog(LL_WARNING, "Rocksdb disk usage exceeds swap_max_db_size %lld > %lld.",
                    server.rocksdb_disk_used, server.swap_max_db_size);
        }
    }

    if (rocks_cron_loops % ROCKSDB_DISK_HEALTH_DETECT_PERIOD == 0) {
        snprintf(path,ROCKS_DIR_MAX_LEN,"%s/%s",
                ROCKS_DATA,ROCKS_DISK_HEALTH_DETECT_FILE);
        int disk_error = 0;
        FILE *fp = fopen(path,"w");
        if (fp == NULL) disk_error = 1;
        if (!disk_error && fprintf(fp,"%lld",server.mstime) < 0) disk_error = 1;
        if (!disk_error && fflush(fp)) disk_error = 1;
        if (disk_error) {
            if (!server.rocksdb_disk_error) {
                server.rocksdb_disk_error = 1;
                server.rocksdb_disk_error_since = server.mstime;
                serverLog(LL_WARNING,"Detected rocksdb disk failed: %s, %s",
                        path, strerror(errno));
            }
        } else {
            if (server.rocksdb_disk_error) {
                server.rocksdb_disk_error = 0;
                server.rocksdb_disk_error_since = 0;
                serverLog(LL_WARNING,"Detected rocksdb disk recovered.");
            }
        }
        if (fp) fclose(fp);
    }

    int collect_interval_second = server.swap_rocksdb_stats_collect_interval_ms/1000;
    if (collect_interval_second <= 0) collect_interval_second = 1;
    if (rocks_cron_loops % collect_interval_second == 0) {
        submitUtilTask(GET_ROCKSDB_STATS_TASK, NULL, NULL);
    }

    rocks_cron_loops++;
}

char *rocksdbVersion(void) {
    return ROCKSDB_VERSION;
}

