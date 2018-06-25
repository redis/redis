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
#pragma once

#include <ctime>
#include <sys/time.h>

#include <chrono>
#define START_TEST(ALLOCATOR, METHOD) \
	memory_operation data; \
	data.allocator_type = ALLOCATOR; \
	data.allocation_method = METHOD;\
	data.error_code = 0; \
	std::chrono::system_clock::time_point last = std::chrono::high_resolution_clock::now();
#define END_TEST \
	std::chrono::system_clock::time_point now = std::chrono::high_resolution_clock::now(); \
	std::chrono::duration<double,std::milli> elapsedTime(now - last); \
	data.total_time = elapsedTime.count() / 1000.0; \
	data.size_of_allocation = size; \
	data.error_code = errno; \
	data.is_allocated = true; \
	return data;
