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

#pragma once

#include <memkind.h>

#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
// Malloc, jemalloc, memkind jemalloc and memkind memory operations definitions
#include "operations.hpp"

/* Framework for testing memory allocators pefromance */
namespace performance_tests
{
// Nanoseconds in second
    const uint32_t NanoSecInSec = 1e9;

    // Simple barrier implementation
    class Barrier
    {
        // Barrier mutex
        std::mutex m_barrierMutex;
        // Contitional variable
        std::condition_variable m_cVar;
        // Number of threads expected to enter the barrier
        size_t m_waiting;
        timespec m_releasedAt;

    public:
        // Called by each thread entering the barrier; returns control to caller
        // only after been called from the last thread expected at the barrier
        void wait();

            // (Re)Initializes the barrier
        void reset(unsigned waiting)
        {
            m_releasedAt.tv_sec = m_releasedAt.tv_nsec = 0;
            m_waiting = waiting;
        }

            // Get time when barrier was released
        timespec& releasedAt()
        {
            return m_releasedAt;
        }

        // Singleton
        static Barrier& GetInstance()
        {
            // Automatically created and deleted one and only instance
            static Barrier instance;
            return instance;
        }
    private:
        Barrier()
        {
            reset(0);
        }
        // Cannot be used with singleton, so prevent compiler from creating them automatically
        Barrier(Barrier const&)        = delete;
        void operator=(Barrier const&) = delete;
    };

    // Data of a single test action, that is, memory operation (malloc, calloc, etc.)
    // to perform and its parameters (size, alignment etc.)
    class Action
    {
    protected:
        Operation* m_operation;
        Operation* m_freeOperation;
        const memkind_t m_kind;
        void* m_allocation;
        const size_t m_size;
        const size_t m_offset;
        const size_t m_alignment;

    public:
        Action(
            Operation* operation,
            Operation* freeOperation,
            const memkind_t kind,
            const size_t size,
            const size_t offset,
            const size_t alignment)
            : m_operation(operation)
            , m_freeOperation(freeOperation)
            , m_kind(kind)
            , m_allocation(nullptr)
            , m_size(size)
            , m_offset(offset)
            , m_alignment(alignment)
        {}

        Action(
            Operation* operation,
            Operation* freeOperation,
            const memkind_t kind)
            : Action(operation, freeOperation, kind, 0, 0, 0)
        {
        }

        void alloc()
        {
            m_operation->perform(m_kind, m_allocation, m_size, m_offset, m_alignment);
        }

        void free()
        {
            m_freeOperation->perform(m_kind, m_allocation);
        }

    };

    // Performs and tracks requested memory operations in a separate thread
    class Worker
    {
    protected:
        #ifdef __DEBUG
        uint16_t m_threadId;
        #endif
        // Requested number of test actions
        const uint32_t m_actionsCount;
        // List of memory block sizes - for each memory allocation operation actual value is chosen randomly
        const vector<size_t> &m_allocationSizes;
        // List of test actions
        vector<Action*> m_actions;
        // Memory free action
        Action *m_freeAction;
        // Operation kind (useful for memkind only)
        memkind_t m_kind;
        // Working thread
        thread* m_thread;

    public:
        Worker(
            uint32_t actionsCount,
            const vector<size_t> &allocationSizes,
            Operation *freeOperation,
            memkind_t kind);

        ~Worker();

        // Set operations list for the worker
        void init(const vector<Operation*> &testOperations, Operation * &freeOperation);

        // Create & start thread
        void run();

        #ifdef __DEBUG
            // Get thread id
        uint16_t getId();

            // Set thread id
        void setId(uint16_t threadId);
        #endif

            // Finish thread and free all allocations made
        void finish();

        // Free allocated memory
        virtual void clean();

    private:
        // Actual thread function (allow inheritance)
        virtual void work();
    };

    enum ExecutionMode
    {
        SingleInteration, // Single iteration, operations listS will be distributed among threads sequentially
        ManyIterations    // Each operations list will be run in separate iteration by each thread
    };

    struct Metrics
    {
        uint64_t executedOperations;
        uint64_t totalDuration;
        double operationsPerSecond;
        double avgOperationDuration;
        double iterationDuration;
        double repeatDuration;
    };

    // Performance test parameters class
    class PerformanceTest
    {
    protected:
        // empirically determined % of worst results needed to be discarded
        // to eliminate malloc() performance results skewness
        static constexpr double distardPercent = 20.0;
    protected:
        // Number of test repeats
        size_t                      m_repeatsCount;
        // Number of test repeats with worst results to be discarded
        size_t                      m_discardCount;
        // Number of threads
        size_t                      m_threadsCount;
        // Number of memory operations in each thread
        uint32_t                    m_operationsCount;
        // List of allocation sizes
        vector<size_t>              m_allocationSizes;
        // List of list of allocation operations, utlization depends on execution mode
        vector<vector<Operation*>>  m_testOperations;
        // Free operation
        Operation*                  m_freeOperation;
        // List of memory kinds (for memkind allocation only)
        // distributed among threads sequentially
        vector<memkind_t>           m_kinds;
        // List of thread workers
        vector<Worker*>             m_workers;
        // Time measurment
        vector<uint64_t>            m_durations;
        // Execution mode
        ExecutionMode               m_executionMode;

    public:
        // Create test
        PerformanceTest(
            size_t repeatsCount,
            size_t threadsCount,
            size_t operationsCount);

        virtual ~PerformanceTest() {}

            // Set list of block sizes
        void setAllocationSizes(const vector<size_t>& allocationSizes);

            // Set list of operations per thread/per iteration (depending on execution mode)
        void setOperations(const vector<vector<Operation*>>& testOperations, Operation* freeOperation);

            // Set per-thread list of memory kinds
        void setKind(const vector<memkind_t>& kinds);

            // Set execution mode (different operations per each thread/same operations for each thread, but many iterations)
        void setExecutionMode(ExecutionMode operationMode);

            // Execute test
        int run();

            // Print test parameters
        virtual void showInfo();

            // Write test metrics
        void writeMetrics(const string& suiteName, const string& caseName, const string &fileName = "");

        Metrics getMetrics();
    private:

        // Run single iteration
        void runIteration();

            // Setup thread workers
        void prepareWorkers();

    };
}
