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

#include <cstdlib>
#include <vector>
#include <assert.h>

#include "Task.hpp"
#include "FootprintTask.h"
#include "FunctionCallsPerformanceTask.h"
#include "Configuration.hpp"


class TaskFactory
{
public:

	enum {FOOTPRINT_TASK, FUNCTION_CALLS_PERFORMANCE_TASK};

	Task* create(unsigned type, TaskConf conf)
	{
		Task* task = NULL;

		switch(type)
		{
			case FOOTPRINT_TASK:
					{
						task = new FootprintTask(conf);
						break;
					}
			case FUNCTION_CALLS_PERFORMANCE_TASK:
					{
						task = new FunctionCallsPerformanceTask(conf);
						break;
					}

			default:
				assert(!"'type' out of range!");
		}

		tasks.push_back(task);

		return task;
	}

	~TaskFactory()
	{
		for (int i=0; i<tasks.size(); i++)
		{
			delete tasks[i];
		}
	}

private:
	std::vector<Task*> tasks;
};
