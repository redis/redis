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

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include "memkind.h"
#include "memkind/internal/memkind_default.h"

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

enum memkind_const_private {
    MEMKIND_NAME_LENGTH_PRIV = 64
};


struct memkind {
    const struct memkind_ops *ops;
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

// common function for registering kinds on creation or first use (in case of static kinds)
void memkind_register_kind(struct memkind *kind);

static inline void *kind_mmap(struct memkind *kind, void* addr, size_t size)
{
    if (MEMKIND_LIKELY(kind->ops->mmap == NULL)) {
        return memkind_default_mmap(kind, addr, size);
    }
    else {
        return kind->ops->mmap(kind, addr, size);
    }
}

#ifdef __cplusplus
}
#endif

