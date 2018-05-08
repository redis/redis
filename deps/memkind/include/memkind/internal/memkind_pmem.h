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

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MEMKIND_INTERNAL_API
#warning "DO NOT INCLUDE THIS FILE! IT IS INTERNAL MEMKIND API AND SOON WILL BE REMOVED FROM BIN & DEVEL PACKAGES"
#endif

#include <memkind.h>
#include "memkind_default.h"
#include "memkind_arena.h"

#include <pthread.h>

/*
 * Header file for the file-backed memory memkind operations.
 * More details in memkind_pmem(3) man page.
 *
 * Functionality defined in this header is considered as EXPERIMENTAL API.
 * API standards are described in memkind(3) man page.
 */

#define	MEMKIND_PMEM_MIN_SIZE (1024 * 1024 * 16)

int memkind_pmem_create(struct memkind *kind, const struct memkind_ops *ops, const char *name);
int memkind_pmem_destroy(struct memkind *kind);
void *memkind_pmem_mmap(struct memkind *kind, void *addr, size_t size);
int memkind_pmem_get_mmap_flags(struct memkind *kind, int *flags);
int memkind_pmem_get_size(struct memkind *kind, size_t *total, size_t *free);

struct memkind_pmem {
    int fd;
    void *addr;
    off_t offset;
    size_t max_size;
    pthread_mutex_t pmem_lock;
};

extern const struct memkind_ops MEMKIND_PMEM_OPS;

#ifdef __cplusplus
}
#endif
