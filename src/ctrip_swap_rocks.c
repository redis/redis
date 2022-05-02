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
#include <stddef.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* unsigned long rocksPendingIOs() { */
    /* int i; */
    /* unsigned long pending = 0; */
    /* for (i = 0; i < server.rocks->threads_num; i++) { */
        /* RIOThread *rt = &server.rocks->threads[i]; */
        /* pthread_mutex_lock(&rt->lock); */
        /* pending += listLength(rt->pending_rios); */
        /* pthread_mutex_unlock(&rt->lock); */
    /* } */
    /* return pending; */
/* } */

/* static int rocksInitCompleteQueue(rocks *rocks) { */
    /* int fds[2]; */
    /* char anetErr[ANET_ERR_LEN]; */
    /* RIOCompleteQueue *cq = &rocks->CQ; */

    /* if (pipe(fds)) { */
        /* perror("Can't create notify pipe"); */
        /* return -1; */
    /* } */

    /* cq->notify_recv_fd = fds[0]; */
    /* cq->notify_send_fd = fds[1]; */

    /* pthread_mutex_init(&cq->lock, NULL); */

    /* cq->complete_queue = listCreate(); */

    /* if (anetNonBlock(anetErr, cq->notify_recv_fd) != ANET_OK) { */
        /* serverLog(LL_WARNING, */
                /* "Fatal: set notify_recv_fd non-blocking failed: %s", */
                /* anetErr); */
        /* return -1; */
    /* } */

    /* if (anetNonBlock(anetErr, cq->notify_send_fd) != ANET_OK) { */
        /* serverLog(LL_WARNING, */
                /* "Fatal: set notify_recv_fd non-blocking failed: %s", */
                /* anetErr); */
        /* return -1; */
    /* } */

    /* if (aeCreateFileEvent(server.el, cq->notify_recv_fd, */
                /* AE_READABLE, RIOFinished, cq) == AE_ERR) { */
        /* serverLog(LL_WARNING,"Fatal: create notify recv event failed: %s", */
                /* strerror(errno)); */
        /* return -1; */
    /* } */

    /* return 0; */
/* } */

/* static void rocksDeinitCompleteQueue(rocks *rocks) { */
    /* RIOCompleteQueue *cq = &rocks->CQ; */
    /* close(cq->notify_recv_fd); */
    /* close(cq->notify_send_fd); */
    /* pthread_mutex_destroy(&cq->lock); */
    /* listRelease(cq->complete_queue); */
/* } */

static int rocksInitDB(rocks *rocks) {
    char *err = NULL, dir[ROCKS_DIR_MAX_LEN];
    rocksdb_cache_t *block_cache;
    rocks->rocksdb_snapshot = NULL;
    rocks->rocksdb_opts = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(rocks->rocksdb_opts, 1); 
    rocksdb_options_enable_statistics(rocks->rocksdb_opts);
    rocksdb_options_set_stats_dump_period_sec(rocks->rocksdb_opts, 60);
    //rocksdb_options_set_max_write_buffer_number(rocks->rocksdb_opts, 6);
    rocksdb_options_set_max_bytes_for_level_base(rocks->rocksdb_opts, 512*1024*1024); 
    struct rocksdb_block_based_table_options_t *block_opts = rocksdb_block_based_options_create();
    rocksdb_block_based_options_set_block_size(block_opts, 8192);
    block_cache = rocksdb_cache_create_lru(1*1024*1024);
    rocks->block_cache = block_cache;
    rocksdb_block_based_options_set_block_cache(block_opts, block_cache);
    rocksdb_block_based_options_set_cache_index_and_filter_blocks(block_opts, 0);
    rocksdb_options_set_block_based_table_factory(rocks->rocksdb_opts, block_opts);
    rocks->block_opts = block_opts;

    rocksdb_options_optimize_for_point_lookup(rocks->rocksdb_opts, 1);
    rocksdb_options_optimize_level_style_compaction(rocks->rocksdb_opts, 256*1024*1024);
    rocksdb_options_set_max_background_compactions(rocks->rocksdb_opts, 4); /* default 1 */
    rocksdb_options_compaction_readahead_size(rocks->rocksdb_opts, 2*1024*1024); /* default 0 */
    rocksdb_options_set_optimize_filters_for_hits(rocks->rocksdb_opts, 1); /* default false */
	rocksdb_options_set_max_write_buffer_number(rocks->rocksdb_opts, 64);

    rocks->rocksdb_ropts = rocksdb_readoptions_create();
    rocksdb_readoptions_set_verify_checksums(rocks->rocksdb_ropts, 0);
    rocksdb_readoptions_set_fill_cache(rocks->rocksdb_ropts, 0);

    rocks->rocksdb_wopts = rocksdb_writeoptions_create();
    rocksdb_writeoptions_disable_WAL(rocks->rocksdb_wopts, 1);

    struct stat statbuf;
    if (!stat(ROCKS_DATA, &statbuf) && S_ISDIR(statbuf.st_mode)) {
        /* "data.rocks" folder already exists, no need to create */
    } else if (mkdir(ROCKS_DATA, 0755)) {
        serverLog(LL_WARNING, "[ROCKS] mkdir %s failed: %s",
                ROCKS_DATA, strerror(errno));
        return -1;
    }

    snprintf(dir, ROCKS_DIR_MAX_LEN, "%s/%d", ROCKS_DATA, rocks->rocksdb_epoch);
    rocks->rocksdb = rocksdb_open(rocks->rocksdb_opts, dir, &err);
    if (err != NULL) {
        serverLog(LL_WARNING, "[ROCKS] rocksdb open failed: %s", err);
        return -1;
    }

    serverLog(LL_NOTICE, "[ROCKS] opened rocks data in (%s).", dir);

    return 0;
}

static void rocksDeinitDB(rocks *rocks) {
    rocksdb_cache_destroy(rocks->block_cache);
    rocksdb_block_based_options_destroy(rocks->block_opts);
    rocksdb_options_destroy(rocks->rocksdb_opts);
    rocksdb_writeoptions_destroy(rocks->rocksdb_wopts);
    rocksdb_readoptions_destroy(rocks->rocksdb_ropts);
    rocksdb_close(rocks->rocksdb);
}

struct rocks *rocksCreate() {
    rocks *rocks = zmalloc(sizeof(struct rocks));
    rocks->rocksdb_epoch = 0;
    if (rocksInitDB(rocks)) goto err;
    if (asyncCompleteQueueInit(rocks)) goto err;
    if (rocksInitThreads(rocks)) goto err;
    return rocks;
err:
    if (rocks != NULL) zfree(rocks);
    return NULL;
}

void rocksCreateSnapshot(rocks *rocks) {
    if (rocks->rocksdb_snapshot) {
        serverLog(LL_WARNING, "[rocks] release snapshot before create.");
        rocksdb_release_snapshot(rocks->rocksdb, rocks->rocksdb_snapshot);
    }
    rocks->rocksdb_snapshot = rocksdb_create_snapshot(rocks->rocksdb);
    serverLog(LL_NOTICE, "[rocks] create rocksdb snapshot ok.");
}

void rocksUseSnapshot(rocks *rocks) {
    if (rocks->rocksdb_snapshot) {
        rocksdb_readoptions_set_snapshot(rocks->rocksdb_ropts, rocks->rocksdb_snapshot);
        serverLog(LL_NOTICE, "[rocks] use snapshot read ok.");
    } else {
        serverLog(LL_WARNING, "[rocks] use snapshot read failed: snapshot not exists.");
    }
}

void rocksReleaseSnapshot(rocks *rocks) {
    if (rocks->rocksdb_snapshot) {
        serverLog(LL_NOTICE, "[rocks] relase snapshot ok.");
        rocksdb_release_snapshot(rocks->rocksdb, rocks->rocksdb_snapshot);
        rocks->rocksdb_snapshot = NULL;
    }
}

void rocksDestroy(rocks *rocks) {
    rocksDeinitThreads(rocks);
    asyncCompleteQueueDeinit(&rocks->CQ);
    rocksDeinitDB(rocks);
    zfree(rocks);
}

static int rmdirRecursive(const char *path) {
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

int rocksFlushAll() {
    char odir[ROCKS_DIR_MAX_LEN];

    snprintf(odir, ROCKS_DIR_MAX_LEN, "%s/%d", ROCKS_DATA, server.rocks->rocksdb_epoch);
    asyncCompleteQueueDrain(server.rocks, -1);
    rocksDeinitDB(server.rocks);
    server.rocks->rocksdb_epoch++;
    if (rocksInitDB(server.rocks)) {
        serverLog(LL_WARNING, "[ROCKS] init new rocksdb failed, trying to resume old one.");
        server.rocks->rocksdb_epoch--;
        if (rocksInitDB(server.rocks)) {
            serverLog(LL_WARNING, "[ROCKS] resume old one failed, oops.");
        } else {
            serverLog(LL_WARNING, "[ROCKS] resume old one success.");
        }
        return -1;
    }
    rmdirRecursive(odir);
    serverLog(LL_NOTICE, "[ROCKS] remove rocks data in (%s).", odir);
    return 0;
}

rocksdb_t *rocksGetDb(rocks *rocks) {
    return rocks->rocksdb;
}

void rocksCron(void) {
    uint64_t property_int = 0;
    if (!rocksdb_property_int(server.rocks->rocksdb,
                "rocksdb.total-sst-files-size", &property_int)) {
        server.rocksdb_disk_used = property_int;
    }
    if (server.maxdisk && server.rocksdb_disk_used > server.maxdisk) {
        serverLog(LL_WARNING, "Rocksdb disk usage exceeds maxdisk %lld > %lld.",
                server.rocksdb_disk_used, server.maxdisk);
    }
}
