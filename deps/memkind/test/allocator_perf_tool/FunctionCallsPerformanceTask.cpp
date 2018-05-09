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
#include "FunctionCallsPerformanceTask.h"


void FunctionCallsPerformanceTask::run()
{
	VectorIterator<size_t> allocation_sizes = AllocationSizes::generate_random_sizes(task_conf.allocation_sizes_conf, task_conf.seed);

	VectorIterator<int> func_calls = FunctionCalls::generate_random_allocator_func_calls(task_conf.n, task_conf.seed, task_conf.func_calls);

	AllocatorFactory allocator_types;
	VectorIterator<Allocator*> allocators_calls = allocator_types.generate_random_allocator_calls(task_conf.n, task_conf.seed, task_conf.allocators_types);

	ScenarioWorkload scenario_workload = ScenarioWorkload(
		&allocators_calls,
		&allocation_sizes,
		&func_calls
	);

	while (scenario_workload.run());

	results = scenario_workload.get_allocations_info();
}