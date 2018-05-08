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

#include "common.h"
#include "allocator_perf_tool/HugePageUnmap.hpp"
#include "allocator_perf_tool/HugePageOrganizer.hpp"
#include "TimerSysTime.hpp"
#include "Thread.hpp"

/*
* This test was created because of the munmap() fail in jemalloc.
* There are two root causes of the error:
* - kernel bug (munmap() fails when the size is not aligned)
* - heap Manager doesn’t provide size aligned to 2MB pages for munmap()
* Test allocates 2000MB = 1000 Huge Pages (50threads*10operations*4MBalloc_size),
* but it needs extra Huge Pages due to overhead caused by heap management.
*/
class  HugePageTest: public :: testing::Test
{
private:
    int initial_nr_hugepages;

protected:
    void SetUp()
    {
        // Get initial value of nr_hugepages
        initial_nr_hugepages = HugePageOrganizer::get_nr_hugepages();
        // Set number of Huge Pages to allocate
        // System command returned -1 on error
        ASSERT_NE(HugePageOrganizer::set_nr_hugepages(1024), -1);
    }

    void TearDown()
    {
        // Set number of Huge Pages to previous value
        // System command returned -1 on error
        ASSERT_NE(HugePageOrganizer::set_nr_hugepages(initial_nr_hugepages), -1);
    }

    void run()
    {
        unsigned mem_operations_num = 10;
        size_t threads_number = 50;
        bool touch_memory = true;
        size_t size_1MB = 1024*1024;
        size_t alignment = 2*size_1MB;
        size_t alloc_size = 4*size_1MB;

        std::vector<Thread*> threads;
        std::vector<Task*> tasks;

        TimerSysTime timer;
        timer.start();

        // This bug occurs more frequently under stress of multithreaded allocations.
        for (int i=0; i<threads_number; i++) {
            Task* task = new HugePageUnmap(mem_operations_num, touch_memory, alignment, alloc_size, HBW_PAGESIZE_2MB);
            tasks.push_back(task);
            threads.push_back(new Thread(task));
        }

        float elapsed_time = timer.getElapsedTime();

        ThreadsManager threads_manager(threads);
        threads_manager.start();
        threads_manager.barrier();
        threads_manager.release();

        //task release
        for (int i=0; i<tasks.size(); i++) {
            delete tasks[i];
        }

        RecordProperty("threads_number", threads_number);
        RecordProperty("memory_operations_per_thread", mem_operations_num);
        RecordProperty("elapsed_time", elapsed_time);
    }
};



// Test passes when there is no crash.
TEST_F(HugePageTest, test_TC_MEMKIND_UNMAP_HUGE_PAGE)
{
    ASSERT_HUGEPAGES_AVAILABILITY();
    int iterations = 10;
    for (int i=0; i<iterations; i++) {
        run();
    }
}
