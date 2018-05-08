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

#include <memkind.h>
#include <memkind/internal/memkind_default.h>
#include <memkind/internal/memkind_arena.h>
#include <memkind/internal/memkind_private.h>
#include <memkind/internal/memkind_log.h>

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <numa.h>
#include <numaif.h>
#include <jemalloc/jemalloc.h>
#include <utmpx.h>
#include <sched.h>
#include <smmintrin.h>
#include <limits.h>
#include <sys/mman.h>

#include "config.h"
#include "config_tls.h"

static void *jemk_mallocx_check(size_t size, int flags);
static void *jemk_rallocx_check(void *ptr, size_t size, int flags);

static unsigned int integer_log2(unsigned int v)
{
    return (sizeof(unsigned) * 8) - (__builtin_clz(v) + 1);
}

static unsigned int round_pow2_up(unsigned int v)
{
    unsigned int v_log2 = integer_log2(v);

    if (v != 1 << v_log2) {
        v = 1 << (v_log2 + 1);
    }
    return v;
}

static int min_int(int a, int b)
{
    return a > b ? b : a;
}

MEMKIND_EXPORT int memkind_set_arena_map_len(struct memkind *kind)
{
    if (kind->ops->get_arena == memkind_bijective_get_arena) {
        kind->arena_map_len = 1;
    }
    else if (kind->ops->get_arena == memkind_thread_get_arena) {
        char *arena_num_env = getenv("MEMKIND_ARENA_NUM_PER_KIND");

        if (arena_num_env) {
            unsigned long int arena_num_value = strtoul(arena_num_env, NULL, 10);

            if ((arena_num_value == 0) || (arena_num_value > INT_MAX)) {
                log_err("Wrong MEMKIND_ARENA_NUM_PER_KIND environment value: %lu.", arena_num_value);
                return MEMKIND_ERROR_ENVIRON;
            }

            kind->arena_map_len = arena_num_value;
        }
        else {
            int calculated_arena_num = numa_num_configured_cpus() * 4;

#if ARENA_LIMIT_PER_KIND != 0
            calculated_arena_num = min_int(ARENA_LIMIT_PER_KIND, calculated_arena_num);
#endif
            kind->arena_map_len = calculated_arena_num;
        }

        kind->arena_map_len = round_pow2_up(kind->arena_map_len);
    }

    kind->arena_map_mask = kind->arena_map_len - 1;
    return 0;
}

static bool memkind_hog_memory;

static pthread_once_t arena_config_once = PTHREAD_ONCE_INIT;
static void arena_config_init() {
    const char* str = getenv("MEMKIND_HOG_MEMORY");
    memkind_hog_memory = str && str[0] == '1';
}

#define MALLOCX_ARENA_MAX 0xffe // copy-pasted from jemalloc/internal/jemalloc_internal.h
static struct memkind *arena_registry_g[MALLOCX_ARENA_MAX];
static pthread_mutex_t arena_registry_write_lock;

struct memkind *get_kind_by_arena(unsigned arena_ind)
{
    if(arena_ind >= MALLOCX_ARENA_MAX) {
        return NULL;
    }
    return arena_registry_g[arena_ind];
}

// Allocates size bytes aligned to alignment. Returns NULL if allocation fails.
static void *alloc_aligned_slow(size_t size, size_t alignment, struct memkind* kind)
{
    // According to jemalloc man page:
    // The size parameter is always a multiple of the chunk size (where chunk size equals 2MB)
    // The alignment parameter is always a power of two at least as large as the chunk size.
    size_t extended_size = size + alignment;
    void* ptr;

    ptr = kind_mmap(kind,  NULL, extended_size);

    if(ptr == MAP_FAILED) {
        return NULL;
    }

    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t aligned_addr = (addr + alignment) & ~(alignment - 1);

    size_t head_len = aligned_addr - addr;
    if (head_len > 0) {
        munmap(ptr, head_len);
    }

    uintptr_t tail = aligned_addr + size;
    size_t tail_len = (addr + extended_size) - (aligned_addr + size);
    if (tail_len > 0) {
        munmap((void*)tail, tail_len);
    }

    return (void*)aligned_addr;
}

void *arena_chunk_alloc(void *chunk, size_t size, size_t alignment,
                        bool *zero, bool *commit, unsigned arena_ind)
{
    int err;
    void *addr = NULL;

    struct memkind *kind = get_kind_by_arena(arena_ind);

    err = memkind_check_available(kind);
    if (err) {
        return NULL;
    }

    addr = kind_mmap(kind, chunk, size);
    if (addr == MAP_FAILED) {
        return NULL;
    }

    if (chunk != NULL && addr != chunk) {
        /* wrong place */
        munmap(addr, size);
        return NULL;
    }

    if ((uintptr_t)addr & (alignment-1)) {
        munmap(addr, size);
        addr = alloc_aligned_slow(size, alignment, kind);
        if(addr == NULL) {
            return NULL;
        }
    }

    *zero = true;
    *commit = true;

    return addr;
}

bool arena_chunk_dalloc(void *chunk, size_t size, bool commited, unsigned arena_ind)
{
    return true;
}

bool arena_chunk_commit(void *chunk, size_t size, size_t offset, size_t length,
                        unsigned arena_ind)
{
    return false;
}

bool arena_chunk_decommit(void *chunk, size_t size, size_t offset, size_t length,
                          unsigned arena_ind)
{
    return true;
}

bool arena_chunk_purge(void *chunk, size_t size, size_t offset, size_t length,
                       unsigned arena_ind)
{
    int err;

    if (memkind_hog_memory) {
        return true;
    }

    err = madvise(chunk + offset, length, MADV_DONTNEED);
    return (err != 0);
}

bool arena_chunk_split(void *chunk, size_t size, size_t size_a, size_t size_b,
        bool commited, unsigned arena_ind)
{
    return false;
}

bool arena_chunk_merge(void *chunk_a, size_t size_a, void *chunk_b,
        size_t size_b, bool commited, unsigned arena_ind)
{
    return false;
}

chunk_hooks_t arena_chunk_hooks = {
    arena_chunk_alloc,
    arena_chunk_dalloc,
    arena_chunk_commit,
    arena_chunk_decommit,
    arena_chunk_purge,
    arena_chunk_split,
    arena_chunk_merge
};

MEMKIND_EXPORT int memkind_arena_create_map(struct memkind *kind, chunk_hooks_t *hooks)
{
    int err = 0;
    size_t unsigned_size = sizeof(unsigned int);
    size_t chunk_hooks_t_size = sizeof(chunk_hooks_t);

    pthread_once(&arena_config_once, arena_config_init);

    if(hooks == NULL) {
        hooks = &arena_chunk_hooks;
    }

    err = memkind_set_arena_map_len(kind);
    if(err) {
        return err;
    }
#ifdef MEMKIND_TLS
    if (kind->ops->get_arena == memkind_thread_get_arena) {
        pthread_key_create(&(kind->arena_key), jemk_free);
    }
#endif

    pthread_mutex_lock(&arena_registry_write_lock);
    unsigned i = 0;
    for(i=0; i < kind->arena_map_len; i++) {
        //create new arena with consecutive index
        unsigned arena_index;
        err = jemk_mallctl("arenas.extend", (void*)&arena_index, &unsigned_size, NULL, 0);
        if(err) {
            goto exit;
        }
        //store index of first arena
        if(i == 0) {
            kind->arena_zero = arena_index;
        }
        //setup chunk_hooks for newly created arena
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "arena.%u.chunk_hooks", arena_index);
        err = jemk_mallctl(cmd, NULL, NULL, (void*)hooks, chunk_hooks_t_size);
        if(err) {
            goto exit;
        }
        arena_registry_g[arena_index] = kind;
    }

exit:
    pthread_mutex_unlock(&arena_registry_write_lock);
    return err;
}

MEMKIND_EXPORT int memkind_arena_create(struct memkind *kind, const struct memkind_ops *ops, const char *name)
{
    int err = 0;

    err = memkind_default_create(kind, ops, name);
    if (!err) {
        err = memkind_arena_create_map(kind, &arena_chunk_hooks);
    }
    return err;
}

MEMKIND_EXPORT int memkind_arena_destroy(struct memkind *kind)
{
    char cmd[128];
    int i;

    if (kind->arena_map_len) {
        for (i = 0; i < kind->arena_map_len; ++i) {
            snprintf(cmd, 128, "arena.%u.purge", kind->arena_zero + i);
            jemk_mallctl(cmd, NULL, NULL, NULL, 0);
        }
#ifdef MEMKIND_TLS
        if (kind->ops->get_arena == memkind_thread_get_arena) {
            pthread_key_delete(kind->arena_key);
        }
#endif
    }

    memkind_default_destroy(kind);
    return 0;
}

MEMKIND_EXPORT void *memkind_arena_malloc(struct memkind *kind, size_t size)
{
    void *result = NULL;
    int err = 0;
    unsigned int arena;

    err = kind->ops->get_arena(kind, &arena, size);
    if (MEMKIND_LIKELY(!err)) {
        result = jemk_mallocx_check(size, MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE);
    }
    return result;
}

MEMKIND_EXPORT void *memkind_arena_realloc(struct memkind *kind, void *ptr, size_t size)
{
    int err = 0;
    unsigned int arena;

    if (size == 0 && ptr != NULL) {
        memkind_free(kind, ptr);
        ptr = NULL;
    }
    else {
        err = kind->ops->get_arena(kind, &arena, size);
        if (MEMKIND_LIKELY(!err)) {
            if (ptr == NULL) {
                ptr = jemk_mallocx_check(size, MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE);
            }
            else {
                ptr = jemk_rallocx_check(ptr, size, MALLOCX_ARENA(arena));
            }
        }
    }
    return ptr;
}

MEMKIND_EXPORT void *memkind_arena_calloc(struct memkind *kind, size_t num, size_t size)
{
    void *result = NULL;
    int err = 0;
    unsigned int arena;

    err = kind->ops->get_arena(kind, &arena, size);
    if (MEMKIND_LIKELY(!err)) {
        result = jemk_mallocx_check(num * size, MALLOCX_ARENA(arena) | MALLOCX_ZERO | MALLOCX_TCACHE_NONE);
    }
    return result;
}

MEMKIND_EXPORT int memkind_arena_posix_memalign(struct memkind *kind, void **memptr, size_t alignment,
                                                size_t size)
{
    int err = 0;
    unsigned int arena;
    int errno_before;

    *memptr = NULL;
    err = kind->ops->get_arena(kind, &arena, size);
    if (MEMKIND_LIKELY(!err)) {
        err = memkind_posix_check_alignment(kind, alignment);
    }
    if (MEMKIND_LIKELY(!err)) {
        /* posix_memalign should not change errno.
           Set it to its previous value after calling jemalloc */
        errno_before = errno;
        *memptr = jemk_mallocx_check(size, MALLOCX_ALIGN(alignment) | MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE);
        errno = errno_before;
        err = *memptr ? 0 : ENOMEM;
    }
    return err;
}

MEMKIND_EXPORT int memkind_bijective_get_arena(struct memkind *kind, unsigned int *arena, size_t size)
{
    *arena = kind->arena_zero;
    return 0;
}

#ifdef MEMKIND_TLS
MEMKIND_EXPORT int memkind_thread_get_arena(struct memkind *kind, unsigned int *arena, size_t size)
{
    int err = 0;
    unsigned int *arena_tsd;
    arena_tsd = pthread_getspecific(kind->arena_key);

    if (MEMKIND_UNLIKELY(arena_tsd == NULL)) {
        arena_tsd = jemk_malloc(sizeof(unsigned int));
        if (arena_tsd == NULL) {
            err = MEMKIND_ERROR_MALLOC;
            log_err("jemk_malloc() failed.");
        }
        if (!err) {
            *arena_tsd = _mm_crc32_u64(0, (uint64_t)pthread_self()) %
                kind->arena_map_len;
            err = pthread_setspecific(kind->arena_key, arena_tsd) ?
                MEMKIND_ERROR_RUNTIME : 0;
        }
    }
    *arena = kind->arena_zero + *arena_tsd;
    return err;
}

#else

/*
 *
 * We use thread control block as unique thread identifier
 * For more read: https://www.akkadia.org/drepper/tls.pdf
 * We could consider using rdfsbase when it will arrive to linux kernel
 *
 */
static uintptr_t get_fs_base() {
    uintptr_t fs_base;
    asm ("movq %%fs:0, %0" : "=r" (fs_base));
    return fs_base;
}

MEMKIND_EXPORT int memkind_thread_get_arena(struct memkind *kind, unsigned int *arena, size_t size)
{
    unsigned int arena_idx;
    // it's likely that each thread control block lies on diffrent page
    // so we extracting page number with >> 12 to improve hashing
    arena_idx = (get_fs_base() >> 12) & kind->arena_map_mask;
    *arena = kind->arena_zero + arena_idx;
    return 0;
}
#endif //MEMKIND_TLS

static void *jemk_mallocx_check(size_t size, int flags)
{
    /*
     * Checking for out of range size due to unhandled error in
     * jemk_mallocx().  Size invalid for the range
     * LLONG_MAX <= size <= ULLONG_MAX
     * which is the result of passing a negative signed number as size
     */
    void *result = NULL;

    if (MEMKIND_UNLIKELY(size >= LLONG_MAX)) {
        errno = ENOMEM;
    }
    else if (size != 0) {
        result = jemk_mallocx(size, flags);
    }
    return result;
}

static void *jemk_rallocx_check(void *ptr, size_t size, int flags)
{
    /*
     * Checking for out of range size due to unhandled error in
     * jemk_mallocx().  Size invalid for the range
     * LLONG_MAX <= size <= ULLONG_MAX
     * which is the result of passing a negative signed number as size
     */
    void *result = NULL;

    if (MEMKIND_UNLIKELY(size >= LLONG_MAX)) {
        errno = ENOMEM;
    }
    else {
        result = jemk_rallocx(ptr, size, flags);
    }
    return result;

}
