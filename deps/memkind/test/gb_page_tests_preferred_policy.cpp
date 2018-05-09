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

#include "memkind.h"

#include "trial_generator.h"

/* This set of tests are intended to use GB pages allocation, it is needed to have
 * GB pages enabled or ignore its execution in case GB can't be enabled on an
 * specifc machine. It uses the trial generator to run the tests and validate
 * its output.
 */
class GBPagesTestPreferredPolicy : public TGTest
{};

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_HBW_Misalign_Preferred_Strict)
{
    tgen->generate_gb (HBW_MEMALIGN_PSIZE, 1, MEMKIND_HBW_PREFERRED_GBTLB, HBW_FREE, true, 2147483648);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_HBW_Memalign_Psize_Preferred_Strict)
{
    tgen->generate_gb (HBW_MEMALIGN_PSIZE, 1, MEMKIND_HBW_PREFERRED_GBTLB, HBW_FREE, true);
    tgen->run(num_bandwidth, bandwidth);
}


TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_HBW_Malloc)
{
    tgen->generate_gb (MEMKIND_MALLOC, 1, MEMKIND_HBW_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}


TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_HBW_Calloc)
{
    tgen->generate_gb (MEMKIND_CALLOC, 1, MEMKIND_HBW_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}


TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_HBW_Realloc)
{
    tgen->generate_gb (MEMKIND_REALLOC, 1, MEMKIND_HBW_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_HBW_Posix_Memalign)
{
    tgen->generate_gb (MEMKIND_POSIX_MEMALIGN, 1, MEMKIND_HBW_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_Malloc_Regular)
{
    tgen->generate_gb (MEMKIND_MALLOC, 1, MEMKIND_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_Calloc_Regular)
{
    tgen->generate_gb (MEMKIND_CALLOC, 1, MEMKIND_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_Realloc_Regular)
{
    tgen->generate_gb (MEMKIND_REALLOC, 1, MEMKIND_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_Posix_Memalign_Regular)
{
    tgen->generate_gb (MEMKIND_POSIX_MEMALIGN, 1, MEMKIND_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

/*
 * Below tests allocate GB pages incrementally.
*/

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_ext_HBW_Memalign_Psize)
{
    tgen->generate_gb (HBW_MEMALIGN_PSIZE, 2, MEMKIND_HBW_PREFERRED_GBTLB, HBW_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_ext_HBW_Malloc)
{
    tgen->generate_gb (MEMKIND_MALLOC, 2, MEMKIND_HBW_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_ext_HBW_Calloc)
{
    tgen->generate_gb (MEMKIND_CALLOC, 2, MEMKIND_HBW_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_ext_HBW_Realloc)
{
    tgen->generate_gb (MEMKIND_REALLOC, 2, MEMKIND_HBW_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_ext_HBW_Posix_Memalign)
{
    tgen->generate_gb (MEMKIND_POSIX_MEMALIGN, 2, MEMKIND_HBW_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_ext_HBW_Memalign_Psize_Strict)
{
    tgen->generate_gb (HBW_MEMALIGN_PSIZE, 3, MEMKIND_HBW_PREFERRED_GBTLB, HBW_FREE, true);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_ext_Malloc_Regular)
{
    tgen->generate_gb (MEMKIND_MALLOC, 2, MEMKIND_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_ext_Calloc_Regular)
{
    tgen->generate_gb (MEMKIND_CALLOC, 2, MEMKIND_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_ext_Realloc_Regular)
{
    tgen->generate_gb (MEMKIND_REALLOC, 2, MEMKIND_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}

TEST_F(GBPagesTestPreferredPolicy, test_TC_MEMKIND_GBPages_ext_Posix_Memalign_Regular)
{
    tgen->generate_gb (MEMKIND_POSIX_MEMALIGN, 2, MEMKIND_GBTLB, MEMKIND_FREE);
    tgen->run(num_bandwidth, bandwidth);
}
