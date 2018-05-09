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
#include <memkind/internal/memkind_gbtlb.h>

#include <fstream>
#include <algorithm>
#include <numa.h>
#include <errno.h>
#include <limits.h>
#include <sys/sysinfo.h>

#include "common.h"
#include "check.h"
#include "omp.h"
#include "trial_generator.h"

/* Set of negative test cases for memkind, its main goal are to verify that the
 * library behaves accordingly to documentation when calling an API with
 * invalid inputs, incorrect usage, NULL pointers.
 */
class NegativeTest: public ::testing::Test
{

protected:
    void SetUp()
    {}

    void TearDown()
    {}
};

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_zero_memtype)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        memkind_memtype_t(), //Set incorrect value.
        MEMKIND_POLICY_PREFERRED_LOCAL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_incorrect_memtype)
{
    memkind_memtype_t memtype_flags;
    //Set incorrect value.
    memset(&memtype_flags, -1, sizeof(memtype_flags));

    memkind_t kind;
    int ret = memkind_create_kind(
        memtype_flags,
        MEMKIND_POLICY_PREFERRED_LOCAL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_incorrect_policy)
{
    memkind_policy_t policy;
    //Set incorrect value.
    memset(&policy, -1, sizeof(policy));

    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        policy,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_incorrect_mask)
{
    memkind_bits_t flags;

    //Set incorrect value.
    memset(&flags, 255, sizeof(flags));

    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        MEMKIND_POLICY_PREFERRED_LOCAL,
        flags,
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_DEFAULT_BIND_LOCAL)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        MEMKIND_POLICY_BIND_LOCAL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_DEFAULT_BIND_LOCAL_PAGE_SIZE_2MB)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        MEMKIND_POLICY_BIND_LOCAL,
        MEMKIND_MASK_PAGE_SIZE_2MB,
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_DEFAULT_BIND_ALL)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        MEMKIND_POLICY_BIND_ALL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_DEFAULT_BIND_ALL_PAGE_SIZE_2MB)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        MEMKIND_POLICY_BIND_ALL,
        MEMKIND_MASK_PAGE_SIZE_2MB,
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_DEFAULT_INTERLEAVE_LOCAL)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        MEMKIND_POLICY_INTERLEAVE_LOCAL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_DEFAULT_INTERLEAVE_LOCAL_PAGE_SIZE_2MB)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        MEMKIND_POLICY_INTERLEAVE_LOCAL,
        MEMKIND_MASK_PAGE_SIZE_2MB,
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_DEFAULT_INTERLEAVE_ALL)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        MEMKIND_POLICY_INTERLEAVE_ALL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_DEFAULT_INTERLEAVE_ALL_PAGE_SIZE_2MB)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_DEFAULT,
        MEMKIND_POLICY_INTERLEAVE_ALL,
        MEMKIND_MASK_PAGE_SIZE_2MB,
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_HIGH_BANDWIDTH_BIND_ALL)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_HIGH_BANDWIDTH,
        MEMKIND_POLICY_BIND_ALL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_HIGH_BANDWIDTH_BIND_ALL_PAGE_SIZE_2MB)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_HIGH_BANDWIDTH,
        MEMKIND_POLICY_BIND_ALL,
        MEMKIND_MASK_PAGE_SIZE_2MB,
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_HIGH_BANDWIDTH_INTERLEAVE_LOCAL)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_HIGH_BANDWIDTH,
        MEMKIND_POLICY_INTERLEAVE_LOCAL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_HIGH_BANDWIDTH_INTERLEAVE_LOCAL_PAGE_SIZE_2MB)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_HIGH_BANDWIDTH,
        MEMKIND_POLICY_INTERLEAVE_LOCAL,
        MEMKIND_MASK_PAGE_SIZE_2MB,
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_HIGH_BANDWIDTH_INTERLEAVE_ALL_PAGE_SIZE_2MB)
{
    memkind_t kind;
    int ret = memkind_create_kind(
        MEMKIND_MEMTYPE_HIGH_BANDWIDTH,
        MEMKIND_POLICY_INTERLEAVE_ALL,
        MEMKIND_MASK_PAGE_SIZE_2MB,
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_DEFAULT_HIGH_BANDWIDTH_BIND_ALL)
{
    memkind_t kind;
    int flags_tmp = MEMKIND_MEMTYPE_DEFAULT | MEMKIND_MEMTYPE_HIGH_BANDWIDTH;
    memkind_memtype_t memtype_flags;
    memcpy(&memtype_flags, &flags_tmp, sizeof(memtype_flags));

    int ret = memkind_create_kind(
        memtype_flags,
        MEMKIND_POLICY_BIND_ALL,
        memkind_bits_t(),
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_create_kind_DEFAULT_HIGH_BANDWIDTH_INTERLEAVE_ALL_PAGE_SIZE_2MB)
{
    memkind_t kind;
    int flags_tmp = MEMKIND_MEMTYPE_DEFAULT | MEMKIND_MEMTYPE_HIGH_BANDWIDTH;
    memkind_memtype_t memtype_flags;
    memcpy(&memtype_flags, &flags_tmp, sizeof(memtype_flags));

    int ret = memkind_create_kind(
        memtype_flags,
        MEMKIND_POLICY_INTERLEAVE_ALL,
        MEMKIND_MASK_PAGE_SIZE_2MB,
        &kind);
    ASSERT_EQ(ret, MEMKIND_ERROR_INVALID);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_ErrorMemAlign)
{
    int ret = 0;
    void *ptr = NULL;
    int err = EINVAL;

    errno = 0;
    ret = memkind_posix_memalign(MEMKIND_DEFAULT,
                                 &ptr,
                                 5,
                                 100);
    EXPECT_EQ(err, ret);
    EXPECT_EQ(errno, 0);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_ErrorAlignment)
{
    int ret = 0;
    void *ptr = NULL;
    int err = EINVAL;

    errno = 0;
    ret = memkind_posix_memalign(MEMKIND_HBW,
                                 &ptr,
                                 5,
                                 100);
    EXPECT_EQ(err, ret);
    EXPECT_EQ(errno, 0);
}


TEST_F(NegativeTest, test_TC_MEMKIND_Negative_ErrorAllocM)
{
    int ret = 0;
    void *ptr = NULL;
    int err = ENOMEM;
    struct sysinfo info;
    unsigned long long MemTotal = 0;

    errno = 0;
    ret = sysinfo(&info);
    EXPECT_EQ(ret, 0);
    EXPECT_EQ(errno, 0);

    //Determine total memory size as totalram (total usable main memory size)
    //multipled by mem_unit (memory unit size in bytes). This value is equal
    //to MemTotal field in /proc/meminfo.
    MemTotal = info.totalram * info.mem_unit;

    RecordProperty("MemTotal_kB", MemTotal/KB);

    errno = 0;
    ret = memkind_posix_memalign(MEMKIND_HBW,
                                 &ptr,
                                 16,
                                 2*MemTotal);
    EXPECT_EQ(err, ret);
    EXPECT_EQ(errno, 0);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_InvalidSizeMalloc)
{
    void *ptr = NULL;
    for (int i = -1; i <= 0; i++) {
        ptr = hbw_malloc(i);
        ASSERT_TRUE(ptr == NULL);
        EXPECT_EQ(errno, ENOMEM);

        ptr = memkind_malloc(MEMKIND_HBW, i);
        ASSERT_TRUE(ptr == NULL);
        EXPECT_EQ(errno, ENOMEM);
    }
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_InvalidSizeCalloc)
{
    void *ptr = NULL;
    for (int i = -1; i <= 0; i++) {
        ptr = hbw_calloc(1, i);
        ASSERT_TRUE(ptr == NULL);
        EXPECT_EQ(errno, ENOMEM);

        ptr = memkind_calloc(MEMKIND_HBW,
                             1,
                             i);
        ASSERT_TRUE(ptr == NULL);
        EXPECT_EQ(errno, ENOMEM);
    }
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_InvalidSizeRealloc)
{
    void *ptr = NULL;
    for (int i = -1; i <= 0; i++) {
        ptr = hbw_realloc(ptr, i);
        ASSERT_TRUE(ptr == NULL);
        EXPECT_EQ(errno, ENOMEM);

        ptr = memkind_realloc(MEMKIND_HBW,
                              ptr,
                              i);
        ASSERT_TRUE(ptr == NULL);
        EXPECT_EQ(errno, ENOMEM);
    }
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_InvalidSizeMemalign)
{
    int ret = 0;
    void *ptr = NULL;
    int err = ENOMEM;

    errno = 0;
    ret = hbw_posix_memalign(&ptr, 4096, ULLONG_MAX);
    EXPECT_TRUE(ptr == NULL);
    EXPECT_EQ(ret, err);
    EXPECT_EQ(errno, 0);

    errno = 0;
    ret = memkind_posix_memalign(MEMKIND_HBW, &ptr, 4096, ULLONG_MAX);
    EXPECT_TRUE(ptr == NULL);
    EXPECT_EQ(err, ret);
    EXPECT_EQ(errno, 0);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_GBFailureMemalign)
{
    int ret = 0;
    void *ptr = NULL;
    int err = EINVAL;

    ret = hbw_posix_memalign_psize(&ptr,
                                   1073741824,
                                   1073741826,
                                   HBW_PAGESIZE_1GB_STRICT);
    EXPECT_EQ(ret, err);
    EXPECT_TRUE(ptr == NULL);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_RegularReallocWithMemAllign)
{
    int ret = 0;
    void *ptr = NULL;

    ret = hbw_posix_memalign_psize(&ptr,
                                   4096,
                                   4096,
                                   HBW_PAGESIZE_4KB);
    EXPECT_EQ(ret, 0);
    ASSERT_TRUE(ptr != NULL);
    memset(ptr, 0, 4096);
    ptr = hbw_realloc(ptr, 8192);
    memset(ptr, 0, 8192);
    hbw_free(ptr);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_SetPolicy)
{
    // First call should be successfull, consequent should generate a warning
    // and be ignored
    EXPECT_EQ(hbw_set_policy(HBW_POLICY_PREFERRED), 0);
    EXPECT_EQ(hbw_set_policy(HBW_POLICY_BIND), EPERM);
    EXPECT_EQ(hbw_set_policy(HBW_POLICY_INTERLEAVE), EPERM);
    EXPECT_EQ(hbw_get_policy(), HBW_POLICY_PREFERRED);
    EXPECT_EQ(hbw_set_policy((hbw_policy_t)0xFF), EINVAL);
}

//Check if hbw_set_policy() will be ignored after malloc.
TEST_F(NegativeTest, test_TC_MEMKIND_Negative_SetPolicyAfterMalloc)
{
    void *ptr = hbw_malloc(512);
    EXPECT_TRUE(ptr != NULL);

    EXPECT_EQ(hbw_set_policy(HBW_POLICY_BIND), EPERM);
    EXPECT_NE(hbw_get_policy(), HBW_POLICY_BIND);
    EXPECT_EQ(hbw_get_policy(), HBW_POLICY_PREFERRED);

    hbw_free(ptr);
}

//Check if hbw_set_policy() will be ignored after calloc.
TEST_F(NegativeTest, test_TC_MEMKIND_Negative_SetPolicyAfterCalloc)
{
    void *ptr = hbw_calloc(512, 1);
    EXPECT_TRUE(ptr != NULL);

    EXPECT_EQ(hbw_set_policy(HBW_POLICY_BIND), EPERM);
    EXPECT_NE(hbw_get_policy(), HBW_POLICY_BIND);
    EXPECT_EQ(hbw_get_policy(), HBW_POLICY_PREFERRED);

    hbw_free(ptr);
}

//Check if hbw_set_policy() will be ignored after realloc.
TEST_F(NegativeTest, test_TC_MEMKIND_Negative_SetPolicyAfterRealloc)
{
    void *ptr = hbw_malloc(512);
    EXPECT_TRUE(ptr != NULL);

    hbw_realloc(ptr, 512);

    EXPECT_EQ(hbw_set_policy(HBW_POLICY_BIND), EPERM);
    EXPECT_NE(hbw_get_policy(), HBW_POLICY_BIND);
    EXPECT_EQ(hbw_get_policy(), HBW_POLICY_PREFERRED);

    hbw_free(ptr);
}

//Check if hbw_set_policy() will be ignored after hbw_posix_memalign.
TEST_F(NegativeTest, test_TC_MEMKIND_Negative_SetPolicyAfterHbwPosixMemalign)
{
    void *ptr = NULL;

    hbw_posix_memalign(&ptr, 2048, 2048);
    EXPECT_TRUE(ptr != NULL);

    EXPECT_EQ(hbw_set_policy(HBW_POLICY_BIND), EPERM);
    EXPECT_NE(hbw_get_policy(), HBW_POLICY_BIND);
    EXPECT_EQ(hbw_get_policy(), HBW_POLICY_PREFERRED);

    hbw_free(ptr);
}

//Check if hbw_set_policy() will be ignored after hbw_posix_memalign_psize.
TEST_F(NegativeTest, test_TC_MEMKIND_Negative_SetPolicyAfterHbwPosixMemalignPsize)
{
    void *ptr = NULL;

    hbw_posix_memalign_psize(&ptr, 2048, 2048, HBW_PAGESIZE_4KB);
    EXPECT_TRUE(ptr != NULL);

    EXPECT_EQ(hbw_set_policy(HBW_POLICY_BIND), EPERM);
    EXPECT_NE(hbw_get_policy(), HBW_POLICY_BIND);
    EXPECT_EQ(hbw_get_policy(), HBW_POLICY_PREFERRED);

    hbw_free(ptr);
}


TEST_F(NegativeTest, test_TC_MEMKIND_Negative_GBMemalignPsizeAllign)
{
    void *ptr = NULL;
    int ret = 0;
    int err = EINVAL;

    ret = hbw_posix_memalign_psize(&ptr, -1, 1024, HBW_PAGESIZE_1GB);
    EXPECT_EQ(err, ret);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_GBNullRealloc)
{
    void *ptr = NULL;
    ptr = memkind_gbtlb_realloc(MEMKIND_HBW_GBTLB, NULL, -1);
    EXPECT_TRUE(ptr == NULL);
}

TEST_F(NegativeTest, test_TC_MEMKIND_Negative_GBNullFree)
{
    memkind_free(MEMKIND_GBTLB, NULL);
}
