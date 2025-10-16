/*
 * Brute force collision tester for 64-bit hashes
 * Part of the xxHash project
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
 *   - xxHash homepage: https://www.xxhash.com
 *   - xxHash source repository: https://github.com/Cyan4973/xxHash
 */

/*
 * The collision tester will generate 24 billion hashes (by default),
 * and count how many collisions were produced by the 64-bit hash algorithm.
 * The optimal amount of collisions for 64-bit is ~18 collisions.
 * A good hash should be close to this figure.
 *
 * This program requires a lot of memory:
 * - Either store hash values directly => 192 GB
 * - Or use a filter:
 *   -  32 GB (by default) for the filter itself
 *   -  + ~14 GB for the list of hashes (depending on the filter's outcome)
 * Due to these memory constraints, it requires a 64-bit system.
 */


 /* ===  Dependencies  === */

#include <stdint.h>   /* uint64_t */
#include <stdlib.h>   /* malloc, free, qsort, exit */
#include <string.h>   /* memset */
#include <stdio.h>    /* printf, fflush */

#undef NDEBUG   /* ensure assert is _not_ disabled */
#include <assert.h>

#include "hashes.h"   /* UniHash, hashfn, hashfnTable */

#include "sort.hh"    /* sort64 */


#ifdef __MINGW32__
/* MINGW claims C11 complians, yet doesn't provide aligned_alloc().
 * _aligned_malloc() isn't a good workaround,
 * as it seems incompatible with realloc().
 * Let's use malloc() instead, array will not be fully aligned,
 * resulting in some performance degradation, but nothing major.
 * This policy can be updated once MINGW becomes really C11 compliant.
 */
# define aligned_alloc(a,s)  ((void)(a), malloc(s))
#endif

typedef enum { ht32, ht64, ht128 } Htype_e;

/* ===  Debug  === */

#define EXIT(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(NULL); exit(1); }

static void hexRaw(const void* buffer, size_t size)
{
    const unsigned char* p = (const unsigned char*)buffer;
    for (size_t i=0; i<size; i++) {
        printf("%02X", p[i]);
    }
}

void printHash(const void* table, size_t n, Htype_e htype)
{
    if ((htype == ht64) || (htype == ht32)){
        uint64_t const h64 = ((const uint64_t*)table)[n];
        hexRaw(&h64, sizeof(h64));
    } else {
        assert(htype == ht128);
        XXH128_hash_t const h128 = ((const XXH128_hash_t*)table)[n];
        hexRaw(&h128, sizeof(h128));
    }
}

/* ===  Generate Random unique Samples to hash  === */

/*
 * These functions will generate and update a sample to hash.
 * initSample() will fill a buffer with random bytes,
 * updateSample() will modify one slab in the input buffer.
 * updateSample() guarantees it will produce unique samples,
 * but it needs to know the total number of samples.
 */


static const uint64_t prime64_1 = 11400714785074694791ULL;   /* 0b1001111000110111011110011011000110000101111010111100101010000111 */
static const uint64_t prime64_2 = 14029467366897019727ULL;   /* 0b1100001010110010101011100011110100100111110101001110101101001111 */
static const uint64_t prime64_3 =  1609587929392839161ULL;   /* 0b0001011001010110011001111011000110011110001101110111100111111001 */

static uint64_t avalanche64(uint64_t h64)
{
    h64 ^= h64 >> 33;
    h64 *= prime64_2;
    h64 ^= h64 >> 29;
    h64 *= prime64_3;
    h64 ^= h64 >> 32;
    return h64;
}

static unsigned char randomByte(uint64_t n)
{
    uint64_t n64 = avalanche64(n+1);
    n64 *= prime64_1;
    return (unsigned char)(n64 >> 56);
}

typedef enum { sf_slab5, sf_sparse } sf_genMode;


#ifdef SLAB5

/*
 * Slab5 sample generation.
 * This algorithm generates unique inputs flipping on average 16 bits per candidate.
 * It is generally much more friendly for most hash algorithms, especially
 * weaker ones, as it shuffles more the input.
 * The algorithm also avoids overfitting the per4 or per8 ingestion patterns.
 */

#define SLAB_SIZE 5

typedef struct {
    void* buffer;
    size_t size;
    sf_genMode mode;
    size_t prngSeed;
    uint64_t hnb;
} sampleFactory;

static void init_sampleFactory(sampleFactory* sf, uint64_t htotal)
{
    uint64_t const minNbSlabs = ((htotal-1) >> 32) + 1;
    uint64_t const minSize = minNbSlabs * SLAB_SIZE;
    if (sf->size < minSize)
        EXIT("sample size must be >= %i bytes for this amount of hashes",
            (int)minSize);

    unsigned char* const p = (unsigned char*)sf->buffer;
    for (size_t n=0; n < sf->size; n++)
        p[n] = randomByte(n);
    sf->hnb = 0;
}

static sampleFactory*
create_sampleFactory(size_t size, uint64_t htotal, uint64_t seed)
{
    sampleFactory* const sf = malloc(sizeof(sampleFactory));
    if (!sf) EXIT("not enough memory");
    void* const buffer = malloc(size);
    if (!buffer) EXIT("not enough memory");
    sf->buffer = buffer;
    sf->size = size;
    sf->mode = sf_slab5;
    sf->prngSeed = seed;
    init_sampleFactory(sf, htotal);
    return sf;
}

static void free_sampleFactory(sampleFactory* sf)
{
    if (!sf) return;
    free(sf->buffer);
    free(sf);
}

static inline void update_sampleFactory(sampleFactory* sf)
{
    size_t const nbSlabs = sf->size / SLAB_SIZE;
    size_t const SlabNb = sf->hnb % nbSlabs;
    sf->hnb++;

    char* const ptr = (char*)sf->buffer;
    size_t const start = (SlabNb * SLAB_SIZE) + 1;
    uint32_t val32;
    memcpy(&val32, ptr+start, sizeof(val32));
    static const uint32_t prime32_5 = 374761393U;
    val32 += prime32_5;
    memcpy(ptr+start, &val32, sizeof(val32));
}

#else

/*
 * Sparse sample generation.
 * This is the default pattern generator.
 * It only flips one bit at a time (mostly).
 * Low hamming distance scenario is more difficult for weak hash algorithms.
 * Note that CRC is immune to this scenario, since they are specifically
 * designed to detect low hamming distances.
 * Prefer the Slab5 pattern generator for collisions on CRC algorithms.
 */

#define SPARSE_LEVEL_MAX 15

/* Nb of combinations of m bits in a register of n bits */
static double Cnm(int n, int m)
{
    assert(n > 0);
    assert(m > 0);
    assert(m <= n);
    double acc = 1;
    for (int i=0; i<m; i++) {
        acc *= n - i;
        acc /= 1 + i;
    }
    return acc;
}

static int enoughCombos(size_t size, uint64_t htotal)
{
    if (size < 2) return 0;   /* ensure no multiplication by negative */
    uint64_t acc = 0;
    uint64_t const srcBits = size * 8; assert(srcBits < INT_MAX);
    int nbBitsSet = 0;
    while (acc < htotal) {
        nbBitsSet++;
        if (nbBitsSet >= SPARSE_LEVEL_MAX) return 0;
        acc += (uint64_t)Cnm((int)srcBits, nbBitsSet);
    }
    return 1;
}

typedef struct {
    void* buffer;
    size_t size;
    sf_genMode mode;
    /* sparse */
    size_t bitIdx[SPARSE_LEVEL_MAX];
    int level;
    size_t maxBitIdx;
    /* slab5 */
    size_t nbSlabs;
    size_t current;
    uint64_t prngSeed;
} sampleFactory;

static void init_sampleFactory(sampleFactory* sf, uint64_t htotal)
{
    if (!enoughCombos(sf->size, htotal)) {
        EXIT("sample size must be larger for this amount of hashes");
    }

    memset(sf->bitIdx, 0, sizeof(sf->bitIdx));
    sf->level = 0;

    unsigned char* const p = (unsigned char*)sf->buffer;
    for (size_t n=0; n<sf->size; n++)
        p[n] = randomByte(sf->prngSeed + n);
}

static sampleFactory*
create_sampleFactory(size_t size, uint64_t htotal, uint64_t seed)
{
    sampleFactory* const sf = malloc(sizeof(sampleFactory));
    if (!sf) EXIT("not enough memory");
    void* const buffer = malloc(size);
    if (!buffer) EXIT("not enough memory");
    sf->buffer = buffer;
    sf->size = size;
    sf->mode = sf_sparse;
    sf->maxBitIdx = size * 8;
    sf->prngSeed = seed;
    init_sampleFactory(sf, htotal);
    return sf;
}

static void free_sampleFactory(sampleFactory* sf)
{
    if (!sf) return;
    free(sf->buffer);
    free(sf);
}

static void flipbit(void* buffer, uint64_t bitID)
{
    size_t const pos = (size_t)(bitID >> 3);
    unsigned char const mask = (unsigned char)(1 << (bitID & 7));
    unsigned char* const p = (unsigned char*)buffer;
    p[pos] ^= mask;
}

static int updateBit(void* buffer, size_t* bitIdx, int level, size_t max)
{
    if (level==0) return 0;   /* can't progress further */

    flipbit(buffer, bitIdx[level]); /* erase previous bits */

    if (bitIdx[level] < max-1) { /* simple case: go to next bit */
        bitIdx[level]++;
        flipbit(buffer, bitIdx[level]); /* set new bit */
        return 1;
    }

    /* reached last bit: need to update a bit from lower level */
    if (!updateBit(buffer, bitIdx, level-1, max-1)) return 0;
    bitIdx[level] = bitIdx[level-1] + 1;
    flipbit(buffer, bitIdx[level]); /* set new bit */
    return 1;
}

static inline void update_sampleFactory(sampleFactory* sf)
{
    if (!updateBit(sf->buffer, sf->bitIdx, sf->level, sf->maxBitIdx)) {
        /* no more room => move to next level */
        sf->level++;
        assert(sf->level < SPARSE_LEVEL_MAX);

        /* set new bits */
        for (int i=1; i <= sf->level; i++) {
            sf->bitIdx[i] = (size_t)(i-1);
            flipbit(sf->buffer, sf->bitIdx[i]);
        }
    }
}

#endif /* pattern generator selection */


/* ===  Candidate Filter  === */

typedef unsigned char Filter;

Filter* create_Filter(int bflog)
{
    assert(bflog < 64 && bflog > 1);
    size_t const bfsize = (size_t)1 << bflog;
    size_t const cacheline_size = 64;
    Filter* const bf = aligned_alloc(cacheline_size, bfsize);  // requires C11
    if (bf==NULL) {
        fprintf(stderr, "Filter creation failed (need %zu MB) \n", bfsize >> 20);
        exit(1);
    }
    memset(bf, 0, bfsize);  // we want memory to be actually used
    return bf;
}

void free_Filter(Filter* bf)
{
    free(bf);
}

#ifdef FILTER_1_PROBE

/*
 * Attach hash to a slot
 * return: Nb of potential collision candidates detected
 *          0: position not yet occupied
 *          2: position previously occupied by a single candidate
 *          1: position already occupied by multiple candidates
 */
inline int Filter_insert(Filter* bf, int bflog, uint64_t hash)
{
    int const slotNb = hash & 3;
    int const shift = slotNb * 2 ;

    size_t const bfmask = ((size_t)1 << bflog) - 1;
    size_t const pos = (hash >> 2) & bfmask;

    int const existingCandidates = ((((unsigned char*)bf)[pos]) >> shift) & 3;

    static const int addCandidates[4] = { 0, 2, 1, 1 };
    static const int nextValue[4] = { 1, 2, 3, 3 };

    ((unsigned char*)bf)[pos] |= (unsigned char)(nextValue[existingCandidates] << shift);
    return addCandidates[existingCandidates];
}

/*
 * Check if provided 64-bit hash is a collision candidate
 * Requires the slot to be occupied by at least 2 candidates.
 * return >0 if hash is a collision candidate
 *         0 otherwise (slot unoccupied, or only one candidate)
 * note: unoccupied slots should not happen in this algorithm,
 *       since all hashes are supposed to have been inserted at least once.
 */
inline int Filter_check(const Filter* bf, int bflog, uint64_t hash)
{
    int const slotNb = hash & 3;
    int const shift = slotNb * 2;

    size_t const bfmask = ((size_t)1 << bflog) - 1;
    size_t const pos = (hash >> 2) & bfmask;

    return (((const unsigned char*)bf)[pos]) >> (shift+1) & 1;
}

#else

/*
 * 2-probes strategy,
 * more efficient at filtering candidates,
 * requires filter size to be > nb of hashes
 */

#define MIN(a,b)   ((a) < (b) ? (a) : (b))
#define MAX(a,b)   ((a) > (b) ? (a) : (b))

/*
 * Attach hash to 2 slots
 * return: Nb of potential candidates detected
 *          0: position not yet occupied
 *          1: position may be already occupied
 */
static inline int Filter_insert(Filter* bf, int bflog, uint64_t hash)
{
    hash = avalanche64(hash);
    unsigned const slot1 = hash & 255;
    hash >>= 8;
    unsigned const slot2 = hash & 255;
    hash >>= 8;

    size_t const fclmask = ((size_t)1 << (bflog-6)) - 1;
    size_t const cacheLineNb = (size_t)hash & fclmask;

    size_t const pos1 = (cacheLineNb << 6) + (slot1 >> 3);
    unsigned const shift1 = slot1 & 7;
    unsigned const bit1 = 1U << shift1;
    unsigned present1 = bf[pos1] & bit1;

    size_t const pos2 = (cacheLineNb << 6) + (slot2 >> 3);
    unsigned const shift2 = slot2 & 7;
    unsigned const bit2 = 1U << shift2;
    unsigned present2 = bf[pos2] & bit2;

    unsigned const maybePresent = (present1 >> shift1) & (present2 >> shift2);
    present1 &= -maybePresent;
    present2 &= -maybePresent;

    // Write presence
    bf[pos1 + 32] |= (Filter)present1;
    bf[pos2 + 32] |= (Filter)present2;
    bf[pos1] |= (Filter)bit1;
    bf[pos2] |= (Filter)bit2;

    return (int)maybePresent;
}


static Filter* Filter_reduce(Filter* bf, int bflog)
{
    assert(6 < bflog && bflog < 64);
    size_t const nbLines = (size_t)1 << (bflog - 6);
    for (size_t lineNb = 0; lineNb < nbLines; lineNb++) {
        memcpy(bf + lineNb*32, bf + lineNb*64 + 32, 32);
    }
    return realloc(bf, nbLines << 5);
}

/*
 * Check if provided 64-bit hash is a collision candidate
 * Requires both bit positions to be set.
 * return >0 if hash is a collision candidate
 *         0 otherwise (slot unoccupied, or only one candidate)
 * note: unoccupied slots should not happen in this algorithm,
 *       since all hashes are supposed to have been inserted at least once.
 */
static inline int Filter_check(const Filter* bf, int bflog, uint64_t hash)
{
    hash = avalanche64(hash);
    unsigned const slot1 = hash & 255;
    hash >>= 8;
    unsigned const slot2 = hash & 255;
    hash >>= 8;

    size_t const fclmask = ((size_t)1 << (bflog-6)) - 1;
    size_t const lineNb = (size_t)hash & fclmask;

    size_t const pos1 = (lineNb << 5) + (slot1 >> 3);
    unsigned const shift1 = slot1 & 7;
    unsigned const present1 = (bf[pos1] >> shift1) & 1;

    size_t const pos2 = (lineNb << 5) + (slot2 >> 3);
    unsigned const shift2 = slot2 & 7;
    unsigned const present2 = (bf[pos2] >> shift2) & 1;

    return (int)(present1 & present2);
}

#endif // FILTER_1_PROBE


/* ===  Display  === */

#include <time.h>   /* clock_t, clock, time_t, time, difftime */

void update_indicator(uint64_t v, uint64_t total)
{
    static clock_t start = 0;
    if (start==0) start = clock();
    clock_t const updateRate = CLOCKS_PER_SEC / 2;

    clock_t const clockSpan = (clock_t)(clock() - start);
    if (clockSpan > updateRate) {
        start = clock();
        assert(v <= total);
        assert(total > 0);
        double share = ((double)v / (double)total) * 100;
        printf("%6.2f%% (%llu)  \r", share, (unsigned long long)v);
        fflush(NULL);
    }
}

/* note: not thread safe */
const char* displayDelay(double delay_s)
{
    static char delayString[50];
    memset(delayString, 0, sizeof(delayString));

    int const mn = ((int)delay_s / 60) % 60;
    int const h = (int)delay_s / 3600;
    int const sec = (int)delay_s % 60;

    char* p = delayString;
    if (h) sprintf(p, "%i h ", h);
    if (mn || h) {
        p = delayString + strlen(delayString);
        sprintf(p, "%i mn ", mn);
    }
    p = delayString + strlen(delayString);
    sprintf(p, "%is ", sec);

    return delayString;
}


/* ===  Math  === */

static double power(uint64_t base, int p)
{
    double value = 1;
    assert(p>=0);
    for (int i=0; i<p; i++) {
        value *= (double)base;
    }
    return value;
}

static double estimateNbCollisions(uint64_t nbH, int nbBits)
{
    return ((double)nbH * (double)(nbH-1)) / power(2, nbBits+1);
}

static int highestBitSet(uint64_t v)
{
    assert(v!=0);
    int bitId = 0;
    while (v >>= 1) bitId++;
    return bitId;
}


/* ===  Filter and search collisions  === */

#undef NDEBUG   /* ensure assert is not disabled */
#include <assert.h>

/* will recommend 24 billion samples for 64-bit hashes,
 * expecting 18 collisions for a good 64-bit hash */
#define NB_BITS_MAX 64   /* can't store nor analyze hash wider than 64-bits for the time being */
uint64_t select_nbh(int nbBits)
{
    assert(nbBits > 0);
    if (nbBits > NB_BITS_MAX) nbBits = NB_BITS_MAX;
    double targetColls = (double)((128 + 17) - (nbBits * 2));
    uint64_t nbH = 24;
    while (estimateNbCollisions(nbH, nbBits) < targetColls) nbH *= 2;
    return nbH;
}


typedef struct {
    uint64_t nbCollisions;
} searchCollisions_results;

typedef struct {
    uint64_t nbH;
    uint64_t mask;
    uint64_t maskSelector;
    size_t sampleSize;
    uint64_t prngSeed;
    int filterLog;      /* <0 = disable filter;  0 = auto-size; */
    int hashID;
    int display;
    int nbThreads;
    searchCollisions_results* resultPtr;
} searchCollisions_parameters;

#define DISPLAY(...) { if (display) printf(__VA_ARGS__); }

static int isEqual(void* hTablePtr, size_t index1, size_t index2, Htype_e htype)
{
    if ((htype == ht64) || (htype == ht32)) {
        uint64_t const h1 = ((const uint64_t*)hTablePtr)[index1];
        uint64_t const h2 = ((const uint64_t*)hTablePtr)[index2];
        return (h1 == h2);
    } else {
        assert(htype == ht128);
        XXH128_hash_t const h1 = ((const XXH128_hash_t*)hTablePtr)[index1];
        XXH128_hash_t const h2 = ((const XXH128_hash_t*)hTablePtr)[index2];
        return XXH128_isEqual(h1, h2);
    }
}

static int isHighEqual(void* hTablePtr, size_t index1, size_t index2, Htype_e htype, int rShift)
{
    uint64_t h1, h2;
    if ((htype == ht64) || (htype == ht32)) {
        h1 = ((const uint64_t*)hTablePtr)[index1];
        h2 = ((const uint64_t*)hTablePtr)[index2];
    } else {
        assert(htype == ht128);
        h1 = ((const XXH128_hash_t*)hTablePtr)[index1].high64;
        h2 = ((const XXH128_hash_t*)hTablePtr)[index2].high64;
        assert(rShift >= 64);
        rShift -= 64;
    }
    assert(0 <= rShift && rShift < 64);
    return (h1 >> rShift) == (h2 >> rShift);
}

/* assumption: (htype*)hTablePtr[index] is valid */
static void addHashCandidate(void* hTablePtr, UniHash h, Htype_e htype, size_t index)
{
    if ((htype == ht64) || (htype == ht32)) {
        ((uint64_t*)hTablePtr)[index] = h.h64;
    } else {
        assert(htype == ht128);
        ((XXH128_hash_t*)hTablePtr)[index] = h.h128;
    }
}

static int getNbBits_fromHtype(Htype_e htype) {
    switch(htype) {
        case ht32: return 32;
        case ht64: return 64;
        case ht128:return 128;
        default: EXIT("hash size not supported");
    }
}

static Htype_e getHtype_fromHbits(int nbBits) {
    switch(nbBits) {
        case 32 : return ht32;
        case 64 : return ht64;
        case 128: return ht128;
        default: EXIT("hash size not supported");
    }
}

static size_t search_collisions(
    searchCollisions_parameters param)
{
    uint64_t totalH = param.nbH;
    const uint64_t hMask = param.mask;
    const uint64_t hSelector = param.maskSelector;
    int bflog = param.filterLog;
    const int filter = (param.filterLog >= 0);
    const size_t sampleSize = param.sampleSize;
    const int hashID = param.hashID;
    const Htype_e htype = getHtype_fromHbits(hashfnTable[hashID].bits);
    const int display = param.display;
    /* init */
    sampleFactory* const sf = create_sampleFactory(sampleSize, totalH, param.prngSeed);
    if (!sf) EXIT("not enough memory");

    //const char* const hname = hashfnTable[hashID].name;
    hashfn const hfunction = hashfnTable[hashID].fn;
    int const hwidth = hashfnTable[hashID].bits;
    if (totalH == 0) totalH = select_nbh(hwidth);
    if (bflog == 0) bflog = highestBitSet(totalH) + 1;   /* auto-size filter */
    uint64_t const bfsize = (1ULL << bflog);


    /* ===  filter hashes (optional)  === */

    Filter* bf = NULL;
    uint64_t maxNbH = totalH;

    if (filter) {
        time_t const filterTBegin = time(NULL);
        DISPLAY(" Creating filter (%i GB) \n", (int)(bfsize >> 30));
        bf = create_Filter(bflog);
        if (!bf) EXIT("not enough memory for filter");

        DISPLAY(" Generate %llu hashes from samples of %u bytes \n",
                (unsigned long long)totalH, (unsigned)sampleSize);
        maxNbH = 0;

        for (uint64_t n=0; n < totalH; n++) {
            if (display && ((n&0xFFFFF) == 1) )
                update_indicator(n, totalH);
            update_sampleFactory(sf);

            UniHash const h = hfunction(sf->buffer, sampleSize);
            if ((h.h64 & hMask) != hSelector) continue;

            maxNbH += 2 * (uint64_t)Filter_insert(bf, bflog, h.h64);
        }

        if (maxNbH==0) {
            DISPLAY(" Analysis completed: No collision detected \n");
            if (param.resultPtr) param.resultPtr->nbCollisions = 0;
            free_Filter(bf);
            free_sampleFactory(sf);
            return 0;
        }

        {   double const filterDelay = difftime(time(NULL), filterTBegin);
            DISPLAY(" Generation and filter completed in %s, detected up to %llu candidates \n",
                    displayDelay(filterDelay), (unsigned long long) maxNbH);
        }

        bf = Filter_reduce(bf, bflog);
        if (bf == NULL) EXIT("filter reduction failed");
    }

    /* === store hash candidates: duplicates will be present here === */

    time_t const storeTBegin = time(NULL);
    size_t const hashByteSize = (htype == ht128) ? 16 : 8;
    size_t const tableSize = (size_t)((maxNbH+1) * hashByteSize);
    assert(tableSize > maxNbH);  /* check tableSize calculation overflow */
    DISPLAY(" Storing hash candidates (%i MB) \n", (int)(tableSize >> 20));

    /* Generate and store hashes */
    void* const hashCandidates = malloc(tableSize);
    if (!hashCandidates) EXIT("not enough memory to store candidates");
    init_sampleFactory(sf, totalH);
    size_t nbCandidates = 0;
    for (uint64_t n=0; n < totalH; n++) {
        if (display && ((n&0xFFFFF) == 1) ) update_indicator(n, totalH);
        update_sampleFactory(sf);

        UniHash const h = hfunction(sf->buffer, sampleSize);
        if ((h.h64 & hMask) != hSelector) continue;

        if (filter) {
            if (Filter_check(bf, bflog, h.h64)) {
                //printf("found %zu candidates / %llu generated \n", nbCandidates, n);
                assert(nbCandidates < maxNbH);
                addHashCandidate(hashCandidates, h, htype, nbCandidates++);
            }
        } else {
            assert(nbCandidates < maxNbH);
            addHashCandidate(hashCandidates, h, htype, nbCandidates++);
        }
    }
    if (nbCandidates < maxNbH) {
        /* Try to mitigate gnuc_quicksort behavior, by reducing allocated memory,
         * since gnuc_quicksort uses a lot of additional memory for mergesort */
        void* const checkPtr = realloc(hashCandidates, nbCandidates * hashByteSize);
        assert(checkPtr != NULL);
        assert(checkPtr == hashCandidates);  /* simplification: since we are reducing the size,
                                              * we hope to keep the same ptr position.
                                              * Otherwise, hashCandidates must be mutable. */
        DISPLAY(" List of hashes reduced to %u MB from %u MB (saved %u MB) \n",
                (unsigned)((nbCandidates * hashByteSize) >> 20),
                (unsigned)(tableSize >> 20),
                (unsigned)((tableSize - (nbCandidates * hashByteSize)) >> 20) );
    }
    double const storeTDelay = difftime(time(NULL), storeTBegin);
    DISPLAY(" Stored %llu hash candidates in %s \n",
            (unsigned long long) nbCandidates, displayDelay(storeTDelay));
    free_Filter(bf);
    free_sampleFactory(sf);


    /* === step 3: look for duplicates === */
    time_t const sortTBegin = time(NULL);
    DISPLAY(" Sorting candidates... ");
    fflush(NULL);
    if ((htype == ht64) || (htype == ht32)) {
        /*
         * Use C++'s std::sort, as it's faster than C stdlib's qsort, and
         * doesn't suffer from gnuc_libsort's memory expansion
         */
        sort64(hashCandidates, nbCandidates);
    } else {
        assert(htype == ht128);
        sort128(hashCandidates, nbCandidates); /* sort with custom comparator */
    }
    double const sortTDelay = difftime(time(NULL), sortTBegin);
    DISPLAY(" Completed in %s \n", displayDelay(sortTDelay));

    /* scan and count duplicates */
    time_t const countBegin = time(NULL);
    DISPLAY(" Looking for duplicates: ");
    fflush(NULL);
    size_t collisions = 0;
    for (size_t n=1; n<nbCandidates; n++) {
        if (isEqual(hashCandidates, n, n-1, htype)) {
#if defined(COL_DISPLAY_DUPLICATES)
            printf("collision: ");
            printHash(hashCandidates, n, htype);
            printf(" / ");
            printHash(hashCandidates, n-1, htype);
            printf(" \n");
#endif
            collisions++;
    }   }

    if (!filter /* all candidates */ && display /*single thead*/ ) {
        /* check partial bitfields (high bits) */
        DISPLAY(" \n");
        int const hashBits = getNbBits_fromHtype(htype);
        double worstRatio = 0.;
        int worstNbHBits = 0;
        for (int nbHBits = 1; nbHBits < hashBits; nbHBits++) {
            uint64_t const nbSlots = (uint64_t)1 << nbHBits;
            double const expectedCollisions = estimateNbCollisions(nbCandidates, nbHBits);
            if ( (nbSlots > nbCandidates * 100)  /* within range for meaningful collision analysis results */
              && (expectedCollisions > 18.0) ) {
                int const rShift = hashBits - nbHBits;
                size_t HBits_collisions = 0;
                for (size_t n=1; n<nbCandidates; n++) {
                    if (isHighEqual(hashCandidates, n, n-1, htype, rShift)) {
                        HBits_collisions++;
                }   }
                double const collisionRatio = (double)HBits_collisions / expectedCollisions;
                if (collisionRatio > 2.0) DISPLAY("WARNING !!!  ===> ");
                DISPLAY(" high %i bits: %zu collision (%.1f expected): x%.2f \n",
                        nbHBits, HBits_collisions, expectedCollisions, collisionRatio);
                if (collisionRatio > worstRatio) {
                    worstNbHBits = nbHBits;
                    worstRatio = collisionRatio;
        }   }   }
        DISPLAY("Worst collision ratio at %i high bits: x%.2f \n",
                worstNbHBits, worstRatio);
    }
    double const countDelay = difftime(time(NULL), countBegin);
    DISPLAY(" Completed in %s \n", displayDelay(countDelay));

    /* clean and exit */
    free (hashCandidates);

#if 0  /* debug */
    for (size_t n=0; n<nbCandidates; n++)
        printf("0x%016llx \n", (unsigned long long)hashCandidates[n]);
#endif

    if (param.resultPtr) param.resultPtr->nbCollisions = collisions;
    return collisions;
}



#if defined(__MACH__) || defined(__linux__)

#include <sys/resource.h>
static size_t getProcessMemUsage(int children)
{
    struct rusage stats;
    if (getrusage(children ? RUSAGE_CHILDREN : RUSAGE_SELF, &stats) == 0)
      return (size_t)stats.ru_maxrss;
    return 0;
}

#else
static size_t getProcessMemUsage(int ignore) { (void)ignore; return 0; }
#endif

void time_collisions(searchCollisions_parameters param)
{
    uint64_t totalH = param.nbH;
    int hashID = param.hashID;
    int display = param.display;

    /* init */
    assert(0 <= hashID && hashID < HASH_FN_TOTAL);
    //const char* const hname = hashfnTable[hashID].name;
    int const hwidth = hashfnTable[hashID].bits;
    if (totalH == 0) totalH = select_nbh(hwidth);
    double const targetColls = estimateNbCollisions(totalH, hwidth);

    /* Start the timer to measure start/end of hashing + collision detection. */
    time_t const programTBegin = time(NULL);

    /* Generate hashes, and count collisions */
    size_t const collisions = search_collisions(param);

    /* display results */
    double const programTDelay = difftime(time(NULL), programTBegin);
    size_t const programBytesSelf = getProcessMemUsage(0);
    size_t const programBytesChildren = getProcessMemUsage(1);
    DISPLAY("\n\n");
    DISPLAY("===>   Found %llu collisions (x%.2f, %.1f expected) in %s\n",
            (unsigned long long)collisions,
            (double)collisions / targetColls,
            targetColls,
            displayDelay(programTDelay));
    if (programBytesSelf)
      DISPLAY("===>   MaxRSS(self) %zuMB, MaxRSS(children) %zuMB\n",
              programBytesSelf>>20,
              programBytesChildren>>20);
    DISPLAY("------------------------------------------ \n");
}

// wrapper for pthread interface
void MT_searchCollisions(void* payload)
{
    search_collisions(*(searchCollisions_parameters*)payload);
}

/* ===  Command Line  === */

/*!
 * readU64FromChar():
 * Allows and interprets K, KB, KiB, M, MB and MiB suffix.
 * Will also modify `*stringPtr`, advancing it to the position where it stopped reading.
 */
static uint64_t readU64FromChar(const char** stringPtr)
{
    static uint64_t const max = (((uint64_t)(-1)) / 10) - 1;
    uint64_t result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9')) {
        assert(result < max);
        result *= 10;
        result += (unsigned)(**stringPtr - '0');
        (*stringPtr)++ ;
    }
    if ((**stringPtr=='K') || (**stringPtr=='M') || (**stringPtr=='G')) {
        uint64_t const maxK = ((uint64_t)(-1)) >> 10;
        assert(result < maxK);
        result <<= 10;
        if ((**stringPtr=='M') || (**stringPtr=='G')) {
            assert(result < maxK);
            result <<= 10;
            if (**stringPtr=='G') {
                assert(result < maxK);
                result <<= 10;
            }
        }
        (*stringPtr)++;  /* skip `K` or `M` */
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    return result;
}


/**
 * longCommandWArg():
 * Checks if *stringPtr is the same as longCommand.
 * If yes, @return 1 and advances *stringPtr to the position which immediately follows longCommand.
 * @return 0 and doesn't modify *stringPtr otherwise.
 */
static int longCommandWArg(const char** stringPtr, const char* longCommand)
{
    assert(longCommand); assert(stringPtr); assert(*stringPtr);
    size_t const comSize = strlen(longCommand);
    int const result = !strncmp(*stringPtr, longCommand, comSize);
    if (result) *stringPtr += comSize;
    return result;
}


#include "pool.h"

/*
 * As some hashes use different algorithms depending on input size,
 * it can be necessary to test multiple input sizes
 * to paint an accurate picture of collision performance
 */
#define SAMPLE_SIZE_DEFAULT 256
#define HASHFN_ID_DEFAULT 0

void help(const char* exeName)
{
    printf("usage: %s [hashName] [opt] \n\n", exeName);
    printf("list of hashNames:");
    printf("%s ", hashfnTable[0].name);
    for (int i=1; i < HASH_FN_TOTAL; i++) {
        printf(", %s ", hashfnTable[i].name);
    }
    printf(" \n");
    printf("Default hashName is %s\n", hashfnTable[HASHFN_ID_DEFAULT].name);

    printf(" \n");
    printf("Optional parameters: \n");
    printf("  --nbh=NB       Select nb of hashes to generate (%llu by default) \n", (unsigned long long)select_nbh(64));
    printf("  --filter       Activates the filter. Slower, but reduces memory usage for the same nb of hashes.\n");
    printf("  --threadlog=NB Use 2^NB threads.\n");
    printf("  --len=MB       Set length of the input (%i bytes by default) \n", SAMPLE_SIZE_DEFAULT);
}

int bad_argument(const char* exeName)
{
    printf("incorrect command: \n");
    help(exeName);
    return 1;
}


int main(int argc, const char** argv)
{
    printf(" *** Collision tester for 64+ bit hashes ***  \n\n");

    assert(argc > 0);
    const char* const exeName = argv[0];
    uint64_t totalH = 0;  /* auto, based on nbBits */
    int bflog = 0;    /* auto */
    int filter = 0;   /* disabled */
    size_t sampleSize = SAMPLE_SIZE_DEFAULT;
    int hashID = HASHFN_ID_DEFAULT;
    int threadlog = 0;
    uint64_t prngSeed = 0;

    int arg_nb;
    for (arg_nb = 1; arg_nb < argc; arg_nb++) {
        const char** arg = argv + arg_nb;

        if (!strcmp(*arg, "-h")) { help(exeName); return 0; }
        if (longCommandWArg(arg, "-T")) { threadlog = (int)readU64FromChar(arg); continue; }

        if (!strcmp(*arg, "--filter"))    { filter=1; continue; }
        if (!strcmp(*arg, "--no-filter")) { filter=0; continue; }

        if (longCommandWArg(arg, "--seed")) { prngSeed = readU64FromChar(arg); continue; }
        if (longCommandWArg(arg, "--nbh=")) { totalH = readU64FromChar(arg); continue; }
        if (longCommandWArg(arg, "--filter=")) { filter=1; bflog = (int)readU64FromChar(arg); assert(bflog < 64); continue; }
        if (longCommandWArg(arg, "--filterlog=")) { filter=1; bflog = (int)readU64FromChar(arg); assert(bflog < 64); continue; }
        if (longCommandWArg(arg, "--size=")) { sampleSize = (size_t)readU64FromChar(arg); continue; }
        if (longCommandWArg(arg, "--len=")) { sampleSize = (size_t)readU64FromChar(arg); continue; }
        if (longCommandWArg(arg, "--threadlog=")) { threadlog = (int)readU64FromChar(arg); continue; }

        /* argument understood as hash name (must be correct) */
        int hnb;
        for (hnb=0; hnb < HASH_FN_TOTAL; hnb++) {
            if (!strcmp(*arg, hashfnTable[hnb].name)) { hashID = hnb; break; }
        }
        if (hnb == HASH_FN_TOTAL) return bad_argument(exeName);
    }

    /* init */
    const char* const hname = hashfnTable[hashID].name;
    int const hwidth = hashfnTable[hashID].bits;
    if (totalH == 0) totalH = select_nbh(hwidth);
    double const targetColls = estimateNbCollisions(totalH, hwidth);
    if (bflog == 0) bflog = highestBitSet(totalH) + 1;   /* auto-size filter */
    if (!filter) bflog = -1; // disable filter

    if (sizeof(size_t) < 8) {
        printf("warning: in 32-bit mode, the program is more likely going to lack address space \n");
    }

    printf("Testing %s algorithm (%i-bit) \n", hname, hwidth);
    printf("This program will allocate a lot of memory,\n");
    printf("generate %llu %i-bit hashes from samples of %u bytes, \n",
            (unsigned long long)totalH, hwidth, (unsigned)sampleSize);
    printf("and attempt to produce %.0f collisions. \n\n", targetColls);

    int const nbThreads = 1 << threadlog;
    if (nbThreads <= 0) EXIT("Invalid --threadlog value.");

    if (nbThreads == 1) {

        searchCollisions_parameters params;
        params.nbH = totalH;
        params.mask = 0;
        params.maskSelector = 0;
        params.sampleSize = sampleSize;
        params.filterLog = bflog;
        params.hashID = hashID;
        params.display = 1;
        params.resultPtr = NULL;
        params.prngSeed = prngSeed;
        params.nbThreads = 1;
        time_collisions(params);

    } else { /*  nbThreads > 1 */

        /* use multithreading */
        if (threadlog >= 30) EXIT("too many threads requested");
        if ((uint64_t)nbThreads > (totalH >> 16))
            EXIT("too many threads requested");
        if (bflog > 0 && threadlog > (bflog-10))
            EXIT("too many threads requested");
        printf("using %i threads ... \n", nbThreads);

        /* allocation */
        time_t const programTBegin = time(NULL);
        POOL_ctx* const pt = POOL_create((size_t)nbThreads, 1);
        if (!pt) EXIT("not enough memory for threads");
        searchCollisions_results* const MTresults = calloc ((size_t)nbThreads, sizeof(searchCollisions_results));
        if (!MTresults) EXIT("not enough memory");
        searchCollisions_parameters* const MTparams = calloc ((size_t)nbThreads, sizeof(searchCollisions_parameters));
        if (!MTparams) EXIT("not enough memory");

        /* distribute jobs */
        for (int tnb=0; tnb<nbThreads; tnb++) {
            MTparams[tnb].nbH = totalH;
            MTparams[tnb].mask = (uint64_t)nbThreads - 1;
            MTparams[tnb].sampleSize = sampleSize;
            MTparams[tnb].filterLog = bflog ? bflog - threadlog : 0;
            MTparams[tnb].hashID = hashID;
            MTparams[tnb].display = 0;
            MTparams[tnb].resultPtr = MTresults+tnb;
            MTparams[tnb].prngSeed = prngSeed;
            MTparams[tnb].maskSelector = (uint64_t)tnb;
            POOL_add(pt, MT_searchCollisions, MTparams + tnb);
        }
        POOL_free(pt);  /* actually joins and free */

        /* Gather results */
        uint64_t nbCollisions=0;
        for (int tnb=0; tnb<nbThreads; tnb++) {
            nbCollisions += MTresults[tnb].nbCollisions;
        }

        double const programTDelay = difftime(time(NULL), programTBegin);
        size_t const programBytesSelf = getProcessMemUsage(0);
        size_t const programBytesChildren = getProcessMemUsage(1);
        printf("\n\n");
        printf("===>   Found %llu collisions (x%.2f, %.1f expected) in %s\n",
                (unsigned long long)nbCollisions,
                (double)nbCollisions / targetColls,
                targetColls,
                displayDelay(programTDelay));
        if (programBytesSelf)
          printf("===>   MaxRSS(self) %zuMB, MaxRSS(children) %zuMB\n",
                 programBytesSelf>>20,
                 programBytesChildren>>20);
        printf("------------------------------------------ \n");

        /* Clean up */
        free(MTparams);
        free(MTresults);
    }

    return 0;
}
