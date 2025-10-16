/*
*  CSV Display module for the hash benchmark program
*  Part of the xxHash project
*  Copyright (C) 2019-2021 Yann Collet
*
*  GPL v2 License
*
*  This program is free software; you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 2 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License along
*  with this program; if not, write to the Free Software Foundation, Inc.,
*  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*
*  You can contact the author at:
*  - xxHash homepage: https://www.xxhash.com
*  - xxHash source repository: https://github.com/Cyan4973/xxHash
*/

#ifndef BH_DISPLAY_H_192088098
#define BH_DISPLAY_H_192088098

#if defined (__cplusplus)
extern "C" {
#endif


/* ===  Dependencies  === */

#include "benchfn.h"   /* BMK_benchFn_t */


/* ===  Declarations  === */

typedef struct {
    const char* name;
    BMK_benchFn_t hash;
} Bench_Entry;

void bench_largeInput(Bench_Entry const* hashDescTable, int nbHashes, int sizeLogMin, int sizeLogMax);

void bench_throughput_smallInputs(Bench_Entry const* hashDescTable, int nbHashes, size_t sizeMin, size_t sizeMax);
void bench_throughput_randomInputLength(Bench_Entry const* hashDescTable, int nbHashes, size_t sizeMin, size_t sizeMax);

void bench_latency_smallInputs(Bench_Entry const* hashDescTable, int nbHashes, size_t sizeMin, size_t sizeMax);
void bench_latency_randomInputLength(Bench_Entry const* hashDescTable, int nbHashes, size_t sizeMin, size_t sizeMax);



#if defined (__cplusplus)
}
#endif

#endif   /* BH_DISPLAY_H_192088098 */
