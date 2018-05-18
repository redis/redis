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
#include "ScenarioWorkload.h"
#include "AllocatorFactory.hpp"

ScenarioWorkload::ScenarioWorkload(VectorIterator<Allocator*>* a, VectorIterator<size_t>* as,  VectorIterator<int>* fc)
{
	allocators = a;
	func_calls = fc;
	alloc_sizes = as;
}

bool ScenarioWorkload::run()
{
	if(func_calls->has_next() && allocators->has_next() && alloc_sizes->has_next())
	{
		switch(func_calls->next())
		{
		case FunctionCalls::MALLOC:
			{
				memory_operation data = allocators->next()->wrapped_malloc(alloc_sizes->next());
				post_allocation_check(data);
				break;
			}
		case FunctionCalls::CALLOC:
			{
				memory_operation data = allocators->next()->wrapped_calloc(1, alloc_sizes->next());
				post_allocation_check(data);
				break;
			}
		case FunctionCalls::REALLOC:
			{
				//Guarantee the memory for realloc.
				Allocator* allocator = allocators->next();
				memory_operation to_realloc = allocator->wrapped_malloc(512);

				memory_operation data = allocator->wrapped_realloc(to_realloc.ptr ,alloc_sizes->next());
				post_allocation_check(data);
				break;
			}
		case FunctionCalls::FREE:
			{
				memory_operation* data = get_allocated_memory();

				if(!allocations.empty() && (data != NULL))
				{
					AllocatorFactory().get_existing(data->allocator_type)->wrapped_free(data->ptr);
					data->is_allocated = false;

					memory_operation free_op = *data;
					free_op.allocation_method = FunctionCalls::FREE;
					allocations.push_back(free_op);
				}

				break;
			}
		default:
			assert(!"Function call identifier out of range.");
			break;
		}

		return true;
	}

	return false;
}

ScenarioWorkload::~ScenarioWorkload(void)
{
	for (int i=0; i<allocations.size(); i++)
	{
		memory_operation data = allocations[i];
		if(data.is_allocated && (data.allocation_method != FunctionCalls::FREE))
			AllocatorFactory().get_existing(data.allocator_type)->wrapped_free(data.ptr);
	}
}

memory_operation* ScenarioWorkload::get_allocated_memory()
{
	for (int i=allocations.size()-1; i>=0; i--)
	{
		memory_operation* data = &allocations[i];
		if(data->is_allocated)
			return data;
	}

	return NULL;
}

void ScenarioWorkload::post_allocation_check(const memory_operation& data)
{
	allocations.push_back(data);
	if(touch_memory_on_allocation && (data.ptr != NULL) && (data.error_code != ENOMEM))
	{
		//Write memory to ensure physical allocation.
		memset(data.ptr, 1, data.size_of_allocation);
	}
}
