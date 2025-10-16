// xxHash/tests/sanity_test_vectors_generator.c
// SPDX-License-Identifier: GPL-2.0-only
//
// So far, this program just generates sanity_test_vectors.h
//
// Building
// ========
//
// cc sanity_test_vectors_generator.c && ./a.out
// less sanity_test_vectors.h
//
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION   /* access definitions */
#include "../cli/xsum_arch.h"
#include "../cli/xsum_os_specific.h"
#include "../xxhash.h"

#include <assert.h> /* assert */

/* use #define to make them constant, required for initialization */
#define PRIME32 2654435761U
#define PRIME64 11400714785074694797ULL

#define SANITY_BUFFER_SIZE (4096 + 64 + 1)


/* TODO : Share these test vector definitions with sanity_check.c and xsum_sanity_check.c */
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

#ifndef   SECRET_SIZE_MAX
#  define SECRET_SIZE_MAX 9867
#endif


/* TODO : Share these generators with sanity_check.c and xsum_sanity_check.c */
/* Test vector generators */
static XSUM_testdata32_t tvgen_XXH32(const XSUM_U8* buf, size_t len, XSUM_U32 seed) {
    XSUM_testdata32_t v;
    v.len     = len;
    v.seed    = seed;
    v.Nresult = XXH32(buf, len, seed);
    return v;
}

static XSUM_testdata64_t tvgen_XXH64(const XSUM_U8* buf, size_t len, XSUM_U64 seed) {
    XSUM_testdata64_t v;
    v.len     = len;
    v.seed    = seed;
    v.Nresult = XXH64(buf, len, seed);
    return v;
}

static XSUM_testdata64_t tvgen_XXH3_64bits_withSeed(const XSUM_U8* buf, size_t len, XSUM_U64 seed) {
    XSUM_testdata64_t v;
    v.len     = len;
    v.seed    = seed;
    v.Nresult = XXH3_64bits_withSeed(buf, len, seed);
    return v;
}

static XSUM_testdata64_t tvgen_XXH3_64bits_withSecret(const XSUM_U8* buf, size_t len, const void* secret, size_t secretSize) {
    XSUM_testdata64_t v;
    v.len     = len;
    v.seed    = 0;
    v.Nresult = XXH3_64bits_withSecret(buf, len, secret, secretSize);
    return v;
}

static XSUM_testdata128_t tvgen_XXH3_128bits_withSeed(const XSUM_U8* buf, size_t len, XSUM_U64 seed) {
    XSUM_testdata128_t v;
    v.len     = len;
    v.seed    = seed;
    v.Nresult = XXH3_128bits_withSeed(buf, len, seed);
    return v;
}

static XSUM_testdata128_t tvgen_XXH3_128bits_withSecret(const XSUM_U8* buf, size_t len, const void* secret, size_t secretSize) {
    XSUM_testdata128_t v;
    v.len     = len;
    v.seed    = 0;
    v.Nresult = XXH3_128bits_withSecret(buf, len, secret, secretSize);
    return v;
}

static XSUM_testdata_sample_t tvgen_XXH3_generateSecret(
    void* secretBuffer,
    size_t secretSize,
    const void* customSeed,
    size_t customSeedSize
) {
    XXH3_generateSecret(secretBuffer, secretSize, customSeed, customSeedSize);

    XSUM_testdata_sample_t v;
    v.seedLen   = customSeedSize;
    v.secretLen = secretSize;

    /* TODO : Share this array with sanity_check.c and xsum_sanity_check.c */
    /* position of sampled bytes */
    static const int sampleIndex[SECRET_SAMPLE_NBBYTES] = { 0, 62, 131, 191, 241 };

    for(int i = 0; i < SECRET_SAMPLE_NBBYTES; ++i) {
        const XSUM_U8* const secretBufferAsU8 = (const XSUM_U8*) secretBuffer;
        v.byte[i] = secretBufferAsU8[sampleIndex[i]];
    }
    return v;
}


/* Test vector serializers */
static void fprintf_XSUM_testdata32_t(FILE* fp, XSUM_testdata32_t const v) {
    fprintf(fp, "{ %4d, 0x%08XU, 0x%08XU },", v.len, v.seed, v.Nresult);
}

static void fprintf_XSUM_testdata64_t(FILE* fp, XSUM_testdata64_t const v) {
    fprintf(fp, "{ %4d, 0x%016llXULL, 0x%016llXULL },", v.len, v.seed, v.Nresult);
}

static void fprintf_XSUM_testdata128_t(FILE* fp, XSUM_testdata128_t const v) {
    fprintf(fp, "{ %4d, 0x%016llXULL, { 0x%016llXULL, 0x%016llXULL } },",
            v.len, v.seed, v.Nresult.low64, v.Nresult.high64);
}

static void fprintf_XSUM_testdata_sample_t(FILE* fp, XSUM_testdata_sample_t const v) {
    fprintf(fp,"{ %4d, %4d, { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X } },",
            v.seedLen, v.secretLen, v.byte[0], v.byte[1], v.byte[2], v.byte[3], v.byte[4]);
}


/* TODO : Share this function with sanity_check.c and xsum_sanity_check.c */
/*
 * Fills a test buffer with pseudorandom data.
 *
 * This is used in the sanity check - its values must not be changed.
 */
static void fillTestBuffer(XSUM_U8* buffer, size_t bufferLenInBytes)
{
    XSUM_U64 byteGen = PRIME32;
    size_t i;

    assert(buffer != NULL);

    for (i = 0; i < bufferLenInBytes; ++i) {
        buffer[i] = (XSUM_U8)(byteGen>>56);
        byteGen *= PRIME64;
    }
}


/* TODO : Share this function with sanity_check.c and xsum_sanity_check.c */
/*
 * Create (malloc) and fill buffer with pseudorandom data for sanity check.
 *
 * Use releaseSanityBuffer() to delete the buffer.
 */
static XSUM_U8* createSanityBuffer(size_t bufferLenInBytes)
{
    XSUM_U8* const buffer = (XSUM_U8*) malloc(bufferLenInBytes);
    assert(buffer != NULL);
    fillTestBuffer(buffer, bufferLenInBytes);
    return buffer;
}


/* TODO : Share this function with sanity_check.c and xsum_sanity_check.c */
/*
 * Delete (free) the buffer which has been generated by createSanityBuffer()
 */
static void releaseSanityBuffer(XSUM_U8* buffer)
{
    assert(buffer != NULL);
    free(buffer);
}


/* Generate test vectors for XXH32() */
static void generate_sanity_test_vectors_xxh32(FILE* fp, size_t maxLen) {
    const char* const arrayTypeName = "XSUM_testdata32_t";
    const char* const arrayName     = "XSUM_XXH32_testdata";
    fprintf(fp, "static const %s %s[] = {\n", arrayTypeName, arrayName);

    XSUM_U8* const sanityBuffer = createSanityBuffer(maxLen);

    size_t index = 0;
    for(size_t len = 0; len < maxLen; ++len) {
        static const uint64_t seeds[] = { 0, PRIME32 };
        for(size_t iSeed = 0; iSeed < sizeof(seeds)/sizeof(seeds[0]); ++iSeed) {
            size_t const seed = seeds[iSeed];
            XSUM_testdata32_t const v = tvgen_XXH32(sanityBuffer, len, seed);

            fprintf(fp, "    ");
            fprintf_XSUM_testdata32_t(fp, v);
            fprintf(fp, " /* %s[%zd] */\n", arrayName, index++);
        }
    }

    releaseSanityBuffer(sanityBuffer);
    fprintf(fp, "};\n");
}


/* Generate test vectors for XXH64() */
static void generate_sanity_test_vectors_xxh64(FILE* fp, size_t maxLen) {
    const char* const arrayTypeName = "XSUM_testdata64_t";
    const char* const arrayName     = "XSUM_XXH64_testdata";
    fprintf(fp, "static const %s %s[] = {\n", arrayTypeName, arrayName);

    XSUM_U8* const sanityBuffer = createSanityBuffer(maxLen);

    size_t index = 0;
    for(size_t len = 0; len < maxLen; ++len) {
        static const uint64_t seeds[] = { 0, PRIME32 };
        for(size_t iSeed = 0; iSeed < sizeof(seeds)/sizeof(seeds[0]); ++iSeed) {
            size_t const seed = seeds[iSeed];
            XSUM_testdata64_t const v = tvgen_XXH64(sanityBuffer, len, seed);

            fprintf(fp, "    ");
            fprintf_XSUM_testdata64_t(fp, v);
            fprintf(fp, " /* %s[%zd] */\n", arrayName, index++);
        }
    }

    releaseSanityBuffer(sanityBuffer);
    fprintf(fp, "};\n");
}


/* Generate test vectors for XXH3_64bits_withSeed() */
static void generate_sanity_test_vectors_xxh3(FILE* fp, size_t maxLen) {
    const char* const arrayTypeName = "XSUM_testdata64_t";
    const char* const arrayName     = "XSUM_XXH3_testdata";
    fprintf(fp, "static const %s %s[] = {\n", arrayTypeName, arrayName);

    XSUM_U8* const sanityBuffer = createSanityBuffer(maxLen);

    size_t index = 0;
    for(size_t len = 0; len < maxLen; ++len) {
        static const uint64_t seeds[] = { 0, PRIME64 };
        for(size_t iSeed = 0; iSeed < sizeof(seeds)/sizeof(seeds[0]); ++iSeed) {
            size_t const seed = seeds[iSeed];
            XSUM_testdata64_t const v = tvgen_XXH3_64bits_withSeed(sanityBuffer, len, seed);

            fprintf(fp, "    ");
            fprintf_XSUM_testdata64_t(fp, v);
            fprintf(fp, " /* %s[%zd] */\n", arrayName, index++);
        }
    }

    releaseSanityBuffer(sanityBuffer);
    fprintf(fp, "};\n");
}


/* Generate test vectors for XXH3_64bits_withSecret() */
static void generate_sanity_test_vectors_xxh3_withSecret(FILE* fp, size_t maxLen) {
    const char* const arrayTypeName = "XSUM_testdata64_t";
    const char* const arrayName     = "XSUM_XXH3_withSecret_testdata";
    fprintf(fp, "static const %s %s[] = {\n", arrayTypeName, arrayName);

    XSUM_U8* const sanityBuffer = createSanityBuffer(maxLen);

    const void* const secret = sanityBuffer + 7;
    size_t const secretSize = XXH3_SECRET_SIZE_MIN + 11;
    assert(maxLen >= 7 + secretSize);

    size_t index = 0;
    for(size_t len = 0; len < maxLen; ++len) {
        XSUM_testdata64_t const v = tvgen_XXH3_64bits_withSecret(sanityBuffer, len, secret, secretSize);

        fprintf(fp, "    ");
        fprintf_XSUM_testdata64_t(fp, v);
        fprintf(fp, " /* %s[%zd] */\n", arrayName, index++);
    }

    releaseSanityBuffer(sanityBuffer);
    fprintf(fp, "};\n");
}


/* Generate test vectors for XXH3_128bits_withSeed() */
static void generate_sanity_test_vectors_xxh128(FILE* fp, size_t maxLen) {
    const char* const arrayTypeName = "XSUM_testdata128_t";
    const char* const arrayName     = "XSUM_XXH128_testdata";
    fprintf(fp, "static const %s %s[] = {\n", arrayTypeName, arrayName);

    XSUM_U8* const sanityBuffer = createSanityBuffer(maxLen);

    size_t index = 0;
    for(size_t len = 0; len < maxLen; ++len) {
        static const uint64_t seeds[] = { 0, PRIME32, PRIME64 };
        for(size_t iSeed = 0; iSeed < sizeof(seeds)/sizeof(seeds[0]); ++iSeed) {
            XSUM_U64 const seed = seeds[iSeed];
            XSUM_testdata128_t const v = tvgen_XXH3_128bits_withSeed(sanityBuffer, len, seed);

            fprintf(fp, "    ");
            fprintf_XSUM_testdata128_t(fp, v);
            fprintf(fp, " /* %s[%zd] */\n", arrayName, index++);
        }
    }
    fprintf(fp, "};\n");
    releaseSanityBuffer(sanityBuffer);
}


/* Generate test vectors for XXH3_128bits_withSecret() */
static void generate_sanity_test_vectors_xxh128_withSecret(FILE* fp, size_t maxLen) {
    const char* const arrayTypeName = "XSUM_testdata128_t";
    const char* const arrayName     = "XSUM_XXH128_withSecret_testdata";
    fprintf(fp, "static const %s %s[] = {\n", arrayTypeName, arrayName);

    XSUM_U8* const sanityBuffer = createSanityBuffer(maxLen);

    const void* const secret = sanityBuffer + 7;
    size_t const secretSize = XXH3_SECRET_SIZE_MIN + 11;
    assert(maxLen >= 7 + secretSize);

    size_t index = 0;
    for(size_t len = 0; len < maxLen; ++len) {
        XSUM_testdata128_t const v = tvgen_XXH3_128bits_withSecret(sanityBuffer, len, secret, secretSize);

        fprintf(fp, "    ");
        fprintf_XSUM_testdata128_t(fp, v);
        fprintf(fp, " /* %s[%zd] */\n", arrayName, index++);
    }

    fprintf(fp, "};\n");
    releaseSanityBuffer(sanityBuffer);
}


/* Generate test vectors for XXH3_generateSecret() */
static void generate_sanity_test_vectors_xxh3_generateSecret(FILE* fp, size_t maxLen) {
    const char* const arrayTypeName = "XSUM_testdata_sample_t";
    const char* const arrayName     = "XSUM_XXH3_generateSecret_testdata";
    fprintf(fp, "static const %s %s[] = {\n", arrayTypeName, arrayName);

    XSUM_U8* const sanityBuffer = createSanityBuffer(maxLen);
    const void* const customSeed = sanityBuffer;
    static const size_t seedLens[] = {
        0,
        1,
        XXH3_SECRET_SIZE_MIN - 1,
        XXH3_SECRET_DEFAULT_SIZE + 500
    };
    static const size_t secretLens[] = {
        192,
        240,
        277,
        SECRET_SIZE_MAX
    };

    size_t index = 0;
    for(size_t iSeedLen = 0; iSeedLen < sizeof(seedLens)/sizeof(seedLens[0]); ++iSeedLen) {
        for(size_t iSecretLen = 0; iSecretLen < sizeof(secretLens)/sizeof(secretLens[0]); ++iSecretLen) {
            size_t const seedLen = seedLens[iSeedLen];
            size_t const secretLen = secretLens[iSecretLen];
            XSUM_U8 secretBuffer[SECRET_SIZE_MAX] = {0};

            assert(seedLen <= maxLen);
            assert(secretLen <= SECRET_SIZE_MAX);

            XSUM_testdata_sample_t const v = tvgen_XXH3_generateSecret(
                secretBuffer,
                secretLen,
                customSeed,
                seedLen
            );

            fprintf(fp, "    ");
            fprintf_XSUM_testdata_sample_t(fp, v);
            fprintf(fp, " /* %s[%zd] */\n", arrayName, index++);
        }
    }
    fprintf(fp, "};\n");

    releaseSanityBuffer(sanityBuffer);
}


/* Generate test vectors */
void generate_sanity_test_vectors(size_t maxLen) {
    const char* filename = "sanity_test_vectors.h";
    fprintf(stderr, "Generating %s\n", filename);
    FILE* fp = fopen(filename, "w");
    fprintf(fp,
        "typedef struct {\n"
        "    XSUM_U32 len;\n"
        "    XSUM_U32 seed;\n"
        "    XSUM_U32 Nresult;\n"
        "} XSUM_testdata32_t;\n"
        "\n"
        "typedef struct {\n"
        "    XSUM_U32 len;\n"
        "    XSUM_U64 seed;\n"
        "    XSUM_U64 Nresult;\n"
        "} XSUM_testdata64_t;\n"
        "\n"
        "typedef struct {\n"
        "    XSUM_U32 len;\n"
        "    XSUM_U64 seed;\n"
        "    XXH128_hash_t Nresult;\n"
        "} XSUM_testdata128_t;\n"
        "\n"
        "#ifndef SECRET_SAMPLE_NBBYTES\n"
        "#define SECRET_SAMPLE_NBBYTES 5\n"
        "#endif\n"
        "\n"
        "typedef struct {\n"
        "    XSUM_U32 seedLen;\n"
        "    XSUM_U32 secretLen;\n"
        "    XSUM_U8 byte[SECRET_SAMPLE_NBBYTES];\n"
        "} XSUM_testdata_sample_t;\n"
        "\n"
        "#ifndef SECRET_SIZE_MAX\n"
        "#define SECRET_SIZE_MAX 9867\n"
        "#endif\n"
        "\n"
    );

    generate_sanity_test_vectors_xxh32(fp, maxLen);
    generate_sanity_test_vectors_xxh64(fp, maxLen);
    generate_sanity_test_vectors_xxh3(fp, maxLen);
    generate_sanity_test_vectors_xxh3_withSecret(fp, maxLen);
    generate_sanity_test_vectors_xxh128(fp, maxLen);
    generate_sanity_test_vectors_xxh128_withSecret(fp, maxLen);
    generate_sanity_test_vectors_xxh3_generateSecret(fp, maxLen);
    fclose(fp);
}


/**/
int main(int argc, const char* argv[])
{
    (void) argc;
    (void) argv;
    const size_t sanityBufferSizeInBytes = SANITY_BUFFER_SIZE;
    generate_sanity_test_vectors(sanityBufferSizeInBytes);
    return EXIT_SUCCESS;
}
