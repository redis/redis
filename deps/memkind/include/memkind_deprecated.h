/*
 * Copyright (C) 2016 Intel Corporation.
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

/*
 * !!!!!!!!!!!!!!!!!!!
 * !!!   WARNING   !!!
 * !!! PLEASE READ !!!
 * !!!!!!!!!!!!!!!!!!!
 *
 * This header file contains all memkind deprecated symbols.
 *
 * Please avoid usage of this API in newly developed code, as
 * eventually this code is subject for removal.
 */

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MEMKIND_DEPRECATED

#ifdef __GNUC__
#define MEMKIND_DEPRECATED(func) func __attribute__ ((deprecated))
#elif defined(_MSC_VER)
#define MEMKIND_DEPRECATED(func) __declspec(deprecated) func
#else
#pragma message("WARNING: You need to implement MEMKIND_DEPRECATED for this compiler")
#define MEMKIND_DEPRECATED(func) func
#endif

#endif

enum memkind_const_deprecated {
    MEMKIND_NAME_LENGTH = 64
};


/*
 * There is a list of error codes that have been removed from memkind.h
 * and in memkind_private.h have been defined internal errors.
 * When the deprecated error codes will be removed then private error codes
 * may be translated to other more appropriate error codes.
 */
enum memkind_error_deprecated {
    MEMKIND_ERROR_MEMALIGN = -4,
    MEMKIND_ERROR_MALLCTL = -5,
    MEMKIND_ERROR_GETCPU = -7,
    MEMKIND_ERROR_PMTT = -8,
    MEMKIND_ERROR_TIEDISTANCE = -9,
    MEMKIND_ERROR_ALIGNMENT = -10,
    MEMKIND_ERROR_MALLOCX = -11,
    MEMKIND_ERROR_REPNAME = -14,
    MEMKIND_ERROR_PTHREAD = -16,
    MEMKIND_ERROR_BADPOLICY = -19,
    MEMKIND_ERROR_REPPOLICY = -20,

};


enum memkind_base_partition {
    MEMKIND_PARTITION_DEFAULT = 0,
    MEMKIND_PARTITION_HBW = 1,
    MEMKIND_PARTITION_HBW_HUGETLB = 2,
    MEMKIND_PARTITION_HBW_PREFERRED = 3,
    MEMKIND_PARTITION_HBW_PREFERRED_HUGETLB = 4,
    MEMKIND_PARTITION_HUGETLB = 5,
    MEMKIND_PARTITION_HBW_GBTLB = 6,
    MEMKIND_PARTITION_HBW_PREFERRED_GBTLB = 7,
    MEMKIND_PARTITION_GBTLB = 8,
    MEMKIND_PARTITION_HBW_INTERLEAVE = 9,
    MEMKIND_PARTITION_INTERLEAVE = 10,
    MEMKIND_NUM_BASE_KIND
};


struct memkind_ops {
    int (* create)(struct memkind *kind, const struct memkind_ops *ops, const char *name);
    int (* destroy)(struct memkind *kind);
    void *(* malloc)(struct memkind *kind, size_t size);
    void *(* calloc)(struct memkind *kind, size_t num, size_t size);
    int (* posix_memalign)(struct memkind *kind, void **memptr, size_t alignment, size_t size);
    void *(* realloc)(struct memkind *kind, void *ptr, size_t size);
    void (* free)(struct memkind *kind, void *ptr);
    void *(* mmap)(struct memkind *kind, void *addr, size_t size);
    int (* mbind)(struct memkind *kind, void *ptr, size_t size);
    int (* madvise)(struct memkind *kind, void *addr, size_t size);
    int (* get_mmap_flags)(struct memkind *kind, int *flags);
    int (* get_mbind_mode)(struct memkind *kind, int *mode);
    int (* get_mbind_nodemask)(struct memkind *kind, unsigned long *nodemask, unsigned long maxnode);
    int (* get_arena)(struct memkind *kind, unsigned int *arena, size_t size);
    int (* get_size)(struct memkind *kind, size_t *total, size_t *free);
    int (* check_available)(struct memkind *kind);
    int (* check_addr)(struct memkind *kind, void *addr);
    void (*init_once)(void);
};

/*
 * Create a new kind
 *
 * DEPRECATION REASON:
 *   There is work in progress to implement new functionality that allows
 *   for dynamic kind creation.
 *
 */
int MEMKIND_DEPRECATED(memkind_create(const struct memkind_ops *ops, const char *name, memkind_t *kind));


/*
 * Free all resources allocated by the library (must be last call to library by the process)
 *
 * DEPRECATION REASON:
 *   The function is called automatically in destructor of library. It should not be exposed in API
 */
int MEMKIND_DEPRECATED(memkind_finalize(void));

/*
 * Get kind associated with a partition (index from 0 to num_kind - 1)
 *
 * DEPRECATION REASON:
 *   Partition is internal mechanism related to jemalloc
 */
int MEMKIND_DEPRECATED(memkind_get_kind_by_partition(int partition, memkind_t *kind));


/*
 * Get kind given the name of the kind
 *
 * DEPRECATION REASON:
 *   Current API does not allow to get/set name of kind. Therefore
 *   choosing kind by name does not make a sense.
 */
int MEMKIND_DEPRECATED(memkind_get_kind_by_name(const char *name, memkind_t *kind));


/*
 * Query the number of kinds instantiated
 *
 * DEPRECATION REASON:
 *   No other API calls related to number of kinds (e.g. dynamic creating of kind).
 */
int MEMKIND_DEPRECATED(memkind_get_num_kind(int *num_kind));


/*
 * ALLOCATOR CALLBACK FUNCTION
 *
 * DEPRECATION REASON:
 *   The current design of allocator back-end API is incomplete, e.g. needed
 *   also unmap() function and maybe more functions for allocator management.
 *   There is work in progress to implement new functionality that allows
 *   connecting other allocator than jemalloc 4.3 that is tightly coupled
 *   today with memkind.
 */
void* MEMKIND_DEPRECATED(memkind_partition_mmap(int partition, void *addr, size_t size));


/*
 * Get the amount in bytes of total and free memory of the NUMA nodes associated with the kind
 *
 * DEPRECATION REASON:
 *   This function is no longer supported.
 */
int MEMKIND_DEPRECATED(memkind_get_size(memkind_t kind, size_t *total, size_t *free));

#ifdef __cplusplus
}
#endif
