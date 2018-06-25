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
#include <dlfcn.h>

#include "common.h"

class DlopenTest: public :: testing::Test
{
protected:
    DlopenTest()
    {
        const char *path = "/usr/lib64/libmemkind.so";
        if (!pathExists(path))
        {
            path = "/usr/lib/libmemkind.so";
        }
        dlerror();
        handle = dlopen(path, RTLD_LAZY);
        assert((handle != NULL && dlerror() == NULL) && "Couldn't open libmemkind.so");
        memkind_malloc = (memkind_malloc_t)dlsym(handle, "memkind_malloc");
        assert(dlerror() == NULL && "Couldn't get memkind_malloc from memkind library");
        memkind_free = (memkind_free_t)dlsym(handle, "memkind_free");
        assert(dlerror() == NULL && "Couldn't get memkind_free from memkind library");
    }

    ~DlopenTest()
    {
        dlclose(handle);
    }

    void test(const char* kind_name, size_t alloc_size)
    {
        void** kind_ptr = (void**)dlsym(handle, kind_name);
        EXPECT_TRUE(dlerror() == NULL) << "Couldn't get kind from memkind library";
        EXPECT_TRUE(kind_ptr != NULL) << "Kind ptr to memkind library is NULL";

        void* allocation_ptr = memkind_malloc((*kind_ptr), alloc_size);
        EXPECT_TRUE(allocation_ptr != NULL) << "Allocation with memkind_malloc failed";

        memset(allocation_ptr, 0, alloc_size);

        memkind_free((*kind_ptr), allocation_ptr);
    }

    bool pathExists(const char *p)
    {
        struct stat info;
        if (0 != stat(p, &info))
        {
            return false;
        }
        return true;
    }

private:
    void* handle;
    typedef void* (*memkind_malloc_t)(void*, size_t);
    typedef void (*memkind_free_t)(void*, void*);
    memkind_malloc_t memkind_malloc;
    memkind_free_t memkind_free;
};

TEST_F(DlopenTest, test_TC_MEMKIND_DEFAULT_4194305_bytes)
{
    test("MEMKIND_DEFAULT", 4194305);
}

TEST_F(DlopenTest, test_TC_MEMKIND_HBW_4194305_bytes)
{
    test("MEMKIND_HBW", 4194305);
}

TEST_F(DlopenTest, test_TC_MEMKIND_HBW_HUGETLB_4194305_bytes)
{
    test("MEMKIND_HBW_HUGETLB", 4194305);
}

TEST_F(DlopenTest, test_TC_MEMKIND_HBW_PREFERRED_4194305_bytes)
{
    test("MEMKIND_HBW_PREFERRED", 4194305);
}

TEST_F(DlopenTest, test_TC_MEMKIND_HBW_INTERLEAVE_4194305_bytes)
{
    test("MEMKIND_HBW_INTERLEAVE", 4194305);
}
