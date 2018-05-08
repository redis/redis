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

#include <stdio.h>
#include <string.h>


int main (int argc, char *argv[])
{
    void *ptr;
    int err = 0;

    hbw_set_policy(HBW_POLICY_BIND);

    printf ("Allocating 400 KB with 1GB pages\n");
    err = hbw_posix_memalign_psize (&ptr,
                                    1073741824,
                                    409600,
                                    HBW_PAGESIZE_1GB);

    if (err) {
        fprintf(stderr, "ERROR: hbw_posix_memalign_size()\n");
    }
    if (!err) {
        memset (ptr, 0, 409600);
        printf ("Reallocing 100KB with 1GB pages\n");
        ptr = hbw_realloc (ptr, 102400);
        if (ptr == NULL) {
            fprintf(stderr, "ERROR: hbw_realloc()\n");
            err = -1;
        }
    }
    if (!err) {
        memset (ptr, 0, 102400);
        printf ("Reallocing 1GB with 1GB pages\n");
        ptr = hbw_realloc (ptr, 1073741824);
        if (ptr == NULL) {
            fprintf(stderr, "ERROR: hbw_realloc()");
            err = -1;
        }
    }
    if (!err) {
        memset (ptr, 0, 1073741824);
        printf ("Reallocing 1073742848 with 1GB pages\n");
        ptr = hbw_realloc (ptr, 1073742848);
        if (ptr == NULL) {
            fprintf(stderr, "ERROR:  hbw_realloc()\n");
            err = -1;
        }
    }
    if (!err) {
        memset (ptr, 0, 1073742848);
    }
    if (ptr) {
        hbw_free(ptr);
    }

    return err;
}
