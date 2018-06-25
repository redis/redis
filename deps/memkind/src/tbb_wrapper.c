/*
 * Copyright (C) 2017 Intel Corporation.
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

#include <memkind/internal/memkind_default.h>
#include <memkind/internal/memkind_log.h>
#include <memkind/internal/memkind_private.h>
#include <memkind/internal/tbb_wrapper.h>
#include <memkind/internal/tbb_mem_pool_policy.h>
#include <limits.h>

#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>

void *(*pool_malloc)(void*, size_t);
void *(*pool_realloc)(void*, void *, size_t);
void *(*pool_aligned_malloc)(void*, size_t, size_t);
bool (*pool_free)(void*, void *);
int (*pool_create_v1)(intptr_t, const struct MemPoolPolicy*, void**);
bool (*pool_destroy)(void*);
void* (*pool_identify)(void *object);

static void* tbb_handle = NULL;

static int load_tbb_symbols()
{
    const char so_name[]="libtbbmalloc.so.2";
    tbb_handle = dlopen(so_name, RTLD_LAZY);
    if(!tbb_handle) {
        log_err("%s not found.", so_name);
        return -1;
    }

    pool_malloc = dlsym(tbb_handle, "_ZN3rml11pool_mallocEPNS_10MemoryPoolEm");
    pool_realloc = dlsym(tbb_handle, "_ZN3rml12pool_reallocEPNS_10MemoryPoolEPvm");
    pool_aligned_malloc = dlsym(tbb_handle, "_ZN3rml19pool_aligned_mallocEPNS_10MemoryPoolEmm");
    pool_free = dlsym(tbb_handle, "_ZN3rml9pool_freeEPNS_10MemoryPoolEPv");
    pool_create_v1 = dlsym(tbb_handle, "_ZN3rml14pool_create_v1ElPKNS_13MemPoolPolicyEPPNS_10MemoryPoolE");
    pool_destroy = dlsym(tbb_handle, "_ZN3rml12pool_destroyEPNS_10MemoryPoolE");
    pool_identify = dlsym(tbb_handle, "_ZN3rml13pool_identifyEPv");

    if(!pool_malloc ||
        !pool_realloc ||
        !pool_aligned_malloc ||
        !pool_free ||
        !pool_create_v1 ||
        !pool_destroy ||
        !pool_identify)

    {
        log_err("Could not find symbols in %s.", so_name);
        dlclose(tbb_handle);
        return -1;
    }

    return 0;
}

//Granularity of raw_alloc allocations
#define GRANULARITY 2*1024*1024
static void *raw_alloc(intptr_t pool_id, size_t* bytes/*=n*GRANULARITY*/)
{
   void* ptr = kind_mmap((struct memkind*)pool_id, NULL, *bytes);
   return (ptr==MAP_FAILED) ? NULL : ptr;
}

static int raw_free(intptr_t pool_id, void* raw_ptr, size_t raw_bytes)
{
  return munmap(raw_ptr, raw_bytes);
}

static void *tbb_pool_malloc(struct memkind* kind, size_t size)
{
    if(size_out_of_bounds(size)) return NULL;
    void* result = pool_malloc(kind->priv, size);
    if (!result)
        errno = ENOMEM;
    return result;
}

static void *tbb_pool_calloc(struct memkind *kind, size_t num, size_t size)
{
    if (size_out_of_bounds(num) || size_out_of_bounds(size)) return NULL;

    const size_t array_size = num*size;
    if (array_size/num != size) {
        errno = ENOMEM;
        return NULL;
    }
    void *result = pool_malloc(kind->priv, array_size);
    if (result) {
        memset(result, 0, array_size);
    }
    else {
        errno = ENOMEM;
    }
    return result;
}

static void *tbb_pool_realloc(struct memkind *kind, void *ptr, size_t size)
{
    if(size_out_of_bounds(size)) return NULL;
    void *result = pool_realloc(kind->priv, ptr, size);
    if (!result && size)
        errno = ENOMEM;
    return result;
}

static int tbb_pool_posix_memalign(struct memkind *kind, void **memptr, size_t alignment, size_t size)
{
    //Check if alignment is "at least as large as sizeof(void *)".
    if(!alignment && (0 != (alignment & (alignment-sizeof(void*))))) return EINVAL;
    //Check if alignment is "a power of 2".
    if(alignment & (alignment-1)) return EINVAL;
    if(size_out_of_bounds(size)) return ENOMEM;
    void *result = pool_aligned_malloc(kind->priv, size, alignment);
    if (!result) {
        return ENOMEM;
    }
    *memptr = result;
    return 0;
}

void tbb_pool_free(struct memkind *kind, void *ptr)
{
    if(kind) {
        pool_free(kind->priv, ptr);
    } else {
        pool_free(pool_identify(ptr), ptr);
    }
}

static int tbb_destroy(struct memkind* kind)
{
    bool pool_destroy_ret = pool_destroy(kind->priv);
    dlclose(tbb_handle);

    if(!pool_destroy_ret) {
        log_err("TBB pool destroy failure.");
        return MEMKIND_ERROR_OPERATION_FAILED;
    }
    return MEMKIND_SUCCESS;
}

void tbb_initialize(struct memkind *kind)
{
    if(!kind || load_tbb_symbols()) {
        log_fatal("Failed to initialize TBB.");
        abort();
    }

    struct MemPoolPolicy policy = {
        .pAlloc = raw_alloc,
        .pFree = raw_free,
        .granularity = GRANULARITY,
        .version = 1,
        .fixedPool = false,
        .keepAllMemory = false,
        .reserved = 0
    };

    pool_create_v1((intptr_t)kind, &policy, &kind->priv);
    if (!kind->priv) {
        log_fatal("Unable to create TBB memory pool.");
        abort();
    }

    kind->ops->malloc = tbb_pool_malloc;
    kind->ops->calloc = tbb_pool_calloc;
    kind->ops->posix_memalign = tbb_pool_posix_memalign;
    kind->ops->realloc = tbb_pool_realloc;
    kind->ops->free = tbb_pool_free;
    kind->ops->finalize = tbb_destroy;
}
