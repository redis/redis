/*
*  Hash benchmark module
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


#ifndef BENCH_HASH_H_983426678
#define BENCH_HASH_H_983426678

#if defined (__cplusplus)
extern "C" {
#endif


/* ===  Dependencies  === */

#include "benchfn.h"   /* BMK_benchFn_t */


/* ===  Declarations  === */

typedef enum { BMK_throughput, BMK_latency } BMK_benchMode;

typedef enum { BMK_fixedSize,   /* hash always `size` bytes */
               BMK_randomSize,  /* hash a random nb of bytes, between 1 and `size` (inclusive) */
} BMK_sizeMode;

/*
 * bench_hash():
 * Returns speed expressed as nb hashes per second.
 * total_time_ms: time spent benchmarking the hash function with given parameters
 * iter_time_ms: time spent for one round. If multiple rounds are run,
 *               bench_hash() will report the speed of best round.
 */
double bench_hash(BMK_benchFn_t hashfn,
                  BMK_benchMode benchMode,
                  size_t size, BMK_sizeMode sizeMode,
                  unsigned total_time_ms, unsigned iter_time_ms);



#if defined (__cplusplus)
}
#endif

#endif /* BENCH_HASH_H_983426678 */
