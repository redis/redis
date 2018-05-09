/*
 * Copyright (C) 2014 - 2016 Intel Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice(s),
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice(s),
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
 * EVENT SHALL THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <memkind/internal/memkind_hugetlb.h>
#include <memkind/internal/memkind_default.h>
#include <memkind/internal/memkind_arena.h>
#include <memkind/internal/memkind_private.h>
#include <memkind/internal/memkind_log.h>

#include <sys/mman.h>
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << 26)
#endif

#include <stdio.h>
#include <errno.h>
#include <numa.h>
#include <pthread.h>
#include <dirent.h>
#include <jemalloc/jemalloc.h>

MEMKIND_EXPORT const struct memkind_ops MEMKIND_HUGETLB_OPS = {
    .create = memkind_arena_create,
    .destroy = memkind_arena_destroy,
    .malloc = memkind_arena_malloc,
    .calloc = memkind_arena_calloc,
    .posix_memalign = memkind_arena_posix_memalign,
    .realloc = memkind_arena_realloc,
    .free = memkind_default_free,
    .check_available = memkind_hugetlb_check_available_2mb,
    .get_mmap_flags = memkind_hugetlb_get_mmap_flags,
    .get_arena = memkind_thread_get_arena,
    .get_size = memkind_default_get_size,
    .init_once = memkind_hugetlb_init_once
};

static int get_nr_overcommit_hugepages_cached(size_t pagesize, size_t *out);
static int get_nr_hugepages_cached(size_t pagesize, struct bitmask* nodemask, size_t* out);

static int memkind_hugetlb_check_available(struct memkind *kind, size_t huge_size);

MEMKIND_EXPORT int memkind_hugetlb_get_mmap_flags(struct memkind *kind, int *flags)
{
    *flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB;
    return 0;
}

MEMKIND_EXPORT void memkind_hugetlb_init_once(void)
{
    memkind_init(MEMKIND_HUGETLB, false);
}

MEMKIND_EXPORT int memkind_hugetlb_check_available_2mb(struct memkind *kind)
{
    return memkind_hugetlb_check_available(kind, 2097152);
}

MEMKIND_EXPORT int memkind_hugetlb_check_available_1gb(struct memkind *kind)
{
    return memkind_hugetlb_check_available(kind, 1073741824);
}

/* huge_size: the huge page size in bytes */
static int memkind_hugetlb_check_available(struct memkind *kind, size_t huge_size)
{
    int err = 0;
    nodemask_t nodemask;
    struct bitmask nodemask_bm = {NUMA_NUM_NODES, nodemask.n};

    /* on x86_64 default huge page size is 2MB */
    if (huge_size == 0) {
        huge_size = 2097152;
    }

    if (kind->ops->get_mbind_nodemask) {
        err = kind->ops->get_mbind_nodemask(kind, nodemask.n, NUMA_NUM_NODES);
    }
    else {
        numa_bitmask_setall(&nodemask_bm);
    }

    size_t nr_persistent_hugepages, nr_overcommit_hugepages;

    err = get_nr_hugepages_cached(huge_size, &nodemask_bm, &nr_persistent_hugepages);
    if(err) {
        return err;
    }

    err = get_nr_overcommit_hugepages_cached(huge_size, &nr_overcommit_hugepages);
    if(err) {
        return err;
    }

    if (!nr_overcommit_hugepages && !nr_persistent_hugepages) {
        log_err("Persistent hugepages and overcommit hugepages are not available.");
        return MEMKIND_ERROR_HUGETLB;
    }

    return err;
}

struct hugepage_size_info {
    size_t size;
    size_t* nr_hugepages_per_node_array;
    size_t  nr_overcommit;
};

struct memkind_hugepages_config_t {
    struct hugepage_size_info **hugepages_info_array;
    int hugepages_info_array_len;
    int err; // 0 if sysfs parsing successful, appropriate memkind_error otherwise
} memkind_hugepages_config;

static pthread_once_t memkind_hugepages_config_once_g = PTHREAD_ONCE_INIT;

static struct hugepage_size_info *allocate_hugepage_size_info()
{
    struct hugepage_size_info *newInfo = jemk_malloc(sizeof(struct hugepage_size_info));
    if(newInfo == NULL) {
        log_err("jemk_malloc() failed.");
        return NULL;
    }

    newInfo->nr_hugepages_per_node_array = jemk_calloc(NUMA_NUM_NODES, sizeof(size_t));
    if(newInfo->nr_hugepages_per_node_array == NULL) {
        jemk_free(newInfo);
        log_err("jemk_calloc() failed.");
        return NULL;
    }

    return newInfo;
}

static size_t get_sysfs_entry_value(const char* entry_path)
{
    int errno_before;
    FILE *fid;
    int num_read;
    size_t value_read, ret = 0;

    errno_before = errno;
    fid = fopen(entry_path, "r");
    if (fid) {
        num_read = fscanf(fid, "%zud", &value_read);
        if(num_read) {
            ret  = value_read;
        }
        fclose(fid);
    }
    else {
        errno = errno_before;
    }
    return ret;
}

// construct hugepage_size_info object and fill it with data for provided pagesize
static void init_hugepage_size_info(size_t pagesize, struct hugepage_size_info *newInfo)
{
    char formatted_path[128];
    const char *nr_path_fmt = "/sys/devices/system/node/node%u/hugepages/hugepages-%zukB/nr_hugepages";
    const char *nr_overcommit_path_fmt = "/sys/kernel/mm/hugepages/hugepages-%zukB/nr_overcommit_hugepages";
    int snprintf_ret = 0;
    size_t node;

    size_t pagesize_kb = pagesize >> 10;

    newInfo->size = pagesize;

    //read overcommit hugepages limit for this pagesize
    snprintf_ret = snprintf(formatted_path, sizeof(formatted_path), nr_overcommit_path_fmt, pagesize_kb);
    if (snprintf_ret > 0 && snprintf_ret < sizeof(formatted_path)) {
        newInfo->nr_overcommit = get_sysfs_entry_value(formatted_path);
        log_info("Overcommit limit for %zu kB hugepages is %zu.", pagesize, newInfo->nr_overcommit);
    }

    //read every node nr_hugepages for this pagesize
    for (node = 0; node < NUMA_NUM_NODES; ++node) {
        snprintf_ret = snprintf(formatted_path, sizeof(formatted_path), nr_path_fmt, node, pagesize_kb);
        if(snprintf_ret > 0 && snprintf_ret < sizeof(formatted_path)) {
            newInfo->nr_hugepages_per_node_array[node] = get_sysfs_entry_value(formatted_path);
            if(node < numa_num_configured_nodes()) {
                log_info("Number of %zu kB hugepages on node %zu equals %zu.", pagesize, node, newInfo->nr_hugepages_per_node_array[node]);
            }
        }
    }
}

// get hugepage size in bytes out of sysfs dir name
static int parse_pagesize_from_sysfs_entry(const char* entry, size_t *out)
{
    size_t pagesize;
    int ret = sscanf(entry, "hugepages-%zukB", &pagesize);

    if(ret == 1) {
        *out = pagesize << 10; //we are using bytes but kernel is using kB
        return 0;
    }

    return -1;
}


static void hugepages_config_init_once()
{
    unsigned j, i = 0;
    size_t pagesize;
    struct hugepage_size_info **hugepages_info_array = NULL;
    struct dirent *dir;
    DIR* hugepages_sysfs = opendir("/sys/kernel/mm/hugepages");
    if(hugepages_sysfs == NULL) {
        memkind_hugepages_config.err = MEMKIND_ERROR_HUGETLB;
        log_err("/sys/kernel/mm/hugepages directory is not available.");
        return;
    }

    unsigned hugepages_info_array_len = 2; //initial size of array
    hugepages_info_array = jemk_malloc(hugepages_info_array_len * sizeof(struct hugepage_size_info *));
    if (hugepages_info_array == NULL) {
        memkind_hugepages_config.err = MEMKIND_ERROR_MALLOC;
        closedir(hugepages_sysfs);
        log_err("jemk_malloc() failed.");
        return;
    }

    while ((dir = readdir(hugepages_sysfs)) != NULL) {
        if(dir->d_type == DT_DIR && parse_pagesize_from_sysfs_entry(dir->d_name, &pagesize) == 0) {
            struct hugepage_size_info *new_hugepage_info = allocate_hugepage_size_info();
            if(new_hugepage_info == NULL) {
                memkind_hugepages_config.err = MEMKIND_ERROR_MALLOC;
                break;
            }

            init_hugepage_size_info(pagesize, new_hugepage_info);

            //there is more hugepage sizes than expected, reallocation of array needed
            if(i == hugepages_info_array_len) {
                hugepages_info_array_len *= 2;
                struct hugepage_size_info **swap_tmp = jemk_realloc(hugepages_info_array, hugepages_info_array_len * sizeof(struct hugepage_size_info *));
                if(swap_tmp == NULL) {
                    jemk_free(new_hugepage_info);
                    memkind_hugepages_config.err = MEMKIND_ERROR_MALLOC;
                    log_err("jemk_realloc() failed.");
                    break;
                }
                hugepages_info_array = swap_tmp;

            }
            hugepages_info_array[i] = new_hugepage_info;
            i++;
        }
    }

    closedir(hugepages_sysfs);

    if(memkind_hugepages_config.err == 0) {
        memkind_hugepages_config.hugepages_info_array = hugepages_info_array;
        memkind_hugepages_config.hugepages_info_array_len = i;
    }
    else {
        for(j=0; j<i; j++) {
            jemk_free(hugepages_info_array[i]);
        }
        jemk_free(hugepages_info_array);
    }

    return;
}

#ifdef __GNUC__
__attribute__((destructor))
#endif
static void destroy_hugepages_per_node()
{
    int i;
    for(i=0; i<memkind_hugepages_config.hugepages_info_array_len; i++) {
        jemk_free(memkind_hugepages_config.hugepages_info_array[i]);
    }
    jemk_free(memkind_hugepages_config.hugepages_info_array);
}

// helper function that find and return hugepage_size_info object for specified pagesize
static struct hugepage_size_info* get_hugepage_info_for_pagesize(size_t pagesize)
{
    int i;

    for(i=0; i<memkind_hugepages_config.hugepages_info_array_len; i++) {
        if(memkind_hugepages_config.hugepages_info_array[i]->size == pagesize) {
            return memkind_hugepages_config.hugepages_info_array[i];
        }
    }
    return NULL;
}

// returns sum of pre-allocated hugepage for specified pagesize and set of nodes
static int get_nr_hugepages_cached(size_t pagesize, struct bitmask* nodemask, size_t *out)
{
    int i;
    size_t nr_hugepages = 0;
    int num_node = numa_num_configured_nodes();
    pthread_once(&memkind_hugepages_config_once_g,
                 hugepages_config_init_once);


    if(memkind_hugepages_config.err != 0) {
        return memkind_hugepages_config.err;
    }

    struct hugepage_size_info* info = get_hugepage_info_for_pagesize(pagesize);
    if(info == NULL) {
        log_err("Unable to allocate hugepages, because info about pre-allocated hugepages is not available.");
        return MEMKIND_ERROR_HUGETLB;
    }

    for(i=0; i<num_node; i++) {
        if(numa_bitmask_isbitset(nodemask, i)) {
            nr_hugepages += info->nr_hugepages_per_node_array[i];
        }
    }

    *out = nr_hugepages;
    return 0;
}

// returns hugepages overcommit limit for specified pagesize
static int get_nr_overcommit_hugepages_cached(size_t pagesize, size_t *out)
{
    pthread_once(&memkind_hugepages_config_once_g,
                 hugepages_config_init_once);

    if(memkind_hugepages_config.err != 0) {
        return memkind_hugepages_config.err;
    }

    struct hugepage_size_info* info = get_hugepage_info_for_pagesize(pagesize);
    if(info == NULL) {
        log_err("Unable to allocate hugepages, because info about overcommit hugepages is not available.");
        return MEMKIND_ERROR_HUGETLB;
    }

    *out = info->nr_overcommit;
    return 0;
}

