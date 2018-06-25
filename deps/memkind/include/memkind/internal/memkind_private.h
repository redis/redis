/*
 * Copyright (C) 2016 - 2017 Intel Corporation.
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

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MEMKIND_INTERNAL_API
#warning "DO NOT INCLUDE THIS FILE! IT IS INTERNAL MEMKIND API AND SOON WILL BE REMOVED FROM BIN & DEVEL PACKAGES"
#endif

#include "memkind.h"

#include <stdbool.h>
#include <pthread.h>

#ifdef __GNUC__
#   define MEMKIND_LIKELY(x)       __builtin_expect(!!(x), 1)
#   define MEMKIND_UNLIKELY(x)     __builtin_expect(!!(x), 0)
#else
#   define MEMKIND_LIKELY(x)       (x)
#   define MEMKIND_UNLIKELY(x)     (x)
#endif

#ifndef MEMKIND_EXPORT
#   define MEMKIND_EXPORT __attribute__((visibility("default")))
#endif

#ifndef JE_PREFIX
#error "Can't find JE_PREFIX define. Define one or use build.sh script."
#endif

// This ladder call is required due to meanders of C's preprocessor logic.
// Without it, JE_PREFIX would be used directly (i.e. 'JE_PREFIX') and not
// substituted with defined value.
#define JE_SYMBOL2(a, b) a ## b
#define JE_SYMBOL1(a, b) JE_SYMBOL2(a, b)
#define JE_SYMBOL(b)     JE_SYMBOL1(JE_PREFIX, b)

// Redefine symbols
#define jemk_malloc         JE_SYMBOL(malloc)
#define jemk_mallocx        JE_SYMBOL(mallocx)
#define jemk_calloc         JE_SYMBOL(calloc)
#define jemk_rallocx        JE_SYMBOL(rallocx)
#define jemk_realloc        JE_SYMBOL(realloc)
#define jemk_mallctl        JE_SYMBOL(mallctl)
#define jemk_memalign       JE_SYMBOL(memalign)
#define jemk_posix_memalign JE_SYMBOL(posix_memalign)
#define jemk_free           JE_SYMBOL(free)

enum memkind_const_private {
    MEMKIND_NAME_LENGTH_PRIV = 64
};

struct memkind_ops {
    int (* create)(struct memkind *kind, struct memkind_ops *ops, const char *name);
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
    int (* check_available)(struct memkind *kind);
    int (* check_addr)(struct memkind *kind, void *addr);
    void (*init_once)(void);
    int (* finalize)(struct memkind *kind);
};

struct memkind {
    struct memkind_ops *ops;
    unsigned int partition;
    char name[MEMKIND_NAME_LENGTH_PRIV];
    pthread_once_t init_once;
    unsigned int arena_map_len; // is power of 2
    unsigned int *arena_map; // To be deleted beyond 1.2.0+
    pthread_key_t arena_key;
    void *priv;
    unsigned int arena_map_mask; // arena_map_len - 1 to optimize modulo operation on arena_map_len
    unsigned int arena_zero; // index first jemalloc arena of this kind
};

void memkind_init(memkind_t kind, bool check_numa);

void *kind_mmap(struct memkind *kind, void* addr, size_t size);

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
    MEMKIND_PARTITION_REGULAR = 11,
    MEMKIND_PARTITION_HBW_ALL = 12,
    MEMKIND_PARTITION_HBW_ALL_HUGETLB = 13,
    MEMKIND_NUM_BASE_KIND
};

#ifdef __cplusplus
}
#endif

