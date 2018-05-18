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

#include "perf_tests.hpp"
#include <iostream>
#include <cmath>
#include <gtest/gtest.h>

// Memkind performance tests
using std::cout;
using std::endl;
using std::abs;
using std::ostringstream;

// Memkind tests
class PerformanceTest : public testing::Test
{
protected:
    const double Tolerance = 0.15;
    const double Confidence = 0.10;

    Metrics referenceMetrics;
    Metrics performanceMetrics;
    PerfTestCase<performance_tests::MallocOperation> referenceTest;
    PerfTestCase<performance_tests::MemkindOperation> performanceTest;

    template<class T>
    static void RecordProperty(const string& key, const T value)
    {
        ostringstream values;
        values << value;
        testing::Test::RecordProperty(key, values.str());
    }

    void SetUp()
    {
    }

    void TearDown()
    {
    }

    bool checkDelta(double value, double reference, const string &info, double delta, bool higherIsBetter = false)
    {
        // If higherIsBetter is true, current value should be >= (reference - delta); otherwise, it should be <= (reference + delta)
        double treshold = higherIsBetter ? reference * (1 - delta) : reference * (1 + delta);
        cout <<
            "Metric: " << info << ". Reference value: " << reference << ". "
            "Expected: " << (higherIsBetter ? ">= " : "<= ") << treshold << " (delta = " << delta << ")."
            "Actual: " << value << " (delta = " <<
                ((value > reference ? (value / reference) : (reference / value)) - 1.0) /
                ((higherIsBetter != (value > reference)) ? 1.0 : -1.0)
            << ")."
            << endl ;
        if (higherIsBetter ? (value <= treshold) : (value >= treshold))
        {
            cout << "WARNING : Value of '" << info << "' outside expected bounds!" << endl;
            return false;
        }
        return true;
    }

    bool compareMetrics(Metrics metrics, Metrics reference, double delta)
    {
        bool result = true;
        result &= checkDelta(metrics.operationsPerSecond, reference.operationsPerSecond, "operationsPerSecond", delta, true);
        result &= checkDelta(metrics.avgOperationDuration, reference.avgOperationDuration, "avgOperationDuration", delta);
        return result;
    }

    void writeMetrics(Metrics metrics, Metrics reference)
    {
        RecordProperty("ops_per_sec", metrics.operationsPerSecond);
        RecordProperty("ops_per_sec_vs_ref",
            (reference.operationsPerSecond - metrics.operationsPerSecond) * 100.0f
            / reference.operationsPerSecond);
        RecordProperty("avg_op_time_nsec", metrics.avgOperationDuration);
        RecordProperty("avg_op_time_nsec_vs_ref",
            (metrics.avgOperationDuration - reference.avgOperationDuration) * 100.0f
            / reference.avgOperationDuration);
    }

    void run()
    {
        cout << "Running reference std::malloc test" << endl;
        referenceMetrics = referenceTest.runTest();

        cout << "Running memkind test" << endl;
        performanceMetrics = performanceTest.runTest();

        writeMetrics(performanceMetrics, referenceMetrics);
        EXPECT_TRUE(compareMetrics(performanceMetrics, referenceMetrics, Tolerance + Confidence));
    }
};

TEST_F(PerformanceTest, test_TC_MEMKIND_perf_single_op_single_iter)
{
    referenceTest.setupTest_singleOpSingleIter();
    performanceTest.setupTest_singleOpSingleIter();
    run();
}

TEST_F(PerformanceTest, test_TC_MEMKIND_perf_many_ops_single_iter)
{
    referenceTest.setupTest_manyOpsSingleIter();
    performanceTest.setupTest_manyOpsSingleIter();
    run();
}

TEST_F(PerformanceTest, test_TC_MEMKIND_perf_many_ops_single_iter_huge_alloc)
{
    referenceTest.setupTest_manyOpsSingleIterHugeAlloc();
    performanceTest.setupTest_manyOpsSingleIterHugeAlloc();
    run();
}

TEST_F(PerformanceTest, test_TC_MEMKIND_perf_single_op_many_iters)
{
    referenceTest.setupTest_singleOpManyIters();
    performanceTest.setupTest_singleOpManyIters();
    run();
}

TEST_F(PerformanceTest, test_TC_MEMKIND_perf_many_ops_many_iters)
{
    referenceTest.setupTest_manyOpsManyIters();
    performanceTest.setupTest_manyOpsManyIters();
    run();
}

TEST_F(PerformanceTest, test_TC_MEMKIND_perf_many_ops_many_iters_many_kinds)
{
    referenceTest.setupTest_manyOpsManyIters();
    performanceTest.setupTest_manyOpsManyIters();
    run();
}
