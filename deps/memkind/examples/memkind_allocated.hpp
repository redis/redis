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

///////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <memkind.h>

#include <cstdlib>
#include <new>

template <class deriving_class>
class memkind_allocated
{
public:
    static memkind_t getClassKind()
    {
        return MEMKIND_DEFAULT;
    }

    void* operator new(std::size_t size)
    {
        return deriving_class::operator new(size, deriving_class::getClassKind());
    }

    void* operator new[](std::size_t size)
    {
        return deriving_class::operator new(size, deriving_class::getClassKind());
    }

    void* operator new(std::size_t size, memkind_t memory_kind)
    {
        void* result_ptr = NULL;
        int allocation_result = 0;

        //This check if deriving_class has specified alignment, which is suitable
        //to be used with posix_memalign()
        if(alignof(deriving_class) <  sizeof(void *)) {
            result_ptr = memkind_malloc(memory_kind, size);
            allocation_result = result_ptr ? 1 : 0;
        }
        else {
            allocation_result = memkind_posix_memalign(memory_kind, &result_ptr, alignof(deriving_class), size);
        }

        if(allocation_result) {
            throw std::bad_alloc();
        }

        return result_ptr;
    }

    void* operator new[](std::size_t size, memkind_t memory_kind)
    {
        return deriving_class::operator new(size, memory_kind);
    }

    void operator delete(void* ptr, memkind_t memory_kind)
    {
        memkind_free(memory_kind, ptr);
    }

    void operator delete(void* ptr)
    {
        memkind_free(0, ptr);
    }

    void operator delete[](void* ptr)
    {
        deriving_class::operator delete(ptr);
    }

    void operator delete[](void* ptr, memkind_t memory_kind)
    {
        deriving_class::operator delete(ptr, memory_kind);
    }

protected:
    memkind_allocated()
    {
    }

    ~memkind_allocated()
    {
    }

};
