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

#include <map>
#include <vector>

#include "Allocation_info.hpp"
#include "Configuration.hpp"
#include "FunctionCalls.hpp"

class MethodStats
{
public:
	MethodStats()
	{
		total_time = 0.0;
		average_time = 0;
		allocation_size = 0;
		samples_num = 0;
	}

	double total_time;
	double average_time;
	unsigned samples_num;
	size_t allocation_size;
};


class TimeStats
{
public:

	TimeStats()
	{
		allocated = 0;
		deallocated = 0;
	}

	std::map<unsigned, std::map<unsigned, MethodStats> > stats;

	TimeStats& operator+=(const std::vector<memory_operation>& data)
	{
		for (size_t i=0; i<data.size(); i++)
		{
			memory_operation tmp = data[i];
			MethodStats& method_stats = stats[tmp.allocator_type][tmp.allocation_method];
			method_stats.allocation_size += tmp.size_of_allocation;
			method_stats.total_time += tmp.total_time;
			method_stats.samples_num++;

			//Update average.
			double total_time = method_stats.total_time;
			double samples_num = method_stats.samples_num;
			double avg = total_time/samples_num;
			method_stats.average_time = avg;

			if(tmp.allocation_method != FunctionCalls::FREE)
			{
				allocated += tmp.size_of_allocation;
			}
			else
			{
				deallocated += tmp.size_of_allocation;
			}
		}

		return *this;
	}

	size_t get_allocated() const {return allocated;}
	size_t get_deallocated() const {return deallocated;}

private:
	size_t allocated;
	size_t deallocated;
};

