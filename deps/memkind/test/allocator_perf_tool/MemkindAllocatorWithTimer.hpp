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

#include <memkind.h>

#include "Allocator.hpp"
#include "Allocation_info.hpp"
#include "Configuration.hpp"
#include "WrappersMacros.hpp"
#include "FunctionCalls.hpp"
#include <cerrno>

#include <stdlib.h>


class MemkindAllocatorWithTimer
	: public Allocator
{
public:
	MemkindAllocatorWithTimer() : kind(MEMKIND_DEFAULT) {}

	MemkindAllocatorWithTimer(memkind_t memory_kind, unsigned kind_type_id)
	{
		kind = memory_kind;
		type_id = kind_type_id;
	}
	~MemkindAllocatorWithTimer(void) {}

	memory_operation wrapped_malloc(size_t size)
	{
		START_TEST(type_id, FunctionCalls::MALLOC)
		data.ptr = memkind_malloc(kind, size);
		data.error_code = errno;
		END_TEST
	}

	memory_operation wrapped_calloc(size_t num, size_t size)
	{
		START_TEST(type_id, FunctionCalls::CALLOC)
		data.ptr = memkind_calloc(kind, num, size);
		data.error_code = errno;
		END_TEST
	}

	memory_operation wrapped_realloc(void* ptr, size_t size)
	{
		START_TEST(type_id, FunctionCalls::REALLOC)
		data.ptr = memkind_realloc(kind, ptr, size);
		data.error_code = errno;
		END_TEST
	}

	void wrapped_free(void* ptr) {memkind_free(kind, ptr);}

	void change_kind(memkind_t memory_kind, unsigned kind_type_id)
	{
		kind = memory_kind;
		type_id = kind_type_id;
	}

	unsigned type() {return type_id;}
	memkind_t get_kind() {return kind;}

private:
	memkind_t kind;
	unsigned type_id;

};
