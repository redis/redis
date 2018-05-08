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

/*
 * Purpose of this tests is to set up bind or preferred policy and then run over
 * hbw memory allocation functions as sanity check of changing a policy.
 *
*/

#include <hbwmalloc.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "common.h"

/* Set of test cases which intention is to used the Set Policy API, set it to a
 * desired policy and do memory allocations of 1MB and 1GB
 */
class PolicyTest : public :: testing :: Test
{
protected:
    void SetUp() {}
    void TearDown() {}
};

int execute_policy(hbw_policy_t set_policy, int size_in_megas)
{

    uint64_t NUMBER_OF_ELEMENTS = 1024*1024*1024;
    hbw_policy_t DESIRED_POLICY = set_policy;

    NUMBER_OF_ELEMENTS = size_in_megas;
    NUMBER_OF_ELEMENTS *= 1024*1024/8;

    // Check if memory is available
    if (hbw_check_available() != 0) {
        printf("No hbw memory available \n");
        return -1;
    }

    const char *HBW_Types[] = {
        "DUMMY",
        "HBW_POLICY_BIND",
        "HBW_POLICY_PREFERRED",
        "HBW_POLICY_INTERLEAVE"
    };

    //Verify that policy is set to bind
    if (hbw_get_policy() != DESIRED_POLICY) {

        printf("hbw policy is NOT %s, changing it...", HBW_Types[set_policy]);

        //set memory policy to desired one
        hbw_set_policy(DESIRED_POLICY);
        if (hbw_get_policy() == DESIRED_POLICY)
            printf(" done\n");
        else
            printf(" failed\n");
    }
    else {
        printf("hbw policy is already set to %s\n", HBW_Types[set_policy]);
    }

    // Make some memory allocations
    printf("Calling hbw_malloc with %ld bytes...",NUMBER_OF_ELEMENTS * sizeof(double));
    fflush(stdout);

    double *A = (double*)hbw_malloc(NUMBER_OF_ELEMENTS * sizeof(double));
    if (!A) {
        printf("Not able to do hbw_malloc\n");
        return -2;
    }
    printf("done\n");

    printf("Calling hbw_calloc with %ld doubles...", NUMBER_OF_ELEMENTS);
    fflush(stdout);
    double *sum = (double*)hbw_calloc(NUMBER_OF_ELEMENTS, sizeof(double));
    if (!sum) {
        printf("Not able to do hbw_calloc\n");
        hbw_free(A);
        return -2;
    }
    printf("done\n");

    // operate on the allocated memory
    printf("Assigning values to memory...");
    fflush(stdout);

    A[0]=0.0;
    uint64_t next_dot = NUMBER_OF_ELEMENTS/10;
    for (uint64_t i=1; i<NUMBER_OF_ELEMENTS; i++) {
        A[i] = (double)i;
        sum[i] += A[i-1]+sum[i-1];
        if (i==next_dot) {
            printf(".");
            fflush(stdout);
            next_dot+=NUMBER_OF_ELEMENTS/10;
        }
    }
    printf("done\n");

    // Do a realloc to store everythin on a single buffer
    printf("Calling hbw_realloc from %ld to %ld bytes...",
           NUMBER_OF_ELEMENTS * sizeof(double), 2*NUMBER_OF_ELEMENTS * sizeof(double));
    fflush(stdout);

    double *newA = (double*)hbw_realloc(A, 2*NUMBER_OF_ELEMENTS * sizeof(double));
    if (!newA) {
        printf("Not able to do hbw_realloc\n");
        hbw_free(sum);
        hbw_free(A);
        return -2;
    }
    printf("done\n");

    // Copy data to newly reallocated memory
    printf("Copying calloc memory to newly reallocated memory...");
    fflush(stdout);
    A = newA;
    next_dot = NUMBER_OF_ELEMENTS/10;
    for (uint64_t i=0; i<NUMBER_OF_ELEMENTS; i++) {
        newA[i+NUMBER_OF_ELEMENTS] = sum[i];
        if (i==next_dot) {
            printf(".");
            fflush(stdout);
            next_dot+=NUMBER_OF_ELEMENTS/10;
        }
    }
    printf("done\n");

    // Release sum buffer
    printf("Calling hbw_free on calloc memory...");
    fflush(stdout);
    hbw_free(sum);
    printf("done\n");

    //Check that memory content is what it should
    printf("Verifying that memory values are correct...");
    fflush(stdout);

    double expected = 0.0;
    next_dot = NUMBER_OF_ELEMENTS/10;
    for (uint64_t i = 0; i<NUMBER_OF_ELEMENTS; i++) {
        double current = (double)i;
        if (A[i]!=current) {
            printf("Value written and reallocated differ from expected value on position A[%ld] (%lf, %lf)\n",
                   i, A[i], current);
        }
        if (A[i+NUMBER_OF_ELEMENTS]!=expected) {
            printf("Value copied to reallocated region is not what is expected on position A[%ld] (%lf, %lf)\n",
                   i, A[i+NUMBER_OF_ELEMENTS], expected);
        }
        expected+=current;
        if (i==next_dot) {
            printf(".");
            fflush(stdout);
            next_dot+=NUMBER_OF_ELEMENTS/10;
        }
    }
    printf("done\n");

    //release rest of memory
    printf("Calling hbw_free on realloc memory...");
    fflush(stdout);
    hbw_free(A);
    printf("done\n");

    printf("Program finished correctly\n");
    return 0;
}

TEST_F(PolicyTest, test_TC_MEMKIND_PolicyBind_1MB)
{
    EXPECT_EQ(0, execute_policy(HBW_POLICY_BIND, 1));
}

TEST_F(PolicyTest, test_TC_MEMKIND_PolicyPreferred_1MB)
{
    EXPECT_EQ(0, execute_policy(HBW_POLICY_PREFERRED, 1));
}

// Extended TC, because it may allocate 2GB pages when reallocating pages using realloc()
TEST_F(PolicyTest, test_TC_MEMKIND_ext_PolicyBind_1GB)
{
    EXPECT_EQ(0, execute_policy(HBW_POLICY_BIND, 1024));
}

TEST_F(PolicyTest, test_TC_MEMKIND_PolicyPreferred_1GB)
{
    EXPECT_EQ(0, execute_policy(HBW_POLICY_PREFERRED, 1024));
}

TEST_F(PolicyTest, test_TC_MEMKIND_PolicyInterleave_1MB)
{
    EXPECT_EQ(0, execute_policy(HBW_POLICY_INTERLEAVE, 1));
}

TEST_F(PolicyTest, test_TC_MEMKIND_PolicyInterleave_1GB)
{
    EXPECT_EQ(0, execute_policy(HBW_POLICY_INTERLEAVE, 1024));
}
