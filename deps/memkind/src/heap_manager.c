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

#include <memkind/internal/heap_manager.h>
#include <memkind/internal/tbb_wrapper.h>
#include <memkind/internal/memkind_arena.h>

#include <stdio.h>
#include <pthread.h>
#include <string.h>

static struct heap_manager_ops* heap_manager_g;

pthread_once_t heap_manager_init_once_g = PTHREAD_ONCE_INIT;

struct heap_manager_ops {
    void (*init)(struct memkind* kind);
    void (*heap_manager_free)(struct memkind* kind, void* ptr);
};

struct heap_manager_ops arena_heap_manager_g = {
    .init = memkind_arena_init,
    .heap_manager_free = memkind_arena_free
};

struct heap_manager_ops tbb_heap_manager_g = {
    .init = tbb_initialize,
    .heap_manager_free = tbb_pool_free
};

static void set_heap_manager()
{
    heap_manager_g = &arena_heap_manager_g;
    const char* env = getenv("MEMKIND_HEAP_MANAGER");
    if(env && strcmp(env, "TBB") == 0) {
        heap_manager_g = &tbb_heap_manager_g;
    }
}

static inline struct heap_manager_ops* get_heap_manager()
{
    pthread_once(&heap_manager_init_once_g, set_heap_manager);
    return heap_manager_g;
}

void heap_manager_init(struct memkind *kind)
{
    get_heap_manager()->init(kind);
}

void heap_manager_free(struct memkind *kind, void* ptr)
{
    get_heap_manager()->heap_manager_free(kind, ptr);
}
