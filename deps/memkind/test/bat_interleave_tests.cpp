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

#include <memkind/internal/memkind_hbw.h>

#include <fstream>
#include <numa.h>

#include "common.h"
#include "check.h"
#include "trial_generator.h"
#include <numa.h>


/* Set of basic acceptance tests for INTERLEAVE policy, the goal of this set of tests
 * is to prove that you can do incremental allocations of memory with different
 * sizes and that pages are actually allocated alternately in HBW and DRAM nodes.
 */
class BATInterleaveTests: public TGTest
{};


TEST_F(BATInterleaveTests, test_TC_MEMKIND_HBW_Interleave_CheckAvailable)
{
    hbw_set_policy(HBW_POLICY_INTERLEAVE);
    ASSERT_EQ(0, hbw_check_available());
}

TEST_F(BATInterleaveTests, test_TC_MEMKIND_HBW_Interleave_Policy)
{
    hbw_set_policy(HBW_POLICY_INTERLEAVE);
    EXPECT_EQ(HBW_POLICY_INTERLEAVE, hbw_get_policy());
}

TEST_F(BATInterleaveTests, test_TC_MEMKIND_HBW_Interleave_MallocIncremental)
{
    hbw_set_policy(HBW_POLICY_INTERLEAVE);
    tgen->generate_interleave(HBW_MALLOC);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(BATInterleaveTests, test_TC_MEMKIND_HBW_Interleave_CallocIncremental)
{
    hbw_set_policy(HBW_POLICY_INTERLEAVE);
    tgen->generate_interleave(HBW_CALLOC);
    tgen->run(num_bandwidth, bandwidth);
}


TEST_F(BATInterleaveTests, test_TC_MEMKIND_HBW_Interleave_ReallocIncremental)
{
    hbw_set_policy(HBW_POLICY_INTERLEAVE);
    tgen->generate_interleave(HBW_REALLOC);
    tgen->run(num_bandwidth, bandwidth);
}
