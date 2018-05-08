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
//#define PRINT_LOG

#include <stdio.h>
#include <assert.h>
#include <map>
#include <iostream>
#include <vector>

#include "Configuration.hpp"
#include "AllocationSizes.hpp"
#include "AllocatorFactory.hpp"
#include "TaskFactory.hpp"
#include "Task.hpp"
#include "ConsoleLog.hpp"
#include "Stats.hpp"
#include "Thread.hpp"
#include "FootprintTask.h"
#include "Tests.hpp"
#include "MemoryFootprintStats.hpp"
#include "Numastat.hpp"
#include "CommandLine.hpp"
#include "FootprintSampling.h"
#include "FunctionCallsPerformanceTask.h"
#include "StressIncreaseToMax.h"
#include "CSVLogger.hpp"

/*
Command line description.
Syntax:
	key=value
Options:
	- 'test' - specify the test case. This option can be used with the following values: 'footprint', 'calls', 'all' or 'self',
	where:
		'footprint' - memory footprint test,
		'calls' - function calls performance test,
		'all' - execute both above ('footprint' and 'calls') tests,
		'self' - execute self tests
		's1' - stress tests
		(perform allocations until the maximum amount of allocated memory has been reached, than frees allocated memory.
		If the time interval has not been exceed, than repeat the test),
	- 'operations' - the number of memory operations per thread
	- 'size_from' - lower bound for the random sizes of allocation
	- 'size_to' - upper bound for the random sizes of allocation
	- 'seed' - random seed
	- 'threads_num' - the number of threads per test case
	- 'time' - minimum execution time interval
	- 'kind' - the kind to test
	- 'reserved_unallocated' - limit memory allocations to leave unallocated memory (in MB), where available_memory = free - reserved_unallocated
	- 'csv_log' - if 'true' then log to csv file memory operations and statistics
	- 'check_memory_availability' - when 'false' does not check memory avaiability before memory operation
	- 'call' specify the allocation function call. This option can be used with the following values: 'malloc' (default), 'calloc', 'realloc',
* - maximum of available memory in OS, or maximum memory based 'operations' parameter
Example:
1. Performance test:
./perf_tool test=all operations=1000 size_from=32 size_to=20480 seed=11 threads_num=200
2. Stress test
./perf_tool test=s1 time=120 kind=MEMKIND_HBW size_from=1048576 csv_log=true reserved_unallocated=15
*/

int main(int argc, char* argv[])
{
	unsigned mem_operations_num = 1000;
	size_t size_from = 32, size_to = 2048*1024;
	unsigned seed = 11;
	//should be at least one
	size_t threads_number = 10;

	CommandLine cmd_line(argc, argv);

	if((argc >= 1) && cmd_line.is_option_set("test", "self"))
	{
		execute_self_tests();
		getchar();
	}

	cmd_line.parse_with_strtol("operations", mem_operations_num);
	cmd_line.parse_with_strtol("size_from", size_from);
	cmd_line.parse_with_strtol("size_to", size_to);
	cmd_line.parse_with_strtol("seed", seed);
	cmd_line.parse_with_strtol("threads_num", threads_number);

	bool is_csv_log_enabled = cmd_line.is_option_set("csv_log", "true");
	bool check_memory_availability = !cmd_line.is_option_set("check_memory_availability", "false");

	unsigned reserved_unallocated = 0;
	cmd_line.parse_with_strtol("reserved_unallocated", reserved_unallocated);

	//Heap Manager initialization
	std::vector<AllocatorFactory::initialization_stat> stats = AllocatorFactory().initialization_test();

	if(!cmd_line.is_option_set("print_init_stats", "false"))
	{
		printf("\nInitialization overhead:\n");
		for (int i=0; i<stats.size(); i++)
		{
			AllocatorFactory::initialization_stat stat = stats[i];
			printf("%32s : time=%7.7f.s, ref_delta_time=%15f, node0=%10fMB, node1=%7.7fMB\n",
				AllocatorTypes::allocator_name(stat.allocator_type).c_str(),
				stat.total_time,
				stat.ref_delta_time,
				stat.memory_overhead[0],
				stat.memory_overhead[1]);
		}
	}

	//Stress test by repeatedly increasing memory (to maximum), until given time interval has been exceed.
	if(cmd_line.is_option_set("test", "s1"))
	{
		printf("Stress test (StressIncreaseToMax) start. \n");

		if(!cmd_line.is_option_present("operations"))
			mem_operations_num = 1000000;

		unsigned time = 120; //Default time interval.
		cmd_line.parse_with_strtol("time", time);

		unsigned allocator = AllocatorTypes::MEMKIND_HBW;
		if(cmd_line.is_option_present("kind"))
		{
			//Enable memkind allocator and specify kind.
			allocator = AllocatorTypes::allocator_type(cmd_line.get_option_value("kind"));
		}
		TypesConf allocator_types;
		allocator_types.enable_type(allocator);

		TypesConf enable_func_calls;
		enable_func_calls.enable_type(FunctionCalls::MALLOC);

		TaskConf task_conf = {
			mem_operations_num,
			{
				mem_operations_num,
				reserved_unallocated,
				size_from, //No random sizes.
				size_from
			},
			enable_func_calls,
			allocator_types,
			11,
			is_csv_log_enabled,
			check_memory_availability
		};

		StressIncreaseToMax::execute_test_iterations(task_conf, time);
		return 0;
	}

	printf("\nTest configuration: \n");
	printf("\t memory operations per thread = %u \n", mem_operations_num);
	printf("\t seed = %d\n", seed);
	printf("\t number of threads = %u\n", threads_number);
	printf("\t size from-to = %u-%u\n\n", size_from, size_to);

	assert(size_from <= size_to);
#ifdef PRINT_LOG
	float min = convert_bytes_to_mb(size_from * N *  threads_number);
	float mid = convert_bytes_to_mb(((size_from + size_to) / 2.0) * N *  threads_number);
	float max = convert_bytes_to_mb(size_to* N *  threads_number);
	printf("Allocation bound: min: %f, mid: %f, max: %f\n",  min, mid, max);
#endif


	TypesConf func_calls;
	func_calls.enable_type(FunctionCalls::FREE);

	if(cmd_line.is_option_present("call"))
	{
		//Enable heap manager function call.
		func_calls.enable_type(FunctionCalls::function_type(cmd_line.get_option_value("call")));
	}
	else
	{
		func_calls.enable_type(FunctionCalls::MALLOC);
	}

	TypesConf allocator_types;
	if(cmd_line.is_option_present("allocator"))
	{
		allocator_types.enable_type(AllocatorTypes::allocator_type(cmd_line.get_option_value("allocator")));
	}
	else
	{
		for(unsigned i=0; i<=AllocatorTypes::MEMKIND_HBW_PREFERRED; i++)
		{
			allocator_types.enable_type(i);
		}
	}

	TaskConf conf = {
		mem_operations_num, //number memory operations
		{
			mem_operations_num, //number of memory operations
			reserved_unallocated, //reserved unallocated memory to limit allocations
			size_from, //min. size of single allocation
			size_to //max. size of single allocatioion
		},
		func_calls, //enable function calls
		allocator_types, //enable allocators
		seed, //random seed
		is_csv_log_enabled,
		check_memory_availability
	};

	//Footprint test
	if(cmd_line.is_option_set("test", "footprint") || cmd_line.is_option_set("test", "all"))
	{

		TypesConf allocator_type;
		allocator_type.enable_type(AllocatorTypes::MEMKIND_HBW);
		conf.allocators_types = allocator_type;

		std::vector<Thread*> threads;
		std::vector<FootprintTask*> tasks;

		FootprintSampling sampling;
		Thread sampling_thread(&sampling);

		TaskFactory task_factory;

		for (int i=0; i<threads_number; i++)
		{
			FootprintTask* task = static_cast<FootprintTask*>(task_factory.create(TaskFactory::FOOTPRINT_TASK, conf));

			sampling.register_task(task);
			tasks.push_back(task);
			threads.push_back(new Thread(task));
		}

		ThreadsManager threads_manager(threads);
		threads_manager.start(); //Threads begins to execute tasks with footprint workload.
		sampling_thread.start(); //Start sampling in separated thread.
		threads_manager.barrier(); //Waiting until each thread has completed.
		sampling.stop(); //Stop sampling.
		sampling_thread.wait(); //Wait for the sampling thread.

		MemoryFootprintStats mem_footprint_stats = sampling.get_memory_footprint_stats();
		TimeStats stats;
		for (int i=0; i<tasks.size(); i++)
		{
			stats += tasks[i]->get_results();
		}

		ConsoleLog::print_stats(mem_footprint_stats);
		ConsoleLog::print_requested_memory(stats, "footprint test");

		threads_manager.release();
	}

	//Function calls test
	if(cmd_line.is_option_set("test", "calls") || cmd_line.is_option_set("test", "all"))
	{
		TaskFactory task_factory;
		std::vector<Thread*> threads;
		std::vector<Task*> tasks;

		for (int i=0; i<threads_number; i++)
		{
			FunctionCallsPerformanceTask* task = static_cast<FunctionCallsPerformanceTask*>(
				task_factory.create(TaskFactory::FUNCTION_CALLS_PERFORMANCE_TASK, conf)
				);
			tasks.push_back(task);
			threads.push_back(new Thread(task));
			conf.seed += 1;
		}

		ThreadsManager threads_manager(threads);
		threads_manager.start();
		threads_manager.barrier();

		TimeStats stats;
		for (int i=0; i<tasks.size(); i++)
		{
			stats += tasks[i]->get_results();
		}

		ConsoleLog::print_table(stats);
		ConsoleLog::print_requested_memory(stats, "func. calls test");

		threads_manager.release();
	}

	return 0;
}
