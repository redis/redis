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

/* benchmark hash functions */

#include <stdlib.h>   // malloc
#include <assert.h>
#include <string.h>

#include "benchHash.h"


static void initBuffer(void* buffer, size_t size)
{
    const unsigned long long k1 = 11400714785074694791ULL;   /* 0b1001111000110111011110011011000110000101111010111100101010000111 */
    const unsigned long long k2 = 14029467366897019727ULL;   /* 0b1100001010110010101011100011110100100111110101001110101101001111 */
    unsigned long long acc = k2;
    unsigned char* const p = (unsigned char*)buffer;
    for (size_t s = 0; s < size; s++) {
        acc *= k1;
        p[s] = (unsigned char)(acc >> 56);
    }
}


#define MARGIN_FOR_LATENCY 1024
#define START_MASK (MARGIN_FOR_LATENCY-1)

typedef size_t (*sizeFunction_f)(size_t targetSize);

/*
 * bench_hash_internal():
 * Benchmarks hashfn repeateadly over single input of size `size`
 * return: nb of hashes per second
 */
static double
bench_hash_internal(BMK_benchFn_t hashfn, void* payload,
                    size_t nbBlocks, sizeFunction_f selectSize, size_t size,
                    unsigned total_time_ms, unsigned iter_time_ms)
{
    BMK_timedFnState_shell shell;
    BMK_timedFnState_t* const txf = BMK_initStatic_timedFnState(&shell, sizeof(shell), total_time_ms, iter_time_ms);
    assert(txf != NULL);

    size_t const srcSize = (size_t)size;
    size_t const srcBufferSize = srcSize + MARGIN_FOR_LATENCY;
    void* const srcBuffer = malloc(srcBufferSize);
    assert(srcBuffer != NULL);
    initBuffer(srcBuffer, srcBufferSize);
    #define FAKE_DSTSIZE 32
    size_t const dstSize = FAKE_DSTSIZE;
    char dstBuffer_static[FAKE_DSTSIZE] = {0};

    #define NB_BLOCKS_MAX 1024
    const void* srcBuffers[NB_BLOCKS_MAX];
    size_t srcSizes[NB_BLOCKS_MAX];
    void* dstBuffers[NB_BLOCKS_MAX];
    size_t dstCapacities[NB_BLOCKS_MAX];
    assert(nbBlocks < NB_BLOCKS_MAX);

    assert(size > 0);
    for (size_t n=0; n < nbBlocks; n++) {
        srcBuffers[n] = srcBuffer;
        srcSizes[n] = selectSize(size);
        dstBuffers[n] = dstBuffer_static;
        dstCapacities[n] = dstSize;
    }


    BMK_benchParams_t params = {
        .benchFn = hashfn,
        .benchPayload = payload,
        .initFn = NULL,
        .initPayload = NULL,
        .errorFn = NULL,
        .blockCount = nbBlocks,
        .srcBuffers = srcBuffers,
        .srcSizes = srcSizes,
        .dstBuffers = dstBuffers,
        .dstCapacities = dstCapacities,
        .blockResults = NULL
    };
    BMK_runOutcome_t result;

    memset(&result, 0, sizeof(result));
    while (!BMK_isCompleted_TimedFn(txf)) {
        result = BMK_benchTimedFn(txf, params);
        assert(BMK_isSuccessful_runOutcome(result));
    }

    BMK_runTime_t const runTime = BMK_extract_runTime(result);

    free(srcBuffer);
    assert(runTime.nanoSecPerRun != 0);
    return (1000000000U / runTime.nanoSecPerRun) * nbBlocks;

}


static size_t rand_1_N(size_t N) { return ((size_t)rand() % N)  + 1; }

static size_t identity(size_t s) { return s; }

static size_t
benchLatency(const void* src, size_t srcSize,
                   void* dst, size_t dstCapacity,
                   void* customPayload)
{
    (void)dst; (void)dstCapacity;
    BMK_benchFn_t benchfn = (BMK_benchFn_t)customPayload;
    static size_t hash = 0;

    const void* const start = (const char*)src + (hash & START_MASK);

    return hash = benchfn(start, srcSize, dst, dstCapacity, NULL);
}



#ifndef SIZE_TO_HASH_PER_ROUND
#  define SIZE_TO_HASH_PER_ROUND 200000
#endif

#ifndef NB_HASH_ROUNDS_MAX
#  define NB_HASH_ROUNDS_MAX 1000
#endif

double bench_hash(BMK_benchFn_t hashfn,
                  BMK_benchMode benchMode,
                  size_t size, BMK_sizeMode sizeMode,
                  unsigned total_time_ms, unsigned iter_time_ms)
{
    sizeFunction_f const sizef = (sizeMode == BMK_fixedSize) ? identity : rand_1_N;
    BMK_benchFn_t const benchfn = (benchMode == BMK_throughput) ? hashfn : benchLatency;
    BMK_benchFn_t const payload = (benchMode == BMK_throughput) ? NULL : hashfn;

    size_t nbBlocks = (SIZE_TO_HASH_PER_ROUND / size) + 1;
    if (nbBlocks > NB_HASH_ROUNDS_MAX) nbBlocks = NB_HASH_ROUNDS_MAX;

    return bench_hash_internal(benchfn, payload,
                               nbBlocks, sizef, size,
                               total_time_ms, iter_time_ms);
}
