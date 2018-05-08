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

#include <hbwmalloc.h>

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

int main(int argc, char **argv)
{
    const size_t size = 512;
    char *default_str = NULL;
    char *hbw_str = NULL;
    char *hbw_hugetlb_str = NULL;
    int err = 0;

    default_str = (char *)malloc(size);
    if (default_str == NULL) {
        perror("malloc()");
        fprintf(stderr, "Unable to allocate default string\n");
        err = errno ? -errno : 1;
        goto exit;
    }
    hbw_str = (char *)hbw_malloc(size);
    if (hbw_str == NULL) {
        perror("hbw_malloc()");
        fprintf(stderr, "Unable to allocate hbw string\n");
        err = errno ? -errno : 1;
        goto exit;
    }
    err = hbw_posix_memalign_psize((void **)&hbw_hugetlb_str, 2097152, size, HBW_PAGESIZE_2MB);
    if (err) {
        perror("hbw_posix_memalign()");
        fprintf(stderr, "Unable to allocate hbw hugetlb string\n");
        err = errno ? -errno : 1;
        goto exit;
    }

    sprintf(default_str, "Hello world from standard memory\n");
    sprintf(hbw_str, "Hello world from high bandwidth memory\n");
    sprintf(hbw_hugetlb_str, "Hello world from high bandwidth 2 MB paged memory\n");

    fprintf(stdout, "%s", default_str);
    fprintf(stdout, "%s", hbw_str);
    fprintf(stdout, "%s", hbw_hugetlb_str);

exit:
    if (hbw_hugetlb_str) {
        hbw_free(hbw_hugetlb_str);
    }
    if (hbw_str) {
        hbw_free(hbw_str);
    }
    if (default_str) {
        free(default_str);
    }
    return err;
}
