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

#ifndef execute_trials_include_h
#define execute_trials_include_h

#include "hbwmalloc.h"
#include "memkind.h"

#include <vector>
#include <stdlib.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <fstream>
#include <iostream>

#include "common.h"


typedef enum {
    HBW_MALLOC,
    HBW_CALLOC,
    HBW_REALLOC,
    HBW_MEMALIGN,
    HBW_MEMALIGN_PSIZE,
    HBW_FREE,
    MEMKIND_MALLOC,
    MEMKIND_CALLOC,
    MEMKIND_REALLOC,
    MEMKIND_POSIX_MEMALIGN,
    MEMKIND_FREE
} alloc_api_t;

typedef struct {
    alloc_api_t api;
    size_t size;
    size_t alignment;
    size_t page_size;
    memkind_t memkind;
    int free_index;
} trial_t;

class TrialGenerator
{
public:
    TrialGenerator() {}
    void generate_incremental(alloc_api_t api);
    void generate_recycle_incremental(alloc_api_t api);
    void generate_recycle_psize_incremental(alloc_api_t api);
    void generate_recycle_psize_2GB(alloc_api_t api);
    void generate_size_1KB_2GB(alloc_api_t api);
    void generate_gb(alloc_api_t api, int number_of_gb_pages, memkind_t memkind, alloc_api_t api_free, bool psize_strict=false, size_t align = GB);
    void generate_size_4GB_8GB(alloc_api_t api);
    void run(int num_bandwidth, std::vector<int> &bandwidths);
    void generate_interleave(alloc_api_t api);
    void generate_size_2bytes_2KB_2MB(alloc_api_t api);
    /*For debugging purposes*/
    void print();
private:
    std::vector<trial_t> trial_vec;
    trial_t create_trial_tuple(alloc_api_t api,
                               size_t size,
                               size_t alignment,
                               int page_size,
                               memkind_t memkind,
                               int free_index);


};

class TGTest : public::testing::Test
{
protected:
    size_t num_bandwidth;
    std::vector<int> bandwidth;
    std::unique_ptr<TrialGenerator> tgen;
    void SetUp();
    void TearDown();
};

#endif
