/*
 * Copyright (C) 2014 - 2017 Intel Corporation.
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
#include <numaif.h>

#include "common.h"
#include "check.h"
#include "omp.h"
#include "trial_generator.h"
#include "allocator_perf_tool/HugePageOrganizer.hpp"
#include "Allocator.hpp"
#include "TestPolicy.hpp"

/*
 * Set of basic acceptance tests.
 */
class BATest: public TGTest
{};

class BATestHuge: public BATest
{
private:
    HugePageOrganizer huge_page_organizer = HugePageOrganizer(8);
};

class BasicAllocTest
{
public:

    BasicAllocTest(Allocator* allocator) : allocator(allocator) {}

    void check_policy_and_numa_node(void* ptr, size_t size)
    {
        int policy = allocator->get_numa_policy();

        EXPECT_NE(-1, policy);

        if (allocator->is_high_bandwidth()) {
            TestPolicy::check_hbw_numa_nodes(policy, ptr, size);
        } else {
            TestPolicy::check_all_numa_nodes(policy, ptr, size);
        }
    }

    void record_page_association(void* ptr, size_t size)
    {
        TestPolicy::record_page_association(ptr, size, allocator->get_page_size());
    }

    void malloc(size_t size)
    {
        void* ptr = allocator->malloc(size);
        ASSERT_TRUE(ptr != NULL) << "malloc() returns NULL";
        void* memset_ret = memset(ptr, 3, size);
        EXPECT_TRUE(memset_ret != NULL);
        record_page_association(ptr, size);
        check_policy_and_numa_node(ptr, size);
        allocator->free(ptr);
    }

    void calloc(size_t num, size_t size)
    {
        void* ptr = allocator->calloc(num, size);
        ASSERT_TRUE(ptr != NULL) << "calloc() returns NULL";
        void* memset_ret = memset(ptr, 3, size);
        ASSERT_TRUE(memset_ret != NULL);
        record_page_association(ptr, size);
        check_policy_and_numa_node(ptr, size);
        allocator->free(ptr);
    }

    void realloc(size_t size)
    {
        void* ptr = allocator->malloc(size);
        ASSERT_TRUE(ptr != NULL) << "malloc() returns NULL";
        size_t realloc_size = size+128;
        ptr = allocator->realloc(ptr, realloc_size);
        ASSERT_TRUE(ptr != NULL) << "realloc() returns NULL";
        void* memset_ret = memset(ptr, 3, realloc_size);
        ASSERT_TRUE(memset_ret != NULL);
        record_page_association(ptr, size);
        check_policy_and_numa_node(ptr, size);
        allocator->free(ptr);
    }

    void memalign(size_t alignment, size_t size)
    {
        void* ptr = NULL;
        int ret = allocator->memalign(&ptr, alignment, size);
        ASSERT_EQ(0, ret) << "posix_memalign() != 0";
        ASSERT_TRUE(ptr != NULL) << "posix_memalign() returns NULL pointer";
        void* memset_ret = memset(ptr, 3, size);
        ASSERT_TRUE(memset_ret != NULL);
        record_page_association(ptr, size);
        check_policy_and_numa_node(ptr, size);
        allocator->free(ptr);
    }

    void free(size_t size)
    {
        void* ptr = allocator->malloc(size);
        ASSERT_TRUE(ptr != NULL) << "malloc() returns NULL";
        allocator->free(ptr);
    }

    virtual ~BasicAllocTest() {}
private:
    Allocator *allocator;
};

static void test_malloc(memkind_memtype_t memtype, memkind_policy_t policy, memkind_bits_t flags, size_t size)
{
    MemkindAllocator memkind_allocator(memtype, policy, flags);
    BasicAllocTest(&memkind_allocator).malloc(size);
}

static void test_calloc(memkind_memtype_t memtype, memkind_policy_t policy, memkind_bits_t flags, size_t size)
{
    MemkindAllocator memkind_allocator(memtype, policy, flags);
    BasicAllocTest(&memkind_allocator).calloc(1, size);
}

static void test_realloc(memkind_memtype_t memtype, memkind_policy_t policy, memkind_bits_t flags, size_t size)
{
    MemkindAllocator memkind_allocator(memtype, policy, flags);
    BasicAllocTest(&memkind_allocator).realloc(size);
}


static void test_memalign(memkind_memtype_t memtype, memkind_policy_t policy, memkind_bits_t flags, size_t size)
{
    MemkindAllocator memkind_allocator(memtype, policy, flags);
    BasicAllocTest(&memkind_allocator).memalign(4096, size);
}

static void test_free(memkind_memtype_t memtype, memkind_policy_t policy, memkind_bits_t flags, size_t size)
{
    MemkindAllocator memkind_allocator(memtype, policy, flags);
    BasicAllocTest(&memkind_allocator).free(size);
}

static void test_malloc(memkind_t kind, size_t size)
{
    MemkindAllocator memkind_allocator(kind);
    BasicAllocTest(&memkind_allocator).malloc(size);
}

static void test_calloc(memkind_t kind, size_t size)
{
    MemkindAllocator memkind_allocator(kind);
    BasicAllocTest(&memkind_allocator).calloc(1, size);
}

static void test_realloc(memkind_t kind, size_t size)
{
    MemkindAllocator memkind_allocator(kind);
    BasicAllocTest(&memkind_allocator).realloc(size);
}

static void test_memalign(memkind_t kind, size_t size)
{
    MemkindAllocator memkind_allocator(kind);
    BasicAllocTest(&memkind_allocator).memalign(4096, size);
}

static void test_free(memkind_t kind, size_t size)
{
    MemkindAllocator memkind_allocator(kind);
    BasicAllocTest(&memkind_allocator).free(size);
}

/*** Kind tests */

TEST_F(BATest, test_TC_MEMKIND_malloc_DEFAULT_PREFERRED_LOCAL_4096_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_DEFAULT_PREFERRED_LOCAL_4194305_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_malloc_DEFAULT_PREFERRED_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_malloc_DEFAULT_PREFERRED_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_BIND_LOCAL_4096_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_BIND_LOCAL_4194305_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_BIND_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_BIND_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_BIND_ALL_4096_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_BIND_ALL_4194305_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_BIND_ALL_PAGE_SIZE_2MB_4096_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_BIND_ALL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_4096_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_4194305_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_INTERLEAVE_ALL_4096_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_INTERLEAVE_ALL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_HIGH_BANDWIDTH_INTERLEAVE_ALL_4194305_bytes)
{
    test_malloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_INTERLEAVE_ALL, memkind_bits_t(), 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_DEFAULT_HIGH_BANDWIDTH_INTERLEAVE_ALL_4194305_bytes)
{
    test_malloc((memkind_memtype_t)(MEMKIND_MEMTYPE_DEFAULT | MEMKIND_MEMTYPE_HIGH_BANDWIDTH), MEMKIND_POLICY_INTERLEAVE_ALL, memkind_bits_t(), 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_DEFAULT_PREFERRED_LOCAL_4096_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_DEFAULT_PREFERRED_LOCAL_4194305_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_calloc_DEFAULT_PREFERRED_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_calloc_DEFAULT_PREFERRED_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_BIND_LOCAL_4096_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_BIND_LOCAL_4194305_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_BIND_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_BIND_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_BIND_ALL_4096_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_BIND_ALL_4194305_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_BIND_ALL_PAGE_SIZE_2MB_4096_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_BIND_ALL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_4096_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_4194305_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_INTERLEAVE_ALL_4096_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_INTERLEAVE_ALL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_HIGH_BANDWIDTH_INTERLEAVE_ALL_4194305_bytes)
{
    test_calloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_INTERLEAVE_ALL, memkind_bits_t(), 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_DEFAULT_PREFERRED_LOCAL_4096_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_DEFAULT_PREFERRED_LOCAL_4194305_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_realloc_DEFAULT_PREFERRED_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_realloc_DEFAULT_PREFERRED_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_BIND_LOCAL_4096_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_BIND_LOCAL_4194305_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_BIND_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_BIND_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_BIND_ALL_4096_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_BIND_ALL_4194305_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_BIND_ALL_PAGE_SIZE_2MB_4096_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_BIND_ALL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_4096_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_4194305_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_PREFERRED_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_INTERLEAVE_ALL_4096_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_INTERLEAVE_ALL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_HIGH_BANDWIDTH_INTERLEAVE_ALL_4194305_bytes)
{
    test_realloc(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_INTERLEAVE_ALL, memkind_bits_t(), 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_DEFAULT_PREFERRED_LOCAL_4096_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_DEFAULT_PREFERRED_LOCAL_4194305_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_memalign_DEFAULT_PREFERRED_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_memalign_DEFAULT_PREFERRED_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_BIND_LOCAL_4096_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_BIND_LOCAL_4194305_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_BIND_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_BIND_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_BIND_ALL_4096_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_BIND_ALL_4194305_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_BIND_ALL_PAGE_SIZE_2MB_4096_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_BIND_ALL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_BIND_ALL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_PREFERRED_LOCAL_4096_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_PREFERRED_LOCAL_4194305_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_PREFERRED_LOCAL_PAGE_SIZE_2MB_4096_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_PREFERRED_LOCAL_PAGE_SIZE_2MB_4194305_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_PREFERRED_LOCAL, MEMKIND_MASK_PAGE_SIZE_2MB, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_INTERLEAVE_ALL_4096_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_INTERLEAVE_ALL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_HIGH_BANDWIDTH_INTERLEAVE_ALL_4194305_bytes)
{
    test_memalign(MEMKIND_MEMTYPE_HIGH_BANDWIDTH, MEMKIND_POLICY_INTERLEAVE_ALL, memkind_bits_t(), 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_free_DEFAULT_PREFERRED_LOCAL_4096_bytes)
{
    test_free(MEMKIND_MEMTYPE_DEFAULT, MEMKIND_POLICY_PREFERRED_LOCAL, memkind_bits_t(), 4096);
}

TEST_F(BATest, test_TC_MEMKIND_free_MEMKIND_DEFAULT_free_with_NULL_kind_4096_bytes)
{
    void* ptr = memkind_malloc(MEMKIND_DEFAULT, 4096);
    ASSERT_TRUE(ptr != NULL) << "malloc() returns NULL";
    memkind_free(0, ptr);
}

TEST_F(BATestHuge, test_TC_MEMKIND_free_ext_MEMKIND_GBTLB_4096_bytes)
{
    HugePageOrganizer huge_page_organizer(260);
    MemkindAllocator memkind_allocator(MEMKIND_GBTLB);
    BasicAllocTest(&memkind_allocator).free(4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_malloc_Bind_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND);
    BasicAllocTest(&hbwmalloc_allocator).malloc(4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_malloc_Bind_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND);
    BasicAllocTest(&hbwmalloc_allocator).malloc(4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_malloc_Bind_All_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND_ALL);
    BasicAllocTest(&hbwmalloc_allocator).malloc(4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_malloc_Bind_All_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND_ALL);
    BasicAllocTest(&hbwmalloc_allocator).malloc(4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_malloc_Preferred_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_PREFERRED);
    BasicAllocTest(&hbwmalloc_allocator).malloc(4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_malloc_Preferred_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_PREFERRED);
    BasicAllocTest(&hbwmalloc_allocator).malloc(4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_malloc_Interleave_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_INTERLEAVE);
    BasicAllocTest(&hbwmalloc_allocator).malloc(4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_malloc_Interleave_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_INTERLEAVE);
    BasicAllocTest(&hbwmalloc_allocator).malloc(4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_calloc_Bind_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND);
    BasicAllocTest(&hbwmalloc_allocator).calloc(1, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_calloc_Bind_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND);
    BasicAllocTest(&hbwmalloc_allocator).calloc(1, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_calloc_Bind_All_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND_ALL);
    BasicAllocTest(&hbwmalloc_allocator).calloc(1, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_calloc_Bind_All_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND_ALL);
    BasicAllocTest(&hbwmalloc_allocator).calloc(1, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_calloc_Preferred_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_PREFERRED);
    BasicAllocTest(&hbwmalloc_allocator).calloc(1, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_calloc_Preferred_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_PREFERRED);
    BasicAllocTest(&hbwmalloc_allocator).calloc(1, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_calloc_Interleave_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_INTERLEAVE);
    BasicAllocTest(&hbwmalloc_allocator).calloc(1, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_calloc_Interleave_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_INTERLEAVE);
    BasicAllocTest(&hbwmalloc_allocator).calloc(1, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_realloc_Bind_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND);
    BasicAllocTest(&hbwmalloc_allocator).realloc(4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_realloc_Bind_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND);
    BasicAllocTest(&hbwmalloc_allocator).realloc(4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_realloc_Bind_All_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND_ALL);
    BasicAllocTest(&hbwmalloc_allocator).realloc(4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_realloc_Bind_All_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND_ALL);
    BasicAllocTest(&hbwmalloc_allocator).realloc(4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_realloc_Preferred_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_PREFERRED);
    BasicAllocTest(&hbwmalloc_allocator).realloc(4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_realloc_Preferred_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_PREFERRED);
    BasicAllocTest(&hbwmalloc_allocator).realloc(4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_realloc_Interleave_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_INTERLEAVE);
    BasicAllocTest(&hbwmalloc_allocator).realloc(4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_realloc_Interleave_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_INTERLEAVE);
    BasicAllocTest(&hbwmalloc_allocator).realloc(4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_memalign_Bind_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_memalign_Bind_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_memalign_Bind_All_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND_ALL);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_memalign_Bind_All_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND_ALL);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_memalign_Preferred_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_PREFERRED);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_memalign_Preferred_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_PREFERRED);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_memalign_Interleave_Policy_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_INTERLEAVE);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_memalign_Interleave_Policy_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_INTERLEAVE);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_hbwmalloc_memalign_psize_Preferred_Policy_PAGE_SIZE_2MB_4096_bytes_)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_PREFERRED);
    hbwmalloc_allocator.set_memalign_page_size(HBW_PAGESIZE_2MB);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_hbwmalloc_memalign_psize_Preferred_Policy_PAGE_SIZE_2MB_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_PREFERRED);
    hbwmalloc_allocator.set_memalign_page_size(HBW_PAGESIZE_2MB);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_hbwmalloc_memalign_psize_Bind_Policy_PAGE_SIZE_2MB_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND);
    hbwmalloc_allocator.set_memalign_page_size(HBW_PAGESIZE_2MB);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_hbwmalloc_memalign_psize_Bind_Policy_PAGE_SIZE_2MB_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND);
    hbwmalloc_allocator.set_memalign_page_size(HBW_PAGESIZE_2MB);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4194305);
}

TEST_F(BATestHuge, test_TC_MEMKIND_hbwmalloc_memalign_psize_Bind_All_Policy_PAGE_SIZE_2MB_4096_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND_ALL);
    hbwmalloc_allocator.set_memalign_page_size(HBW_PAGESIZE_2MB);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4096);
}

TEST_F(BATestHuge, test_TC_MEMKIND_hbwmalloc_memalign_psize_Bind_All_Policy_PAGE_SIZE_2MB_4194305_bytes)
{
    HbwmallocAllocator hbwmalloc_allocator(HBW_POLICY_BIND_ALL);
    hbwmalloc_allocator.set_memalign_page_size(HBW_PAGESIZE_2MB);
    BasicAllocTest(&hbwmalloc_allocator).memalign(4096, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_Pref_CheckAvailable)
{
    ASSERT_EQ(0, hbw_check_available());
}

TEST_F(BATest, test_TC_MEMKIND_hbwmalloc_Pref_Policy)
{
    hbw_set_policy(HBW_POLICY_PREFERRED);
    EXPECT_EQ(HBW_POLICY_PREFERRED, hbw_get_policy());
}

/* MEMKIND REGULAR */
#include <memkind/internal/memkind_regular.h>

TEST_F(BATest, test_TC_MEMKIND_malloc_REGULAR_4096_bytes)
{
    test_malloc(MEMKIND_REGULAR, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_malloc_REGULAR_4194305_bytes)
{
    test_malloc(MEMKIND_REGULAR, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_REGULAR_4096_bytes)
{
    test_realloc(MEMKIND_REGULAR, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_realloc_REGULAR_4194305_bytes)
{
    test_realloc(MEMKIND_REGULAR, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_REGULAR_4096_bytes)
{
    test_calloc(MEMKIND_REGULAR, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_calloc_REGULAR_4194305_bytes)
{
    test_calloc(MEMKIND_REGULAR, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_REGULAR_4096_bytes)
{
    test_memalign(MEMKIND_REGULAR, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_memalign_REGULAR_4194305_bytes)
{
    test_memalign(MEMKIND_REGULAR, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_free_REGULAR_4096_bytes)
{
    test_free(MEMKIND_REGULAR, 4096);
}

TEST_F(BATest, test_TC_MEMKIND_free_REGULAR_4194305_bytes)
{
    test_free(MEMKIND_REGULAR, 4194305);
}

TEST_F(BATest, test_TC_MEMKIND_REGULAR_nodemask)
{
    using namespace TestPolicy;
    void *mem = memkind_malloc(MEMKIND_REGULAR, 1234567);
    unique_bitmask_ptr kind_nodemask = make_nodemask_ptr();
    ASSERT_EQ(0, memkind_regular_all_get_mbind_nodemask(MEMKIND_REGULAR,
                                                        kind_nodemask->maskp,
                                                        kind_nodemask->size));

    check_numa_nodes(kind_nodemask, MPOL_BIND, mem, 1234567);
    memkind_free(MEMKIND_REGULAR, mem);
}

