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

#include <vector>
#include <cstdlib>

#include "VectorIterator.hpp"

class AllocationSizes
{
public:

	static VectorIterator<size_t> generate_random_sizes(int sizes_num, size_t from, size_t to, int seed)
	{
		srand(seed);
		std::vector<size_t> sizes;
		size_t range = to - from;

		if(from == to)
		{
			for (int i=0; i<sizes_num; i++)
				sizes.push_back(from);
		}
		else
		{
			for (int i=0; i<sizes_num; i++)
				sizes.push_back(rand() % (range-1)+from);
		}

		return VectorIterator<size_t>::create(sizes);
	}

	static VectorIterator<size_t> generate_random_sizes(AllocationSizesConf conf, int seed)
	{
		return generate_random_sizes(conf.n, conf.size_from, conf.size_to, seed);
	}

};