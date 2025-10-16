/*
 * sort.cc - C++ sort functions
 * Copyright (C) 2019-2021 Yann Collet
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
 *   - xxHash homepage: https://www.xxhash.com
 *   - xxHash source repository: https://github.com/Cyan4973/xxHash
 */

/*
 * C++ sort functions tend to run faster than C ones due to templates allowing
 * inline optimizations.
 * Also, glibc's qsort() seems to inflate memory usage, resulting in OOM
 * crashes on the test server.
 */

#include <algorithm>  // std::sort
#define XXH_INLINE_ALL  // XXH128_cmp
#include <xxhash.h>

#include "sort.hh"

void sort64(uint64_t* table, size_t size)
{
    std::sort(table, table + size);
}

#include <stdlib.h>  // qsort

void sort128(XXH128_hash_t* table, size_t size)
{
#if 0
    // C++ sort using a custom function object
    struct {
        bool operator()(XXH128_hash_t a, XXH128_hash_t b) const
        {
            return XXH128_cmp(&a, &b);
        }
    } customLess;
    std::sort(table, table + size, customLess);
#else
    qsort(table, size, sizeof(*table), XXH128_cmp);
#endif
}
