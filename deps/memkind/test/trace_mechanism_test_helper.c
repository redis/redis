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
#include "memkind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
    int err = 0;
    const size_t size = 1024 * 1024;
    void *buf = NULL;

    //It is expected that "MEMKIND_HBW" or "MEMKIND_HUGETLB" argument is passed
    if (argc != 2)
    {
        printf("Error: Wrong number of parameters\n");
        err = -1;
        return err;
    }

    if (strcmp(argv[1], "MEMKIND_HBW") == 0)
    {
        buf = memkind_malloc(MEMKIND_HBW, size);
        if (buf == NULL)
        {
            printf("Allocation of MEMKIND_HBW failed\n");
            err = -1;
        }
        memkind_free(MEMKIND_HBW, buf);
    }
    else if (strcmp(argv[1], "MEMKIND_HUGETLB") == 0)
    {
        buf = memkind_malloc(MEMKIND_HUGETLB, size);
        if (buf == NULL)
        {
            printf("Allocation of MEMKIND_HUGETLB failed\n");
            err = -1;
        }
        memkind_free(MEMKIND_HUGETLB, buf);
    }
    else
    {
        printf("Error: unknown parameter\n");
        err = -1;
    }

    return err;
}