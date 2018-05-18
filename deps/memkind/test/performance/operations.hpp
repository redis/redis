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

#include <malloc.h>
#include <cassert>
#include "jemalloc/jemalloc.h"

// Malloc, jemalloc, memkind jemalloc and memkind memory operations definitions
namespace performance_tests
{
    using std::vector;
    using std::string;
    using std::thread;

#ifdef __DEBUG
    #include <mutex>
    // Write entire text at once, avoiding switching to another thread
    extern int g_msgLevel;
    extern std::mutex g_coutMutex;
    #define EMIT(LEVEL, TEXT) \
    if (g_msgLevel >= LEVEL) \
    { \
        g_coutMutex.lock(); std::cout << TEXT << std::endl; g_coutMutex.unlock(); \
    }
#else
#define EMIT(LEVEL, TEXT)
#endif

    // Use "vanilia" jemalloc, compiled with unique prefix (--with-jemalloc-prefix= configure option)
    #ifdef SYSTEM_JEMALLOC_PREFIX
    #define TOKENPASTE(x, y) x ## y
    #define JE(x, y) TOKENPASTE(x, y)
    #define jexx_malloc JE(SYSTEM_JEMALLOC_PREFIX, malloc)
    #define jexx_calloc JE(SYSTEM_JEMALLOC_PREFIX, calloc)
    #define jexx_memalign JE(SYSTEM_JEMALLOC_PREFIX, memalign)
    #define jexx_realloc JE(SYSTEM_JEMALLOC_PREFIX, realloc)
    #define jexx_free JE(SYSTEM_JEMALLOC_PREFIX, free)
    extern "C" {
        // "vanilia" jemalloc 4.3.1 function prototypes
        // full header cannot be include due to conflict with memkind jemalloc
        extern void * jexx_malloc(size_t size);
        extern void * jexx_calloc(size_t num, size_t size);
        extern void * jexx_memalign(size_t alignment, size_t size);
        extern void * jexx_realloc(void *ptr, size_t size);
        extern void  jexx_free(void *ptr);
    }
    #endif

    // Available memory operations
    enum OperationName
    {
        Malloc,
        Calloc,
        Realloc,
        Align,
        Free,
        Invalid
    };

    // Reprents a memory operation
    class Operation
    {
    public:
        // Each operation is assigned a bucket size from range (0, MaxBucketSize)
        static const unsigned MaxBucketSize = 100;
        // For memalign operation, alignment parameter will be a random value
        // from range (sizeof(void*), sizeof(void*) * MemalignMaxMultiplier)
        static const unsigned MemalignMaxMultiplier = 4;

    protected:
        OperationName m_name;
        // If random number from range (0, MaxBucketSize) is lower than m_bucketSize, an operation will be performed
        unsigned m_bucketSize;

    public:
        Operation()
        {
        }

            // Creates an operation with given name and bucket size
            // If no bucket size is given, operation will be always performed
        Operation(
            OperationName name,
            unsigned bucketSize = MaxBucketSize)
            : m_name(name)
            , m_bucketSize(bucketSize)
        {
            assert(bucketSize <= MaxBucketSize);
        }

        virtual ~Operation() {}
        ;

            // Check if operation should be performed (currently drawn random number lower than bucket size)
        bool checkCondition(unsigned ballSize) const
        {
            return (ballSize < m_bucketSize);
        }

        OperationName getName() const
        {
            return m_name;
        }

        string getNameStr() const
        {
            switch (m_name)
            {
            case OperationName::Malloc:
                return "malloc";

            case OperationName::Calloc:
                return "calloc";

            case OperationName::Realloc:
                return "realloc";

            case OperationName::Align:
                return "align";

            case OperationName::Free:
                return "free";

            default:
                return "<unknown>";
            }
        }

            // Get operation bucket size
        unsigned getBucketSize() const
        {
            return m_bucketSize;
        }

            // perform memory operation
        virtual void perform(const memkind_t& kind,
            void * &mem,
            size_t size = 0,
            size_t offset=0,
            size_t alignment=0)
            const = 0;

    };

    // Malloc memory operations
    class MallocOperation : public Operation
    {
    public:
        MallocOperation(
            OperationName name)
            : Operation(name)
        {
        }

        MallocOperation(
            OperationName name,
            unsigned bucketSize)
            : Operation(name, bucketSize)
        {
        }

        virtual void perform(const memkind_t& kind,
            void * &mem,
            size_t size,
            size_t offset,
            size_t alignment) const override
        {
            EMIT(2, "Entering Operation::" << getNameStr()
                <<  ", size=" << size
                << ", offset=" << offset
                << ", alignment=" << alignment
                << ", mem=" << mem)
            switch(m_name)
            {
            case Malloc:
                {
                    if (mem != nullptr)
                    {
                        free(mem);
                    }
                    mem = malloc(size);
                    break;
                }
            case Calloc:
                {
                    if (mem != nullptr)
                    {
                        free(mem);
                    }
                    // split allocation size randomly
                    // between number of elements and element size
                    mem = calloc((1 << offset), (size >> offset));
                    break;
                }
            case Realloc:
                {
                    mem = realloc(mem, size);
                    break;
                }
            case Align:
                {
                    if (mem != nullptr)
                    {
                        free(mem);
                    }
                    // randomly choose alignment from (8, 8 * MemalignMaxMultiplie)
                    mem = memalign(alignment, size);
                    break;
                }
            case Free:
                {
                    free(mem);
                    mem = nullptr;
                    break;
                }

            default:
                throw "Not implemented";
                break;
            }
            EMIT(2, "Exiting Operation::" << getNameStr()
                <<  ", size=" << size
                << ", offset=" << offset
                << ", alignment=" << alignment
                << ", mem=" << mem)
        }
    };

    #ifdef SYSTEM_JEMALLOC_PREFIX
    // Jemalloc memory operations
    class JemallocOperation : public Operation
    {
    public:
        JemallocOperation(
            OperationName name)
            : Operation(name)
        {
        }

        JemallocOperation(
            OperationName name,
            unsigned bucketSize)
            : Operation(name, bucketSize)
        {
        }

        virtual void perform(const memkind_t& kind,
            void * &mem,
            size_t size,
            size_t offset,
            size_t alignment) const override
        {
            EMIT(2, "Entering Operation::" << getNameStr()
                <<  ", size=" << size
                << ", offset=" << offset
                << ", alignment=" << alignment
                << ", mem=" << mem)
            switch(m_name)
            {
            case Malloc:
                {
                    if (mem != nullptr)
                    {
                        jexx_free(mem);
                    }
                    mem = jexx_malloc(size);
                    break;
                }
            case Calloc:
                {
                    if (mem != nullptr)
                    {
                        jexx_free(mem);
                    }
                    // split allocation size randomly
                    // between number of elements and element size
                    mem = jexx_calloc((1 << , offset), (size >> , offset));
                    break;
                }
            case Realloc:
                {
                    mem = jexx_realloc(mem, size);
                    break;
                }
            case Align:
                {
                    if (mem != nullptr)
                    {
                        jexx_free(mem);
                    }
                    // randomly choose alignment from (8, 8 * MemalignMaxMultiplie)
                    mem = jexx_memalign(alignment, size);
                    break;
                }
            case Free:
                {
                    jexx_free(mem);
                    mem = nullptr;
                    break;
                }

            default:
                throw "Not implemented";
                break;
            }
            EMIT(2, "Exiting Operation::" << getNameStr()
                <<  ", size=" << size
                << ", offset=" << offset
                << ", alignment=" << alignment
                << ", mem=" << mem)
        }
    };
    #endif
    // Jemkmalloc memory operations
    class JemkmallocOperation : public Operation
    {
    public:
        JemkmallocOperation(
            OperationName name)
            : Operation(name)
        {
        }

        JemkmallocOperation(
            OperationName name,
            unsigned bucketSize)
            : Operation(name, bucketSize)
        {
        }

        virtual void perform(const memkind_t& kind,
            void * &mem,
            size_t size,
            size_t offset,
            size_t alignment) const override
        {
        #ifdef JEMK
            EMIT(2, "Entering Operation::" << getNameStr()
                <<  ", size=" << size
                << ", offset=" << offset
                << ", alignment=" << alignment
                << ", mem=" << mem)
            switch(m_name)
            {
            case Malloc:
                {
                    if (mem != nullptr)
                    {
                        jemk_free(mem);
                    }
                    mem = jemk_malloc(size);
                    break;
                }
            case Calloc:
                {
                    if (mem != nullptr)
                    {
                        jemk_free(mem);
                    }
                    // split allocation size randomly
                    // between number of elements and element size
                    mem = jemk_calloc((1 << offset), (size >> offset));
                    break;
                }
            case Realloc:
                {
                    mem = jemk_realloc(mem, size);
                    break;
                }
            case Align:
                {
                    if (mem != nullptr)
                    {
                        jemk_free(mem);
                    }
                    // randomly choose alignment from (8, 8 * MemalignMaxMultiplie)
                    mem = jemk_memalign(alignment, size);
                    break;
                }
            case Free:
                {
                    jemk_free(mem);
                    mem = nullptr;
                    break;
                }

            default:
                throw "Not implemented";
                break;
            }
            EMIT(2, "Exiting Operation::" << getNameStr()
                <<  ", size=" << size
                << ", offset=" << offset
                << ", alignment=" << alignment
                << ", mem=" << mem)
            #endif // JE_MK
        }
    };

    // Memkind memory operations
    class MemkindOperation : public Operation
    {
    public:
        MemkindOperation()
        {}

        MemkindOperation(
            OperationName name)
            : Operation(name)
        {
        }

        MemkindOperation(
            OperationName name,
            size_t bucketSize)
            : Operation(name, bucketSize)
        {
        }

        virtual void perform(const memkind_t& kind,
            void * &mem,
            size_t size,
            size_t offset,
            size_t alignment) const override
        {
            EMIT(2, "Entering Operation::" << getNameStr()
                <<  ", size=" << size
                << ", offset=" << offset
                << ", alignment=" << alignment
                << ", mem=" << mem)
            switch (m_name)
            {
            case Malloc:
                {
                    if (mem != nullptr)
                    {
                        memkind_free(kind, mem);
                    }
                    mem = memkind_malloc(kind, size);
                    break;
                }
            case Calloc:
                {
                    if (mem != nullptr)
                    {
                        memkind_free(kind, mem);
                    }
                    // split allocation size randomly
                    // between number of elements and element size
                    mem = memkind_calloc(kind, 1 << offset, size >> offset);
                    break;
                }
            case Realloc:
                {
                    mem = memkind_realloc(kind, mem, size);
                    break;
                }

            case Align:
                {
                    if (mem != nullptr)
                    {
                        memkind_free(kind, mem);
                        mem = nullptr;
                    }
                    // randomly choose alignment from (sizeof(void*), sizeof(void*) * MemalignMaxMultiplie)
                    if (memkind_posix_memalign(kind, &mem, alignment, size) != 0)
                    {
                        // failure
                        mem = nullptr;
                    }
                    break;
                }
            case Free:
                {
                    memkind_free(kind, mem);
                    mem = nullptr;
                    break;
                }

            default:
                break;
            }
            EMIT(2, "Exiting Operation::" << getNameStr()
                <<  ", size=" << size
                << ", offset=" << offset
                << ", alignment=" << alignment
                << ", mem=" << mem)
        }
    };
}
