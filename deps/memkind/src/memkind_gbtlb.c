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

#include <memkind/internal/memkind_gbtlb.h>
#include <memkind/internal/memkind_hugetlb.h>
#include <memkind/internal/memkind_default.h>
#include <memkind/internal/memkind_hbw.h>
#include <memkind/internal/memkind_private.h>
#include <memkind/internal/memkind_log.h>

#include <numa.h>
#include <numaif.h>
#include <smmintrin.h>
#include <stdio.h>
#include <errno.h>
#include <jemalloc/jemalloc.h>
#include <sys/mman.h>
#include <assert.h>
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << 26)
#endif

MEMKIND_EXPORT const struct memkind_ops MEMKIND_HBW_GBTLB_OPS = {
    .create = memkind_default_create,
    .destroy = memkind_default_destroy,
    .malloc = memkind_gbtlb_malloc,
    .calloc = memkind_gbtlb_calloc,
    .posix_memalign = memkind_gbtlb_posix_memalign,
    .realloc = memkind_gbtlb_realloc,
    .free = memkind_gbtlb_free,
    .check_available = memkind_hbw_gbtlb_check_available,
    .mbind = memkind_default_mbind,
    .get_mmap_flags = memkind_gbtlb_get_mmap_flags,
    .get_mbind_mode = memkind_default_get_mbind_mode,
    .get_mbind_nodemask = memkind_hbw_get_mbind_nodemask,
    .get_size = memkind_default_get_size,
    .check_addr = memkind_gbtlb_check_addr,
    .init_once = memkind_hbw_gbtlb_init_once
};

MEMKIND_EXPORT const struct memkind_ops MEMKIND_HBW_PREFERRED_GBTLB_OPS = {
    .create = memkind_default_create,
    .destroy = memkind_default_destroy,
    .malloc = memkind_gbtlb_malloc,
    .calloc = memkind_gbtlb_calloc,
    .posix_memalign = memkind_gbtlb_posix_memalign,
    .realloc = memkind_gbtlb_realloc,
    .free = memkind_gbtlb_free,
    .check_available = memkind_hbw_gbtlb_check_available,
    .mbind = memkind_default_mbind,
    .get_mmap_flags = memkind_gbtlb_get_mmap_flags,
    .get_mbind_mode = memkind_preferred_get_mbind_mode,
    .get_mbind_nodemask = memkind_hbw_get_mbind_nodemask,
    .get_size = memkind_default_get_size,
    .check_addr = memkind_gbtlb_check_addr,
    .init_once = memkind_hbw_preferred_gbtlb_init_once

};

MEMKIND_EXPORT const struct memkind_ops MEMKIND_GBTLB_OPS = {
    .create = memkind_default_create,
    .destroy = memkind_default_destroy,
    .malloc = memkind_gbtlb_malloc,
    .calloc = memkind_gbtlb_calloc,
    .posix_memalign = memkind_gbtlb_posix_memalign,
    .realloc = memkind_gbtlb_realloc,
    .free = memkind_gbtlb_free,
    .check_available = memkind_hugetlb_check_available_1gb,
    .get_mmap_flags = memkind_gbtlb_get_mmap_flags,
    .get_size = memkind_default_get_size,
    .check_addr = memkind_gbtlb_check_addr,
    .init_once = memkind_gbtlb_init_once
};

#define ONE_GB 1073741824ULL

enum {
    GBTLB_STORE_INSERT,
    GBTLB_STORE_REMOVE,
    GBTLB_STORE_QUERY
};

typedef struct memkind_list_node_s {
    void *ptr;
    void *mmapptr;
    size_t requested_size;
    size_t size;
    struct memkind *kind;
    struct memkind_list_node_s *next;
} memkind_list_node_t;

typedef struct {
    pthread_mutex_t mutex;
    memkind_list_node_t *list;
} memkind_table_node_t;

static int ptr_hash(void *ptr, int table_len);
static int memkind_store(void *ptr, void **mmapptr, struct memkind **kind,
                         size_t *req_size, size_t *size, int mode);
static int memkind_gbtlb_mmap(struct memkind *kind, size_t size,
                              void **result);
static void memkind_gbtlb_ceil_size(size_t *size);

MEMKIND_EXPORT void *memkind_gbtlb_malloc(struct memkind *kind, size_t size)
{
    void *result = NULL;
    int err = 0;
    size_t req_size = size;

    memkind_gbtlb_ceil_size(&size);
    err = memkind_gbtlb_mmap(kind, size, &result);
    if (!err && kind->ops->mbind) {
        err = kind->ops->mbind(kind, result, size);
    }
    if (!err) {
        err = memkind_store(result, &result, &kind,
                            &req_size, &size, GBTLB_STORE_INSERT);
    }
    if (err && result) {
        munmap(result, size);
        result = NULL;
    }
    return result;
}

MEMKIND_EXPORT void *memkind_gbtlb_calloc(struct memkind *kind, size_t num, size_t size)
{
    return kind->ops->malloc(kind, num * size);
}

MEMKIND_EXPORT int memkind_gbtlb_posix_memalign(struct memkind *kind, void **memptr, size_t alignment, size_t size)
{
    int err = 0;
    int do_shift = 0;
    void *mmapptr = NULL;
    size_t req_size = size;

    *memptr = NULL;

    if (alignment > ONE_GB) {
        do_shift = 1;
        size += alignment;
    }
    err = memkind_posix_check_alignment(kind, alignment);
    if (!err) {
        *memptr = memkind_gbtlb_malloc(kind, size);
        if (*memptr == NULL) {
            err = ENOMEM;
        }
    }
    if (!err && do_shift) {
        /* Remove the entry from the store */
        err = memkind_store(*memptr, &mmapptr, &kind,
                            &req_size, &size, GBTLB_STORE_REMOVE) ? ENOMEM : 0;
        if (!err) {
            /* Adjust for alignment */
            *memptr = (void *) ((char *)mmapptr +
                                (size_t)(mmapptr) % alignment);
            /* Store the modified pointer */
            err = memkind_store(*memptr, &mmapptr, &kind,
                                &req_size, &size, GBTLB_STORE_INSERT) ? ENOMEM : 0;
        }
    }
    if (err && mmapptr) {
        munmap(mmapptr, size);
    }
    return err;
}

/*FIXME : Handle scenario when 2MB and 1GB pages are mixed with realloc*/
MEMKIND_EXPORT void *memkind_gbtlb_realloc(struct memkind *kind, void *ptr, size_t size)
{
    void *result = NULL;
    void *mmap_ptr = NULL;
    size_t orig_size, copy_size;
    size_t req_size;
    int err;

    if (ptr != NULL) {
        err = memkind_store(ptr, &mmap_ptr, &kind, &req_size,
                            &orig_size, GBTLB_STORE_QUERY);
        if (!err) {
            /*Optimization when we grow the array
              with Realloc*/
            if ((req_size < orig_size) &&
                (req_size < size) &&
                (req_size + size < orig_size)) {
                /*There is no need to allocate*/
                result = ptr;
            }
            else {
                result = kind->ops->malloc(kind, size);
                if (result != NULL) {
                    /*Tried using mremap : Failed for 1GB Pages: Err - Did not unmap*/
                    /* result = mremap(ptr, orig_size, size, MREMAP_MAYMOVE|flags);*/
                    copy_size = size > orig_size ? orig_size : size;
                    memcpy(result, ptr, copy_size);
                }
                kind->ops->free(kind, ptr);
            }
        }
        else {
            if (result != NULL) {
                kind->ops->free(kind, result);
                result = NULL;
            }
        }
    }
    else {
        result = kind->ops->malloc(kind, size);
    }
    return result;
}

MEMKIND_EXPORT void memkind_gbtlb_free(struct memkind *kind, void *ptr)
{
    int err;
    void *mmapptr = NULL;
    size_t size = 0, req_size = 0;

    err = memkind_store(ptr, &mmapptr, &kind, &req_size,
                        &size, GBTLB_STORE_REMOVE);
    if (!err) {
        munmap(mmapptr, size);
    }
}

MEMKIND_EXPORT int memkind_gbtlb_get_mmap_flags(struct memkind *kind, int *flags)
{
    *flags = MAP_PRIVATE | MAP_HUGETLB | MAP_HUGE_1GB | MAP_ANONYMOUS;
    return 0;
}

MEMKIND_EXPORT int memkind_gbtlb_check_addr(struct memkind *kind, void *addr)
{

    void *mmapptr = NULL;
    size_t size = 0;
    size_t req_size = 0;
    int err = 0;
    struct memkind *ptr_kind;
    int ret = MEMKIND_ERROR_INVALID;

    err = memkind_store(addr, &mmapptr, &ptr_kind, &req_size,
                        &size, GBTLB_STORE_QUERY);

    if ((!err) && (kind == ptr_kind)) {
        ret = 0;
    }

    return ret;
}

static int memkind_store(void *memptr, void **mmapptr, struct memkind **kind,
                         size_t *req_size, size_t *size, int mode)
{
    static int table_len = 0;
    static int is_init = 0;
    static memkind_table_node_t *table = NULL;
    static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
    int err = 0;
    int hash, i;
    memkind_list_node_t *storeptr, *lastptr;

    if (!is_init && *mmapptr == NULL) {
        return -1;
    }

    if (!is_init) {
        if (pthread_mutex_lock(&init_mutex) != 0)
            assert(0 && "failed to acquire mutex");

        if (!is_init) {
            table_len = numa_num_configured_cpus();
            table = jemk_malloc(sizeof(memkind_table_node_t) * table_len);
            if (table == NULL) {
                log_err("jemk_malloc() failed.");
                err = MEMKIND_ERROR_MALLOC;
            }
            else {
                for (i = 0; i < table_len; ++i) {
                    pthread_mutex_init(&table[i].mutex, NULL);
                    table[i].list = NULL;
                }
                is_init = 1;
            }
        }
        pthread_mutex_unlock(&init_mutex);
    }
    if (is_init) {
        hash = ptr_hash(memptr, table_len);
        if (pthread_mutex_lock(&table[hash].mutex) != 0)
            assert(0 && "failed to acquire mutex");

        if (mode == GBTLB_STORE_REMOVE || mode == GBTLB_STORE_QUERY) {
            /*
               memkind_store() call is a query
               GBTLB_STORE_REMOVE -> Query if found remove and
               return the address and size;
               GBTLB_STORE_QUERTY -> Query if found and return;
            */
            storeptr = table[hash].list;
            lastptr = NULL;
            while (storeptr && storeptr->ptr != memptr) {
                lastptr = storeptr;
                storeptr = storeptr->next;
            }
            if (storeptr == NULL) {
                err = MEMKIND_ERROR_RUNTIME;
            }
            if (!err) {
                *mmapptr = storeptr->mmapptr;
                *size = storeptr->size;
                *req_size = storeptr->requested_size;
                *kind = storeptr->kind;
            }
            if (!err && mode == GBTLB_STORE_REMOVE) {
                if (lastptr) {
                    lastptr->next = storeptr->next;
                }
                else {
                    table[hash].list = storeptr->next;
                }
                jemk_free(storeptr);
            }
        }
        else { /* memkind_store() call is a store */
            storeptr = table[hash].list;
            table[hash].list = (memkind_list_node_t*)jemk_malloc(sizeof(memkind_list_node_t));
            table[hash].list->ptr = memptr;
            table[hash].list->mmapptr = *mmapptr;
            table[hash].list->size = *size;
            table[hash].list->requested_size = *req_size;
            table[hash].list->kind = *kind;
            table[hash].list->next = storeptr;
        }
        pthread_mutex_unlock(&table[hash].mutex);
    }
    else {
        err = MEMKIND_ERROR_MALLOC;
        log_err("jemk_malloc() failed.");
    }
    return err;
}


static int ptr_hash(void *ptr, int table_len)
{
    return _mm_crc32_u64(0, (size_t)ptr) % table_len;
}

static void memkind_gbtlb_ceil_size(size_t *size)
{
    *size = *size % ONE_GB ? ((*size / ONE_GB) + 1) * ONE_GB : *size;
}

static int memkind_gbtlb_mmap(struct memkind *kind, size_t size, void **result)
{
    int err = 0;
    int flags;

    *result = NULL;
    if (kind->ops->get_mmap_flags == NULL) {
        log_err("memkind_ops->mmap_flags is NULL.");
        err = MEMKIND_ERROR_BADOPS;
    }
    if (!err) {
        err = kind->ops->get_mmap_flags(kind, &flags);
    }
    if (!err) {
        *result = mmap(NULL, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | flags,
                       -1, 0);
        if (*result == MAP_FAILED) {
            err = MEMKIND_ERROR_MMAP;
            *result = NULL;
        }
    }
    return err;
}

MEMKIND_EXPORT void memkind_hbw_gbtlb_init_once(void)
{
    memkind_register_kind(MEMKIND_HBW_GBTLB);
}

MEMKIND_EXPORT void memkind_hbw_preferred_gbtlb_init_once(void)
{
    memkind_register_kind(MEMKIND_HBW_PREFERRED_GBTLB);
}

MEMKIND_EXPORT void memkind_gbtlb_init_once(void)
{
    memkind_register_kind(MEMKIND_GBTLB);
}
