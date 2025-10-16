/*
 * List of hashes for the brute force collision tester
 * Part of xxHash project
 * Copyright (C) 2019-2021 Yann Collet
 *
 * GPL v2 License
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * You can contact the author at:
 * - xxHash homepage: https://www.xxhash.com
 * - xxHash source repository: https://github.com/Cyan4973/xxHash
 */

#ifndef HASHES_H_1235465
#define HASHES_H_1235465

#include <stddef.h>      /* size_t */
#include <stdint.h>      /* uint64_t */
#define XXH_INLINE_ALL   /* XXH128_hash_t */
#include "xxhash.h"


/* return type */

typedef union {
    uint64_t       h64;
    XXH128_hash_t h128;
} UniHash;

UniHash uniHash32(uint64_t v32)
{   UniHash unih;
    unih.h64 = v32;
    return unih;
}

UniHash uniHash64(uint64_t v64)
{   UniHash unih;
    unih.h64 = v64;
    return unih;
}

UniHash uniHash128(XXH128_hash_t v128)
{   UniHash unih;
    unih.h128 = v128;
    return unih;
}


/* ===  xxHash  === */

UniHash XXH3_wrapper (const void* data, size_t size)
{
    return uniHash64( XXH3_64bits(data, size) );
}

UniHash XXH128_wrapper (const void* data, size_t size)
{
    return uniHash128( XXH3_128bits(data, size) );
}

UniHash XXH128l_wrapper (const void* data, size_t size)
{
    return uniHash64( XXH3_128bits(data, size).low64 );
}

UniHash XXH128h_wrapper (const void* data, size_t size)
{
    return uniHash64( XXH3_128bits(data, size).high64 );
}

UniHash XXH64_wrapper (const void* data, size_t size)
{
    return uniHash64 ( XXH64(data, size, 0) );
}

UniHash XXH32_wrapper (const void* data, size_t size)
{
    return uniHash32( XXH32(data, size, 0) );
}

/* ===  Dummy integration example  === */

#include "dummy.h"

UniHash badsum32_wrapper (const void* data, size_t size)
{
    return uniHash32( badsum32(data, size, 0) );
}



/* ===  Table  === */

typedef UniHash (*hashfn) (const void* data, size_t size);

typedef struct {
    const char* name;
    hashfn fn;
    int bits;
} hashDescription;

#define HASH_FN_TOTAL 7

hashDescription hashfnTable[HASH_FN_TOTAL] = {
    { "xxh3"  ,  XXH3_wrapper,     64 },
    { "xxh64" ,  XXH64_wrapper,    64 },
    { "xxh128",  XXH128_wrapper,  128 },
    { "xxh128l", XXH128l_wrapper,  64 },
    { "xxh128h", XXH128h_wrapper,  64 },
    { "xxh32" ,  XXH32_wrapper,    32 },
    { "badsum32",badsum32_wrapper, 32 },
};

#endif   /* HASHES_H_1235465 */
