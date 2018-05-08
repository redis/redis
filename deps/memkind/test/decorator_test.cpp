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

#include "common.h"
#include "decorator_test.h"
#include "config.h"

size_t size = 16;
memkind_t kind = MEMKIND_DEFAULT;

class DecoratorTest: public :: testing::Test
{

protected:
    void SetUp()
    {
        decorators_state = new decorators_flags();
    }

    void TearDown()
    {
        free(decorators_state);
    }
};

TEST_F(DecoratorTest, test_TC_MEMKIND_DT_malloc)
{
#ifdef MEMKIND_DECORATION_ENABLED
    void *buffer = memkind_malloc(kind, size);

    ASSERT_TRUE(buffer != NULL);
    EXPECT_EQ(1, decorators_state->malloc_pre);
    EXPECT_EQ(1, decorators_state->malloc_post);

    memkind_free(0, buffer);
#endif
}

TEST_F(DecoratorTest, test_TC_MEMKIND_DT_calloc)
{
#ifdef MEMKIND_DECORATION_ENABLED
    void *buffer = memkind_calloc(kind, 1, size);

    ASSERT_TRUE(buffer != NULL);
    EXPECT_EQ(1, decorators_state->calloc_pre);
    EXPECT_EQ(1, decorators_state->calloc_post);

    memkind_free(0, buffer);
#endif
}

TEST_F(DecoratorTest, test_TC_MEMKIND_DT_posix_memalign)
{
#ifdef MEMKIND_DECORATION_ENABLED
    void *buffer;

    int res = memkind_posix_memalign(kind, &buffer, 8, size);

    ASSERT_TRUE(buffer != NULL);
    ASSERT_EQ(0, res);
    EXPECT_EQ(1, decorators_state->posix_memalign_pre);
    EXPECT_EQ(1, decorators_state->posix_memalign_post);

    memkind_free(0, buffer);
#endif
}

TEST_F(DecoratorTest, test_TC_MEMKIND_DT_realloc)
{
#ifdef MEMKIND_DECORATION_ENABLED
    void *buffer = memkind_realloc(kind, NULL, size);

    ASSERT_TRUE(buffer != NULL);
    EXPECT_EQ(1, decorators_state->realloc_pre);
    EXPECT_EQ(1, decorators_state->realloc_post);

    memkind_free(0, buffer);
#endif
}

TEST_F(DecoratorTest, test_TC_MEMKIND_DT_free)
{
#ifdef MEMKIND_DECORATION_ENABLED
    void *buffer = memkind_malloc(kind, size);

    ASSERT_TRUE(buffer != NULL);

    memkind_free(0, buffer);

    EXPECT_EQ(1, decorators_state->free_pre);
    EXPECT_EQ(1, decorators_state->free_post);
#endif
}