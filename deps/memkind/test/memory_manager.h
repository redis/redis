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
#include "common.h"
#include <memkind.h>

class MemoryManager
{
private:
    memkind_t kind;
    size_t memory_size;
    void* memory_pointer;

    void move(MemoryManager&& other)
    {
        kind = other.kind;
        memory_size = other.memory_size;
        if (memory_pointer)
            memkind_free(kind, memory_pointer);
        memory_pointer = std::move(other.memory_pointer);
        other.memory_pointer = nullptr;
    }

public:
    MemoryManager(memkind_t kind, size_t size) :
        kind(kind),
        memory_size(size),
        memory_pointer(memkind_malloc(kind, size))
    {}

    size_t size()
    {
        return memory_size;
    }

    MemoryManager(const MemoryManager&) = delete;

    MemoryManager(MemoryManager&& other)
    {
        move(std::move(other));
    }

    MemoryManager& operator=(MemoryManager&& other)
    {
        move(std::move(other));
        return *this;
    }

    ~MemoryManager()
    {
        if (memory_pointer)
            memkind_free(kind, memory_pointer);
    }
};

