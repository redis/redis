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

#include "Allocator.hpp"
#include "Allocation_info.hpp"
#include "Configuration.hpp"
#include "WrappersMacros.hpp"
#include "FunctionCalls.hpp"

#include <stdlib.h>

class StandardAllocatorWithTimer
	: public Allocator
{
public:

	memory_operation wrapped_malloc(size_t size)
	{
		START_TEST(AllocatorTypes::STANDARD_ALLOCATOR, FunctionCalls::MALLOC)
		data.ptr = malloc(size);
		END_TEST
	}

	memory_operation wrapped_calloc(size_t num, size_t size)
	{
		START_TEST(AllocatorTypes::STANDARD_ALLOCATOR, FunctionCalls::CALLOC)
		data.ptr = calloc(num, size);
		END_TEST
	}
	memory_operation wrapped_realloc(void* ptr, size_t size)
	{
		START_TEST(AllocatorTypes::STANDARD_ALLOCATOR, FunctionCalls::REALLOC)
		data.ptr = realloc(ptr, size);
		END_TEST
	}

	void wrapped_free(void* ptr) {free(ptr);}

	unsigned type() {return AllocatorTypes::STANDARD_ALLOCATOR;}
};
