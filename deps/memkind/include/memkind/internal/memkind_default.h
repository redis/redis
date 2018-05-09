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

/*
 * Header file for the default implementations for memkind operations.
 * More details in memkind_default(3) man page.
 *
 * Functionality defined in this header is considered as EXPERIMENTAL API.
 * API standards are described in memkind(3) man page.
 */

int memkind_default_create(struct memkind *kind, const struct memkind_ops *ops, const char *name);
int memkind_default_destroy(struct memkind *kind);
void *memkind_default_malloc(struct memkind *kind, size_t size);
void *memkind_default_calloc(struct memkind *kind, size_t num, size_t size);
int memkind_default_posix_memalign(struct memkind *kind, void **memptr, size_t alignment, size_t size);
void *memkind_default_realloc(struct memkind *kind, void *ptr, size_t size);
void memkind_default_free(struct memkind *kind, void *ptr);
void *memkind_default_mmap(struct memkind *kind, void *addr, size_t size);
int memkind_default_mbind(struct memkind *kind, void *ptr, size_t size);
int memkind_default_get_mmap_flags(struct memkind *kind, int *flags);
int memkind_default_get_mbind_mode(struct memkind *kind, int *mode);
int memkind_default_get_mbind_nodemask(struct memkind *kind, unsigned long *nodemask, unsigned long maxnode);
int memkind_preferred_get_mbind_mode(struct memkind *kind, int *mode);
int memkind_interleave_get_mbind_mode(struct memkind *kind, int *mode);
int memkind_nohugepage_madvise(struct memkind *kind, void *addr, size_t size);
int memkind_default_get_size(struct memkind *kind, size_t *total, size_t *free);
int memkind_posix_check_alignment(struct memkind *kind, size_t alignment);
void memkind_default_init_once(void);

extern const struct memkind_ops MEMKIND_DEFAULT_OPS;

#ifdef __cplusplus
}
#endif
