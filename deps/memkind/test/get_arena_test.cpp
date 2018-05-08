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

#include <memkind/internal/memkind_arena.h>

#include <algorithm>
#include <vector>
#include <gtest/gtest.h>
#include <omp.h>
#include <pthread.h>

class GetArenaTest: public :: testing::Test
{

protected:
    void SetUp()
    {}

    void TearDown()
    {}

};

bool uint_comp(unsigned int a, unsigned int b)
{
    return (a < b);
}

TEST_F(GetArenaTest, test_TC_MEMKIND_ThreadHash)
{
    int num_threads = omp_get_max_threads();
    std::vector<unsigned int> arena_idx(num_threads);
    unsigned int thread_idx, idx;
    int err = 0;
    size_t size = 0;
    int i;
    unsigned max_collisions, collisions;
    const unsigned collisions_limit = 5;

    //Initialize kind
    memkind_malloc(MEMKIND_HBW, 0);

    #pragma omp parallel shared(arena_idx) private(thread_idx)
    {
        thread_idx = omp_get_thread_num();
        err = memkind_thread_get_arena(MEMKIND_HBW, &(arena_idx[thread_idx]), size);
    }
    ASSERT_TRUE(err == 0);
    std::sort(arena_idx.begin(), arena_idx.end(), uint_comp);
    idx = arena_idx[0];
    collisions = 0;
    max_collisions = 0;
    for (i = 1; i < num_threads; ++i) {
        if (arena_idx[i] == idx) {
            collisions++;
        }
        else {
            if (collisions > max_collisions) {
                max_collisions = collisions;
            }
            idx = arena_idx[i];
            collisions = 0;
        }
    }
    EXPECT_LE(max_collisions, collisions_limit);
    RecordProperty("max_collisions", max_collisions);
}
