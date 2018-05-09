/*
 * Copyright (C) 2015 - 2016 Intel Corporation.
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

#include <chrono>
#include "common.h"
#include "allocator_perf_tool/Configuration.hpp"
#include "allocator_perf_tool/StressIncreaseToMax.h"
#include "allocator_perf_tool/HugePageOrganizer.hpp"

//memkind stress and longevity tests using Allocatr Perf Tool.
class AllocateToMaxStressTests: public :: testing::Test
{

protected:
    void SetUp()
    {}

    void TearDown()
    {}

    void run(unsigned kind, unsigned operations, unsigned size_from, unsigned size_to, unsigned reserved_unallocated)
    {
        RecordProperty("kind", AllocatorTypes::allocator_name(kind));
        RecordProperty("memory_operations", operations);
        RecordProperty("size_from", size_from);
        RecordProperty("size_to", size_to);

        TaskConf task_conf = {
            operations, //number of memory operations
            {
                operations, //number of memory operations
                reserved_unallocated, //reserved unallocated
                size_from, //no random sizes.
                size_to
            },
            TypesConf(FunctionCalls::MALLOC), //enable allocator function call
            TypesConf(kind), //enable allocator
            11, //random seed
            false, //disable csv logging
            true //check memory availability
        };

        std::chrono::time_point<std::chrono::system_clock> start, end;
        start = std::chrono::system_clock::now();

        //Execute test iterations.
        std::vector<iteration_result> results = StressIncreaseToMax::execute_test_iterations(task_conf, 1);

        end = std::chrono::system_clock::now();

        std::chrono::duration<double> elapsed_time = end - start;

        RecordProperty("elapsed_time", elapsed_time.count());

        //Check finish status.
        EXPECT_EQ(check_allocation_errors(results, task_conf), 0);
    }

    //Check true allocation errors over all iterations.
    //Return iteration number (>0) when error occurs, or zero
    int check_allocation_errors(std::vector<iteration_result>& results, const TaskConf& task_conf)
    {
        for (size_t i=0; i<results.size(); i++)
        {
            //Check if test ends with allocation error when reserved unallocated limit is enabled.
           if(results[i].is_allocation_error
                && task_conf.allocation_sizes_conf.reserved_unallocated)
           {
                return i+1;
           }
        }

        return 0;
    }

};

//Allocate memory to max using MEMKIND_HBW kind.
//NOTE: Allocated memory is limited (allocated_memory = total_free - reserved_unallocated).
TEST_F(AllocateToMaxStressTests, test_TC_MEMKIND_slts_ALLOCATE_TO_MAX_MEMKIND_HBW)
{
    run(AllocatorTypes::MEMKIND_HBW, 10000, 2048, 2048, 15);
}

//Allocate memory to max using MEMKIND_INTERLEAVE kind.
//NOTE: Allocated memory is limited (allocated_memory = total_free - reserved_unallocated).
TEST_F(AllocateToMaxStressTests, test_TC_MEMKIND_slts_ALLOCATE_TO_MAX_MEMKIND_INTERLEAVE)
{
    run(AllocatorTypes::MEMKIND_INTERLEAVE, 1000, 1048576, 1048576, 15000);
}

//Allocate memory to max using MEMKIND_HBW_PREFERRED kind.
//NOTE: Allocated memory is limited (allocated_memory = total_free - reserved_unallocated).
TEST_F(AllocateToMaxStressTests, test_TC_MEMKIND_slts_ALLOCATE_TO_MAX_MEMKIND_HBW_PREFERRED)
{
    run(AllocatorTypes::MEMKIND_HBW_PREFERRED, 10000, 1048576, 1048576, 0);
}

//Allocate memory to max using MEMKIND_HBW_HUGETLB kind.
//NOTE: Allocated memory is limited (allocated_memory = total_free - reserved_unallocated).
TEST_F(AllocateToMaxStressTests, test_TC_MEMKIND_2MBPages_slts_ALLOCATE_TO_MAX_MEMKIND_HBW_HUGETLB)
{
    ASSERT_HUGEPAGES_AVAILABILITY();
    run(AllocatorTypes::MEMKIND_HBW_HUGETLB, 10000, 2048, 2048, 8);
}

//Allocate memory to max using MEMKIND_HBW kind with different sizes.
//NOTE: Allocated memory is limited (allocated_memory = total_free - reserved_unallocated).
TEST_F(AllocateToMaxStressTests, test_TC_MEMKIND_slts_ALLOCATE_TO_MAX_DIFFERENT_SIZES)
{
    run(AllocatorTypes::MEMKIND_HBW, 2500, 1, 8*1024*1024, 150);
}
