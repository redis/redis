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
#include "Workload.hpp"
#include "FunctionCalls.hpp"
#include "Allocator.hpp"

#include <string.h>

class ScenarioWorkload :
	public Workload
{
public:
	ScenarioWorkload(VectorIterator<Allocator*>* a, VectorIterator<size_t>* as,  VectorIterator<int>* fc);
	~ScenarioWorkload(void);

	double get_time_costs();

	const std::vector<memory_operation>& get_allocations_info() const {return allocations;}

	bool run();

	memory_operation* get_allocated_memory();

	void enable_touch_memory_on_allocation(bool enable)
	{
		touch_memory_on_allocation = enable;
	}

	void post_allocation_check(const memory_operation& data);

private:
	std::vector<memory_operation> allocations;

	bool touch_memory_on_allocation;

	VectorIterator<int>* func_calls;
	VectorIterator<size_t>* alloc_sizes;
	VectorIterator<Allocator*>* allocators;
};

