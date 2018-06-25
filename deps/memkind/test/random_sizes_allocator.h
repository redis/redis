/*
 * Copyright (C) 2017 Intel Corporation.
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
#include "memory_manager.h"
#include <vector>
#include <random>

class RandomSizesAllocator
{
private:
    std::vector<MemoryManager> allocated_memory;
    memkind_t kind;
    std::default_random_engine generator;
    std::uniform_int_distribution<int> memory_distribution;

    size_t get_random_size()
    {
        return memory_distribution(generator);
    }

public:
    RandomSizesAllocator(memkind_t kind, size_t min_size, size_t max_size, int max_allocations_number) :
        kind(kind),
        memory_distribution(min_size, max_size)
    {
        allocated_memory.reserve(max_allocations_number);
    }

    size_t malloc_random_memory()
    {
        size_t size = get_random_size();
        allocated_memory.emplace_back(kind, size);
        return size;
    }

    size_t free_random_memory()
    {
        if (empty())
            return 0;
        std::uniform_int_distribution<int> distribution(0, allocated_memory.size() - 1);
        int random_index = distribution(generator);
        auto it = std::begin(allocated_memory) + random_index;
        size_t size = it->size();
        allocated_memory.erase(it);
        return size;
    }

    bool empty()
    {
        return allocated_memory.empty();
    }
};

