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
#include <assert.h>
#include <math.h>

#include "Allocation_info.hpp"
#include "Sample.hpp"

class MemoryFootprintStats
{
public:
	MemoryFootprintStats() {}

	MemoryFootprintStats(
		double total_mem_overhed,
		double max_mem_overhead,
		double average_mem_overhead,
		double max_memory_usage,
		double min_memory_usage,
		double standard_deviation,
		size_t number_of_samples
		)
	{
		this->total_mem_overhed = total_mem_overhed;
		this->max_mem_overhead = max_mem_overhead;
		this->average_mem_overhead = average_mem_overhead;
		this->max_memory_usage = max_memory_usage;
		this->min_memory_usage = min_memory_usage;
		this->standard_deviation = standard_deviation;
		this->number_of_samples = number_of_samples;
	}

	static MemoryFootprintStats generate_stats(
		double before_running_test_memory_usage_sample, // sample in MB
		const std::vector<Sample>& samples // samples in MB
		)
	{
		size_t samples_size = samples.size();

		double max_mem_overhead = -1.0;
		double total_mem_overhed = 0.0;
		double max_memory_usage = 0.0;
		double min_memory_usage = before_running_test_memory_usage_sample;
		double total_mem_overhed_sq = 0.0;

		for (size_t i=0; i<samples_size; i++)
		{
			double memory_usage_sample = samples[i].get_memory_usage();

			double overhead_per_sample = (memory_usage_sample - before_running_test_memory_usage_sample) - samples[i].get_requested_memory();

			//if overhead_per_sample < 0.0 then there is no overhead
			if(overhead_per_sample < 0.0)
				overhead_per_sample = 0;

			total_mem_overhed += overhead_per_sample;
			total_mem_overhed_sq += (overhead_per_sample * overhead_per_sample);

			if(max_memory_usage < memory_usage_sample)
				max_memory_usage = memory_usage_sample;

			if(max_mem_overhead < overhead_per_sample)
				max_mem_overhead = overhead_per_sample;

			if(min_memory_usage > memory_usage_sample)
				min_memory_usage = memory_usage_sample;

#ifdef PRINT_LOG
			printf("samples[%d].get_memory_usage(): %f \n", i, memory_usage_sample);
			printf("max_memory_usage: %f \n", max_memory_usage);
			printf("overhead_per_sample: %f \n", overhead_per_sample);
			printf("before_running_test_memory_usage_sample: %f \n", before_running_test_memory_usage_sample);
			getchar();
#endif
		}

		double average_mem_overhead = total_mem_overhed / samples_size;
		double standard_deviation = sqrt((total_mem_overhed_sq / samples_size) - (average_mem_overhead * average_mem_overhead));

		return MemoryFootprintStats(
			total_mem_overhed,
			max_mem_overhead,
			average_mem_overhead,
			max_memory_usage,
			min_memory_usage,
			standard_deviation,
			samples_size
			);
	}

	double get_total_mem_overhed() const {return total_mem_overhed;}
	double get_max_mem_overhead() const {return max_mem_overhead;}
	double get_average_mem_overhead() const {return average_mem_overhead;}
	double get_max_memory_usage() const {return max_memory_usage;}
	double get_min_memory_usage() const {return min_memory_usage;}
	double get_standard_deviation() const {return standard_deviation;}
	size_t get_number_of_samples() const {return number_of_samples;}

private:
	double total_mem_overhed;
	double max_mem_overhead;
	double average_mem_overhead;
	double max_memory_usage;
	double min_memory_usage;
	double standard_deviation;
	size_t number_of_samples;
};