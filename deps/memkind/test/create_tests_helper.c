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
#include <memkind/internal/memkind_hbw.h>

#include <stdio.h>
#include <sys/mman.h>

void *mmap_deadbeef(struct memkind *kind, void *addr, size_t size);

const struct memkind_ops MEMKIND_BAD_OPS[] = {{
        .create = NULL,
        .destroy = memkind_default_destroy,
        .malloc = memkind_default_malloc,
        .calloc = memkind_default_calloc,
        .posix_memalign = memkind_default_posix_memalign,
        .realloc = memkind_default_realloc,
        .free = memkind_default_free,
        .get_size = memkind_default_get_size
    }, {
        .create = memkind_default_create,
        .destroy = NULL,
        .malloc = memkind_default_malloc,
        .calloc = memkind_default_calloc,
        .posix_memalign = memkind_default_posix_memalign,
        .realloc = memkind_default_realloc,
        .free = memkind_default_free,
        .get_size = memkind_default_get_size
    }, {
        .create = memkind_default_create,
        .destroy = memkind_default_destroy,
        .malloc = NULL,
        .calloc = memkind_default_calloc,
        .posix_memalign = memkind_default_posix_memalign,
        .realloc = memkind_default_realloc,
        .free = memkind_default_free,
        .get_size = memkind_default_get_size
    }, {
        .create = memkind_default_create,
        .destroy = memkind_default_destroy,
        .malloc = memkind_default_malloc,
        .calloc = NULL,
        .posix_memalign = memkind_default_posix_memalign,
        .realloc = memkind_default_realloc,
        .free = memkind_default_free,
        .get_size = memkind_default_get_size
    }, {
        .create = memkind_default_create,
        .destroy = memkind_default_destroy,
        .malloc = memkind_default_malloc,
        .calloc = memkind_default_calloc,
        .posix_memalign = NULL,
        .realloc = memkind_default_realloc,
        .free = memkind_default_free,
        .get_size = memkind_default_get_size
    }, {
        .create = memkind_default_create,
        .destroy = memkind_default_destroy,
        .malloc = memkind_default_malloc,
        .calloc = memkind_default_calloc,
        .posix_memalign = memkind_default_posix_memalign,
        .realloc = NULL,
        .free = memkind_default_free,
        .get_size = memkind_default_get_size
    }, {
        .create = memkind_default_create,
        .destroy = memkind_default_destroy,
        .malloc = memkind_default_malloc,
        .calloc = memkind_default_calloc,
        .posix_memalign = memkind_default_posix_memalign,
        .realloc = memkind_default_realloc,
        .free = NULL,
        .get_size = memkind_default_get_size
    }, {
        .create = memkind_default_create,
        .destroy = memkind_default_destroy,
        .malloc = memkind_default_malloc,
        .calloc = memkind_default_calloc,
        .posix_memalign = memkind_default_posix_memalign,
        .realloc = memkind_default_realloc,
        .free = memkind_default_free,
        .get_size = NULL
    }, {
        .create = memkind_default_create,
        .destroy = memkind_default_destroy,
        .malloc = memkind_default_malloc,
        .calloc = memkind_default_calloc,
        .posix_memalign = memkind_default_posix_memalign,
        .realloc = memkind_default_realloc,
        .free = memkind_default_free,
        .get_size = memkind_default_get_size,
        .init_once = memkind_hbw_init_once
    }
};

const struct memkind_ops MEMKIND_GOOD_OPS = {
    .create = memkind_default_create,
    .destroy = memkind_default_destroy,
    .malloc = memkind_default_malloc,
    .calloc = memkind_default_calloc,
    .posix_memalign = memkind_default_posix_memalign,
    .realloc = memkind_default_realloc,
    .free = memkind_default_free,
    .get_size = memkind_default_get_size,
};

const size_t MEMKIND_BAD_OPS_LEN = sizeof(MEMKIND_BAD_OPS)/sizeof(struct memkind_ops);

const struct memkind_ops deadbeef_ops = {
    .create = memkind_arena_create,
    .destroy = memkind_arena_destroy,
    .malloc = memkind_arena_malloc,
    .calloc = memkind_arena_calloc,
    .posix_memalign = memkind_arena_posix_memalign,
    .realloc = memkind_default_realloc,
    .free = memkind_default_free,
    .mmap = mmap_deadbeef,
    .get_arena = memkind_bijective_get_arena,
    .get_size = memkind_default_get_size
};

void *mmap_deadbeef(struct memkind *kind, void *addr, size_t size)
{
    int i;
    void *buffer = mmap(addr, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    printf("Hello from mmap_deadbeef\n");

    for(i=0; i< size/sizeof(int); ++i) {
        ((int *)buffer) [i] = 0xDEADBEEF;
    }
    return buffer;
}
