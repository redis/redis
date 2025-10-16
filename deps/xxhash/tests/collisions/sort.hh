/*
 * sort.hh - headers for C++ sort functions
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
 * You can contact the author at :
 *   - xxHash homepage : https://www.xxhash.com
 *   - xxHash source repository : https://github.com/Cyan4973/xxHash
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>   // size
#include <stdint.h>   // uint64_t
#define XXH_STATIC_LINKING_ONLY  // XXH128_hash_t
#include "xxhash.h"

void sort64(uint64_t* table, size_t size);

void sort128(XXH128_hash_t* table, size_t size);

#ifdef __cplusplus
}  // extern C
#endif
