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

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

int main(int argc, char **argv)
{
    const size_t stream_len = 1024 * 1024;
    const size_t filter_len = 1024;
    const size_t num_filter = stream_len / filter_len;
    size_t i, j;
    double *stream = NULL;
    double *filter = NULL;
    double *result = NULL;

    srandom(0);

    stream = (double *)memkind_malloc(MEMKIND_DEFAULT, stream_len * sizeof(double));
    if (stream == NULL) {
        perror("<memkind>");
        fprintf(stderr, "Unable to allocate stream\n");
        return errno ? -errno : 1;
    }

    filter = (double *)memkind_malloc(MEMKIND_HBW, filter_len * sizeof(double));
    if (filter == NULL) {
        perror("<memkind>");
        fprintf(stderr, "Unable to allocate filter\n");
        return errno ? -errno : 1;
    }

    result = (double *)memkind_calloc(MEMKIND_HBW, filter_len, sizeof(double));
    if (result == NULL) {
        perror("<memkind>");
        fprintf(stderr, "Unable to allocate result\n");
        return errno ? -errno : 1;
    }

    for (i = 0; i < stream_len; i++) {
        stream[i] = (double)(random())/(double)(RAND_MAX);
    }

    for (i = 0; i < filter_len; i++) {
        filter[i] = (double)(i)/(double)(filter_len);
    }

    for (i = 0; i < num_filter; i++) {
        for (j = 0; j < filter_len; j++) {
            result[j] += stream[i * filter_len + j] * filter[j];
        }
    }

    for (i = 0; i < filter_len; i++) {
        fprintf(stdout, "%.6e\n", result[i]);
    }

    memkind_free(MEMKIND_HBW, result);
    memkind_free(MEMKIND_HBW, filter);
    memkind_free(MEMKIND_DEFAULT, stream);

    return 0;
}
