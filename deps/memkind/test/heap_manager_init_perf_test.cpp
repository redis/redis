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
#include <sstream>
#include <vector>

#include "common.h"
#include "allocator_perf_tool/Configuration.hpp"
#include "allocator_perf_tool/AllocatorFactory.hpp"
#include "allocator_perf_tool/HugePageOrganizer.hpp"

//Test heap managers initialization performance.
class HeapManagerInitPerfTest: public :: testing::Test
{

protected:
    void SetUp()
    {
        //Calculate reference statistics.
        ref_time = allocator_factory.initialize_allocator(AllocatorTypes::STANDARD_ALLOCATOR).total_time;
    }

    void TearDown()
    {}

    void run_test(unsigned allocator_type)
    {
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_DEFAULT);

        post_test(stat);
    }

    void post_test(AllocatorFactory::initialization_stat& stat)
    {
        //Calculate (%) distance to the reference time for function calls.
        stat.ref_delta_time = allocator_factory.calc_ref_delta(ref_time, stat.total_time);

        std::stringstream elapsed_time;
        elapsed_time << stat.total_time;

        std::stringstream ref_delta_time;
        ref_delta_time << std::fixed << stat.ref_delta_time << std::endl;

        RecordProperty("elapsed_time", elapsed_time.str());
        RecordProperty("ref_delta_time_percent_rate", ref_delta_time.str());

        for (int i=0; i<stat.memory_overhead.size(); i++)
        {
            std::stringstream node;
            node << "memory_overhad_node_" << i;
            std::stringstream memory_overhead;
            memory_overhead << stat.memory_overhead[i];
            RecordProperty(node.str(), memory_overhead.str());
        }
    }

    AllocatorFactory allocator_factory;
    float ref_time;
};


TEST_F(HeapManagerInitPerfTest, test_TC_MEMKIND_perf_libinit_DEFAULT)
{
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_DEFAULT);
        post_test(stat);
}

TEST_F(HeapManagerInitPerfTest, test_TC_MEMKIND_perf_libinit_HBW)
{
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_HBW);
        post_test(stat);
}

TEST_F(HeapManagerInitPerfTest, test_TC_MEMKIND_perf_libinit_INTERLEAVE)
{
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_INTERLEAVE);
        post_test(stat);
}

TEST_F(HeapManagerInitPerfTest, test_TC_MEMKIND_perf_libinit_HBW_INTERLEAVE)
{
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_HBW_INTERLEAVE);
        post_test(stat);
}

TEST_F(HeapManagerInitPerfTest, test_TC_MEMKIND_perf_libinit_HBW_PREFERRED)
{
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_HBW_PREFERRED);
        post_test(stat);
}

TEST_F(HeapManagerInitPerfTest, test_TC_MEMKIND_perf_libinit_HUGETLB)
{
        ASSERT_HUGEPAGES_AVAILABILITY();
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_HUGETLB);
        post_test(stat);
}

TEST_F(HeapManagerInitPerfTest, test_TC_MEMKIND_perf_libinit_GBTLB)
{
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_GBTLB);
        post_test(stat);
}

TEST_F(HeapManagerInitPerfTest, test_TC_MEMKIND_perf_libinit_HBW_HUGETLB)
{
        ASSERT_HUGEPAGES_AVAILABILITY();
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_HBW_HUGETLB);
        post_test(stat);
}

TEST_F(HeapManagerInitPerfTest, test_TC_MEMKIND_perf_libinit_HBW_PREFERRED_HUGETLB)
{
        ASSERT_HUGEPAGES_AVAILABILITY();
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_HBW_PREFERRED_HUGETLB);
        post_test(stat);
}

TEST_F(HeapManagerInitPerfTest, test_TC_MEMKIND_perf_ext_libinit_HBW_GBTLB)
{
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_HBW_GBTLB);
        post_test(stat);
}

TEST_F(HeapManagerInitPerfTest, test_TC_MEMKIND_perf_libinit_HBW_PREFERRED_GBTLB)
{
        AllocatorFactory::initialization_stat stat = allocator_factory.initialize_allocator(AllocatorTypes::MEMKIND_HBW_PREFERRED_GBTLB);
        post_test(stat);
}

