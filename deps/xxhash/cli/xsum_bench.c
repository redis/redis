/*
 * xsum_bench - Benchmark functions for xxhsum
 * Copyright (C) 2013-2021 Yann Collet
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
 *   - xxHash homepage: https://www.xxhash.com
 *   - xxHash source repository: https://github.com/Cyan4973/xxHash
 */

#include "xsum_output.h"  /* XSUM_logLevel */
#include "xsum_bench.h"
#include "xsum_sanity_check.h" /* XSUM_fillTestBuffer */
#include "xsum_os_specific.h"  /* XSUM_getFileSize */
#ifndef XXH_STATIC_LINKING_ONLY
#  define XXH_STATIC_LINKING_ONLY
#endif
#include "../xxhash.h"
#ifdef XXHSUM_DISPATCH
#  include "../xxh_x86dispatch.h"  /* activate _dispatch() redirectors */
#endif

#include <stdlib.h>  /* malloc, free */
#include <assert.h>
#include <string.h>  /* strlen, memcpy */
#include <time.h>   /* clock_t, clock, CLOCKS_PER_SEC */
#include <errno.h>  /* errno */

#define TIMELOOP_S 1
#define TIMELOOP  (TIMELOOP_S * CLOCKS_PER_SEC)   /* target timing per iteration */
#define TIMELOOP_MIN (TIMELOOP / 2)               /* minimum timing to validate a result */

/* Each benchmark iteration attempts to match TIMELOOP (1 second).
 * The nb of loops is adjusted at each iteration to reach that target.
 * However, initially, there is no information, so 1st iteration blindly targets an arbitrary speed.
 * If it's too small, it will be adjusted, and a new attempt will be made.
 * But if it's too large, the first iteration can be very long,
 * before being fixed at second attempt.
 * So prefer starting with small speed targets.
 * XXH_1ST_SPEED_TARGET is defined in MB/s */
#ifndef XXH_1ST_SPEED_TARGET
# define XXH_1ST_SPEED_TARGET 10
#endif

#define MAX_MEM    (2 GB - 64 MB)

static clock_t XSUM_clockSpan( clock_t start )
{
    return clock() - start;   /* works even if overflow; Typical max span ~ 30 mn */
}

static size_t XSUM_findMaxMem(XSUM_U64 requiredMem)
{
    size_t const step = 64 MB;
    void* testmem = NULL;

    requiredMem = (((requiredMem >> 26) + 1) << 26);
    requiredMem += 2*step;
    if (requiredMem > MAX_MEM) requiredMem = MAX_MEM;

    while (!testmem) {
        if (requiredMem > step) requiredMem -= step;
        else requiredMem >>= 1;
        testmem = malloc ((size_t)requiredMem);
    }
    free (testmem);

    /* keep some space available */
    if (requiredMem > step) requiredMem -= step;
    else requiredMem >>= 1;

    return (size_t)requiredMem;
}

/*
 * A secret buffer used for benchmarking XXH3's withSecret variants.
 *
 * In order for the bench to be realistic, the secret buffer would need to be
 * pre-generated.
 *
 * Adding a pointer to the parameter list would be messy.
 */
static XSUM_U8 g_benchSecretBuf[XXH3_SECRET_SIZE_MIN];

/*
 * Wrappers for the benchmark.
 *
 * If you would like to add other hashes to the bench, create a wrapper and add
 * it to the g_hashesToBench table. It will automatically be added.
 */
typedef XSUM_U32 (*hashFunction)(const void* buffer, size_t bufferSize, XSUM_U32 seed);

static XSUM_U32 localXXH32(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    return XXH32(buffer, bufferSize, seed);
}
static XSUM_U32 localXXH32_stream(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    XXH32_state_t state;
    (void)seed;
    XXH32_reset(&state, seed);
    XXH32_update(&state, buffer, bufferSize);
    return (XSUM_U32)XXH32_digest(&state);
}
static XSUM_U32 localXXH64(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    return (XSUM_U32)XXH64(buffer, bufferSize, seed);
}
static XSUM_U32 localXXH64_stream(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    XXH64_state_t state;
    (void)seed;
    XXH64_reset(&state, seed);
    XXH64_update(&state, buffer, bufferSize);
    return (XSUM_U32)XXH64_digest(&state);
}
static XSUM_U32 localXXH3_64b(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    (void)seed;
    return (XSUM_U32)XXH3_64bits(buffer, bufferSize);
}
static XSUM_U32 localXXH3_64b_seeded(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    return (XSUM_U32)XXH3_64bits_withSeed(buffer, bufferSize, seed);
}
static XSUM_U32 localXXH3_64b_secret(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    (void)seed;
    return (XSUM_U32)XXH3_64bits_withSecret(buffer, bufferSize, g_benchSecretBuf, sizeof(g_benchSecretBuf));
}
static XSUM_U32 localXXH3_128b(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    (void)seed;
    return (XSUM_U32)(XXH3_128bits(buffer, bufferSize).low64);
}
static XSUM_U32 localXXH3_128b_seeded(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    return (XSUM_U32)(XXH3_128bits_withSeed(buffer, bufferSize, seed).low64);
}
static XSUM_U32 localXXH3_128b_secret(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    (void)seed;
    return (XSUM_U32)(XXH3_128bits_withSecret(buffer, bufferSize, g_benchSecretBuf, sizeof(g_benchSecretBuf)).low64);
}
static XSUM_U32 localXXH3_stream(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    XXH3_state_t state;
    (void)seed;
    XXH3_64bits_reset(&state);
    XXH3_64bits_update(&state, buffer, bufferSize);
    return (XSUM_U32)XXH3_64bits_digest(&state);
}
static XSUM_U32 localXXH3_stream_seeded(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    XXH3_state_t state;
    XXH3_INITSTATE(&state);
    XXH3_64bits_reset_withSeed(&state, (XXH64_hash_t)seed);
    XXH3_64bits_update(&state, buffer, bufferSize);
    return (XSUM_U32)XXH3_64bits_digest(&state);
}
static XSUM_U32 localXXH128_stream(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    XXH3_state_t state;
    (void)seed;
    XXH3_128bits_reset(&state);
    XXH3_128bits_update(&state, buffer, bufferSize);
    return (XSUM_U32)(XXH3_128bits_digest(&state).low64);
}
static XSUM_U32 localXXH128_stream_seeded(const void* buffer, size_t bufferSize, XSUM_U32 seed)
{
    XXH3_state_t state;
    XXH3_INITSTATE(&state);
    XXH3_128bits_reset_withSeed(&state, (XXH64_hash_t)seed);
    XXH3_128bits_update(&state, buffer, bufferSize);
    return (XSUM_U32)(XXH3_128bits_digest(&state).low64);
}


typedef struct {
    const char*  name;
    hashFunction func;
} hashInfo;

static const hashInfo g_hashesToBench[] = {
    { "XXH32",             &localXXH32 },
    { "XXH64",             &localXXH64 },
    { "XXH3_64b",          &localXXH3_64b },
    { "XXH3_64b w/seed",   &localXXH3_64b_seeded },
    { "XXH3_64b w/secret", &localXXH3_64b_secret },
    { "XXH128",            &localXXH3_128b },
    { "XXH128 w/seed",     &localXXH3_128b_seeded },
    { "XXH128 w/secret",   &localXXH3_128b_secret },
    { "XXH32_stream",      &localXXH32_stream },
    { "XXH64_stream",      &localXXH64_stream },
    { "XXH3_stream",       &localXXH3_stream },
    { "XXH3_stream w/seed",&localXXH3_stream_seeded },
    { "XXH128_stream",     &localXXH128_stream },
    { "XXH128_stream w/seed",&localXXH128_stream_seeded },
};
#define NB_HASHFUNC (sizeof(g_hashesToBench) / sizeof(*g_hashesToBench))

#define NB_TESTFUNC (1 + 2 * NB_HASHFUNC)
int const g_nbTestFunctions = NB_TESTFUNC;
char g_testIDs[NB_TESTFUNC] = { 0 };
const char k_testIDs_default[NB_TESTFUNC] = { 0,
        1 /*XXH32*/, 0,
        1 /*XXH64*/, 0,
        1 /*XXH3*/, 0, 0, 0, 0, 0,
        1 /*XXH128*/ };

int g_nbIterations = NBLOOPS_DEFAULT;
#define HASHNAME_MAX 29
static void XSUM_benchHash(hashFunction h, const char* hName, int testID,
                           const void* buffer, size_t bufferSize)
{
    XSUM_U32 nbh_perIteration = (XSUM_U32)((XXH_1ST_SPEED_TARGET MB) / (bufferSize+1)) + 1;
    int iterationNb, nbIterations = g_nbIterations + !g_nbIterations /* min 1 */;
    double fastestH = 100000000.;
    assert(HASHNAME_MAX > 2);
    XSUM_logVerbose(2, "\r%80s\r", "");       /* Clean display line */

    for (iterationNb = 1; iterationNb <= nbIterations; iterationNb++) {
        XSUM_U32 r=0;
        clock_t cStart;

        XSUM_logVerbose(2, "%2i-%-*.*s : %10u ->\r",
                        iterationNb,
                        HASHNAME_MAX, HASHNAME_MAX, hName,
                        (unsigned)bufferSize);
        cStart = clock();
        while (clock() == cStart);   /* starts clock() at its exact beginning */
        cStart = clock();

        {   XSUM_U32 u;
            for (u=0; u<nbh_perIteration; u++)
                r += h(buffer, bufferSize, u);
        }
        if (r==0) XSUM_logVerbose(3,".\r");  /* do something with r to defeat compiler "optimizing" hash away */

        {   clock_t const nbTicks = XSUM_clockSpan(cStart);
            double const ticksPerHash = ((double)nbTicks / TIMELOOP) / nbh_perIteration;
            /*
             * clock() is the only decent portable timer, but it isn't very
             * precise.
             *
             * Sometimes, this lack of precision is enough that the benchmark
             * finishes before there are enough ticks to get a meaningful result.
             *
             * For example, on a Core 2 Duo (without any sort of Turbo Boost),
             * the imprecise timer caused peculiar results like so:
             *
             *    XXH3_64b                   4800.0 MB/s // conveniently even
             *    XXH3_64b unaligned         4800.0 MB/s
             *    XXH3_64b seeded            9600.0 MB/s // magical 2x speedup?!
             *    XXH3_64b seeded unaligned  4800.0 MB/s
             *
             * If we sense a suspiciously low number of ticks, we increase the
             * iterations until we can get something meaningful.
             */
            if (nbTicks < TIMELOOP_MIN) {
                /* Not enough time spent in benchmarking, risk of rounding bias */
                if (nbTicks == 0) { /* faster than resolution timer */
                    nbh_perIteration *= 100;
                } else {
                    /*
                     * update nbh_perIteration so that the next round lasts
                     * approximately 1 second.
                     */
                    double nbh_perSecond = (1 / ticksPerHash) + 1;
                    if (nbh_perSecond > (double)(4000U<<20)) nbh_perSecond = (double)(4000U<<20);   /* avoid overflow */
                    nbh_perIteration = (XSUM_U32)nbh_perSecond;
                }
                /* g_nbIterations==0 => quick evaluation, no claim of accuracy */
                if (g_nbIterations>0) {
                    iterationNb--;   /* new round for a more accurate speed evaluation */
                    continue;
                }
            }
            if (ticksPerHash < fastestH) fastestH = ticksPerHash;
            if (fastestH>0.) { /* avoid div by zero */
                XSUM_logVerbose(2, "%2i-%-*.*s : %10u -> %8.0f it/s (%7.1f MB/s) \r",
                            iterationNb,
                            HASHNAME_MAX, HASHNAME_MAX, hName,
                            (unsigned)bufferSize,
                            (double)1 / fastestH,
                            ((double)bufferSize / (1 MB)) / fastestH);
        }   }
        {   double nbh_perSecond = (1 / fastestH) + 1;
            if (nbh_perSecond > (double)(4000U<<20)) nbh_perSecond = (double)(4000U<<20);   /* avoid overflow */
            nbh_perIteration = (XSUM_U32)nbh_perSecond;
        }
    }
    XSUM_logVerbose(1, "%2i#%-*.*s : %10u -> %8.0f it/s (%7.1f MB/s) \n",
                    testID,
                    HASHNAME_MAX, HASHNAME_MAX, hName,
                    (unsigned)bufferSize,
                    (double)1 / fastestH,
                    ((double)bufferSize / (1 MB)) / fastestH);
    if (XSUM_logLevel<1)
        XSUM_logVerbose(0, "%u, ", (unsigned)((double)1 / fastestH));
}


/*
 * Allocates a string containing s1 and s2 concatenated. Acts like strdup.
 * The result must be freed.
 */
static char* XSUM_strcatDup(const char* s1, const char* s2)
{
    assert(s1 != NULL);
    assert(s2 != NULL);
    {   size_t len1 = strlen(s1);
        size_t len2 = strlen(s2);
        char* buf = (char*)malloc(len1 + len2 + 1);
        if (buf != NULL) {
            /* strcpy(buf, s1) */
            memcpy(buf, s1, len1);
            /* strcat(buf, s2) */
            memcpy(buf + len1, s2, len2 + 1);
        }
        return buf;
    }
}


/*!
 * XSUM_benchMem():
 * Benchmark provided content up to twice per function:
 * - once at provided aligned memory address (%16)
 * - second time at unaligned memory address (+3)
 * Enabled functions and modes are provided via @g_hashesToBench global variable.
 * @buffer: Must be 16-byte aligned.
 * The allocated size of underlying @buffer must be >= (@bufferSize+3).
 * This function also fills @g_benchSecretBuf, to bench XXH3's _withSecret() variants.
 */
static void XSUM_benchMem(const void* buffer, size_t bufferSize)
{
    assert((((size_t)buffer) & 15) == 0);  /* ensure alignment */
    XSUM_fillTestBuffer(g_benchSecretBuf, sizeof(g_benchSecretBuf));
    {   int i;
        for (i = 1; i < (int)NB_TESTFUNC; i++) {
            int const hashFuncID = (i-1) / 2;
            assert(g_hashesToBench[hashFuncID].name != NULL);
            if (g_testIDs[i] == 0) continue;
            /* aligned */
            if ((i % 2) == 1) {
                XSUM_benchHash(g_hashesToBench[hashFuncID].func, g_hashesToBench[hashFuncID].name, i, buffer, bufferSize);
            }
            /* unaligned */
            if ((i % 2) == 0) {
                /* Append "unaligned". */
                char* const hashNameBuf = XSUM_strcatDup(g_hashesToBench[hashFuncID].name, " unaligned");
                assert(hashNameBuf != NULL);
                XSUM_benchHash(g_hashesToBench[hashFuncID].func, hashNameBuf, i, ((const char*)buffer)+3, bufferSize);
                free(hashNameBuf);
            }
    }   }
}

static size_t XSUM_selectBenchedSize(const char* fileName)
{
    XSUM_U64 const inFileSize = XSUM_getFileSize(fileName);
    size_t benchedSize = (size_t) XSUM_findMaxMem(inFileSize);
    if ((XSUM_U64)benchedSize > inFileSize) benchedSize = (size_t)inFileSize;
    if (benchedSize < inFileSize) {
        XSUM_log("Not enough memory for '%s' full size; testing %i MB only...\n", fileName, (int)(benchedSize>>20));
    }
    return benchedSize;
}


int XSUM_benchFiles(const char* fileNamesTable[], int nbFiles)
{
    int fileIdx;
    for (fileIdx=0; fileIdx<nbFiles; fileIdx++) {
        const char* const inFileName = fileNamesTable[fileIdx];
        assert(inFileName != NULL);

        {   FILE* const inFile = XSUM_fopen( inFileName, "rb" );
            size_t const benchedSize = XSUM_selectBenchedSize(inFileName);
            char* const buffer = (char*)calloc(benchedSize+16+3, 1);
            void* const alignedBuffer = (buffer+15) - (((size_t)(buffer+15)) & 0xF);  /* align on next 16 bytes */

            /* Checks */
            if (inFile==NULL){
                XSUM_log("Error: Could not open '%s': %s.\n", inFileName, strerror(errno));
                free(buffer);
                exit(11);
            }
            if(!buffer) {
                XSUM_log("\nError: Out of memory.\n");
                fclose(inFile);
                exit(12);
            }

            /* Fill input buffer */
            {   size_t const readSize = fread(alignedBuffer, 1, benchedSize, inFile);
                fclose(inFile);
                if(readSize != benchedSize) {
                    XSUM_log("\nError: Could not read '%s': %s.\n", inFileName, strerror(errno));
                    free(buffer);
                    exit(13);
            }   }

            /* bench */
            XSUM_benchMem(alignedBuffer, benchedSize);

            free(buffer);
    }   }
    return 0;
}


int XSUM_benchInternal(size_t keySize)
{
    void* const buffer = calloc(keySize+16+3, 1);
    if (buffer == NULL) {
        XSUM_log("\nError: Out of memory.\n");
        exit(12);
    }

    {   const void* const alignedBuffer = ((char*)buffer+15) - (((size_t)((char*)buffer+15)) & 0xF);  /* align on next 16 bytes */

        /* bench */
        XSUM_logVerbose(1, "Sample of ");
        if (keySize > 10 KB) {
            XSUM_logVerbose(1, "%u KB", (unsigned)(keySize >> 10));
        } else {
            XSUM_logVerbose(1, "%u bytes", (unsigned)keySize);
        }
        XSUM_logVerbose(1, "...        \n");

        XSUM_benchMem(alignedBuffer, keySize);
        free(buffer);
    }
    return 0;
}
