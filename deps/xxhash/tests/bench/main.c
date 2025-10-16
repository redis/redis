/*
 * Main program to benchmark hash functions
 * Part of the xxHash project
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
 * - xxHash homepage: https://www.xxhash.com
 * - xxHash source repository: https://github.com/Cyan4973/xxHash
 */


/* ===  dependencies  === */

#include <stdio.h>       /* printf */
#include <limits.h>      /* INT_MAX */
#include "bhDisplay.h"   /* bench_x */


/* ===  defines list of hashes `hashCandidates` and NB_HASHES  *** */

#include "hashes.h"


/* ===  parse command line  === */

#undef NDEBUG
#include <assert.h>


/*!
 * readIntFromChar():
 * Allows and interprets K, KB, KiB, M, MB and MiB suffix.
 * Will also modify `*stringPtr`, advancing it to position where it stopped reading.
 */
static int readIntFromChar(const char** stringPtr)
{
    static int const max = (INT_MAX / 10) - 1;
    int result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9')) {
        assert(result < max);
        result *= 10;
        result += (unsigned)(**stringPtr - '0');
        (*stringPtr)++ ;
    }
    if ((**stringPtr=='K') || (**stringPtr=='M')) {
        int const maxK = INT_MAX >> 10;
        assert(result < maxK);
        result <<= 10;
        if (**stringPtr=='M') {
            assert(result < maxK);
            result <<= 10;
        }
        (*stringPtr)++;  /* skip `K` or `M` */
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    return result;
}


/**
 * isCommand():
 * Checks if string is the same as longCommand.
 * If yes, @return 1, otherwise @return 0
 */
static int isCommand(const char* string, const char* longCommand)
{
    assert(string);
    assert(longCommand);
    size_t const comSize = strlen(longCommand);
    return !strncmp(string, longCommand, comSize);
}

/*
 * longCommandWArg():
 * Checks if *stringPtr is the same as longCommand.
 * If yes, @return 1 and advances *stringPtr to the position which immediately
 * follows longCommand.
 * @return 0 and doesn't modify *stringPtr otherwise.
 */
static int longCommandWArg(const char** stringPtr, const char* longCommand)
{
    assert(stringPtr);
    assert(longCommand);
    size_t const comSize = strlen(longCommand);
    int const result = isCommand(*stringPtr, longCommand);
    if (result) *stringPtr += comSize;
    return result;
}


/* ===   default values - can be redefined at compilation time   === */

#ifndef SMALL_SIZE_MIN_DEFAULT
#  define SMALL_SIZE_MIN_DEFAULT   1
#endif
#ifndef SMALL_SIZE_MAX_DEFAULT
#  define SMALL_SIZE_MAX_DEFAULT  30
#endif
#ifndef LARGE_SIZELOG_MIN_DEFAULT
#  define LARGE_SIZELOG_MIN_DEFAULT   9
#endif
#ifndef LARGE_SIZELOG_MAX_DEFAULT
#  define LARGE_SIZELOG_MAX_DEFAULT  27
#endif


static int display_hash_names(void)
{
    int i;
    printf("available hashes : \n");
    for (i=0; i<NB_HASHES; i++) {
        printf("%s, ", hashCandidates[i].name);
    }
    printf("\b\b  \n");
    return 0;
}

/*
 * @return: hashID (necessarily between 0 and NB_HASHES) if present
 *          -1 on error (hname not present)
 */
static int hashID(const char* hname)
{
    int id;
    assert(hname);
    for (id=0; id < NB_HASHES; id++) {
        assert(hashCandidates[id].name);
        if (strlen(hname) != strlen(hashCandidates[id].name)) continue;
        if (isCommand(hname, hashCandidates[id].name)) return id;
    }
    return -1;
}

static int help(const char* exename)
{
    printf("Usage: %s [options]... [hash]\n", exename);
    printf("Runs various benchmarks at various lengths for the listed hash functions\n");
    printf("and outputs them in a CSV format.\n\n");
    printf("Options: \n");
    printf("  --list       Name available hash algorithms and exit \n");
    printf("  --mins=LEN   Starting length for small size bench (default: %i) \n", SMALL_SIZE_MIN_DEFAULT);
    printf("  --maxs=LEN   End length for small size bench (default: %i) \n", SMALL_SIZE_MAX_DEFAULT);
    printf("  --minl=LEN   Starting log2(length) for large size bench (default: %i) \n", LARGE_SIZELOG_MIN_DEFAULT);
    printf("  --maxl=LEN   End log2(length) for large size bench (default: %i) \n", LARGE_SIZELOG_MAX_DEFAULT);
    printf("  [hash]       Optional, bench all available hashes if not provided \n");
    return 0;
}

static int badusage(const char* exename)
{
    printf("Bad command ... \n");
    help(exename);
    return 1;
}

int main(int argc, const char* argv[])
{
    const char* const exename = argv[0];
    int hashNb = 0;
    int nb_h_test = NB_HASHES;
    int largeTest_log_min = LARGE_SIZELOG_MIN_DEFAULT;
    int largeTest_log_max = LARGE_SIZELOG_MAX_DEFAULT;
    size_t smallTest_size_min = SMALL_SIZE_MIN_DEFAULT;
    size_t smallTest_size_max = SMALL_SIZE_MAX_DEFAULT;

    int arg_nb;
    for (arg_nb = 1; arg_nb < argc; arg_nb++) {
        const char** arg = argv + arg_nb;
        if (isCommand(*arg, "-h")) { assert(argc >= 1); return help(exename); }
        if (isCommand(*arg, "--list")) { return display_hash_names(); }
        if (longCommandWArg(arg, "--n=")) { nb_h_test = readIntFromChar(arg); continue; }  /* hidden command */
        if (longCommandWArg(arg, "--minl=")) { largeTest_log_min = readIntFromChar(arg); continue; }
        if (longCommandWArg(arg, "--maxl=")) { largeTest_log_max = readIntFromChar(arg); continue; }
        if (longCommandWArg(arg, "--mins=")) { smallTest_size_min = (size_t)readIntFromChar(arg); continue; }
        if (longCommandWArg(arg, "--maxs=")) { smallTest_size_max = (size_t)readIntFromChar(arg); continue; }
        /* not a command: must be a hash name */
        hashNb = hashID(*arg);
        if (hashNb >= 0) {
            nb_h_test = 1;
        } else {
            /* not a hash name: error */
            return badusage(exename);
        }
    }

    /* border case (requires (mis)using hidden command `--n=#`) */
    if (hashNb + nb_h_test > NB_HASHES) {
        printf("wrong hash selection \n");
        return 1;
    }

    printf(" ===  benchmarking %i hash functions  === \n", nb_h_test);
    if (largeTest_log_max >= largeTest_log_min) {
        bench_largeInput(hashCandidates+hashNb, nb_h_test, largeTest_log_min, largeTest_log_max);
    }
    if (smallTest_size_max >= smallTest_size_min) {
        bench_throughput_smallInputs(hashCandidates+hashNb, nb_h_test, smallTest_size_min, smallTest_size_max);
        bench_throughput_randomInputLength(hashCandidates+hashNb, nb_h_test, smallTest_size_min, smallTest_size_max);
        bench_latency_smallInputs(hashCandidates+hashNb, nb_h_test, smallTest_size_min, smallTest_size_max);
        bench_latency_randomInputLength(hashCandidates+hashNb, nb_h_test, smallTest_size_min, smallTest_size_max);
    }

    return 0;
}
