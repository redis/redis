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
#include "memkind_allocated.hpp"

// This code is example usage of C++11 features with custom allocator,
// support for C++11 is required.
#include <iostream>

#if __cplusplus > 199711L

#include <cstdlib>
#include <new>
#include <string>

//example class definition, which derive from  memkind_allocated template to
//have objects allocated with memkind, and have alignment specified by alignas()
class alignas(128) memkind_allocated_example : public memkind_allocated<memkind_allocated_example>
{
    std::string message;

public:
    //Override method for returning class default kind to make objects be by default allocated on High-Bandwith Memory
    static memkind_t getClassKind()
    {
        return MEMKIND_HBW;
    }

    memkind_allocated_example(std::string my_message)
    {
        this->message = my_message;
    }

    memkind_allocated_example()
    {
    }

    void print_message()
    {
        std::cout << message << std::endl;
        std::cout << "Memory adress of this object is: " <<  (void*)this << std::endl << std::endl;
    }
};

int main()
{
    memkind_t specified_kind = MEMKIND_HBW_HUGETLB;

    memkind_allocated_example* default_kind_example = new memkind_allocated_example( std::string("This object has been allocated using class default kind, which is: MEMKIND_DEFAULT") );
    default_kind_example->print_message();
    delete default_kind_example;

    memkind_allocated_example* specified_kind_example = new(specified_kind) memkind_allocated_example( std::string("This object has been allocated using specified kind, which is: MEMKIND_HBW_HUGETLB") );
    specified_kind_example->print_message();
    delete specified_kind_example;

    //examples for using same aproach for allocating arrays of objects, note that objects created that way can be initialized only with default (unparameterized) constructor
    memkind_allocated_example* default_kind_array_example = new memkind_allocated_example[5]();
    delete[] default_kind_array_example;

    memkind_allocated_example* specified_kind_array_example = new(specified_kind) memkind_allocated_example[5]();
    delete[] specified_kind_array_example;

    return 0;
}
#else //If C++11 is not avaiable - do nothing.
int main()
{
    std::cout << "WARNING: because your compiler does not support C++11 standard," << std::endl;
    std::cout << "this example is only as a dummy placeholder." << std::endl;
    return 0;
}
#endif
