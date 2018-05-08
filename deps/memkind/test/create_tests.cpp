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
#include <memkind/internal/memkind_private.h>
#include <memkind/internal/memkind_default.h>

#include <gtest/gtest.h>


extern const struct memkind_ops MEMKIND_BAD_OPS[];
extern const struct memkind_ops MEMKIND_GOOD_OPS;
extern const size_t MEMKIND_BAD_OPS_LEN;
extern const struct memkind_ops deadbeef_ops;

/*These set of test cases are using defined ops (operations) structures to
 * validate the memkind_create API.
 */
class MemkindCreatePrivate: public :: testing :: Test { };

/* bad_ops -> tests a set of invalid operations */
TEST_F(MemkindCreatePrivate, test_TC_MEMKIND_CreateBadOps)
{
    size_t i;
    int err;
    memkind_t kind;
    for (i = 0; i < MEMKIND_BAD_OPS_LEN; ++i) {
        err = memkind_create(MEMKIND_BAD_OPS + i, "bad_ops", &kind);
        EXPECT_TRUE(err == MEMKIND_ERROR_BADOPS);
        EXPECT_TRUE(kind == NULL);
    }
}

/* rep_name-> test will verify that memkind does not allows to add a repeated
 * kind name
 */
TEST_F(MemkindCreatePrivate, test_TC_MEMKIND_CreateRepName)
{
    int i, err;
    int num_bad_ops = sizeof(*MEMKIND_BAD_OPS)/sizeof(memkind_ops);
    memkind_t kind;
    for (i = 0; i < num_bad_ops; ++i) {
        err = memkind_create(&MEMKIND_GOOD_OPS, "memkind_default", &kind);
        EXPECT_TRUE(err == MEMKIND_ERROR_INVALID);
        EXPECT_TRUE(kind == NULL);
    }
}

/* partitions-> will verify that a user can defined its own way to treat mmaps as
 * defined in its ops list
 */
TEST_F(MemkindCreatePrivate, test_TC_MEMKIND_CreatePartitions)
{
    int res;
    size_t SIZE = 8*1024*1024;
    memkind_t deadbeef_kind;
    void *buffer = NULL;

    res = memkind_create(&deadbeef_ops, "deadbeef_ops", &deadbeef_kind);
    ASSERT_EQ(res, 0);
    ASSERT_FALSE(deadbeef_kind == NULL);

    buffer = memkind_malloc(MEMKIND_DEFAULT, SIZE);
    memkind_free(MEMKIND_DEFAULT, buffer);
    buffer = memkind_malloc(deadbeef_kind, SIZE);
    EXPECT_EQ(*((unsigned int*)buffer), 0xDEADBEEF);
}
