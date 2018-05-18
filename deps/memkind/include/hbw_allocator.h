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

#include <hbwmalloc.h>

#include <stddef.h>
#include <new>
/*
 * Header file for the C++ allocator compatible with the C++ standard library allocator concepts.
 * More details in hbwallocator(3) man page.
 * Note: memory heap management is based on hbwmalloc, refer to the hbwmalloc man page for more information.
 *
 * Functionality defined in this header is considered as EXPERIMENTAL API.
 * API standards are described in memkind(3) man page.
 */
namespace hbw
{

template <class T>
class allocator
{
public:
    /*
     *  Public member types required and defined by the standard library allocator concepts.
     */
    typedef size_t size_type;
    typedef ptrdiff_t difference_type;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T& reference;
    typedef const T& const_reference;
    typedef T value_type;

    template <class U>
    struct rebind {
        typedef hbw::allocator<U> other;
    };

    /*
     *  Public member functions required and defined by the standard library allocator concepts.
     */
    allocator() throw() { }

    template <class U>
    allocator(const allocator<U>&) throw() { }

    ~allocator() throw() { }

    pointer address(reference x) const
    {
        return &x;
    }

    const_pointer address(const_reference x) const
    {
        return &x;
    }

    /*
     *  Allocates n*sizeof(T) bytes of high bandwidth memory using hbw_malloc().
     *  Throws std::bad_alloc when cannot allocate memory.
     */
    pointer allocate(size_type n, const void * = 0)
    {
        if (n > this->max_size()) {
            throw std::bad_alloc();
        }
        pointer result = static_cast<pointer>(hbw_malloc(n * sizeof(T)));
        if (!result) {
            throw std::bad_alloc();
        }
        return result;
    }

    /*
     *  Deallocates memory associated with pointer returned by allocate() using hbw_free().
     */
    void deallocate(pointer p, size_type n)
    {
        hbw_free(static_cast<void*>(p));
    }

    size_type max_size() const throw()
    {
        return size_t(-1) / sizeof(T);
    }

    void construct(pointer p, const_reference val)
    {
        ::new(p) value_type(val);
    }

    void destroy(pointer p)
    {
        p->~T();
    }
};

template <class T, class U>
bool operator==(const allocator<T>&, const allocator<U>&)
{
    return true;
}

template <class T, class U>
bool operator!=(const allocator<T>&, const allocator<U>&)
{
    return false;
}

}
