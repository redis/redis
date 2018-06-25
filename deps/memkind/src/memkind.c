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
#define MEMKIND_VERSION_MAJOR 1
#define MEMKIND_VERSION_MINOR 6
#define MEMKIND_VERSION_PATCH 0

#include <memkind.h>
#include <memkind/internal/memkind_default.h>
#include <memkind/internal/memkind_hugetlb.h>
#include <memkind/internal/memkind_arena.h>
#include <memkind/internal/memkind_hbw.h>
#include <memkind/internal/memkind_regular.h>
#include <memkind/internal/memkind_gbtlb.h>
#include <memkind/internal/memkind_pmem.h>
#include <memkind/internal/memkind_interleave.h>
#include <memkind/internal/memkind_private.h>
#include <memkind/internal/memkind_log.h>
#include <memkind/internal/tbb_wrapper.h>
#include <memkind/internal/heap_manager.h>

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
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <jemalloc/jemalloc.h>

/* Clear bits in x, but only this specified in mask. */
#define CLEAR_BIT(x, mask) ((x) &= (~(mask)))

extern struct memkind_ops MEMKIND_HBW_GBTLB_OPS;
extern struct memkind_ops MEMKIND_HBW_PREFERRED_GBTLB_OPS;
extern struct memkind_ops MEMKIND_GBTLB_OPS;

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

static struct memkind MEMKIND_HBW_ALL_STATIC = {
    .ops = &MEMKIND_HBW_ALL_OPS,
    .partition = MEMKIND_PARTITION_HBW_ALL,
    .name = "memkind_hbw_all",
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

static struct memkind MEMKIND_HBW_ALL_HUGETLB_STATIC = {
    .ops = &MEMKIND_HBW_ALL_HUGETLB_OPS,
    .partition = MEMKIND_PARTITION_HBW_ALL_HUGETLB,
    .name = "memkind_hbw_all_hugetlb",
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

static struct memkind MEMKIND_REGULAR_STATIC = {
    .ops = &MEMKIND_REGULAR_OPS,
    .partition = MEMKIND_PARTITION_REGULAR,
    .name = "memkind_regular",
    .init_once = PTHREAD_ONCE_INIT,
};

MEMKIND_EXPORT struct memkind *MEMKIND_DEFAULT = &MEMKIND_DEFAULT_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HUGETLB = &MEMKIND_HUGETLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_INTERLEAVE = &MEMKIND_INTERLEAVE_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW = &MEMKIND_HBW_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_ALL = &MEMKIND_HBW_ALL_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_PREFERRED = &MEMKIND_HBW_PREFERRED_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_HUGETLB = &MEMKIND_HBW_HUGETLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_ALL_HUGETLB = &MEMKIND_HBW_ALL_HUGETLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_PREFERRED_HUGETLB = &MEMKIND_HBW_PREFERRED_HUGETLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_GBTLB = &MEMKIND_HBW_GBTLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_PREFERRED_GBTLB = &MEMKIND_HBW_PREFERRED_GBTLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_GBTLB = &MEMKIND_GBTLB_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_HBW_INTERLEAVE = &MEMKIND_HBW_INTERLEAVE_STATIC;
MEMKIND_EXPORT struct memkind *MEMKIND_REGULAR = &MEMKIND_REGULAR_STATIC;

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
        [MEMKIND_PARTITION_REGULAR] = &MEMKIND_REGULAR_STATIC,
        [MEMKIND_PARTITION_HBW_ALL] = &MEMKIND_HBW_ALL_STATIC,
        [MEMKIND_PARTITION_HBW_ALL_HUGETLB] = &MEMKIND_HBW_ALL_HUGETLB_STATIC,
    },
    MEMKIND_NUM_BASE_KIND,
    PTHREAD_MUTEX_INITIALIZER
};

void *kind_mmap(struct memkind *kind, void* addr, size_t size)
{
    if (MEMKIND_LIKELY(kind->ops->mmap == NULL)) {
        return memkind_default_mmap(kind, addr, size);
    }
    else {
        return kind->ops->mmap(kind, addr, size);
    }
}

static size_t Chunksize = 0;

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

struct create_args {
    memkind_t kind;
    memkind_policy_t policy;
    memkind_bits_t flags;
    memkind_memtype_t memtype_flags;
};

static struct create_args supported_args[] = {

    {&MEMKIND_HBW_STATIC,                    MEMKIND_POLICY_BIND_LOCAL,      0,                          MEMKIND_MEMTYPE_HIGH_BANDWIDTH},
    {&MEMKIND_HBW_HUGETLB_STATIC,            MEMKIND_POLICY_BIND_LOCAL,      MEMKIND_MASK_PAGE_SIZE_2MB, MEMKIND_MEMTYPE_HIGH_BANDWIDTH},
    {&MEMKIND_HBW_ALL_STATIC,                MEMKIND_POLICY_BIND_ALL,        0,                          MEMKIND_MEMTYPE_HIGH_BANDWIDTH},
    {&MEMKIND_HBW_ALL_HUGETLB_STATIC,        MEMKIND_POLICY_BIND_ALL,        MEMKIND_MASK_PAGE_SIZE_2MB, MEMKIND_MEMTYPE_HIGH_BANDWIDTH},
    {&MEMKIND_HBW_PREFERRED_STATIC,          MEMKIND_POLICY_PREFERRED_LOCAL, 0,                          MEMKIND_MEMTYPE_HIGH_BANDWIDTH},
    {&MEMKIND_HBW_PREFERRED_HUGETLB_STATIC,  MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, MEMKIND_MEMTYPE_HIGH_BANDWIDTH},
    {&MEMKIND_HBW_INTERLEAVE_STATIC,         MEMKIND_POLICY_INTERLEAVE_ALL,  0,                          MEMKIND_MEMTYPE_HIGH_BANDWIDTH},
    {&MEMKIND_DEFAULT_STATIC,                MEMKIND_POLICY_PREFERRED_LOCAL, 0,                          MEMKIND_MEMTYPE_DEFAULT},
    {&MEMKIND_HUGETLB_STATIC,                MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, MEMKIND_MEMTYPE_DEFAULT},
    {&MEMKIND_INTERLEAVE_STATIC,             MEMKIND_POLICY_INTERLEAVE_ALL,  0,                          MEMKIND_MEMTYPE_HIGH_BANDWIDTH | MEMKIND_MEMTYPE_DEFAULT},
};

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

    int i, num_supported_args = sizeof(supported_args) / sizeof(struct create_args);
    for(i = 0; i < num_supported_args; i++) {
        if((supported_args[i].memtype_flags == memtype_flags) &&
           (supported_args[i].policy == policy) &&
           (supported_args[i].flags == flags)) {

            if(memkind_check_available(supported_args[i].kind) == 0) {
                *kind = supported_args[i].kind;
                return MEMKIND_SUCCESS;
            } else if(policy == MEMKIND_POLICY_PREFERRED_LOCAL) {
                *kind = MEMKIND_DEFAULT;
                return MEMKIND_SUCCESS;
            }
            log_err("Cannot create kind: requested memory type is not available.");
            return MEMKIND_ERROR_MEMTYPE_NOT_AVAILABLE;
        }
    }

    log_err("Cannot create kind: unsupported set of capabilities.");
    return MEMKIND_ERROR_INVALID;
}

/* Kind destruction. */
MEMKIND_EXPORT int memkind_destroy_kind(memkind_t kind)
{
    return kind->ops->destroy(kind);
}

/* Declare weak symbols for allocator decorators */
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
    heap_manager_init(kind);
    if (check_numa) {
        int err = numa_available();
        if (err) {
            log_fatal("[%s] NUMA not available (error code:%d).", kind->name, err);
            abort();
        }
    }
}

static void nop(void) {}

static int memkind_create(struct memkind_ops *ops, const char *name, struct memkind **kind)
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
static int memkind_finalize(void)
{
    struct memkind *kind;
    int i;
    int err = 0;

    if (pthread_mutex_lock(&memkind_registry_g.lock) != 0)
        assert(0 && "failed to acquire mutex");

    for (i = 0; i < memkind_registry_g.num_kind; ++i) {
        kind = memkind_registry_g.partition_map[i];
        if (kind && kind->ops->finalize) {
            err = kind->ops->finalize(kind);
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
#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_free_pre) {
        memkind_free_pre(&kind, &ptr);
    }
#endif
    if (!kind) {
        heap_manager_free(kind, ptr);
    }
    else {
        pthread_once(&kind->init_once, kind->ops->init_once);
        kind->ops->free(kind, ptr);
    }

#ifdef MEMKIND_DECORATION_ENABLED
    if (memkind_free_post) {
        memkind_free_post(kind, ptr);
    }
#endif
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
