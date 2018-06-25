/*
 * Copyright (C) 2017 Intel Corporation.
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

#include "common.h"
#include "random_sizes_allocator.h"
#include "proc_stat.h"
#include "allocator_perf_tool/GTestAdapter.hpp"
#include "allocator_perf_tool/Allocation_info.hpp"
#include <memkind.h>
#include <random>

class MemoryFootprintTest: public :: testing::Test
{
private:
    std::default_random_engine generator;

    bool get_random_bool(double probability)
    {
        std::bernoulli_distribution distribution(probability);
        return distribution(generator);
    }
public:
    void test_malloc_free(memkind_t kind, size_t min_size, size_t max_size, int calls_number, double malloc_probability=1.0)
    {
        RandomSizesAllocator allocated_memory(kind, min_size, max_size, calls_number);
        size_t requested_memory_sum = 0;
        size_t max_overhead = 0, current_overhead = 0, overhead_sum = 0;
        size_t initial_virtual_memory = ProcStat::get_virtual_memory_size();
        size_t physical_memory_diff_sum = 0, current_physical_memory_diff = 0, max_physical_memory_diff = 0;
        size_t initial_physical_memory = ProcStat::get_physical_memory_size();
        for (int i = 0; i < calls_number; i++)
        {
            if (allocated_memory.empty() || get_random_bool(malloc_probability))
                requested_memory_sum += allocated_memory.malloc_random_memory();
            else
                requested_memory_sum -= allocated_memory.free_random_memory();

            current_overhead = ProcStat::get_virtual_memory_size() - initial_virtual_memory - requested_memory_sum;
            overhead_sum += current_overhead;
            if (current_overhead > max_overhead)
                max_overhead = current_overhead;

            current_physical_memory_diff = ProcStat::get_physical_memory_size();
            physical_memory_diff_sum += current_physical_memory_diff - initial_physical_memory;
            if (current_physical_memory_diff > max_physical_memory_diff)
                max_physical_memory_diff = current_physical_memory_diff;
        }
        GTestAdapter::RecordProperty("avg_vm_overhead_per_operation_mb", convert_bytes_to_mb(overhead_sum) / calls_number);
        GTestAdapter::RecordProperty("overhead_to_requested_memory_ratio_percent", 100.f * current_overhead / requested_memory_sum);
        GTestAdapter::RecordProperty("avg_vm_overhead_growth_per_operation_mb", convert_bytes_to_mb(current_overhead) / calls_number);
        GTestAdapter::RecordProperty("max_vm_overhead_mb", convert_bytes_to_mb(max_overhead));
        GTestAdapter::RecordProperty("avg_phys_memory_mb", convert_bytes_to_mb(physical_memory_diff_sum) / calls_number);
        GTestAdapter::RecordProperty("max_phys_memory_mb", convert_bytes_to_mb(max_physical_memory_diff));
    }
};

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_DEFAULT_only_malloc_small_allocations)
{
    test_malloc_free(MEMKIND_DEFAULT, 128, 15 * KB, 1000);
}

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_DEFAULT_only_malloc_medium_allocations)
{
    test_malloc_free(MEMKIND_DEFAULT, 16 * KB, 1 * MB, 100);
}

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_DEFAULT_only_malloc_large_allocations)
{
    test_malloc_free(MEMKIND_DEFAULT, 2 * MB, 100 * MB, 20);
}

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_DEFAULT_random_malloc80_free20_random_small_allocations)
{
    test_malloc_free(MEMKIND_DEFAULT, 128, 15 * KB, 1000, 0.8);
}

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_DEFAULT_random_malloc80_free20_random_medium_allocations)
{
    test_malloc_free(MEMKIND_DEFAULT, 16 * KB, 1 * MB, 100, 0.8);
}

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_DEFAULT_random_malloc80_free20_random_large_allocations)
{
    test_malloc_free(MEMKIND_DEFAULT, 2 * MB, 100 * MB, 20, 0.8);
}

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_HBW_only_malloc_small_allocations)
{
    test_malloc_free(MEMKIND_HBW, 128, 15 * KB, 1000);
}

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_HBW_only_malloc_medium_allocations)
{
    test_malloc_free(MEMKIND_HBW, 16 * KB, 1 * MB, 100);
}

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_HBW_only_malloc_large_allocations)
{
    test_malloc_free(MEMKIND_HBW, 2 * MB, 100 * MB, 20);
}

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_HBW_random_malloc80_free20_random_small_allocations)
{
    test_malloc_free(MEMKIND_HBW, 128, 15 * KB, 1000, 0.8);
}

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_HBW_random_malloc80_free20_random_medium_allocations)
{
    test_malloc_free(MEMKIND_HBW, 16 * KB, 1 * MB, 100, 0.8);
}

TEST_F(MemoryFootprintTest, test_TC_MEMKIND_HBW_random_malloc80_free20_random_large_allocations)
{
    test_malloc_free(MEMKIND_HBW, 2 * MB, 100 * MB, 20, 0.8);
}

