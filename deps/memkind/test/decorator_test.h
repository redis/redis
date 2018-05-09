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

#include <stdlib.h>
#include <stdio.h>
#include <memkind.h>

struct decorators_flags {
    int malloc_pre;
    int malloc_post;
    int calloc_pre;
    int calloc_post;
    int posix_memalign_pre;
    int posix_memalign_post;
    int realloc_pre;
    int realloc_post;
    int free_pre;
    int free_post;
};

struct decorators_flags *decorators_state;

extern "C" {

    void memkind_malloc_pre(struct memkind *kind, size_t size)
    {
        decorators_state->malloc_pre++;
    }

    void memkind_malloc_post(struct memkind *kind, size_t size, void **result)
    {
        decorators_state->malloc_post++;
    }

    void memkind_calloc_pre(struct memkind *kind, size_t nmemb, size_t size)
    {
        decorators_state->calloc_pre++;
    }

    void memkind_calloc_post(struct memkind *kind, size_t nmemb, size_t size, void **result)
    {
        decorators_state->calloc_post++;
    }

    void memkind_posix_memalign_pre(struct memkind *kind, void **memptr, size_t alignment, size_t size)
    {
        decorators_state->posix_memalign_pre++;
    }

    void memkind_posix_memalign_post(struct memkind *kind, void **memptr, size_t alignment, size_t size, int *err)
    {
        decorators_state->posix_memalign_post++;
    }

    void memkind_realloc_pre(struct memkind *kind, void *ptr, size_t size)
    {
        decorators_state->realloc_pre++;
    }

    void memkind_realloc_post(struct memkind *kind, void *ptr, size_t size, void **result)
    {
        decorators_state->realloc_post++;
    }

    void memkind_free_pre(struct memkind **kind, void **ptr)
    {
        decorators_state->free_pre++;
    }

    void memkind_free_post(struct memkind **kind, void **ptr)
    {
        decorators_state->free_post++;
    }

}