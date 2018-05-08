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
#define MEMKIND_VERSION_MAJOR 1
#define MEMKIND_VERSION_MINOR 4
#define MEMKIND_VERSION_PATCH 0

#include <memkind.h>
#include <memkind/internal/memkind_default.h>
#include <memkind/internal/memkind_hugetlb.h>
#include <memkind/internal/memkind_arena.h>
#include <memkind/internal/memkind_hbw.h>
#include <memkind/internal/memkind_gbtlb.h>
#include <memkind/internal/memkind_pmem.h>
#include <memkind/internal/memkind_interleave.h>
#include <memkind/internal/memkind_private.h>
#include <memkind/internal/memkind_log.h>

#include "config.h"

#include <numa.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <jemalloc/jemalloc.h>

/* Clear bits in x, but only this specified in mask. */
#define CLEAR_BIT(x, mask) ((x) &= (~(mask)))

static struct memkind MEMKIND_DEFAULT_STATIC = {
    .ops =  &MEMKIND_DEFAULT_OPS,
    .partition = MEMKIND_PARTITION_DEFAULT,
    .name = "memkind_default",
    .init_once = PTHREAD_ONCE_INIT,
};

static struct memkind MEMKIND_HUGETLB_STATIC = {
    .ops = &MEMKIND_HUGETLB_OPS,
    .partition = MEMKIND_PARTITION_HUGETLB,
    .name = "memkind_hugetlb",
    .init_once = PTHREAD_ONCE_INIT,
};

static struct memkind MEMKIND_INTERLEAVE_STATIC = {
    .ops = &MEMKIND_INTERLEAVE_OPS,
    .partition = MEMKIND_PARTITION_INTERLEAVE,
    .name = "memkind_interleave",
    .init_once = PTHREAD_ONCE_INIT,
};

static struct memkind MEMKIND_HBW_STATIC = {
    .ops = &MEMKIND_HBW_OPS,
    .partition = MEMKIND_PARTITION_HBW,
    .name = "memkind_hbw",
    .init_once = PTHREAD_ONCE_INIT,
};

static struct memkind MEMKIND_HBW_PREFERRED_STATIC = {
    .ops = &MEMKIND_HBW_PREFERRED_OPS,
    .partition = MEMKIND_PARTITION_HBW_PREFERRED,
    .name = "memkind_hbw_preferred",
    .init_once = PTHREAD_ONCE_INIT,
};

static struct memkind MEMKIND_HBW_HUGETLB_STATIC = {
    .ops = &MEMKIND_HBW_HUGETLB_OPS,
    .partition = MEMKIND_PARTITION_HBW_HUGETLB,
    .name = "memkind_hbw_hugetlb",
    .init_once = PTHREAD_ONCE_INIT,
};

static struct memkind MEMKIND_HBW_PREFERRED_HUGETLB_STATIC = {
    .ops = &MEMKIND_HBW_PREFERRED_HUGETLB_OPS,
    .partition = MEMKIND_PARTITION_HBW_PREFERRED_HUGETLB,
    .name = "memkind_hbw_preferred_hugetlb",
    .init_once = PTHREAD_ONCE_INIT,
};

static struct memkind MEMKIND_HBW_GBTLB_STATIC = {
    .ops = &MEMKIND_HBW_GBTLB_OPS,
    .partition = MEMKIND_PARTITION_HBW_GBTLB,
    .name = "memkind_hbw_gbtlb",
    .init_once = PTHREAD_ONCE_INIT,
};

static struct memkind MEMKIND_HBW_PREFERRED_GBTLB_STATIC = {
    .ops = &MEMKIND_HBW_PREFERRED_GBTLB_OPS,
    .partition = MEMKIND_PARTITION_HBW_PREFERRED_GBTLB,
    .name = "memkind_hbw_preferred_gbtlb",
    .init_once = PTHREAD_ONCE_INIT,
};

static struct memkind MEMKIND_GBTLB_STATIC = {
    .ops = &MEMKIND_GBTLB_OPS,
    .partition = MEMKIND_PARTITION_GBTLB,
    .name = "memkind_gbtlb",
    .init_once = PTHREAD_ONCE_INIT,
};

static struct memkind MEMKIND_HBW_INTERLEAVE_STATIC = {
    .ops = &MEMKIND_HBW_INTERLEAVE_OPS,
    .partition = MEMKIND_PARTITION_HBW_INTERLEAVE,
    .name = "memkind_hbw_interleave",
    .init_once = PTHREAD_ONCE_INIT,
};

MEMKIND_EXPORT struct memkind *MEMKIND_DEFAULT = &MEMKIND_DEFAULT_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HUGETLB = &MEMKIND_HUGETLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_INTERLEAVE = &MEMKIND_INTERLEAVE_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW = &MEMKIND_HBW_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_PREFERRED = &MEMKIND_HBW_PREFERRED_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_HUGETLB = &MEMKIND_HBW_HUGETLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_PREFERRED_HUGETLB = &MEMKIND_HBW_PREFERRED_HUGETLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_GBTLB = &MEMKIND_HBW_GBTLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_PREFERRED_GBTLB = &MEMKIND_HBW_PREFERRED_GBTLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_GBTLB = &MEMKIND_GBTLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_INTERLEAVE = &MEMKIND_HBW_INTERLEAVE_STATIC;

struct memkind_registry {
    struct memkind *partition_map[MEMKIND_MAX_KIND];
    int num_kind;
    pthread_mutex_t lock;
};

static struct memkind_registry memkind_registry_g = {
    {
        [MEMKIND_PARTITION_DEFAULT] = &MEMKIND_DEFAULT_STATIC,
        [MEMKIND_PARTITION_HBW] = &MEMKIND_HBW_STATIC,
        [MEMKIND_PARTITION_HBW_PREFERRED] = &MEMKIND_HBW_PREFERRED_STATIC,
        [MEMKIND_PARTITION_HBW_HUGETLB] = &MEMKIND_HBW_HUGETLB_STATIC,
        [MEMKIND_PARTITION_HBW_PREFERRED_HUGETLB] = &MEMKIND_HBW_PREFERRED_HUGETLB_STATIC,
        [MEMKIND_PARTITION_HUGETLB] = &MEMKIND_HUGETLB_STATIC,
        [MEMKIND_PARTITION_HBW_GBTLB] = &MEMKIND_HBW_GBTLB_STATIC,
        [MEMKIND_PARTITION_HBW_PREFERRED_GBTLB] = &MEMKIND_HBW_PREFERRED_GBTLB_STATIC,
        [MEMKIND_PARTITION_GBTLB] = &MEMKIND_GBTLB_STATIC,
        [MEMKIND_PARTITION_HBW_INTERLEAVE] = &MEMKIND_HBW_INTERLEAVE_STATIC,
        [MEMKIND_PARTITION_INTERLEAVE] = &MEMKIND_INTERLEAVE_STATIC,
    },
    MEMKIND_NUM_BASE_KIND,
    PTHREAD_MUTEX_INITIALIZER
};

// subset of kind universe
struct memkind_subregistry {
    int kind_partition[MEMKIND_MAX_KIND]; // array containing indexes from memkind_registry->partition_map
    int num_kind;
    pthread_mutex_t lock;
};

// adding kind to subregistry; does not check for duplicates
static void subregistry_add(struct memkind_subregistry* subregistry, memkind_t kind);

// returns kind element at specified position in subregistry; index should be >=0 and < MEMKIND_MAX_KIND
static struct memkind* subregistry_get(struct memkind_subregistry* subregistry, int index);

// returns current number of elements in subregistry
static int subregistry_size(struct memkind_subregistry* subregistry);

// subset of kinds universe containing only kinds that implements check_addr operation
static struct memkind_subregistry memkind_check_addr_subregistry_g = {
    {},
    0,
    PTHREAD_MUTEX_INITIALIZER
};

static size_t Chunksize = 0;

static int memkind_get_kind_for_free(void *ptr, struct memkind **kind);

static int validate_memtype_bits(memkind_memtype_t memtype) {
    if(memtype == 0) return -1;

    CLEAR_BIT(memtype, MEMKIND_MEMTYPE_DEFAULT);
    CLEAR_BIT(memtype, MEMKIND_MEMTYPE_HIGH_BANDWIDTH);

    if(memtype != 0) return -1;
    return 0;
}

static int validate_flags_bits(memkind_bits_t flags) {
    CLEAR_BIT(flags, MEMKIND_MASK_PAGE_SIZE_2MB);

    if(flags != 0) return -1;
    return 0;
}

static int validate_policy(memkind_policy_t policy) {
    if((policy >= 0) && (policy < MEMKIND_POLICY_MAX_VALUE)) return 0;
    return -1;
}

/* Kind creation */
MEMKIND_EXPORT int memkind_create_kind(memkind_memtype_t memtype_flags,
                                       memkind_policy_t policy,
                                       memkind_bits_t flags,
                                       memkind_t* kind)
{
    if(validate_memtype_bits(memtype_flags) != 0) {
        log_err("Cannot create kind: incorrect memtype_flags.");
        return MEMKIND_ERROR_INVALID;
    }

    if(validate_flags_bits(flags) != 0) {
        log_err("Cannot create kind: incorrect flags.");
        return MEMKIND_ERROR_INVALID;
    }

    if(validate_policy(policy) != 0) {
        log_err("Cannot create kind: incorrect policy.");
        return MEMKIND_ERROR_INVALID;
    }

    if(kind == NULL) {
        log_err("Cannot create kind: 'kind' is NULL pointer.");
        return MEMKIND_ERROR_INVALID;
    }

    memkind_t tmp_kind = NULL;

    /* This implementation reuse old static kinds, which means that kind object
     * is not created here. */
    if(memtype_flags == MEMKIND_MEMTYPE_DEFAULT) {
        if(policy == MEMKIND_POLICY_PREFERRED_LOCAL) {
            if(flags & MEMKIND_MASK_PAGE_SIZE_2MB)
                tmp_kind = MEMKIND_HUGETLB;
            else
                tmp_kind = MEMKIND_DEFAULT;
        }
    }

    if(memtype_flags == MEMKIND_MEMTYPE_HIGH_BANDWIDTH) {
        if(policy == MEMKIND_POLICY_BIND_LOCAL) {
            if(flags & MEMKIND_MASK_PAGE_SIZE_2MB)
                tmp_kind = MEMKIND_HBW_HUGETLB;
            else
                tmp_kind = MEMKIND_HBW;
        }

        if(policy == MEMKIND_POLICY_PREFERRED_LOCAL) {
            if(flags & MEMKIND_MASK_PAGE_SIZE_2MB)
                tmp_kind = MEMKIND_HBW_PREFERRED_HUGETLB;
            else
                tmp_kind = MEMKIND_HBW_PREFERRED;
        }

        if((policy == MEMKIND_POLICY_INTERLEAVE_ALL)
            && !(flags & MEMKIND_MASK_PAGE_SIZE_2MB))
        {
            tmp_kind = MEMKIND_HBW_INTERLEAVE;
        }
    }

    if((memtype_flags & MEMKIND_MEMTYPE_HIGH_BANDWIDTH)
        && (memtype_flags & MEMKIND_MEMTYPE_DEFAULT)
        && (policy == MEMKIND_POLICY_INTERLEAVE_ALL)
        && !(flags & MEMKIND_MASK_PAGE_SIZE_2MB))
    {
        tmp_kind = MEMKIND_INTERLEAVE;
    }

    if(tmp_kind == NULL) {
        log_err("Cannot create kind: invalid argument.");
        return MEMKIND_ERROR_INVALID;
    }

    if(memkind_check_available(tmp_kind) != 0) {
        if(policy == MEMKIND_POLICY_PREFERRED_LOCAL) {
            tmp_kind = MEMKIND_DEFAULT;
        } else {
            log_err("Cannot create kind: requested memory type is not available.");
            return MEMKIND_ERROR_MEMTYPE_NOT_AVAILABLE;
        }
    }

    *kind = tmp_kind;
    return MEMKIND_SUCCESS;
}

/* Kind destruction. */
MEMKIND_EXPORT int memkind_destroy_kind(memkind_t kind) {
    /* For now the implementation is based on old static kinds, so we don't destroy
     * kind object here. This might be changed in furue. */
    return MEMKIND_SUCCESS;
}

/* Declare weak symbols for alloctor decorators */
extern void memkind_malloc_pre(struct memkind **, size_t *) __attribute__((weak));
extern void memkind_malloc_post(struct memkind *, size_t, void **) __attribute__((weak));
extern void memkind_calloc_pre(struct memkind **, size_t *, size_t *) __attribute__((weak));
extern void memkind_calloc_post(struct memkind *, size_t, size_t, void **) __attribute__((weak));
extern void memkind_posix_memalign_pre(struct memkind **, void **, size_t *, size_t *) __attribute__((weak));
extern void memkind_posix_memalign_post(struct memkind *, void **, size_t, size_t, int *) __attribute__((weak));
extern void memkind_realloc_pre(struct memkind **, void **, size_t *) __attribute__((weak));
extern void memkind_realloc_post(struct memkind *, void *, size_t, void **) __attribute__((weak));
extern void memkind_free_pre(struct memkind **, void **) __attribute__((weak));
extern void memkind_free_post(struct memkind *, void *) __attribute__((weak));

MEMKIND_EXPORT int memkind_get_version()
{
    return MEMKIND_VERSION_MAJOR * 1000000 + MEMKIND_VERSION_MINOR * 1000 + MEMKIND_VERSION_PATCH;
}

MEMKIND_EXPORT void memkind_error_message(int err, char *msg, size_t size)
{
    switch (err) {
        case MEMKIND_ERROR_UNAVAILABLE:
            strncpy(msg, "<memkind> Requested memory kind is not available", size);
            break;
        case MEMKIND_ERROR_MBIND:
            strncpy(msg, "<memkind> Call to mbind() failed", size);
            break;
        case MEMKIND_ERROR_MMAP:
            strncpy(msg, "<memkind> Call to mmap() failed", size);
            break;
        case MEMKIND_ERROR_MALLOC:
            strncpy(msg, "<memkind> Call to jemk_malloc() failed", size);
            break;
        case MEMKIND_ERROR_ENVIRON:
            strncpy(msg, "<memkind> Error parsing environment variable (MEMKIND_*)", size);
            break;
        case MEMKIND_ERROR_INVALID:
            strncpy(msg, "<memkind> Invalid input arguments to memkind routine", size);
            break;
        case MEMKIND_ERROR_TOOMANY:
            snprintf(msg, size, "<memkind> Attempted to initialize more than maximum (%i) number of kinds", MEMKIND_MAX_KIND);
            break;
        case MEMKIND_ERROR_RUNTIME:
            strncpy(msg, "<memkind> Unspecified run-time error", size);
            break;
        case EINVAL:
            strncpy(msg, "<memkind> Alignment must be a power of two and larger than sizeof(void *)", size);
            break;
        case ENOMEM:
            strncpy(msg, "<memkind> Call to jemk_mallocx() failed", size);
            break;
        case MEMKIND_ERROR_HUGETLB:
            strncpy(msg, "<memkind> unable to allocate huge pages", size);
            break;
        case MEMKIND_ERROR_BADOPS:
            strncpy(msg, "<memkind> memkind_ops structure is poorly formed (missing or incorrect functions)", size);
            break;
        default:
            snprintf(msg, size, "<memkind> Undefined error number: %i", err);
            break;
    }
    if (size > 0) {
        msg[size-1] = '\0';
    }
}

void memkind_init(memkind_t kind, bool check_numa)
{
    log_info("Initializing kind %s.", kind->name);
    int err = memkind_arena_create_map(kind, NULL);
    if (err) {
        log_fatal("[%s] Failed to create arena map (error code:%d).", kind->name, err);
        abort();
    }
    if (check_numa) {
        err = numa_available();
        if (err) {
            log_fatal("[%s] NUMA not available (error code:%d).", kind->name, err);
            abort();
        }
    }
    memkind_register_kind(kind);
}

void memkind_register_kind(memkind_t kind)
{
    if(kind && kind->ops->check_addr)
    {
        subregistry_add(&memkind_check_addr_subregistry_g, kind);
    }
}

static void subregistry_add(struct memkind_subregistry* subregistry, memkind_t kind)
{
    assert(subregistry && kind && subregistry->num_kind < MEMKIND_MAX_KIND);
    if (pthread_mutex_lock(&subregistry->lock) != 0)
        assert(0 && "failed to acquire mutex");

    subregistry->kind_partition[subregistry->num_kind++]= kind->partition;
    pthread_mutex_unlock(&subregistry->lock);
}

static struct memkind* subregistry_get(struct memkind_subregistry* subregistry, int index)
{
    assert(subregistry && index >= 0);
    if(index > subregistry->num_kind) {
        return NULL;
    }
    return memkind_registry_g.partition_map[subregistry->kind_partition[index]];
}

static inline int subregistry_size(struct memkind_subregistry* subregistry)
{
    assert(subregistry);
    return subregistry->num_kind;
}

static void nop(void) {}

MEMKIND_EXPORT int memkind_create(const struct memkind_ops *ops, const char *name, struct memkind **kind)
{
    int err;
    int i;

    *kind = NULL;
    if (pthread_mutex_lock(&memkind_registry_g.lock) != 0)
        assert(0 && "failed to acquire mutex");

    if (memkind_registry_g.num_kind == MEMKIND_MAX_KIND) {
        log_err("Attempted to initialize more than maximum (%i) number of kinds.", MEMKIND_MAX_KIND);
        err = MEMKIND_ERROR_TOOMANY;
        goto exit;
    }
    if (ops->create == NULL ||
        ops->destroy == NULL ||
        ops->malloc == NULL ||
        ops->calloc == NULL ||
        ops->realloc == NULL ||
        ops->posix_memalign == NULL ||
        ops->free == NULL ||
        ops->get_size == NULL ||
        ops->init_once != NULL) {
        err = MEMKIND_ERROR_BADOPS;
        goto exit;
    }
    for (i = 0; i < memkind_registry_g.num_kind; ++i) {
        if (strcmp(name, memkind_registry_g.partition_map[i]->name) == 0) {
            err = MEMKIND_ERROR_INVALID;
            goto exit;
        }
    }
    *kind = (struct memkind *)jemk_calloc(1, sizeof(struct memkind));
    if (!*kind) {
        err = MEMKIND_ERROR_MALLOC;
        log_err("jemk_calloc() failed.");
        goto exit;
    }

    (*kind)->partition = memkind_registry_g.num_kind;
    err = ops->create(*kind, ops, name);
    if (err) {
        goto exit;
    }
    memkind_registry_g.partition_map[memkind_registry_g.num_kind] = *kind;
    ++memkind_registry_g.num_kind;
    memkind_register_kind(*kind);

    (*kind)->init_once = PTHREAD_ONCE_INIT;
    pthread_once(&(*kind)->init_once, nop); //this is done to avoid init_once for dynamic kinds
exit:
    if (pthread_mutex_unlock(&memkind_registry_g.lock) != 0)
        assert(0 && "failed to release mutex");

    return err;
}

#ifdef __GNUC__
__attribute__((destructor))
#endif
MEMKIND_EXPORT int memkind_finalize(void)
{
    struct memkind *kind;
    int i;
    int err = 0;

    if (pthread_mutex_lock(&memkind_registry_g.lock) != 0)
        assert(0 && "failed to acquire mutex");

    for (i = 0; i < memkind_registry_g.num_kind; ++i) {
        kind = memkind_registry_g.partition_map[i];
        if (kind) {
            err = kind->ops->destroy(kind);
            if (err) {
                goto exit;
            }
            memkind_registry_g.partition_map[i] = NULL;
            if (i >= MEMKIND_NUM_BASE_KIND) {
                jemk_free(kind);
            }
        }
    }

exit:
    if (pthread_mutex_unlock(&memkind_registry_g.lock) != 0)
        assert(0 && "failed to release mutex");

    return err;
}

MEMKIND_EXPORT int memkind_get_num_kind(int *num_kind)
{
    *num_kind = memkind_registry_g.num_kind;
    return 0;
}

static int memkind_get_kind_by_partition_internal(int partition, struct memkind **kind)
{
    int err = 0;

    if (MEMKIND_LIKELY(partition >= 0 &&
        partition < MEMKIND_MAX_KIND &&
        memkind_registry_g.partition_map[partition] != NULL)) {
        *kind = memkind_registry_g.partition_map[partition];
    }
    else {
        *kind = NULL;
        err = MEMKIND_ERROR_UNAVAILABLE;
    }
    return err;
}

MEMKIND_EXPORT int memkind_get_kind_by_partition(int partition, struct memkind **kind)
{
    return memkind_get_kind_by_partition_internal(partition, kind);
}

MEMKIND_EXPORT int memkind_get_kind_by_name(const char *name, struct memkind **kind)
{
    int err = 0;
    int i;

    *kind = NULL;
    for (i = 0; i < memkind_registry_g.num_kind; ++i) {
        if (strcmp(name, memkind_registry_g.partition_map[i]->name) == 0) {
            *kind = memkind_registry_g.partition_map[i];
            break;
        }
    }
    if (*kind == NULL) {
        err = MEMKIND_ERROR_UNAVAILABLE;
    }
    return err;
}

MEMKIND_EXPORT void *memkind_partition_mmap(int partition, void *addr, size_t size)
{
    int err;
    struct memkind *kind;

    err = memkind_get_kind_by_partition_internal(partition, &kind);
    if (MEMKIND_UNLIKELY(err)) {
        return MAP_FAILED;
    }
    err = memkind_check_available(kind);
    if (MEMKIND_UNLIKELY(err)) {
        return MAP_FAILED;
    }
    return kind_mmap(kind, addr, size);
}

MEMKIND_EXPORT int memkind_check_available(struct memkind *kind)
{
    int err = 0;

    if (MEMKIND_LIKELY(kind->ops->check_available)) {
        err = kind->ops->check_available(kind);
    }
    return err;
}

MEMKIND_EXPORT void *memkind_malloc(struct memkind *kind, size_t size)
{
    void *result;

    pthread_once(&kind->init_once, kind->ops->init_once);

#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_malloc_pre) {
        memkind_malloc_pre(&kind, &size);
    }
#endif

    result = kind->ops->malloc(kind, size);

#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_malloc_post) {
        memkind_malloc_post(kind, size, &result);
    }
#endif

    return result;
}

MEMKIND_EXPORT void *memkind_calloc(struct memkind *kind, size_t num, size_t size)
{
    void *result;

    pthread_once(&kind->init_once, kind->ops->init_once);

#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_calloc_pre) {
        memkind_calloc_pre(&kind, &num, &size);
    }
#endif

    result = kind->ops->calloc(kind, num, size);

#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_calloc_post) {
        memkind_calloc_post(kind, num, size, &result);
    }
#endif

    return result;
}

MEMKIND_EXPORT int memkind_posix_memalign(struct memkind *kind, void **memptr, size_t alignment,
                           size_t size)
{
    int err;

    pthread_once(&kind->init_once, kind->ops->init_once);

#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_posix_memalign_pre) {
        memkind_posix_memalign_pre(&kind, memptr, &alignment, &size);
    }
#endif

    err = kind->ops->posix_memalign(kind, memptr, alignment, size);

#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_posix_memalign_post) {
        memkind_posix_memalign_post(kind, memptr, alignment, size, &err);
    }
#endif

    return err;
}

MEMKIND_EXPORT void *memkind_realloc(struct memkind *kind, void *ptr, size_t size)
{
    void *result;

    pthread_once(&kind->init_once, kind->ops->init_once);

#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_realloc_pre) {
        memkind_realloc_pre(&kind, &ptr, &size);
    }
#endif

    result = kind->ops->realloc(kind, ptr, size);

#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_realloc_post) {
        memkind_realloc_post(kind, ptr, size, &result);
    }
#endif

    return result;
}

MEMKIND_EXPORT void memkind_free(struct memkind *kind, void *ptr)
{
    if (!kind) {
        memkind_get_kind_for_free(ptr, &kind);
    }
    pthread_once(&kind->init_once, kind->ops->init_once);

#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_free_pre) {
        memkind_free_pre(&kind, &ptr);
    }
#endif

    kind->ops->free(kind, ptr);

#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_free_post) {
        memkind_free_post(kind, ptr);
    }
#endif
}

MEMKIND_EXPORT int memkind_get_size(memkind_t kind, size_t *total, size_t *free)
{
    return kind->ops->get_size(kind, total, free);
}

static inline int memkind_get_kind_for_free(void *ptr, struct memkind **kind)
{
    int i, num_kind;
    struct memkind *test_kind;

    *kind = MEMKIND_DEFAULT;
    num_kind = subregistry_size(&memkind_check_addr_subregistry_g);
    for (i = 0; i < num_kind; ++i) {
        test_kind = subregistry_get(&memkind_check_addr_subregistry_g, i);
        if (test_kind && test_kind->ops->check_addr(test_kind, ptr) == 0) {
            *kind = test_kind;
            break;
        }
    }
    return 0;
}

static int memkind_tmpfile(const char *dir, size_t size, int *fd, void **addr)
{
    static char template[] = "/memkind.XXXXXX";
    int err = 0;
    int oerrno;
    int dir_len = strlen(dir);

    if (dir_len > PATH_MAX) {
        return MEMKIND_ERROR_RUNTIME;
    }

    char fullname[dir_len + sizeof (template)];
    (void) strcpy(fullname, dir);
    (void) strcat(fullname, template);

    sigset_t set, oldset;
    sigfillset(&set);
    (void) sigprocmask(SIG_BLOCK, &set, &oldset);

    if ((*fd = mkstemp(fullname)) < 0) {
        err = MEMKIND_ERROR_RUNTIME;
        goto exit;
    }

    (void) unlink(fullname);
    (void) sigprocmask(SIG_SETMASK, &oldset, NULL);

    if (ftruncate(*fd, size) != 0) {
        err = MEMKIND_ERROR_RUNTIME;
        goto exit;
    }

    *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    if (*addr == MAP_FAILED) {
        err = MEMKIND_ERROR_RUNTIME;
        log_err("mmap() returned MAP_FAILED.");
        goto exit;
    }

    return err;

exit:
    oerrno = errno;
    (void) sigprocmask(SIG_SETMASK, &oldset, NULL);
    if (*fd != -1) {
        (void) close(*fd);
    }
    *fd = -1;
    *addr = NULL;
    errno = oerrno;
    return err;
}

MEMKIND_EXPORT int memkind_create_pmem(const char *dir, size_t max_size,
                        struct memkind **kind)
{
    int err = 0;
    int oerrno;

    size_t s = sizeof (Chunksize);

    if (Chunksize == 0) {
        err = jemk_mallctl("opt.lg_chunk", &Chunksize, &s, NULL, 0);
        if (err) {
            return MEMKIND_ERROR_RUNTIME;
        }
        Chunksize = 1 << Chunksize; /* 2^N */
    }

    if (max_size < MEMKIND_PMEM_MIN_SIZE) {
        return MEMKIND_ERROR_INVALID;
    }

    /* round up to a multiple of jemalloc chunk size */
    max_size = roundup(max_size, Chunksize);

    int fd = -1;
    void *addr;
    char name[16];

    err = memkind_tmpfile(dir, max_size, &fd, &addr);
    if (err) {
        goto exit;
    }

    snprintf(name, sizeof (name), "pmem%08x", fd);

    err = memkind_create(&MEMKIND_PMEM_OPS, name, kind);
    if (err) {
        goto exit;
    }

    void *aligned_addr = (void *)roundup((uintptr_t)addr, Chunksize);
    struct memkind_pmem *priv = (*kind)->priv;

    priv->fd = fd;
    priv->addr = addr;
    priv->max_size = max_size;
    priv->offset = (uintptr_t)aligned_addr - (uintptr_t)addr;

    return err;

exit:
    oerrno = errno;
    if (fd != -1) {
        (void) close(fd);
    }
    errno = oerrno;
    return err;
}
