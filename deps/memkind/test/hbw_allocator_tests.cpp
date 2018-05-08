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

#include <hbw_allocator.h>

#include "common.h"
#include <vector>

// Tests for hbw::allocator class.
class HbwAllocatorTests: public :: testing::Test
{

protected:
    void SetUp()
    {}

    void TearDown()
    {}

};

//Test standard memory allocation and deallocation.
TEST_F(HbwAllocatorTests, test_TC_MEMKIND_DetaultAllocatorTest)
{
    const size_t size = 512;
    hbw::allocator<size_t> allocator;

    hbw::allocator<size_t>::pointer ptr = allocator.allocate(size);

    ASSERT_TRUE(NULL != ptr);

    //Do the actually memory writing
    for (size_t i=0; i<size; i++) {
        ptr[i] = i;
    }

    allocator.deallocate(ptr, size);
}

//Test address convertion functionality.
TEST_F(HbwAllocatorTests, test_TC_MEMKIND_AddressConvertion)
{
    const size_t size = 512;
    hbw::allocator<int> allocator;

    int* ptr = allocator.allocate(size);

    ASSERT_TRUE(NULL != ptr);

    const int excpected_val = 4;
    ptr[0] = excpected_val;

    hbw::allocator<int>::reference reference = *ptr;
    hbw::allocator<int>::const_reference const_reference = *ptr;

    hbw::allocator<int>::pointer test_ptr = allocator.address(reference);
    hbw::allocator<int>::const_pointer test_const_ptr = allocator.address(const_reference);

    ASSERT_TRUE(NULL != test_ptr);
    ASSERT_TRUE(NULL != test_const_ptr);

    EXPECT_EQ(excpected_val, test_ptr[0]);
    EXPECT_EQ(excpected_val, test_const_ptr[0]);

    allocator.deallocate(ptr, size);
}

//Test for boundaries of allocation sizes.
//We expect to catch allocation exceptions caused by out of bound sizes.
TEST_F(HbwAllocatorTests, test_TC_MEMKIND_AllocationSizeOutOfBounds)
{
    hbw::allocator<size_t> allocator;

    const size_t over_size = allocator.max_size() + 1;

    ASSERT_THROW(allocator.allocate(over_size), std::bad_alloc);

    const size_t max_size = -1; //This will give maximum value of size_t.

    ASSERT_THROW(allocator.allocate(max_size), std::bad_alloc);

    ASSERT_THROW(allocator.allocate(0), std::bad_alloc);
}

//Test if variable will be constructed.
TEST_F(HbwAllocatorTests, test_TC_MEMKIND_AllocatorConstruct)
{
    hbw::allocator<int> allocator;

    int x = 0;
    int expect_val = 4;
    allocator.construct(&x, expect_val);

    EXPECT_EQ(expect_val, x);
}

//Test the integration with std::vector.
TEST_F(HbwAllocatorTests, test_TC_MEMKIND_StandardVector)
{
    std::vector<int, hbw::allocator<int> > vec;
    const size_t size = 10000;

    for (size_t i=0; i<size; i++) {
        vec.push_back(i);
    }

    EXPECT_EQ(size, vec.size());
    EXPECT_EQ(1, vec[1]);

    vec.clear();

    EXPECT_EQ((size_t)0, vec.size());
}
