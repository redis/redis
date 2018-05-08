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
#include "FootprintSampling.h"


FootprintSampling::~FootprintSampling(void)
{
}

void FootprintSampling::run()
{
	before_running_sample = Numastat::get_total_memory(1);
	while(is_running)
	{
		usleep(1);
		float total_requested_memory = 0.0;
		for (int i=0; i<tasks.size(); i++)
		{
			tasks[i]->run_pause();
			float requested_mem_per_task = tasks[i]->get_current_requested_memory();
			total_requested_memory += requested_mem_per_task;
		}

		samples.push_back(Sample(Numastat::get_total_memory(1), total_requested_memory));

#ifdef PRINT_LOG
		printf("Sample[%d]: numastat = %f, requested = %f\n",
			samples.size(),
			samples.back().get_acctual_memory(),
			samples.back().get_requested_memory()
			);
		getchar();
#endif

		for (int i=0; i<tasks.size(); i++)
		{
			tasks[i]->run_continue();
		}
	}
}
