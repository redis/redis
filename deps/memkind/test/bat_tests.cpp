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

#include <fstream>
#include <algorithm>

#include "common.h"
#include "check.h"
#include "omp.h"
#include "trial_generator.h"

/*
 * Set of basic acceptance tests.
 */
class BATest: public TGTest
{
};

void test_allocation(memkind_t kind, size_t size)
{
    ASSERT_TRUE(kind != NULL);
    void* ptr = memkind_malloc(kind, size);
    ASSERT_TRUE(ptr != NULL);
    void* memset_ret = memset(ptr, 3, size);
    ASSERT_TRUE(memset_ret != NULL);
    memkind_free(kind, ptr);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_DEFAULT_PREFERRED_LOCAL)
{
    memkind_t kind = NULL;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        MEMKIND_POLICY_PREFERRED_LOCAL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);

    test_allocation(kind, 4096);

    ret = memkind_destroy_kind(kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_DEFAULT_PREFERRED_LOCAL_PAGE_SIZE_2MB)
{
    memkind_t kind = NULL;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        MEMKIND_POLICY_PREFERRED_LOCAL,
        MEMKIND_MASK_PAGE_SIZE_2MB,
        &kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);

    test_allocation(kind, 4096);

    ret = memkind_destroy_kind(kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_BIND_LOCAL)
{
    memkind_t kind = NULL;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_HIGH_BANDWIDTH,
        MEMKIND_POLICY_BIND_LOCAL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);

    test_allocation(kind, 4096);

    ret = memkind_destroy_kind(kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);
}

TEST_F(BATest, test_TC_MEMKIND_2MBPages_malloc_HIGH_BANDWIDTH_BIND_LOCAL_PAGE_SIZE_2MB)
{
    memkind_t kind = NULL;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_HIGH_BANDWIDTH,
        MEMKIND_POLICY_BIND_LOCAL,
        MEMKIND_MASK_PAGE_SIZE_2MB,
        &kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);

    test_allocation(kind, 4096);

    ret = memkind_destroy_kind(kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_PREFERRED_LOCAL)
{
    memkind_t kind = NULL;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_HIGH_BANDWIDTH,
        MEMKIND_POLICY_PREFERRED_LOCAL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);

    test_allocation(kind, 4096);

    ret = memkind_destroy_kind(kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_PAGE_SIZE_2MB)
{
    memkind_t kind = NULL;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_HIGH_BANDWIDTH,
        MEMKIND_POLICY_PREFERRED_LOCAL,
        MEMKIND_MASK_PAGE_SIZE_2MB,
        &kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);

    test_allocation(kind, 4096);

    ret = memkind_destroy_kind(kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_INTERLEAVE_ALL)
{
    memkind_t kind = NULL;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_HIGH_BANDWIDTH,
        MEMKIND_POLICY_INTERLEAVE_ALL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);

    test_allocation(kind, 4096);

    ret = memkind_destroy_kind(kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_DEFAULT_HIGH_BANDWIDTH_INTERLEAVE_ALL)
{
    memkind_t kind = NULL;
    int flags_tmp = MEMKIND_MEMTYPE_DEFAULT | MEMKIND_MEMTYPE_HIGH_BANDWIDTH;
    memkind_memtype_t memtype_flags;
    memcpy(&memtype_flags, &flags_tmp, sizeof(memtype_flags));

    int ret = memkind_create_kind(
        memtype_flags,
        MEMKIND_POLICY_INTERLEAVE_ALL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);

    test_allocation(kind, 4096);

    ret = memkind_destroy_kind(kind);
    ASSERT_EQ(ret, MEMKIND_SUCCESS);
}

TEST_F(BATest, test_TC_MEMKIND_HBW_Pref_CheckAvailable)
{
    ASSERT_EQ(0, hbw_check_available());
}

TEST_F(BATest, test_TC_MEMKIND_HBW_Pref_Policy)
{
    hbw_set_policy(HBW_POLICY_PREFERRED);
    EXPECT_EQ(HBW_POLICY_PREFERRED, hbw_get_policy());
}

TEST_F(BATest, test_TC_MEMKIND_HBW_Pref_MallocIncremental)
{
    tgen->generate_incremental(HBW_MALLOC);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(BATest, test_TC_MEMKIND_HBW_Pref_CallocIncremental)
{
    tgen->generate_incremental(HBW_CALLOC);
    tgen->run(num_bandwidth, bandwidth);
}


TEST_F(BATest, test_TC_MEMKIND_HBW_Pref_ReallocIncremental)
{
    tgen->generate_incremental(HBW_REALLOC);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(BATest, test_TC_MEMKIND_HBW_Pref_MemalignIncremental)
{
    tgen->generate_incremental(HBW_MEMALIGN);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(BATest, test_TC_MEMKIND_2MBPages_HBW_Pref_MemalignPsizeIncremental)
{
    tgen->generate_incremental(HBW_MEMALIGN_PSIZE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(BATest, test_TC_MEMKIND_HBW_Pref_MallocRecycle)
{
    tgen->generate_recycle_incremental(MEMKIND_MALLOC);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(BATest, test_TC_MEMKIND_2MBPages_HBW_Pref_MallocRecyclePsize)
{
    tgen->generate_recycle_psize_incremental(MEMKIND_MALLOC);
    tgen->run(num_bandwidth, bandwidth);
}
