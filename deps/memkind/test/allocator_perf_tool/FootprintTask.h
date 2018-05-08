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
#include <jemalloc/jemalloc.h>
#include <pthread.h>

#include "Configuration.hpp"
#include "ScenarioWorkload.h"
#include "FunctionCalls.hpp"
#include "AllocationSizes.hpp"
#include "VectorIterator.hpp"
#include "AllocatorFactory.hpp"
#include "Task.hpp"

class FootprintTask
	: public Task
{
public:
	FootprintTask(TaskConf conf)
	{
		task_conf = conf;
		pthread_mutex_init(&mutex, NULL);
		scenario_workload = NULL;
	}
	~FootprintTask(void)
	{
		pthread_mutex_destroy(&mutex);
		delete scenario_workload;
	}

	void run();

	void run_pause() {pthread_mutex_lock(&mutex);}
	void run_continue() {pthread_mutex_unlock(&mutex);}

	//Calculate number of requested memory by current run.
	//Return number in MB.
	static float calc_allocated_memory(const std::vector<memory_operation>& operations);
	//Return currently requested memory in MB.
	float get_current_requested_memory();

	//Return memory operations from the last run.
	std::vector<memory_operation> get_results() {return results;}

private:
	TaskConf task_conf;
	ScenarioWorkload* scenario_workload;
	std::vector<memory_operation> results;
	pthread_mutex_t mutex;
};

