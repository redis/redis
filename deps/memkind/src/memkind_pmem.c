/*
 * Copyright (C) 2015 - 2016 Intel Corporation.
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

#include <memkind/internal/memkind_arena.h>
#include <memkind/internal/memkind_pmem.h>
#include <memkind/internal/memkind_private.h>
#include <memkind/internal/memkind_log.h>

#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <jemalloc/jemalloc.h>
#include <assert.h>

MEMKIND_EXPORT const struct memkind_ops MEMKIND_PMEM_OPS = {
    .create = memkind_pmem_create,
    .destroy = memkind_pmem_destroy,
    .malloc = memkind_arena_malloc,
    .calloc = memkind_arena_calloc,
    .posix_memalign = memkind_arena_posix_memalign,
    .realloc = memkind_arena_realloc,
    .free = memkind_default_free,
    .mmap = memkind_pmem_mmap,
    .get_mmap_flags = memkind_pmem_get_mmap_flags,
    .get_arena = memkind_thread_get_arena,
    .get_size = memkind_pmem_get_size,
};


void *pmem_chunk_alloc(void *chunk, size_t size, size_t alignment,
                       bool *zero, bool *commit, unsigned arena_ind)
{
    int err;
    void *addr = NULL;

    if (chunk != NULL) {
        /* not supported */
        goto exit;
    }

    struct memkind *kind;
    kind = get_kind_by_arena(arena_ind);
    if (kind == NULL) {
        return NULL;
    }

    err = memkind_check_available(kind);
    if (err) {
        goto exit;
    }

    addr = memkind_pmem_mmap(kind, chunk, size);

    if (addr != MAP_FAILED) {
        *zero = true;
        *commit = true;

        /* XXX - check alignment */
    } else {
        addr = NULL;
    }

exit:
    return addr;
}

bool pmem_chunk_dalloc(void *chunk, size_t size, bool commited,
                        unsigned arena_ind)
{
    /* do nothing - report failure (opt-out) */
    return true;
}

bool pmem_chunk_commit(void *chunk, size_t size, size_t offset, size_t length,
                        unsigned arena_ind)
{
    /* do nothing - report success */
    return false;
}

bool pmem_chunk_decommit(void *chunk, size_t size, size_t offset, size_t length,
                          unsigned arena_ind)
{
    /* do nothing - report failure (opt-out) */
    return true;
}

bool pmem_chunk_purge(void *chunk, size_t size, size_t offset, size_t length,
                       unsigned arena_ind)
{
    /* do nothing - report failure (opt-out) */
    return true;
}

bool pmem_chunk_split(void *chunk, size_t size, size_t size_a, size_t size_b,
                       bool commited, unsigned arena_ind)
{
    /* do nothing - report success */
    return false;
}

bool pmem_chunk_merge(void *chunk_a, size_t size_a, void *chunk_b,
                       size_t size_b, bool commited, unsigned arena_ind)
{
    /* do nothing - report success */
    return false;
}

static chunk_hooks_t pmem_chunk_hooks = {
    pmem_chunk_alloc,
    pmem_chunk_dalloc,
    pmem_chunk_commit,
    pmem_chunk_decommit,
    pmem_chunk_purge,
    pmem_chunk_split,
    pmem_chunk_merge
};

MEMKIND_EXPORT int memkind_pmem_create(struct memkind *kind, const struct memkind_ops *ops,
                        const char *name)
{
    struct memkind_pmem *priv;
    int err;

    priv = (struct memkind_pmem *)jemk_malloc(sizeof(struct memkind_pmem));
    if (!priv) {
        log_err("jemk_malloc() failed.");
        return MEMKIND_ERROR_MALLOC;
    }

    if (pthread_mutex_init(&priv->pmem_lock, NULL) != 0) {
        err = MEMKIND_ERROR_RUNTIME;
        goto exit;
    }

    err = memkind_default_create(kind, ops, name);
    if (err) {
        goto exit;
    }

    err = memkind_arena_create_map(kind, &pmem_chunk_hooks);
    if (err) {
        goto exit;
    }

    kind->priv = priv;
    return 0;

exit:
    /* err is set, please don't overwrite it with result of pthread_mutex_destroy */
    pthread_mutex_destroy(&priv->pmem_lock);
    jemk_free(priv);
    return err;
}

MEMKIND_EXPORT int memkind_pmem_destroy(struct memkind *kind)
{
    struct memkind_pmem *priv = kind->priv;

    memkind_arena_destroy(kind);

    pthread_mutex_destroy(&priv->pmem_lock);
    (void) munmap(priv->addr, priv->max_size);
    (void) close(priv->fd);
    jemk_free(priv);

    return 0;
}

MEMKIND_EXPORT void *memkind_pmem_mmap(struct memkind *kind, void *addr, size_t size)
{
    struct memkind_pmem *priv = kind->priv;
    void *result;

    if (pthread_mutex_lock(&priv->pmem_lock) != 0)
        assert(0 && "failed to acquire mutex");

    if (priv->offset + size > priv->max_size) {
        pthread_mutex_unlock(&priv->pmem_lock);
        return MAP_FAILED;
    }

    if ((errno = posix_fallocate(priv->fd, priv->offset, size)) != 0) {
        pthread_mutex_unlock(&priv->pmem_lock);
        return MAP_FAILED;
    }

    result = priv->addr + priv->offset;
    priv->offset += size;

    pthread_mutex_unlock(&priv->pmem_lock);

    return result;
}

MEMKIND_EXPORT int memkind_pmem_get_mmap_flags(struct memkind *kind, int *flags)
{
    *flags = MAP_SHARED;
    return 0;
}

MEMKIND_EXPORT int memkind_pmem_get_size(struct memkind *kind, size_t *total, size_t *free)
{
    struct memkind_pmem *priv = kind->priv;

    *total = priv->max_size;
    *free = priv->max_size - priv->offset; /* rough estimation */

    return 0;
}
