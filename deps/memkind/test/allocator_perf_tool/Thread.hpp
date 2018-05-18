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
#include <pthread.h>
#include <assert.h>

#include "Runnable.hpp"

class Thread
{
public:
	Thread(Runnable* runnable) : runnable_task(runnable) {}

	void start()
	{
		int err = pthread_create(&thread_handle, NULL, execute_thread, static_cast<void*>(runnable_task));
		assert(!err);
	};

	void wait() {pthread_join(thread_handle, NULL);};

	Runnable* get_runnable_task() {return runnable_task;}

private:
	static void* execute_thread(void * ptr)
	{
		Runnable* runnable = static_cast<Runnable*>(ptr);
		assert(runnable);
		runnable->run();
		pthread_exit(NULL);
	}

	pthread_t thread_handle;
	Runnable* runnable_task;
};


class ThreadsManager
{
public:
	ThreadsManager(std::vector<Thread*>& threads_vec) : threads(threads_vec) {}
	~ThreadsManager()
	{
		release();
	}

	void start()
	{
		for (int i=0; i<threads.size(); i++)
		{
			threads[i]->start();
		}
	}

	void barrier()
	{
		for (int i=0; i<threads.size(); i++)
		{
			threads[i]->wait();
		}
	}

	void release()
	{
		for (int i=0; i<threads.size(); i++)
		{
			delete threads[i];
		}
		threads.clear();
	}

private:
	std::vector<Thread*>& threads;
};
