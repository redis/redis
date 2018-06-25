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

#pragma once

#include <hbwmalloc.h>

#include "Allocator.hpp"
#include "Allocation_info.hpp"
#include "Configuration.hpp"
#include "WrappersMacros.hpp"
#include "FunctionCalls.hpp"
#include <cerrno>

#include <stdlib.h>


class HBWmallocAllocatorWithTimer
	: public Allocator
{
public:
	memory_operation wrapped_malloc(size_t size)
	{
		START_TEST(AllocatorTypes::HBWMALLOC_ALLOCATOR, FunctionCalls::MALLOC)
		data.ptr = hbw_malloc(size);
		END_TEST
	}

	memory_operation wrapped_calloc(size_t num, size_t size)
	{
		START_TEST(AllocatorTypes::HBWMALLOC_ALLOCATOR, FunctionCalls::CALLOC)
		data.ptr = hbw_calloc(num, size);
		END_TEST
	}

	memory_operation wrapped_realloc(void* ptr, size_t size)
	{
		START_TEST(AllocatorTypes::HBWMALLOC_ALLOCATOR, FunctionCalls::REALLOC)
		data.ptr = hbw_realloc(ptr, size);
		END_TEST
	}

	void wrapped_free(void* ptr) {hbw_free(ptr);}

	unsigned type() {return AllocatorTypes::HBWMALLOC_ALLOCATOR;}

};
