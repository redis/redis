/*
* Copyright (C) 2016 Intel Corporation.
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

#include <vector>
#include "Task.hpp"

class HugePageUnmap: public Task
{

public:
	HugePageUnmap(int operations, bool touch_memory, size_t alignment_size, size_t alloc_size, hbw_pagesize_t page_size) :
	 mem_operations_num(operations),
	 touch_memory(touch_memory),
	 alignment_size(alignment_size),
	 alloc_size(alloc_size),
	 page_size(page_size)
	{}

	~HugePageUnmap()
	{
		for(int i=0; i<mem_operations_num; i++)
		{
			hbw_free(results[i].ptr);
		}
	};

	void run()
	{
		void* ptr = NULL;

		for(int i=0; i<mem_operations_num; i++)
		{
			int ret = hbw_posix_memalign_psize(&ptr, alignment_size, alloc_size, page_size);

			ASSERT_EQ(ret, 0);
			ASSERT_FALSE(ptr == NULL);

			if(touch_memory)
			{
				memset(ptr, alignment_size, alignment_size);
			}

			memory_operation data;
			data.ptr = ptr;
			results.push_back(data);
		}
	}

	std::vector<memory_operation> get_results() {return results;}

private:
	int mem_operations_num;
	std::vector<memory_operation> results;

	bool touch_memory;
	size_t alignment_size;
	size_t alloc_size;
	hbw_pagesize_t page_size;
};

