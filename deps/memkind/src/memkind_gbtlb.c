/*
 * Copyright (C) 2014 - 2017 Intel Corporation.
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
#include <memkind/internal/memkind_arena.h>

static void memkind_hbw_gbtlb_init_once(void);
static void memkind_hbw_preferred_gbtlb_init_once(void);
static void memkind_gbtlb_init_once(void);

static void *gbtlb_mmap(struct memkind *kind, void *addr, size_t size);

MEMKIND_EXPORT struct memkind_ops MEMKIND_HBW_GBTLB_OPS = {
    .create = memkind_arena_create,
    .destroy = memkind_default_destroy,
    .malloc = memkind_arena_malloc,
    .calloc = memkind_arena_calloc,
    .posix_memalign = memkind_arena_posix_memalign,
    .realloc = memkind_arena_realloc,
    .free = memkind_default_free,
    .mmap = gbtlb_mmap,
    .check_available = memkind_hugetlb_check_available_2mb,
    .mbind = memkind_default_mbind,
    .get_mmap_flags = memkind_hugetlb_get_mmap_flags,
    .get_mbind_mode = memkind_default_get_mbind_mode,
    .get_mbind_nodemask = memkind_hbw_get_mbind_nodemask,
    .get_arena = memkind_thread_get_arena,
    .init_once = memkind_hbw_gbtlb_init_once,
    .finalize = memkind_arena_finalize
};

MEMKIND_EXPORT struct memkind_ops MEMKIND_HBW_PREFERRED_GBTLB_OPS = {
    .create = memkind_arena_create,
    .destroy = memkind_default_destroy,
    .malloc = memkind_arena_malloc,
    .calloc = memkind_arena_calloc,
    .posix_memalign = memkind_arena_posix_memalign,
    .realloc = memkind_arena_realloc,
    .free = memkind_default_free,
    .mmap = gbtlb_mmap,
    .check_available = memkind_hugetlb_check_available_2mb,
    .mbind = memkind_default_mbind,
    .get_mmap_flags = memkind_hugetlb_get_mmap_flags,
    .get_mbind_mode = memkind_preferred_get_mbind_mode,
    .get_mbind_nodemask = memkind_hbw_get_mbind_nodemask,
    .get_arena = memkind_thread_get_arena,
    .init_once = memkind_hbw_preferred_gbtlb_init_once,
    .finalize = memkind_arena_finalize
};

MEMKIND_EXPORT struct memkind_ops MEMKIND_GBTLB_OPS = {
    .create = memkind_arena_create,
    .destroy = memkind_default_destroy,
    .malloc = memkind_arena_malloc,
    .calloc = memkind_arena_calloc,
    .posix_memalign = memkind_arena_posix_memalign,
    .realloc = memkind_arena_realloc,
    .free = memkind_default_free,
    .mmap = gbtlb_mmap,
    .check_available = memkind_hugetlb_check_available_2mb,
    .get_mmap_flags = memkind_hugetlb_get_mmap_flags,
    .get_arena = memkind_thread_get_arena,
    .init_once = memkind_gbtlb_init_once,
    .finalize = memkind_arena_finalize
};

#define ONE_GB 1073741824ULL

static void memkind_gbtlb_ceil_size(size_t *size)
{
    *size = *size % ONE_GB ? ((*size / ONE_GB) + 1) * ONE_GB : *size;
}

static void *gbtlb_mmap(struct memkind *kind, void *addr, size_t size)
{
    memkind_gbtlb_ceil_size(&size);
    return memkind_default_mmap(kind, addr, size);
}

static void memkind_hbw_gbtlb_init_once(void)
{
    memkind_init(MEMKIND_HBW_GBTLB, false);
}

static void memkind_hbw_preferred_gbtlb_init_once(void)
{
    memkind_init(MEMKIND_HBW_PREFERRED_GBTLB, false);
}

static void memkind_gbtlb_init_once(void)
{
    memkind_init(MEMKIND_GBTLB, false);
}
