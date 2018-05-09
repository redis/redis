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
#include "memkind/internal/memkind_hbw.h"
#include "allocator_perf_tool/HugePageOrganizer.hpp"
#include "hbwmalloc.h"
#include "common.h"

#include <sys/mman.h>
#include <numaif.h>
#include <numa.h>
#include <stdint.h>

#define SHIFT_BYTES(ptr, bytes) ((char*)(ptr)+(bytes))

/*
 * Set of tests for hbw_verify_memory_region() function,
 * which intend to check if allocated memory fully fall into high bandwidth memory.
 * Note: in this tests we are using internal function memkind_hbw_all_get_mbind_nodemask().
 * In future we intend to rewrite this function when we will have replacement in new API.
 */
class HbwVerifyFunctionTest: public :: testing::Test
{
protected:
    const size_t BLOCK_SIZE = 64;
    const size_t page_size = sysconf(_SC_PAGESIZE);
    const int flags = MAP_ANONYMOUS | MAP_PRIVATE;
};

/*
 * Group of basic tests for different sizes of allocations
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_HBW_page_size_not_round)
{
    size_t size = page_size * 1024 + 5;
    char* ptr = (char*) hbw_malloc(size);
    ASSERT_FALSE(ptr == NULL);
    EXPECT_EQ(hbw_verify_memory_region(ptr, size, HBW_TOUCH_PAGES), 0);
    hbw_free(ptr);
}

TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_HBW_page_size_round)
{
    size_t size = page_size * 1024;
    char* ptr = (char*) hbw_malloc(size);
    ASSERT_FALSE(ptr == NULL);
    EXPECT_EQ(hbw_verify_memory_region(ptr, size, HBW_TOUCH_PAGES), 0);
    hbw_free(ptr);
}

TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_HBW_iterate_1_byte_to_8194_bytes)
{
    for (size_t size = 1; size <= (page_size * 2 + 2); size++) { //iterate through 2 pages and 2 bytes
        char* ptr = (char*) hbw_malloc(size);
        ASSERT_FALSE(ptr == NULL);
        EXPECT_EQ(hbw_verify_memory_region(ptr, size, HBW_TOUCH_PAGES), 0);
        hbw_free(ptr);
    }
}

TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_HBW_ext_5GB)
{
    size_t size = 5ull*(1<<30); //5GB - big allocation
    char* ptr = (char*) hbw_malloc(size);
    ASSERT_FALSE(ptr == NULL);
    EXPECT_EQ(hbw_verify_memory_region(ptr, size, HBW_TOUCH_PAGES), 0);
    hbw_free(ptr);
}

/*
 * Test setting memory without HBW_TOUCH_PAGES flag
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_HBW_setting_memory_without_flag)
{
    size_t size = page_size;
    char* ptr = (char*) hbw_malloc(size);
    ASSERT_FALSE(ptr == NULL);
    memset(ptr, '.', size);
    EXPECT_EQ(hbw_verify_memory_region(ptr, size, 0), 0);
    for (size_t i = 0; i < size; i++) { //check that content is unchanged
        EXPECT_TRUE(ptr[i] == '.');
    }
    hbw_free(ptr);
}

/*
 * Test HBW_TOUCH_PAGES flag
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_HBW_TOUCH_PAGES_check_overwritten_content)
{
    size_t size = 5 * page_size;
    char* ptr = (char*) hbw_malloc(size);
    ASSERT_FALSE(ptr == NULL);
    memset(ptr, '.', size);
    EXPECT_EQ(hbw_verify_memory_region(ptr, size, HBW_TOUCH_PAGES), 0);
    for (size_t i = 0; i < size; i++) { //check that content of all pages doesn't change
        EXPECT_TRUE(ptr[i] == '.');
    }
    hbw_free(ptr);
}

/*
 * Tests check if number of 64-page blocks are working correctly
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_HBW_many_blocks_round)
{
    size_t size = 16 * (BLOCK_SIZE * page_size); //exactly 16 * 64-page blocks
    char* ptr = (char*) hbw_malloc(size);
    ASSERT_FALSE(ptr == NULL);
    EXPECT_EQ(hbw_verify_memory_region(ptr, size, HBW_TOUCH_PAGES), 0);
    hbw_free(ptr);
}

TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_HBW_many_blocks_not_round)
{
    size_t size = (16 * BLOCK_SIZE * page_size) + (8 * page_size); //16 * 64-page blocks and 1 * 8-paged block
    char* ptr = (char*) hbw_malloc(size);
    ASSERT_FALSE(ptr == NULL);
    EXPECT_EQ(hbw_verify_memory_region(ptr, size, HBW_TOUCH_PAGES), 0);
    hbw_free(ptr);
}

/*
 * Check if 2 blocks and parts of the third block are working correctly.
 * With this test we can be sure that different sizes of allocation (not only whole blocks) are ok.
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_HBW_iterate_not_round)
{
    for (size_t i = 1; i < BLOCK_SIZE; i++) {
        size_t size = (2 * BLOCK_SIZE * page_size) + (i * page_size); // 2 * 64-paged blocks and iterate through whole 3rd 64-paged block
        char* ptr = (char*) hbw_malloc(size);
        ASSERT_FALSE(ptr == NULL);
        EXPECT_EQ(hbw_verify_memory_region(ptr, size, HBW_TOUCH_PAGES), 0);
        hbw_free(ptr);
    }
}

/*
 * Tests for other kinds and malloc
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_2MBPages_HBW_HUGETLB)
{
    ASSERT_HUGEPAGES_AVAILABILITY();
    size_t size = 2 * 1024 * 1024 * 10; //10 * 2MB pages
    char* ptr = (char*) memkind_malloc(MEMKIND_HBW_HUGETLB, size);
    ASSERT_FALSE(ptr == NULL);
    EXPECT_EQ(hbw_verify_memory_region(ptr, size, HBW_TOUCH_PAGES), 0);
    memkind_free(MEMKIND_HBW_HUGETLB, ptr);
}

TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_DEFAULT)
{
    size_t size = page_size * 1024;
    char* ptr = (char*) memkind_malloc(MEMKIND_DEFAULT, size);
    ASSERT_FALSE(ptr == NULL);
    EXPECT_EQ(hbw_verify_memory_region(ptr, size, HBW_TOUCH_PAGES), -1);
    memkind_free(MEMKIND_DEFAULT, ptr);
}

TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_malloc)
{
    size_t size = page_size * 1024;
    char* ptr = (char*) malloc(size);
    ASSERT_FALSE(ptr == NULL);
    EXPECT_EQ(hbw_verify_memory_region(ptr, size, HBW_TOUCH_PAGES), -1);
    free(ptr);
}

/*
 * Group of negative tests
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_Negative_size_0_and_SET_MEMORY_flag)
{
    void* ptr = hbw_malloc(page_size);
    ASSERT_FALSE(ptr == NULL);
    EXPECT_EQ(hbw_verify_memory_region(ptr, 0, HBW_TOUCH_PAGES), EINVAL);
    hbw_free(ptr);
}

TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_Negative_size_0_without_flag)
{
    void* ptr = hbw_malloc(page_size);
    ASSERT_FALSE(ptr == NULL);
    EXPECT_EQ(hbw_verify_memory_region(ptr, 0, 0), EINVAL);
    hbw_free(ptr);
}

TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_Negative_Uninitialized_Memory_without_flag)
{
    void* ptr = NULL;
    EXPECT_EQ(hbw_verify_memory_region(ptr, page_size * 1024, 0), EINVAL);
}

TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_Negative_Uninitialized_Memory_and_SET_MEMORY_flag)
{
    void* ptr = NULL;
    EXPECT_EQ(hbw_verify_memory_region(ptr, page_size * 1024, HBW_TOUCH_PAGES), EINVAL);
}

TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_Negative_Without_Memset)
{
    size_t size = page_size * 1024;
    void* ptr = hbw_malloc(size);
    ASSERT_FALSE(ptr == NULL);
    EXPECT_EQ(hbw_verify_memory_region(ptr, size, 0), -1);
    hbw_free(ptr);
}

/*
 * Corner cases: tests for half of pages
 * + HBM memory
 * # HBM memory and verified
 * - not HBM memory, but allocated
 * = not HBM memory and verified
 */

 /* 3 pages:
  * |++##|####|##++|
  */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_Half_Pages)
{
    size_t size = 3 * page_size;
    nodemask_t nodemask;
    struct bitmask hbw_nodemask = {NUMA_NUM_NODES, nodemask.n};
    //function memkind_hbw_all_get_mbind_nodemask() has to be rewritten, when we will have replacement in new API.
    memkind_hbw_all_get_mbind_nodemask(NULL, hbw_nodemask.maskp, hbw_nodemask.size);
    char* ptr = (char*) mmap(NULL, size , PROT_READ | PROT_WRITE, flags, -1, 0);
    ASSERT_FALSE(ptr == NULL);

    //all pages should fall on HBM
    mbind(ptr, size, MPOL_BIND, hbw_nodemask.maskp, NUMA_NUM_NODES, 0);

    //verified are: half of the first page, second page and half of the third page
    EXPECT_EQ(hbw_verify_memory_region(SHIFT_BYTES(ptr, page_size/2), size - page_size, HBW_TOUCH_PAGES), 0);
    EXPECT_EQ(munmap(ptr, size), 0);
}

/* 3 pages:
 * |####|####|----|
 * |++##|####|==--|
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_Half_Pages_1_and_2_page)
{
    size_t size = 3 * page_size;
    nodemask_t nodemask;
    struct bitmask hbw_nodemask = {NUMA_NUM_NODES, nodemask.n};
    //function memkind_hbw_all_get_mbind_nodemask() has to be rewritten, when we will have replacement in new API.
    memkind_hbw_all_get_mbind_nodemask(NULL, hbw_nodemask.maskp, hbw_nodemask.size);
    char* ptr = (char*) mmap(NULL, size , PROT_READ | PROT_WRITE, flags, -1, 0);
    ASSERT_FALSE(ptr == NULL);

    //first and second page should fall on HBM
    mbind(ptr, size - page_size, MPOL_BIND, hbw_nodemask.maskp, NUMA_NUM_NODES, 0);

    //check if mbind is successful |####|####|----|
    EXPECT_EQ(hbw_verify_memory_region(ptr, size - page_size, HBW_TOUCH_PAGES), 0);

    //verified are: half of the first page, second page and half of the third page |++##|####|==--|
    EXPECT_EQ(hbw_verify_memory_region(SHIFT_BYTES(ptr, page_size/2), size - page_size, HBW_TOUCH_PAGES), -1);
    EXPECT_EQ(munmap(ptr, size), 0);
}

/* 3 pages:
 * |----|####|####|
 * |--==|####|##++|
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_Half_Pages_2_and_3_page)
{
    size_t size = 3 * page_size;
    nodemask_t nodemask;
    struct bitmask hbw_nodemask = {NUMA_NUM_NODES, nodemask.n};
    //function memkind_hbw_all_get_mbind_nodemask() has to be rewritten, when we will have replacement in new API.
    memkind_hbw_all_get_mbind_nodemask(NULL, hbw_nodemask.maskp, hbw_nodemask.size);
    char* ptr = (char*) mmap(NULL, size , PROT_READ | PROT_WRITE, flags, -1, 0);
    ASSERT_FALSE(ptr == NULL);

    //second and third page should fall on HBM
    mbind(SHIFT_BYTES(ptr, page_size), size - page_size, MPOL_BIND, hbw_nodemask.maskp, NUMA_NUM_NODES, 0);

    //check if mbind is successful |----|####|####|
    EXPECT_EQ(hbw_verify_memory_region(SHIFT_BYTES(ptr, page_size), size - page_size, HBW_TOUCH_PAGES), 0);

    //verified are: half of the first page, second page and half of the third page
    EXPECT_EQ(hbw_verify_memory_region(SHIFT_BYTES(ptr, page_size/2), size - page_size, HBW_TOUCH_PAGES), -1);
    EXPECT_EQ(munmap(ptr, size), 0);
}

/* 3 pages:
 * |####|----|++++| and  |++++|----|####|
 * |++##|====|##++|
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_Half_Pages_1_and_3_page)
{
    size_t size = 3 * page_size;
    nodemask_t nodemask;
    struct bitmask hbw_nodemask = {NUMA_NUM_NODES, nodemask.n};
    //function memkind_hbw_all_get_mbind_nodemask() has to be rewritten, when we will have replacement in new API.
    memkind_hbw_all_get_mbind_nodemask(NULL, hbw_nodemask.maskp, hbw_nodemask.size);
    char* ptr = (char*) mmap(NULL, size , PROT_READ | PROT_WRITE, flags, -1, 0);
    ASSERT_FALSE(ptr == NULL);

    //first and third page should fall on HBM
    mbind(ptr, page_size, MPOL_BIND, hbw_nodemask.maskp, NUMA_NUM_NODES, 0);
    mbind(SHIFT_BYTES(ptr, 2 * page_size), page_size, MPOL_BIND, hbw_nodemask.maskp, NUMA_NUM_NODES, 0);

    //check if mbind is successful |####|----|++++| and  |++++|----|####|
    EXPECT_EQ(hbw_verify_memory_region(ptr, size - 2*page_size, HBW_TOUCH_PAGES), 0);
    EXPECT_EQ(hbw_verify_memory_region(SHIFT_BYTES(ptr, 2*page_size), size - 2*page_size, HBW_TOUCH_PAGES), 0);

    //verified are: half of the first page, second page and half of the third page
    EXPECT_EQ(hbw_verify_memory_region(SHIFT_BYTES(ptr, page_size/2), size - page_size, HBW_TOUCH_PAGES), -1);
    EXPECT_EQ(munmap(ptr, size), 0);
}

/* 5 pages:
 * |----|+###|####|###+|----|
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_Boundaries_CornerCase)
{
    size_t size = 5 * page_size;
    nodemask_t nodemask;
    struct bitmask hbw_nodemask = {NUMA_NUM_NODES, nodemask.n};
    //function memkind_hbw_all_get_mbind_nodemask() has to be rewritten, when we will have replacement in new API.
    memkind_hbw_all_get_mbind_nodemask(NULL, hbw_nodemask.maskp, hbw_nodemask.size);
    char* ptr = (char*) mmap(NULL, size , PROT_READ | PROT_WRITE, flags, -1, 0);
    ASSERT_FALSE(ptr == NULL);

    size_t tested_pages_size = size - 2 * page_size;

    //second, third and fourth page should fall on HBM
    mbind(SHIFT_BYTES(ptr, page_size), tested_pages_size, MPOL_BIND, hbw_nodemask.maskp, NUMA_NUM_NODES, 0);

    //verified are: second byte of the second page to last byte -1 of fourth page
    EXPECT_EQ(hbw_verify_memory_region(SHIFT_BYTES(ptr, page_size + 1), tested_pages_size - 2, HBW_TOUCH_PAGES), 0);
    EXPECT_EQ(munmap(ptr, size), 0);
}

/* 5 pages:
 * |-###|####|####|####|###-|
 */
TEST_F(HbwVerifyFunctionTest, test_TC_MEMKIND_HBW_partial_verification)
{
    size_t size = 5 * page_size;

    //all pages should fall on HBM
    void* ptr = hbw_malloc(size);
    ASSERT_FALSE(ptr == NULL);

    //but only second byte of the first page to last byte -1 of the last page are touched
    memset(SHIFT_BYTES(ptr, 1), 0, size - 2);

    //verified are: second byte of the first page to last byte -1 of last page
    EXPECT_EQ(hbw_verify_memory_region(SHIFT_BYTES(ptr, 1), size - 2, 0), 0);
    hbw_free(ptr);
}
