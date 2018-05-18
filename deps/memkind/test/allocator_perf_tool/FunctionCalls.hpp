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

#include "VectorIterator.hpp"
#include "Configuration.hpp"
#include <vector>
#include <cstdlib>

class FunctionCalls
{
public:
	enum {FREE, MALLOC, CALLOC, REALLOC, NUM_OF_FUNCTIONS};

	static const std::string function_name(unsigned type)
	{
		static std::string names[] = {"free", "malloc", "calloc", "realloc"};

		if(type >= NUM_OF_FUNCTIONS) assert(!"Invalidate input argument!");

		return names[type];
	}

	static unsigned function_type(const std::string& name)
	{
		for (unsigned i=0; i<NUM_OF_FUNCTIONS; i++)
		{
			if(function_name(i) == name)
				return i;
		}

		assert(!"Invalid input argument!");
	}

	static VectorIterator<int> generate_random_allocator_func_calls(int call_num, int seed, TypesConf func_calls)
	{
		std::vector<unsigned> avail_types;

		std::srand(seed);
		std::vector<int> calls;

		for (int i=0; i<call_num; i++)
		{
			int index;
			do
			{
				index = rand() % (NUM_OF_FUNCTIONS);
			}
			while(!func_calls.is_enabled(index));

			calls.push_back(index);
		}

		return VectorIterator<int>::create(calls);
	}

};