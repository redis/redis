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

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <memkind.h>
#include <jemalloc/jemalloc.h>

/*
 * Header file for the jemalloc arena allocation memkind operations.
 * More details in memkind_arena(3) man page.
 *
 * Functionality defined in this header is considered as EXPERIMENTAL API.
 * API standards are described in memkind(3) man page.
 */

struct memkind *get_kind_by_arena(unsigned arena_ind);

int memkind_arena_create(struct memkind *kind, const struct memkind_ops *ops, const char *name);
int memkind_arena_create_map(struct memkind *kind, chunk_hooks_t *hooks);
int memkind_arena_destroy(struct memkind *kind);
void *memkind_arena_malloc(struct memkind *kind, size_t size);
void *memkind_arena_calloc(struct memkind *kind, size_t num, size_t size);
int memkind_arena_posix_memalign(struct memkind *kind, void **memptr, size_t alignment, size_t size);
void *memkind_arena_realloc(struct memkind *kind, void *ptr, size_t size);
int memkind_bijective_get_arena(struct memkind *kind, unsigned int *arena, size_t size);
int memkind_thread_get_arena(struct memkind *kind, unsigned int *arena, size_t size);
#ifdef __cplusplus
}
#endif
