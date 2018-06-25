/*
* Copyright (C) 2015 - 2017 Intel Corporation.
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

#include "StressIncreaseToMax.h"


void StressIncreaseToMax::run()
{
	//Generate constant allocation sizes.
	VectorIterator<size_t> allocation_sizes = AllocationSizes::generate_random_sizes(task_conf.allocation_sizes_conf, task_conf.seed);
	//Generate only mallocs.
	VectorIterator<int> func_calls = FunctionCalls::generate_random_allocator_func_calls(task_conf.n, task_conf.seed, task_conf.func_calls);

	unsigned type;
	for (type = 0; type < AllocatorTypes::NUM_OF_ALLOCATOR_TYPES; type++)
	{
		if(task_conf.allocators_types.is_enabled(type))
			break; //Assume that there is only one type.
	}


	AllocatorFactory allocator_factory;
	VectorIterator<Allocator*> allocators_calls = allocator_factory.generate_random_allocator_calls(task_conf.n, task_conf.seed, task_conf.allocators_types);

	ScenarioWorkload scenario_workload(
		&allocators_calls,
		&allocation_sizes,
		&func_calls
	);

	scenario_workload.enable_touch_memory_on_allocation(task_conf.touch_memory);

	test_status.is_allocation_error = false;

	size_t requested_memory = 0;
	bool has_reach_memory_request_limit = false;

	while (!has_reach_memory_request_limit &&
	      !test_status.is_allocation_error &&
	      (test_status.has_next_memory_operation = scenario_workload.run()))
	{
		memory_operation data = scenario_workload.get_allocations_info().back();
		test_status.is_allocation_error = (data.error_code == ENOMEM) || (data.ptr == NULL);

		if(data.allocation_method != FunctionCalls::FREE)
		{
			requested_memory += data.size_of_allocation;
			has_reach_memory_request_limit = requested_memory >= req_mem_limit;
		}
	}

	if(!scenario_workload.get_allocations_info().size() < task_conf.n &&
		!has_reach_memory_request_limit) printf("\nWARNING: Too few memory operations to reach the limit.\n");
	if(test_status.is_allocation_error) printf("\nWARNING: Allocation error. \n");

	results = scenario_workload.get_allocations_info();
}

std::vector<iteration_result> StressIncreaseToMax::execute_test_iterations(
	const TaskConf& task_conf,
	unsigned time,
	size_t requested_memory_limit)
{
		TimerSysTime timer;
		unsigned itr = 0;
		std::vector<iteration_result> results;

		std::ofstream csv_file;

		csv::Row row;
		row.append("Iteration");
		row.append("Allocated memory (MB)");
		row.append("Elapsed time (seconds)");
		if(task_conf.is_csv_log_enabled)
		{
			csv_file.open("stress_test_increase_to_max.csv");
			csv_file << row.export_row();
		}
		printf("%s", row.export_row().c_str());

		timer.start();

		while (timer.getElapsedTime() < time)
		{
			StressIncreaseToMax stress_test(task_conf, requested_memory_limit);
			stress_test.run();
			float elapsed_time = timer.getElapsedTime();

			TimeStats stats;
			stats += stress_test.get_results();

			results.push_back(stress_test.get_test_status());

			//Log every interation of StressIncreaseToMax test.
			csv::Row row;
			row.append(itr);
			row.append(convert_bytes_to_mb(stats.get_allocated()));
			row.append(elapsed_time);
			if(task_conf.is_csv_log_enabled)
			{
				csv_file << row.export_row();
			}
			printf("%s", row.export_row().c_str());
			fflush(stdout);

			itr++;
		}

		printf("\nStress test (StressIncreaseToMax) finish in time %f.\n",timer.getElapsedTime());

		csv_file.close();

		return results;
}