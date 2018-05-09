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
#pragma once
#include "Configuration.hpp"
#include "Stats.hpp"
#include "FunctionCalls.hpp"
#include "MemoryFootprintStats.hpp"

#include <stdio.h>

class ConsoleLog
{
public:
	static void print_stats(TimeStats& stats, unsigned allocator_type, unsigned func_calls)
	{
		if(stats.stats.count(allocator_type))
		{
			if(stats.stats[allocator_type].count(func_calls))
			{
				MethodStats method_stats = stats.stats[allocator_type][func_calls];
				printf(" %20s (%u) | %7s | %10f.s | %10f.s  | %u bytes/%f MB \n",
					AllocatorTypes::allocator_name(allocator_type).c_str(),
					allocator_type,
					FunctionCalls::function_name(func_calls).c_str(),
					method_stats.total_time,
					method_stats.average_time,
					method_stats.allocation_size,
					convert_bytes_to_mb(method_stats.allocation_size)
					);
			}
		}
	}

	static void print_table(TimeStats& stats)
	{
		printf("\n====== Allocators function calls performance =================================================\n");
		printf(" %20s Id:   Method:    Total time:    Average time:  Allocated memory bytes/MB: \n", "Allocator:");
		for (unsigned i=0; i<=AllocatorTypes::MEMKIND_HBW_PREFERRED; i++)
		{
			for (unsigned func_call=FunctionCalls::FREE+1; func_call<FunctionCalls::NUM_OF_FUNCTIONS; func_call++)
			{
				print_stats(stats, i, func_call);
			}
		}
		printf("==============================================================================================\n");
	}

	static void print_stats(const MemoryFootprintStats& stats)
	{
		printf("\n====== Memory footprint stats =======================================\n");
		printf("Overhead sum over every sample: %f MB \n", stats.get_total_mem_overhed());
		printf("Avg overhead: %f MB\n", stats.get_average_mem_overhead());
		printf("Standard deviation: %f MB\n", stats.get_standard_deviation());
		printf("Max overhead: %f MB\n", stats.get_max_mem_overhead());
		printf("Max memory usage: %f MB\n", stats.get_max_memory_usage());
		printf("Min memory usage: %f MB\n", stats.get_min_memory_usage());
		printf("Number of samples: %d\n", stats.get_number_of_samples());
		printf("=====================================================================\n");
	}

	static void print_requested_memory(TimeStats& stats, std::string test_name)
	{
		printf("\n====== Requested memory stats for %s =================\n", test_name.c_str());
		printf("Total requested allocations: %u bytes/%f MB. \n",stats.get_allocated(), convert_bytes_to_mb(stats.get_allocated()));
		printf("Total requested deallocations: %u bytes/%f MB. \n",stats.get_deallocated(), convert_bytes_to_mb(stats.get_deallocated()));
		printf("=====================================================================\n");
	}
};