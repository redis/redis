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
*  You can contact the author at :
*  - xxHash homepage : https://www.xxhash.com
*  - xxHash source repository : https://github.com/Cyan4973/xxHash
*/


/* ===  Dependencies  === */

#include <stdlib.h>   /* rand */
#include <stdio.h>    /* printf */
#include <assert.h>

#include "benchHash.h"
#include "bhDisplay.h"


/* ===  benchmark large input  === */

#define MB_UNIT           1000000
#define BENCH_LARGE_ITER_MS   490
#define BENCH_LARGE_TOTAL_MS 1010
static void bench_oneHash_largeInput(Bench_Entry hashDesc, int minlog, int maxlog)
{
    printf("%-7s", hashDesc.name);
    for (int sizelog=minlog; sizelog<=maxlog; sizelog++) {
        size_t const inputSize = (size_t)1 << sizelog;
        double const nbhps = bench_hash(hashDesc.hash, BMK_throughput,
                                        inputSize, BMK_fixedSize,
                                        BENCH_LARGE_TOTAL_MS, BENCH_LARGE_ITER_MS);
        printf(",%6.0f", nbhps * inputSize / MB_UNIT); fflush(NULL);
    }
    printf("\n");
}

void bench_largeInput(Bench_Entry const* hashDescTable, int nbHashes, int minlog, int maxlog)
{
    assert(maxlog <  31);
    assert(minlog >=  0);
    printf("benchmarking large inputs : from %u bytes (log%i) to %u MB (log%i) \n",
        1U << minlog, minlog,
        (1U << maxlog) >> 20, maxlog);
    for (int i=0; i<nbHashes; i++)
        bench_oneHash_largeInput(hashDescTable[i], minlog, maxlog);
}



/* ===  Benchmark small inputs  === */

#define BENCH_SMALL_ITER_MS   170
#define BENCH_SMALL_TOTAL_MS  490
static void bench_throughput_oneHash_smallInputs(Bench_Entry hashDesc, size_t sizeMin, size_t sizeMax)
{
    printf("%-7s", hashDesc.name);
    for (size_t s=sizeMin; s<sizeMax+1; s++) {
        double const nbhps = bench_hash(hashDesc.hash, BMK_throughput,
                                        s, BMK_fixedSize,
                                        BENCH_SMALL_TOTAL_MS, BENCH_SMALL_ITER_MS);
        printf(",%10.0f", nbhps); fflush(NULL);
    }
    printf("\n");
}

void bench_throughput_smallInputs(Bench_Entry const* hashDescTable, int nbHashes, size_t sizeMin, size_t sizeMax)
{
    printf("Throughput small inputs of fixed size (from %zu to %zu bytes): \n",
            sizeMin, sizeMax);
    for (int i=0; i<nbHashes; i++)
        bench_throughput_oneHash_smallInputs(hashDescTable[i], sizeMin, sizeMax);
}



/* ===   Latency measurements (small keys)   === */

static void bench_latency_oneHash_smallInputs(Bench_Entry hashDesc, size_t size_min, size_t size_max)
{
    printf("%-7s", hashDesc.name);
    for (size_t s=size_min; s<size_max+1; s++) {
        double const nbhps = bench_hash(hashDesc.hash, BMK_latency,
                                        s, BMK_fixedSize,
                                        BENCH_SMALL_TOTAL_MS, BENCH_SMALL_ITER_MS);
        printf(",%10.0f", nbhps); fflush(NULL);
    }
    printf("\n");
}

void bench_latency_smallInputs(Bench_Entry const* hashDescTable, int nbHashes, size_t size_min, size_t size_max)
{
    printf("Latency for small inputs of fixed size : \n");
    for (int i=0; i<nbHashes; i++)
        bench_latency_oneHash_smallInputs(hashDescTable[i], size_min, size_max);
}


/* ===   Random input Length   === */

static void bench_randomInputLength_withOneHash(Bench_Entry hashDesc, size_t size_min, size_t size_max)
{
    printf("%-7s", hashDesc.name);
    for (size_t s=size_min; s<size_max+1; s++) {
        srand((unsigned)s);   /* ensure random sequence of length will be the same for a given s */
        double const nbhps = bench_hash(hashDesc.hash, BMK_throughput,
                                        s, BMK_randomSize,
                                        BENCH_SMALL_TOTAL_MS, BENCH_SMALL_ITER_MS);
        printf(",%10.0f", nbhps); fflush(NULL);
    }
    printf("\n");
}

void bench_throughput_randomInputLength(Bench_Entry const* hashDescTable, int nbHashes, size_t size_min, size_t size_max)
{
    printf("benchmarking random size inputs [1-N] : \n");
    for (int i=0; i<nbHashes; i++)
        bench_randomInputLength_withOneHash(hashDescTable[i], size_min, size_max);
}


/* ===   Latency with Random input Length   === */

static void bench_latency_oneHash_randomInputLength(Bench_Entry hashDesc, size_t size_min, size_t size_max)
{
    printf("%-7s", hashDesc.name);
    for (size_t s=size_min; s<size_max+1; s++) {
        srand((unsigned)s);   /* ensure random sequence of length will be the same for a given s */
        double const nbhps = bench_hash(hashDesc.hash, BMK_latency,
                                        s, BMK_randomSize,
                                        BENCH_SMALL_TOTAL_MS, BENCH_SMALL_ITER_MS);
        printf(",%10.0f", nbhps); fflush(NULL);
    }
    printf("\n");
}

void bench_latency_randomInputLength(Bench_Entry const* hashDescTable, int nbHashes, size_t size_min, size_t size_max)
{
    printf("Latency for small inputs of random size [1-N] : \n");
    for (int i=0; i<nbHashes; i++)
        bench_latency_oneHash_randomInputLength(hashDescTable[i], size_min, size_max);
}
