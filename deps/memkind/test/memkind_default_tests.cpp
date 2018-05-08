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

#include "memkind.h"

#include <numa.h>
#include <stdio.h>

#include "common.h"

/* Calling the basic APIs (calloc, realloc and get_size) with
 * MEMKIND_DEFAULT kind.
 */
class MemkindDefaultTests: public :: testing::Test
{

protected:
    void SetUp()
    {}

    void TearDown()
    {}

};

TEST_F(MemkindDefaultTests, test_TC_MEMKIND_DefaultCalloc)
{
    const size_t size = 1024;
    const size_t num = 1;
    char *default_str = NULL;

    default_str = (char *)memkind_calloc(MEMKIND_DEFAULT, num, size);
    EXPECT_TRUE(NULL != default_str);

    sprintf(default_str, "memkind_calloc MEMKIND_DEFAULT\n");
    printf("%s", default_str);

    memkind_free(MEMKIND_DEFAULT, default_str);
}

TEST_F(MemkindDefaultTests, test_TC_MEMKIND_DefaultGetSize)
{
    const size_t size = 512;
    char *default_str = NULL;
    int err = 0;
    size_t *total, *free;
    total = (size_t*) malloc(sizeof(size_t));
    free = (size_t*) malloc(sizeof(size_t));

    default_str = (char *)memkind_malloc(MEMKIND_DEFAULT, size);
    EXPECT_TRUE(NULL != default_str);

    err = memkind_get_size(MEMKIND_DEFAULT, total, free);

    EXPECT_EQ(0,err);

    memkind_free(MEMKIND_DEFAULT, default_str);
}

TEST_F(MemkindDefaultTests, test_TC_MEMKIND_DefaultRealloc)
{
    const size_t size1 = 512;
    const size_t size2 = 1024;
    char *default_str = NULL;

    default_str = (char *)memkind_realloc(MEMKIND_DEFAULT, default_str, size1);
    EXPECT_TRUE(NULL != default_str);

    sprintf(default_str, "memkind_realloc MEMKIND_DEFAULT with size %zu\n", size1);
    printf("%s", default_str);

    default_str = (char *)memkind_realloc(MEMKIND_DEFAULT, default_str, size2);
    EXPECT_TRUE(NULL != default_str);

    sprintf(default_str, "memkind_realloc MEMKIND_DEFAULT with size %zu\n", size2);
    printf("%s", default_str);

    memkind_free(MEMKIND_DEFAULT, default_str);
}
