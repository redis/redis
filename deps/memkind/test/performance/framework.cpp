/*
* Copyright (C) 2014 - 2016 Intel Corporation.
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

#include <algorithm> // sort
#include <math.h> // log2
#include <cassert>
#include <iostream>
#include "framework.hpp"

namespace performance_tests
{
    namespace ch = std::chrono;
    using std::cout;
    using std::endl;
    using std::unique_lock;
    using std::mutex;


    #ifdef __DEBUG
    mutex g_coutMutex;
    int g_msgLevel = 1;
    #endif

    void Barrier::wait()
    {
        unique_lock<mutex> lock(m_barrierMutex);
        // Decrement number of threads awaited at the barrier
        m_waiting--;
        if (m_waiting == 0)
        {
            // Called by the last expected thread - notify all waiting threads and exit
            m_cVar.notify_all();
            // Store the time when barrier was released
            if (m_releasedAt.tv_sec == 0 && m_releasedAt.tv_nsec == 0)
            {
                clock_gettime(CLOCK_MONOTONIC, &m_releasedAt);
            }
            return;
        }
        // Wait unitl the last expected thread calls wait() on Barrier instance, or timeout occurs
        m_cVar.wait_until(lock, ch::system_clock::now() + ch::seconds(10), [](){ return GetInstance().m_waiting == 0;});
    }

    // Worker class
    Worker::Worker(
        uint32_t actionsCount,
        const vector<size_t> &allocationSizes,
        Operation *freeOperation,
        memkind_t kind)
        : m_actionsCount(actionsCount)
        , m_allocationSizes(allocationSizes)
        , m_actions(vector<Action*>(actionsCount, nullptr))
        , m_kind(kind)
    {
        assert(freeOperation->getName() == OperationName::Free);
    }

    Worker::~Worker()
    {
        for (Action *action : m_actions) //each action
        {
            delete action;
        }

    }

    void Worker::init(const vector<Operation*> &testOperations, Operation* &freeOperation)
    {
        for(uint32_t i = 0 ; i < m_actionsCount ; i++)
        {
            int bucketSize = rand() % Operation::MaxBucketSize;

            for (Operation *operation : testOperations) //each operation
            {
                if (operation->checkCondition(bucketSize))
                {
                    size_t size = m_allocationSizes[m_allocationSizes.size() > 1 ? rand() % m_allocationSizes.size() : 0];
                    m_actions[i] = new Action(
                        operation,
                        freeOperation,
                        m_kind,
                        size,
                        log2(rand() % size),
                        sizeof(void*) * (1 << ((rand() % Operation::MemalignMaxMultiplier))));
                    break;
                }
            }
        }
    }

    void Worker::run()
    {
        m_thread = new thread(&Worker::work, this);
    }

#ifdef __DEBUG
    uint16_t Worker::getId()
    {
        return m_threadId;
    }
    void Worker::setId(uint16_t threadId)
    {
        m_threadId = threadId;
    }
#endif

    void Worker::finish()
    {
        if (m_thread != nullptr)
        {
            m_thread->join();
            delete m_thread;
        }
    }

    void Worker::work()
    {
        EMIT(1, "Entering barrier " << m_threadId)
        Barrier::GetInstance().wait();
        EMIT(1, "Starting thread " << m_threadId)
        for (Action *action : m_actions)
        {
            action->alloc();
        }
    }

    void Worker::clean()
    {
        EMIT(2, "Cleaning thread " << m_threadId)
        for (Action *action : m_actions)
        {
            action->free();
        }
        EMIT(1, "Thread " << m_threadId << " finished")
    }

    // PerformanceTest class
    PerformanceTest::PerformanceTest(
        size_t repeatsCount,
        size_t threadsCount,
        size_t operationsCount)
        : m_repeatsCount(repeatsCount)
        , m_discardCount(repeatsCount * (distardPercent / 100.0))
        , m_threadsCount(threadsCount)
        , m_operationsCount(operationsCount)
        , m_executionMode(ExecutionMode::SingleInteration)
    {
    }

    void PerformanceTest::setAllocationSizes(const vector<size_t> &allocationSizes)
    {
        m_allocationSizes = allocationSizes;
    }

    void PerformanceTest::setOperations(const vector<vector<Operation*>> &testOperations, Operation *freeOperation)
    {
        m_testOperations = testOperations;
        m_freeOperation = freeOperation;
    }

    void PerformanceTest::setExecutionMode(ExecutionMode executionMode)
    {
        m_executionMode = executionMode;
    }

    void PerformanceTest::setKind(const vector<memkind_t> &kinds)
    {
        m_kinds = kinds;
    }

    inline void PerformanceTest::runIteration()
    {
        timespec iterationStop, iterationStart;

        Barrier::GetInstance().reset(m_threadsCount);
        for (Worker * worker : m_workers)
        {
            worker->run();
        }
        for (Worker * worker : m_workers)
        {
            worker->finish();
        }
        EMIT(1, "Alloc completed");
        clock_gettime(CLOCK_MONOTONIC, &iterationStop);
        iterationStart = Barrier::GetInstance().releasedAt();
        m_durations.push_back(
            (iterationStop.tv_sec  * NanoSecInSec + iterationStop.tv_nsec) -
            (iterationStart.tv_sec * NanoSecInSec + iterationStart.tv_nsec)
        );
        for (Worker * worker : m_workers)
        {
            worker->clean();
        }
    }

    void PerformanceTest::prepareWorkers()
    {
        for (size_t threadId = 0; threadId < m_threadsCount; threadId++)
        {
            m_workers.push_back(
                new Worker(
                m_operationsCount,
                m_allocationSizes,
                m_freeOperation,
                m_kinds.size() > 0 ? m_kinds[threadId % m_kinds.size()] : nullptr)
            );
    #ifdef __DEBUG
            m_workers.back()->setId(threadId);
    #endif
            if (m_executionMode == ExecutionMode::SingleInteration)
            {
                // In ManyIterations mode, operations will be set for each thread at the beginning of each iteration
                m_workers.back()->init(m_testOperations[threadId % m_testOperations.size()], m_freeOperation);
            }
        }
    }

    Metrics PerformanceTest::getMetrics()
    {
        uint64_t totalDuration = 0;

        std::sort(m_durations.begin(), m_durations.end());

        m_durations.erase(m_durations.end() - m_discardCount, m_durations.end());
        for (uint64_t& duration : m_durations)
        {
            totalDuration += duration;
        }

        Metrics metrics;

        metrics.executedOperations = m_durations.size() * m_threadsCount * m_operationsCount;
        metrics.totalDuration = totalDuration;
        metrics.repeatDuration       = (double) totalDuration / ((uint64_t)m_durations.size() * NanoSecInSec);
        metrics.iterationDuration    = metrics.repeatDuration;
        if (m_executionMode == ExecutionMode::ManyIterations)
        {
            metrics.executedOperations *= m_testOperations.size();
            metrics.iterationDuration  /= m_testOperations.size();
        }
        metrics.operationsPerSecond  = (double) metrics.executedOperations * NanoSecInSec / totalDuration;
        metrics.avgOperationDuration = (double) totalDuration / metrics.executedOperations;
        assert(metrics.iterationDuration != 0.0);
        return metrics;
    }

    void PerformanceTest::writeMetrics(const string &suiteName, const string &caseName, const string& fileName)
    {
        Metrics metrics = getMetrics();

        // For thousands separation
        setlocale(LC_ALL, "");
        if (!fileName.empty())
        {
            FILE* f;
            if((f = fopen(fileName.c_str(), "a+")))
            {
                fprintf(f,
                    "%s;%s;%zu;%zu;%lu;%f;%f;%f;%f\n",
                    suiteName.c_str(),
                    caseName.c_str(),
                    m_repeatsCount,
                    m_threadsCount,
                    metrics.executedOperations,
                    metrics.operationsPerSecond,
                    metrics.avgOperationDuration,
                    metrics.iterationDuration,
                    metrics.repeatDuration);
                fclose(f);
            }

        }
        printf("Operations/sec:\t\t\t%'f\n"
               "Avg. operation duration:\t%f nsec\n"
               "Iteration duration:\t\t%f sec\n"
               "Repeat duration:\t\t%f sec\n",
            metrics.operationsPerSecond,
            metrics.avgOperationDuration,
            metrics.iterationDuration,
            metrics.repeatDuration);
    }

    int PerformanceTest::run()
    {
        if (m_testOperations.empty() ||
            m_allocationSizes.empty() ||
            m_freeOperation == nullptr)
        {
            cout << "ERROR: Test not initialized" << endl;
            return 1;
        }
        // Create threads
        prepareWorkers();
        //warmup kinds
        void *alloc = nullptr;

        for (const memkind_t& kind : m_kinds)
        {
            m_testOperations[0][0]->perform(kind, alloc, 1e6);
            m_freeOperation->perform(kind, alloc);
        }
        for (size_t repeat = 0; repeat < m_repeatsCount; repeat++)
        {
            EMIT(1, "Test run #" << repeat)
            if (m_executionMode == ExecutionMode::SingleInteration)
            {
                runIteration();
            }
            else
            {
                // Perform each operations list in separate iteration, for each thread
                for (vector<Operation*> & ops : m_testOperations)
                {
                    for (Worker * worker : m_workers)
                    {
                        worker->init(ops, m_freeOperation);
                    }
                    runIteration();
                }
            }
        }
        return 0;
    }

    void PerformanceTest::showInfo()
    {
        printf("Test parameters: %lu repeats, %lu threads, %d operations per thread\n",
            m_repeatsCount,
            m_threadsCount,
            m_operationsCount);
        printf("Thread memory allocation operations:\n");
        for (unsigned long i = 0; i < m_testOperations.size(); i++)
        {
            if (m_executionMode == ExecutionMode::SingleInteration)
            {
                printf("\tThread %lu,%lu,...\n", i, i + (m_testOperations.size()));
            }
            else
            {
                printf("\tIteration %lu\n", i);
            }
            for (const Operation* op : m_testOperations[i])
            {
                printf("\t\t %s (bucket size: %d)\n", op->getNameStr().c_str(), op->getBucketSize());
            }
        }
        printf("Memory free operation:\n\t\t%s\n", m_freeOperation->getNameStr().c_str());
        printf("Allocation sizes:\n");
        for (size_t size : m_allocationSizes)
        {
            printf("\t\t%lu bytes\n", size);
        }
    }
}
