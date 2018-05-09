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

#include "FootprintTask.h"


void FootprintTask::run()
{
	VectorIterator<size_t> allocation_sizes = AllocationSizes::generate_random_sizes(task_conf.allocation_sizes_conf, task_conf.seed);
	VectorIterator<int> func_calls = FunctionCalls::generate_random_allocator_func_calls(task_conf.n, task_conf.seed, task_conf.func_calls);

	AllocatorFactory allocator_factory;
	VectorIterator<Allocator*> allocators_calls = allocator_factory.generate_random_allocator_calls(task_conf.n, task_conf.seed, task_conf.allocators_types);

	scenario_workload = new ScenarioWorkload(
		&allocators_calls,
		&allocation_sizes,
		&func_calls
	);

	scenario_workload->enable_touch_memory_on_allocation(true);

	bool has_next;

	do
	{
		pthread_mutex_lock(&mutex);
		has_next = scenario_workload->run();
		pthread_mutex_unlock(&mutex);
	}
	while(has_next);

	results = scenario_workload->get_allocations_info();

	delete scenario_workload;
	scenario_workload = NULL;
}

float FootprintTask::calc_allocated_memory(const std::vector<memory_operation>& operations)
{
	float total_requested_memory = 0.0;

	for (int i=0; i<operations.size(); i++)
	{
		memory_operation data = operations[i];

		double requested_mem_per_sample = convert_bytes_to_mb(data.size_of_allocation);

		if(data.allocation_method == FunctionCalls::FREE)
			total_requested_memory -= requested_mem_per_sample;
		else
			total_requested_memory += requested_mem_per_sample;
	}

	return total_requested_memory;
}

float FootprintTask::get_current_requested_memory()
{
	if(scenario_workload == NULL) return 0.0;

	std::vector<memory_operation> operations = scenario_workload->get_allocations_info();

	return calc_allocated_memory(operations);
}

