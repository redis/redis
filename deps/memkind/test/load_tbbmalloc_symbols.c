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

#include "tbbmalloc.h"

int load_tbbmalloc_symbols()
{
    const char so_name[]="libtbbmalloc.so.2";
    void* tbb_handle = dlopen(so_name, RTLD_LAZY);
    if(!tbb_handle) {
       printf("Cannot load %s\n", so_name);
       return -1;
    }

    scalable_malloc = dlsym(tbb_handle, "scalable_malloc");
    if(!scalable_malloc) {
        printf("Cannot load scalable_malloc symbol from %s\n", so_name);
        return -1;
    }

    scalable_realloc = dlsym(tbb_handle, "scalable_realloc");
    if(!scalable_realloc) {
        printf("Cannot load scalable_realloc symbol from %s\n", so_name);
        return -1;
    }

    scalable_calloc = dlsym(tbb_handle, "scalable_calloc");
    if(!scalable_calloc) {
        printf("Cannot load scalable_calloc symbol from %s\n", so_name);
        return -1;
    }

    scalable_free = dlsym(tbb_handle, "scalable_free");
    if(!scalable_free) {
        printf("Cannot load scalable_free symbol from %s\n", so_name);
        return -1;
    }

    return 0;
}
