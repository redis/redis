/*
 * xxhsum - Command line interface for xxhash algorithms
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

#include "xsum_sanity_check.h"
#include "xsum_output.h"  /* XSUM_log */
#ifndef XXH_STATIC_LINKING_ONLY
#  define XXH_STATIC_LINKING_ONLY
#endif
#include "../xxhash.h"

#include <stdlib.h>  /* exit */
#include <assert.h>
#include <string.h>  /* memcmp */

/* use #define to make them constant, required for initialization */
#define PRIME32 2654435761U
#define PRIME64 11400714785074694797ULL

/*
 * Fills a test buffer with pseudorandom data.
 *
 * This is used in the sanity check - its values must not be changed.
 */
XSUM_API void XSUM_fillTestBuffer(XSUM_U8* buffer, size_t len)
{
    XSUM_U64 byteGen = PRIME32;
    size_t i;

    assert(buffer != NULL);

    for (i=0; i<len; i++) {
        buffer[i] = (XSUM_U8)(byteGen>>56);
        byteGen *= PRIME64;
    }
}



/* ************************************************
 * Self-test:
 * ensure results consistency across platforms
 *********************************************** */
#if XSUM_NO_TESTS
XSUM_API void XSUM_sanityCheck(void)
{
    XSUM_log("This version of xxhsum is not verified.\n");
}
#else

/*
 * Test data vectors
 */
typedef struct {
    XSUM_U32 len;
    XSUM_U32 seed;
    XSUM_U32 Nresult;
} XSUM_testdata32_t;

typedef struct {
    XSUM_U32 len;
    XSUM_U64 seed;
    XSUM_U64 Nresult;
} XSUM_testdata64_t;

typedef struct {
    XSUM_U32 len;
    XSUM_U64 seed;
    XXH128_hash_t Nresult;
} XSUM_testdata128_t;

#define SECRET_SAMPLE_NBBYTES 5
typedef struct {
    XSUM_U32 seedLen;
    XSUM_U32 secretLen;
    XSUM_U8 byte[SECRET_SAMPLE_NBBYTES];
} XSUM_testdata_sample_t;

/* XXH32 */
static const XSUM_testdata32_t XSUM_XXH32_testdata[] = {
    {   0,       0, 0x02CC5D05U },
    {   0, PRIME32, 0x36B78AE7U },
    {   1,       0, 0xCF65B03EU },
    {   1, PRIME32, 0xB4545AA4U },
    {  14,       0, 0x1208E7E2U },
    {  14, PRIME32, 0x6AF1D1FEU },
    { 222,       0, 0x5BD11DBDU },
    { 222, PRIME32, 0x58803C5FU }
};

/* XXH64 */
static const XSUM_testdata64_t XSUM_XXH64_testdata[] = {
    {   0,       0, 0xEF46DB3751D8E999ULL },
    {   0, PRIME32, 0xAC75FDA2929B17EFULL },
    {   1,       0, 0xE934A84ADB052768ULL },
    {   1, PRIME32, 0x5014607643A9B4C3ULL },
    {   4,       0, 0x9136A0DCA57457EEULL },
    {  14,       0, 0x8282DCC4994E35C8ULL },
    {  14, PRIME32, 0xC3BD6BF63DEB6DF0ULL },
    { 222,       0, 0xB641AE8CB691C174ULL },
    { 222, PRIME32, 0x20CB8AB7AE10C14AULL }
};
/*
 * XXH3:
 * Due to being a more complex hash function with specializations for certain
 * lengths, a more extensive test is used for XXH3.
 */

/* XXH3_64bits, seeded */
static const XSUM_testdata64_t XSUM_XXH3_testdata[] = {
    {    0,       0, 0x2D06800538D394C2ULL },  /* empty string */
    {    0, PRIME64, 0xA8A6B918B2F0364AULL },
    {    1,       0, 0xC44BDFF4074EECDBULL },  /*  1 -  3 */
    {    1, PRIME64, 0x032BE332DD766EF8ULL },
    {    6,       0, 0x27B56A84CD2D7325ULL },  /*  4 -  8 */
    {    6, PRIME64, 0x84589C116AB59AB9ULL },
    {   12,       0, 0xA713DAF0DFBB77E7ULL },  /*  9 - 16 */
    {   12, PRIME64, 0xE7303E1B2336DE0EULL },
    {   24,       0, 0xA3FE70BF9D3510EBULL },  /* 17 - 32 */
    {   24, PRIME64, 0x850E80FC35BDD690ULL },
    {   48,       0, 0x397DA259ECBA1F11ULL },  /* 33 - 64 */
    {   48, PRIME64, 0xADC2CBAA44ACC616ULL },
    {   80,       0, 0xBCDEFBBB2C47C90AULL },  /* 65 - 96 */
    {   80, PRIME64, 0xC6DD0CB699532E73ULL },
    {  195,       0, 0xCD94217EE362EC3AULL },  /* 129-240 */
    {  195, PRIME64, 0xBA68003D370CB3D9ULL },

    {  403,       0, 0xCDEB804D65C6DEA4ULL },  /* one block, last stripe is overlapping */
    {  403, PRIME64, 0x6259F6ECFD6443FDULL },
    {  512,       0, 0x617E49599013CB6BULL },  /* one block, finishing at stripe boundary */
    {  512, PRIME64, 0x3CE457DE14C27708ULL },
    { 2048,       0, 0xDD59E2C3A5F038E0ULL },  /* 2 blocks, finishing at block boundary */
    { 2048, PRIME64, 0x66F81670669ABABCULL },
    { 2099,       0, 0xC6B9D9B3FC9AC765ULL },  /* 2 blocks + 1 partial block, to detect off-by-one scrambling issues, like #816 */
    { 2099, PRIME64, 0x184F316843663974ULL },
    { 2240,       0, 0x6E73A90539CF2948ULL },  /* 3 blocks, finishing at stripe boundary */
    { 2240, PRIME64, 0x757BA8487D1B5247ULL },
    { 2367,       0, 0xCB37AEB9E5D361EDULL },  /* 3 blocks, last stripe is overlapping */
    { 2367, PRIME64, 0xD2DB3415B942B42AULL }
};
/* XXH3_64bits, custom secret */
static const XSUM_testdata64_t XSUM_XXH3_withSecret_testdata[] = {
    {       0, 0, 0x3559D64878C5C66CULL },  /* empty string */
    {       1, 0, 0x8A52451418B2DA4DULL },  /*  1 -  3 */
    {       6, 0, 0x82C90AB0519369ADULL },  /*  4 -  8 */
    {      12, 0, 0x14631E773B78EC57ULL },  /*  9 - 16 */
    {      24, 0, 0xCDD5542E4A9D9FE8ULL },  /* 17 - 32 */
    {      48, 0, 0x33ABD54D094B2534ULL },  /* 33 - 64 */
    {      80, 0, 0xE687BA1684965297ULL },  /* 65 - 96 */
    {     195, 0, 0xA057273F5EECFB20ULL },  /* 129-240 */

    {     403, 0, 0x14546019124D43B8ULL },  /* one block, last stripe is overlapping */
    {     512, 0, 0x7564693DD526E28DULL },  /* one block, finishing at stripe boundary */
    {    2048, 0, 0xD32E975821D6519FULL },  /* >= 2 blodcks, at least one scrambling */
    {    2367, 0, 0x293FA8E5173BB5E7ULL },  /* >= 2 blocks, at least one scrambling, last stripe unaligned */

    { 64*10*3, 0, 0x751D2EC54BC6038BULL }   /* exactly 3 full blocks, not a multiple of 256 */
};
/* XXH3_128bits, seeded */
static const XSUM_testdata128_t XSUM_XXH128_testdata[] = {
    {    0,       0, { 0x6001C324468D497FULL, 0x99AA06D3014798D8ULL } },  /* empty string */
    {    0, PRIME32, { 0x5444F7869C671AB0ULL, 0x92220AE55E14AB50ULL } },
    {    1,       0, { 0xC44BDFF4074EECDBULL, 0xA6CD5E9392000F6AULL } },  /*  1 -  3 */
    {    1, PRIME32, { 0xB53D5557E7F76F8DULL, 0x89B99554BA22467CULL } },
    {    6,       0, { 0x3E7039BDDA43CFC6ULL, 0x082AFE0B8162D12AULL } },  /*  4 -  8 */
    {    6, PRIME32, { 0x269D8F70BE98856EULL, 0x5A865B5389ABD2B1ULL } },
    {   12,       0, { 0x061A192713F69AD9ULL, 0x6E3EFD8FC7802B18ULL } },  /*  9 - 16 */
    {   12, PRIME32, { 0x9BE9F9A67F3C7DFBULL, 0xD7E09D518A3405D3ULL } },
    {   24,       0, { 0x1E7044D28B1B901DULL, 0x0CE966E4678D3761ULL } },  /* 17 - 32 */
    {   24, PRIME32, { 0xD7304C54EBAD40A9ULL, 0x3162026714A6A243ULL } },
    {   48,       0, { 0xF942219AED80F67BULL, 0xA002AC4E5478227EULL } },  /* 33 - 64 */
    {   48, PRIME32, { 0x7BA3C3E453A1934EULL, 0x163ADDE36C072295ULL } },
    {   81,       0, { 0x5E8BAFB9F95FB803ULL, 0x4952F58181AB0042ULL } },  /* 65 - 96 */
    {   81, PRIME32, { 0x703FBB3D7A5F755CULL, 0x2724EC7ADC750FB6ULL } },
    {  222,       0, { 0xF1AEBD597CEC6B3AULL, 0x337E09641B948717ULL } },  /* 129-240 */
    {  222, PRIME32, { 0xAE995BB8AF917A8DULL, 0x91820016621E97F1ULL } },

    {  403,       0, { 0xCDEB804D65C6DEA4ULL, 0x1B6DE21E332DD73DULL } },  /* one block, last stripe is overlapping */
    {  403, PRIME64, { 0x6259F6ECFD6443FDULL, 0xBED311971E0BE8F2ULL } },
    {  512,       0, { 0x617E49599013CB6BULL, 0x18D2D110DCC9BCA1ULL } },  /* one block, finishing at stripe boundary */
    {  512, PRIME64, { 0x3CE457DE14C27708ULL, 0x925D06B8EC5B8040ULL } },
    { 2048,       0, { 0xDD59E2C3A5F038E0ULL, 0xF736557FD47073A5ULL } },  /* 2 blocks, finishing at block boundary */
    { 2048, PRIME32, { 0x230D43F30206260BULL, 0x7FB03F7E7186C3EAULL } },
    { 2240,       0, { 0x6E73A90539CF2948ULL, 0xCCB134FBFA7CE49DULL } },  /* 3 blocks, finishing at stripe boundary */
    { 2240, PRIME32, { 0xED385111126FBA6FULL, 0x50A1FE17B338995FULL } },
    { 2367,       0, { 0xCB37AEB9E5D361EDULL, 0xE89C0F6FF369B427ULL } },  /* 3 blocks, last stripe is overlapping */
    { 2367, PRIME32, { 0x6F5360AE69C2F406ULL, 0xD23AAE4B76C31ECBULL } }
};

/* XXH128, custom secret */
static const XSUM_testdata128_t XSUM_XXH128_withSecret_testdata[] = {
    {  0, 0, { 0x005923CCEECBE8AEULL, 0x5F70F4EA232F1D38ULL } },  /* empty string */
    {  1, 0, { 0x8A52451418B2DA4DULL, 0x3A66AF5A9819198EULL } },  /*  1 -  3 */
    {  6, 0, { 0x0B61C8ACA7D4778FULL, 0x376BD91B6432F36DULL } },  /*  4 -  8 */
    { 12, 0, { 0xAF82F6EBA263D7D8ULL, 0x90A3C2D839F57D0FULL } }   /*  9 - 16 */
};

#define SECRET_SIZE_MAX 9867
static const XSUM_testdata_sample_t XSUM_XXH3_generateSecret_testdata[] = {
    {                              0, 192, { 0xE7, 0x8C, 0x77, 0x77, 0x00 } },
    {                              1, 240, { 0x2B, 0x3E, 0xDE, 0xC1, 0x00 } },
    {     XXH3_SECRET_SIZE_MIN -   1, 277, { 0xE8, 0x39, 0x6C, 0xCC, 0x7B } },
    { XXH3_SECRET_DEFAULT_SIZE + 500, SECRET_SIZE_MAX, { 0xD6, 0x1C, 0x41, 0x17, 0xB3 } }
};

static void XSUM_checkResult32(XXH32_hash_t r1, XXH32_hash_t r2)
{
    static int nbTests = 1;
    if (r1!=r2) {
        XSUM_log("\rError: 32-bit hash test %i: Internal sanity check failed!\n", nbTests);
        XSUM_log("\rGot 0x%08X, expected 0x%08X.\n", (unsigned)r1, (unsigned)r2);
        XSUM_log("\rNote: If you modified the hash functions, make sure to either update the values\n"
                  "or temporarily recompile with XSUM_NO_TESTS=1.\n");
        exit(1);
    }
    nbTests++;
}

static void XSUM_checkResult64(XXH64_hash_t r1, XXH64_hash_t r2)
{
    static int nbTests = 1;
    if (r1!=r2) {
        XSUM_log("\rError: 64-bit hash test %i: Internal sanity check failed!\n", nbTests);
        XSUM_log("\rGot 0x%08X%08XULL, expected 0x%08X%08XULL.\n",
                (unsigned)(r1>>32), (unsigned)r1, (unsigned)(r2>>32), (unsigned)r2);
        XSUM_log("\rNote: If you modified the hash functions, make sure to either update the values\n"
                  "or temporarily recompile with XSUM_NO_TESTS=1.\n");
        exit(1);
    }
    nbTests++;
}

static void XSUM_checkResult128(XXH128_hash_t r1, XXH128_hash_t r2)
{
    static int nbTests = 1;
    if ((r1.low64 != r2.low64) || (r1.high64 != r2.high64)) {
        XSUM_log("\rError: 128-bit hash test %i: Internal sanity check failed.\n", nbTests);
        XSUM_log("\rGot { 0x%08X%08XULL, 0x%08X%08XULL }, expected { 0x%08X%08XULL, 0x%08X%08XULL } \n",
                (unsigned)(r1.low64>>32), (unsigned)r1.low64, (unsigned)(r1.high64>>32), (unsigned)r1.high64,
                (unsigned)(r2.low64>>32), (unsigned)r2.low64, (unsigned)(r2.high64>>32), (unsigned)r2.high64 );
        XSUM_log("\rNote: If you modified the hash functions, make sure to either update the values\n"
                  "or temporarily recompile with XSUM_NO_TESTS=1.\n");
        exit(1);
    }
    nbTests++;
}


static void XSUM_testXXH32(const void* data, const XSUM_testdata32_t* testData)
{
    XXH32_state_t *state = XXH32_createState();
    size_t pos;

    size_t len = testData->len;
    XSUM_U32 seed = testData->seed;
    XSUM_U32 Nresult = testData->Nresult;

    if (len == 0) {
        data = NULL;
    } else {
        assert(data != NULL);
    }

    assert(state != NULL);

    XSUM_checkResult32(XXH32(data, len, seed), Nresult);

    (void)XXH32_reset(state, seed);
    (void)XXH32_update(state, data, len);
    XSUM_checkResult32(XXH32_digest(state), Nresult);

    (void)XXH32_reset(state, seed);
    for (pos=0; pos<len; pos++)
        (void)XXH32_update(state, ((const char*)data)+pos, 1);
    XSUM_checkResult32(XXH32_digest(state), Nresult);
    XXH32_freeState(state);
}

static void XSUM_testXXH64(const void* data, const XSUM_testdata64_t* testData)
{
    XXH64_state_t *state = XXH64_createState();
    size_t pos;

    size_t len = (size_t)testData->len;
    XSUM_U64 seed = testData->seed;
    XSUM_U64 Nresult = testData->Nresult;

    if (len == 0) {
        data = NULL;
    } else {
        assert(data != NULL);
    }

    assert(state != NULL);

    XSUM_checkResult64(XXH64(data, len, seed), Nresult);

    (void)XXH64_reset(state, seed);
    (void)XXH64_update(state, data, len);
    XSUM_checkResult64(XXH64_digest(state), Nresult);

    (void)XXH64_reset(state, seed);
    for (pos=0; pos<len; pos++)
        (void)XXH64_update(state, ((const char*)data)+pos, 1);
    XSUM_checkResult64(XXH64_digest(state), Nresult);
    XXH64_freeState(state);
}

/*
 * Used to get "random" (but actually 100% reproducible) lengths for
 * XSUM_XXH3_randomUpdate.
 */
static XSUM_U32 XSUM_rand(void)
{
    static XSUM_U64 seed = PRIME32;
    seed *= PRIME64;
    return (XSUM_U32)(seed >> 40);
}

/*
 * Technically, XXH3_64bits_update is identical to XXH3_128bits_update as of
 * v0.8.0, but we treat them as separate.
 */
typedef XXH_errorcode (*XSUM_XXH3_update_t)(XXH_NOESCAPE XXH3_state_t* state, XXH_NOESCAPE const void* input, size_t length);

/*
 * Runs the passed XXH3_update variant on random lengths. This is to test the
 * more complex logic of the update function, catching bugs like this one:
 *    https://github.com/Cyan4973/xxHash/issues/378
 */
static void XSUM_XXH3_randomUpdate(XXH3_state_t* state, const void* data,
                                   size_t len, XSUM_XXH3_update_t update_fn)
{
    size_t p = 0;
    while (p < len) {
        size_t const modulo = len > 2 ? len : 2;
        size_t l = (size_t)(XSUM_rand()) % modulo;
        if (p + l > len) l = len - p;
        (void)update_fn(state, (const char*)data+p, l);
        p += l;
    }
}

static void XSUM_testXXH3(const void* data, const XSUM_testdata64_t* testData)
{
    size_t len = testData->len;
    XSUM_U64 seed = testData->seed;
    XSUM_U64 Nresult = testData->Nresult;
    if (len == 0) {
        data = NULL;
    } else {
        assert(data != NULL);
    }
    {   XSUM_U64 const Dresult = XXH3_64bits_withSeed(data, len, seed);
        XSUM_checkResult64(Dresult, Nresult);
    }

    /* check that the no-seed variant produces same result as seed==0 */
    if (seed == 0) {
        XSUM_U64 const Dresult = XXH3_64bits(data, len);
        XSUM_checkResult64(Dresult, Nresult);
    }

    /* check that the combination of
     * XXH3_generateSecret_fromSeed() and XXH3_64bits_withSecretandSeed()
     * results in exactly the same hash generation as XXH3_64bits_withSeed() */
    {   char secretBuffer[XXH3_SECRET_DEFAULT_SIZE+1];
        char* const secret = secretBuffer + 1;  /* intentional unalignment */
        XXH3_generateSecret_fromSeed(secret, seed);
        {   XSUM_U64 const Dresult = XXH3_64bits_withSecretandSeed(data, len, secret, XXH3_SECRET_DEFAULT_SIZE, seed);
            XSUM_checkResult64(Dresult, Nresult);
        }

        /* check that XXH3_64bits_withSecretandSeed()
         * results in exactly the same return value as XXH3_64bits_withSeed()
         * when len <= XXH3_MIDSIZE_MAX, whatever the content of @secret */
        memset(secret, 0x99, 9);
        if (len <= XXH3_MIDSIZE_MAX) {
            XSUM_U64 const Dresult = XXH3_64bits_withSecretandSeed(data, len, secret, XXH3_SECRET_DEFAULT_SIZE, seed);
            XSUM_checkResult64(Dresult, Nresult);
        }
    }

    /* streaming API test */
    {   XXH3_state_t* const state = XXH3_createState();
        assert(state != NULL);
        /* single ingestion */
        (void)XXH3_64bits_reset_withSeed(state, seed);
        (void)XXH3_64bits_update(state, data, len);
        XSUM_checkResult64(XXH3_64bits_digest(state), Nresult);

        /* random ingestion */
        (void)XXH3_64bits_reset_withSeed(state, seed);
        XSUM_XXH3_randomUpdate(state, data, len, XXH3_64bits_update);
        XSUM_checkResult64(XXH3_64bits_digest(state), Nresult);

        /* byte by byte ingestion */
        {   size_t pos;
            (void)XXH3_64bits_reset_withSeed(state, seed);
            for (pos=0; pos<len; pos++)
                (void)XXH3_64bits_update(state, ((const char*)data)+pos, 1);
            XSUM_checkResult64(XXH3_64bits_digest(state), Nresult);
        }

        /* check that streaming with a combination of
         * XXH3_generateSecret_fromSeed() and XXH3_64bits_reset_withSecretandSeed()
         * results in exactly the same hash generation as XXH3_64bits_reset_withSeed() */
        {   char secretBuffer[XXH3_SECRET_DEFAULT_SIZE+1];
            char* const secret = secretBuffer + 1;  /* intentional unalignment */
            XXH3_generateSecret_fromSeed(secret, seed);
            /* single ingestion */
            (void)XXH3_64bits_reset_withSecretandSeed(state, secret, XXH3_SECRET_DEFAULT_SIZE, seed);
            (void)XXH3_64bits_update(state, data, len);
            XSUM_checkResult64(XXH3_64bits_digest(state), Nresult);

            /* check that XXH3_64bits_withSecretandSeed()
             * results in exactly the same return value as XXH3_64bits_withSeed()
             * when len <= XXH3_MIDSIZE_MAX, whatever the content of @secret */
            if (len <= XXH3_MIDSIZE_MAX) {
                /* single ingestion */
                memset(secret, 0x99, 9);
                (void)XXH3_64bits_reset_withSecretandSeed(state, secret, XXH3_SECRET_DEFAULT_SIZE, seed);
                (void)XXH3_64bits_update(state, data, len);
                XSUM_checkResult64(XXH3_64bits_digest(state), Nresult);
            }
        }

        XXH3_freeState(state);
    }

}


static void XSUM_testXXH3_withSecret(const void* data, const void* secret,
                                     size_t secretSize, const XSUM_testdata64_t* testData)
{
    size_t len = (size_t)testData->len;
    XSUM_U64 Nresult = testData->Nresult;

    if (len == 0) {
        data = NULL;
    } else {
        assert(data != NULL);
    }
    {   XSUM_U64 const Dresult = XXH3_64bits_withSecret(data, len, secret, secretSize);
        XSUM_checkResult64(Dresult, Nresult);
    }

    /* check that XXH3_64bits_withSecretandSeed()
     * results in exactly the same return value as XXH3_64bits_withSecret() */
    if (len > XXH3_MIDSIZE_MAX)
    {   XSUM_U64 const Dresult = XXH3_64bits_withSecretandSeed(data, len, secret, secretSize, 0);
        XSUM_checkResult64(Dresult, Nresult);
    }

    /* streaming API test */
    {   XXH3_state_t *state = XXH3_createState();
        assert(state != NULL);
        (void)XXH3_64bits_reset_withSecret(state, secret, secretSize);
        (void)XXH3_64bits_update(state, data, len);
        XSUM_checkResult64(XXH3_64bits_digest(state), Nresult);

        /* random ingestion */
        (void)XXH3_64bits_reset_withSecret(state, secret, secretSize);
        XSUM_XXH3_randomUpdate(state, data, len, XXH3_64bits_update);
        XSUM_checkResult64(XXH3_64bits_digest(state), Nresult);

        /* byte by byte ingestion */
        {   size_t pos;
            (void)XXH3_64bits_reset_withSecret(state, secret, secretSize);
            for (pos=0; pos<len; pos++)
                (void)XXH3_64bits_update(state, ((const char*)data)+pos, 1);
            XSUM_checkResult64(XXH3_64bits_digest(state), Nresult);
        }

        /* check that XXH3_64bits_reset_withSecretandSeed()
         * results in exactly the same return value as XXH3_64bits_reset_withSecret()
         * when len > XXH3_MIDSIZE_MAX, whatever the value of @seed */
         if (len > XXH3_MIDSIZE_MAX) {
            /* single ingestion */
            (void)XXH3_64bits_reset_withSecretandSeed(state, secret, secretSize, 17);
            (void)XXH3_64bits_update(state, data, len);
            XSUM_checkResult64(XXH3_64bits_digest(state), Nresult);
        }

        XXH3_freeState(state);
    }
}

static void XSUM_testXXH128(const void* data, const XSUM_testdata128_t* testData)
{
    size_t len = (size_t)testData->len;
    XSUM_U64 seed = testData->seed;
    XXH128_hash_t const Nresult = testData->Nresult;
    if (len == 0) {
        data = NULL;
    } else {
        assert(data != NULL);
    }

    {   XXH128_hash_t const Dresult = XXH3_128bits_withSeed(data, len, seed);
        XSUM_checkResult128(Dresult, Nresult);
    }

    /* check that XXH128() is identical to XXH3_128bits_withSeed() */
    {   XXH128_hash_t const Dresult2 = XXH128(data, len, seed);
        XSUM_checkResult128(Dresult2, Nresult);
    }

    /* check that the no-seed variant produces same result as seed==0 */
    if (seed == 0) {
        XXH128_hash_t const Dresult = XXH3_128bits(data, len);
        XSUM_checkResult128(Dresult, Nresult);
    }

    /* check that the combination of
     * XXH3_generateSecret_fromSeed() and XXH3_128bits_withSecretandSeed()
     * results in exactly the same hash generation as XXH3_64bits_withSeed() */
    {   char secretBuffer[XXH3_SECRET_DEFAULT_SIZE+1];
        char* const secret = secretBuffer + 1;  /* intentional unalignment */
        XXH3_generateSecret_fromSeed(secret, seed);
        {   XXH128_hash_t const Dresult = XXH3_128bits_withSecretandSeed(data, len, secret, XXH3_SECRET_DEFAULT_SIZE, seed);
            XSUM_checkResult128(Dresult, Nresult);
        }

        /* check that XXH3_128bits_withSecretandSeed()
         * results in exactly the same return value as XXH3_128bits_withSeed()
         * if len <= XXH3_MIDSIZE_MAX, whatever the content of @secret */
        memset(secret, 0x99, 9);
        if (len <= XXH3_MIDSIZE_MAX) {
            XXH128_hash_t const Dresult = XXH3_128bits_withSecretandSeed(data, len, secret, XXH3_SECRET_DEFAULT_SIZE, seed);
            XSUM_checkResult128(Dresult, Nresult);
        }
    }

    /* streaming API test */
    {   XXH3_state_t *state = XXH3_createState();
        assert(state != NULL);

        /* single ingestion */
        (void)XXH3_128bits_reset_withSeed(state, seed);
        (void)XXH3_128bits_update(state, data, len);
        XSUM_checkResult128(XXH3_128bits_digest(state), Nresult);

        /* random ingestion */
        (void)XXH3_128bits_reset_withSeed(state, seed);
        XSUM_XXH3_randomUpdate(state, data, len, XXH3_128bits_update);
        XSUM_checkResult128(XXH3_128bits_digest(state), Nresult);

        /* byte by byte ingestion */
        {   size_t pos;
            (void)XXH3_128bits_reset_withSeed(state, seed);
            for (pos=0; pos<len; pos++)
                (void)XXH3_128bits_update(state, ((const char*)data)+pos, 1);
            XSUM_checkResult128(XXH3_128bits_digest(state), Nresult);
        }

        /* check that streaming with a combination of
         * XXH3_generateSecret_fromSeed() and XXH3_128bits_reset_withSecretandSeed()
         * results in exactly the same hash generation as XXH3_128bits_reset_withSeed() */
        {   char secretBuffer[XXH3_SECRET_DEFAULT_SIZE+1];
            char* const secret = secretBuffer + 1;  /* intentional unalignment */
            XXH3_generateSecret_fromSeed(secret, seed);
            /* single ingestion */
            (void)XXH3_128bits_reset_withSecretandSeed(state, secret, XXH3_SECRET_DEFAULT_SIZE, seed);
            (void)XXH3_128bits_update(state, data, len);
            XSUM_checkResult128(XXH3_128bits_digest(state), Nresult);

            /* check that XXH3_128bits_reset_withSecretandSeed()
             * results in exactly the same return value as XXH3_128bits_reset_withSeed()
             * if len <= XXH3_MIDSIZE_MAX, whatever the content of @secret */
            if (len <= XXH3_MIDSIZE_MAX) {
                /* single ingestion */
                memset(secret, 0x99, 9);
                (void)XXH3_128bits_reset_withSecretandSeed(state, secret, XXH3_SECRET_DEFAULT_SIZE, seed);
                (void)XXH3_128bits_update(state, data, len);
                XSUM_checkResult128(XXH3_128bits_digest(state), Nresult);
            }
        }

        XXH3_freeState(state);
    }
}

static void XSUM_testXXH128_withSecret(const void* data, const void* secret, size_t secretSize, const XSUM_testdata128_t* testData)
{
    size_t len = testData->len;
    XXH128_hash_t Nresult = testData->Nresult;
    if (len == 0) {
        data = NULL;
    } else {
        assert(data != NULL);
    }
    {   XXH128_hash_t const Dresult = XXH3_128bits_withSecret(data, len, secret, secretSize);
        XSUM_checkResult128(Dresult, Nresult);
    }

    /* check that XXH3_128bits_withSecretandSeed()
     * results in exactly the same return value as XXH3_128bits_withSecret() */
    if (len > XXH3_MIDSIZE_MAX)
    {   XXH128_hash_t const Dresult = XXH3_128bits_withSecretandSeed(data, len, secret, secretSize, 0);
        XSUM_checkResult128(Dresult, Nresult);
    }

    /* streaming API test */
    {   XXH3_state_t* const state = XXH3_createState();
        assert(state != NULL);
        (void)XXH3_128bits_reset_withSecret(state, secret, secretSize);
        (void)XXH3_128bits_update(state, data, len);
        XSUM_checkResult128(XXH3_128bits_digest(state), Nresult);

        /* random ingestion */
        (void)XXH3_128bits_reset_withSecret(state, secret, secretSize);
        XSUM_XXH3_randomUpdate(state, data, len, XXH3_128bits_update);
        XSUM_checkResult128(XXH3_128bits_digest(state), Nresult);

        /* byte by byte ingestion */
        {   size_t pos;
            (void)XXH3_128bits_reset_withSecret(state, secret, secretSize);
            for (pos=0; pos<len; pos++)
                (void)XXH3_128bits_update(state, ((const char*)data)+pos, 1);
            XSUM_checkResult128(XXH3_128bits_digest(state), Nresult);
        }

        /* check that XXH3_128bits_reset_withSecretandSeed()
         * results in exactly the same return value as XXH3_128bits_reset_withSecret() */
         if (len > XXH3_MIDSIZE_MAX) {
            /* single ingestion */
            (void)XXH3_128bits_reset_withSecretandSeed(state, secret, secretSize, 0);
            (void)XXH3_128bits_update(state, data, len);
            XSUM_checkResult128(XXH3_128bits_digest(state), Nresult);
        }

        XXH3_freeState(state);
    }
}

static void XSUM_testSecretGenerator(const void* customSeed, const XSUM_testdata_sample_t* testData)
{
    static int nbTests = 1;
    const int sampleIndex[SECRET_SAMPLE_NBBYTES] = { 0, 62, 131, 191, 241 };  /* position of sampled bytes */
    XSUM_U8 secretBuffer[SECRET_SIZE_MAX] = {0};
    XSUM_U8 samples[SECRET_SAMPLE_NBBYTES];
    int i;

    assert(testData->secretLen <= SECRET_SIZE_MAX);
    XXH3_generateSecret(secretBuffer, testData->secretLen, customSeed, testData->seedLen);
    for (i=0; i<SECRET_SAMPLE_NBBYTES; i++) {
        samples[i] = secretBuffer[sampleIndex[i]];
    }
    if (memcmp(samples, testData->byte, sizeof(testData->byte))) {
        XSUM_log("\rError: Secret generation test %i: Internal sanity check failed. \n", nbTests);
        XSUM_log("\rGot { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X }, expected { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X } \n",
                samples[0], samples[1], samples[2], samples[3], samples[4],
                testData->byte[0], testData->byte[1], testData->byte[2], testData->byte[3], testData->byte[4] );
        exit(1);
    }
    nbTests++;
}

/*!
 * XSUM_sanityCheck():
 * Runs a sanity check before the benchmark.
 *
 * Exits on an incorrect output.
 */
XSUM_API void XSUM_sanityCheck(void)
{
    size_t i;
#define SANITY_BUFFER_SIZE 2367
    XSUM_U8 sanityBuffer[SANITY_BUFFER_SIZE];
    const void* const secret = sanityBuffer + 7;
    const size_t secretSize = XXH3_SECRET_SIZE_MIN + 11;
    assert(sizeof(sanityBuffer) >= 7 + secretSize);

    XSUM_fillTestBuffer(sanityBuffer, sizeof(sanityBuffer));

    /* XXH32 */
    for (i = 0; i < (sizeof(XSUM_XXH32_testdata)/sizeof(XSUM_XXH32_testdata[0])); i++) {
        XSUM_testXXH32(sanityBuffer, &XSUM_XXH32_testdata[i]);
    }
    /* XXH64 */
    for (i = 0; i < (sizeof(XSUM_XXH64_testdata)/sizeof(XSUM_XXH64_testdata[0])); i++) {
        XSUM_testXXH64(sanityBuffer, &XSUM_XXH64_testdata[i]);
    }
    /* XXH3_64bits, seeded */
    for (i = 0; i < (sizeof(XSUM_XXH3_testdata)/sizeof(XSUM_XXH3_testdata[0])); i++) {
        XSUM_testXXH3(sanityBuffer, &XSUM_XXH3_testdata[i]);
    }
    /* XXH3_64bits, custom secret */
    for (i = 0; i < (sizeof(XSUM_XXH3_withSecret_testdata)/sizeof(XSUM_XXH3_withSecret_testdata[0])); i++) {
        XSUM_testXXH3_withSecret(sanityBuffer, secret, secretSize, &XSUM_XXH3_withSecret_testdata[i]);
    }
    /* XXH128 */
    for (i = 0; i < (sizeof(XSUM_XXH128_testdata)/sizeof(XSUM_XXH128_testdata[0])); i++) {
        XSUM_testXXH128(sanityBuffer, &XSUM_XXH128_testdata[i]);
    }
    /* XXH128 with custom Secret */
    for (i = 0; i < (sizeof(XSUM_XXH128_withSecret_testdata)/sizeof(XSUM_XXH128_withSecret_testdata[0])); i++) {
        XSUM_testXXH128_withSecret(sanityBuffer, secret, secretSize, &XSUM_XXH128_withSecret_testdata[i]);
    }
    /* secret generator */
    for (i = 0; i < (sizeof(XSUM_XXH3_generateSecret_testdata)/sizeof(XSUM_XXH3_generateSecret_testdata[0])); i++) {
        assert(XSUM_XXH3_generateSecret_testdata[i].seedLen <= SANITY_BUFFER_SIZE);
        XSUM_testSecretGenerator(sanityBuffer, &XSUM_XXH3_generateSecret_testdata[i]);
    }

    XSUM_logVerbose(3, "\r%70s\r", "");       /* Clean display line */
    XSUM_logVerbose(3, "Sanity check -- all tests ok\n");
}

#endif /* !XSUM_NO_TESTS */
