/*
 * xxhsum - Command line interface for xxhash algorithms
 * Copyright (C) 2013-2023 Yann Collet
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
 * xxhsum:
 * Provides hash value of a file content, or a list of files, or stdin
 * Display convention is Big Endian, for both 32 and 64 bits algorithms
 */

/* Transitional headers */
#include "xsum_arch.h"         /* XSUM_PROGRAM_VERSION */
#include "xsum_os_specific.h"  /* XSUM_setBinaryMode */
#include "xsum_output.h"       /* XSUM_output */
#include "xsum_sanity_check.h" /* XSUM_sanityCheck */
#include "xsum_bench.h"        /* NBLOOPS_DEFAULT */

/* ************************************
 *  Includes
 **************************************/
#include <stdlib.h>     /* malloc, calloc, free, exit */
#include <string.h>     /* strerror, strcmp, memcpy */
#include <assert.h>     /* assert */
#include <errno.h>      /* errno */

#define XXH_STATIC_LINKING_ONLY   /* *_state_t */
#include "../xxhash.h"

#ifdef XXHSUM_DISPATCH
#  include "../xxh_x86dispatch.h"
#endif

static unsigned XSUM_isLittleEndian(void)
{
    const union { XSUM_U32 u; XSUM_U8 c[4]; } one = { 1 };   /* don't use static: performance detrimental  */
    return one.c[0];
}

static const int g_nbBits = (int)(sizeof(void*)*8);
static const char g_lename[] = "little endian";
static const char g_bename[] = "big endian";
#define ENDIAN_NAME (XSUM_isLittleEndian() ? g_lename : g_bename)
static const char author[] = "Yann Collet";
#define WELCOME_MESSAGE(exename) "%s %s by %s \n", exename, XSUM_PROGRAM_VERSION, author
#define FULL_WELCOME_MESSAGE(exename) "%s %s by %s \n" \
                    "compiled as %i-bit %s %s with " XSUM_CC_VERSION_FMT " \n", \
                    exename, XSUM_PROGRAM_VERSION, author, \
                    g_nbBits, XSUM_ARCH, ENDIAN_NAME, XSUM_CC_VERSION


static const char stdinName[] = "-";
static const char stdinFileName[] = "stdin";
typedef enum { algo_xxh32=0, algo_xxh64=1, algo_xxh128=2, algo_xxh3=3 } AlgoSelected;
static AlgoSelected g_defaultAlgo = algo_xxh64;    /* required within main() & XSUM_usage() */

typedef enum {
    algo_bitmask_xxh32  = 1 << algo_xxh32,      /* 1 << 0 */
    algo_bitmask_xxh64  = 1 << algo_xxh64,      /* 1 << 1 */
    algo_bitmask_xxh128 = 1 << algo_xxh128,     /* 1 << 2 */
    algo_bitmask_xxh3   = 1 << algo_xxh3,       /* 1 << 3 */
    algo_bitmask_all    = algo_bitmask_xxh32    /* All algorithms */
                        | algo_bitmask_xxh64
                        | algo_bitmask_xxh128
                        | algo_bitmask_xxh3
} AlgoBitmask;


/* <16 hex char> <SPC> <SPC> <filename> <'\0'>
 * '4096' is typical Linux PATH_MAX configuration. */
#define DEFAULT_LINE_LENGTH (sizeof(XXH64_hash_t) * 2 + 2 + 4096 + 1)

/* Maximum acceptable line length. */
#define MAX_LINE_LENGTH (32 KB)

static size_t XSUM_DEFAULT_SAMPLE_SIZE = 100 KB;


/* ********************************************************
*  Filename (un)escaping
**********************************************************/
static int XSUM_filenameNeedsEscape(const char* filename) {
    return strchr(filename, '\\')
        || strchr(filename, '\n')
        || strchr(filename, '\r');
}

static int XSUM_lineNeedsUnescape(const char* line) {
    /* Skip white-space characters */
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    /* Returns true if first non-white-space character is '\\' (0x5c) */
    return *line == '\\';
}

static void XSUM_printFilename(const char* filename, int needsEscape) {
    if (!needsEscape) {
        XSUM_output("%s", filename);
    } else {
        const char* p;
        for (p = filename; *p != '\0'; ++p) {
            switch (*p)
            {
            case '\n':
                XSUM_output("\\n");
                break;
            case '\r':
                XSUM_output("\\r");
                break;
            case '\\':
                XSUM_output("\\\\");
                break;
            default:
                XSUM_output("%c", *p);
                break;
            }
        }
    }
}

/* Unescape filename in place.

   - Replace '\\', 'n'  (0x5c, 0x6e) with '\n' (0x0a).
   - Replace '\\', 'r'  (0x5c, 0x72) with '\r' (0x0d).
   - Replace '\\', '\\' (0x5c, 0x5c) with '\\' (0x5c).
   - filename may not contain other backslash sequences.
   - filename may not ends with backslash.
   - filename may not contain NUL (0x00).

   Return filename if everything is okay.
   Return NULL if something wrong.
*/
static char* XSUM_filenameUnescape(char* filename, size_t filenameLen) {
    char *p = filename;
    size_t i;
    for (i = 0; i < filenameLen; ++i) {
        switch (filename[i])
        {
        case '\\':
            ++i;
            if (i == filenameLen) {
                return NULL; /* Don't accept '\\', <EOL> */
            }
            switch (filename[i])
            {
            case 'n':
                *p++ = '\n';
                break;
            case 'r':
                *p++ = '\r';
                break;
            case '\\':
                *p++ = '\\';
                break;
            default:
                return NULL; /* Don't accept any other backslash sequence */
            }
            break;
        case '\0':
            return NULL; /* Don't accept NUL (0x00) */
        default:
            *p++ = filename[i];
            break;
        }
    }
    if (p < filename + filenameLen) {
        *p = '\0';
    }
    return filename;
}


/* ********************************************************
*  Algorithm Bitmask
**********************************************************/
/* Compute AlgoBitmask (as a U32) from AlgoSelected */
static XSUM_U32 XSUM_algoBitmask_ComputeAlgoBitmaskFromAlgoSelected(AlgoSelected algoSelected) {
    return (XSUM_U32) (1U << algoSelected);
}

/* Returns true (!0) if algoBitmask contains (accepts) parsedLineAlgo */
static int XSUM_algoBitmask_Accepts(XSUM_U32 algoBitmask, AlgoSelected parsedLineAlgo) {
    const XSUM_U32 mask = XSUM_algoBitmask_ComputeAlgoBitmaskFromAlgoSelected(parsedLineAlgo);
    return (algoBitmask & mask) != 0;
}


/* ********************************************************
*  File Hashing
**********************************************************/

#define XXHSUM32_DEFAULT_SEED 0                   /* Default seed for algo_xxh32 */
#define XXHSUM64_DEFAULT_SEED 0                   /* Default seed for algo_xxh64 */

/* for support of --little-endian display mode */
static void XSUM_display_LittleEndian(const void* ptr, size_t length)
{
    const XSUM_U8* const p = (const XSUM_U8*)ptr;
    size_t idx;
    for (idx=length-1; idx<length; idx--)    /* intentional underflow to negative to detect end */
        XSUM_output("%02x", p[idx]);
}

static void XSUM_display_BigEndian(const void* ptr, size_t length)
{
    const XSUM_U8* const p = (const XSUM_U8*)ptr;
    size_t idx;
    for (idx=0; idx<length; idx++)
        XSUM_output("%02x", p[idx]);
}

typedef union {
    XXH32_hash_t  hash32;
    XXH64_hash_t  hash64;  /* also for xxh3_64bits */
    XXH128_hash_t hash128;
} Multihash;

/*
 * XSUM_hashStream:
 * Reads data from `inFile`, generating an incremental hash of type hashType,
 * using `buffer` of size `blockSize` for temporary storage.
 */
static Multihash
XSUM_hashStream(FILE* inFile,
                AlgoSelected hashType,
                void* buffer, size_t blockSize)
{
    XXH32_state_t state32;
    XXH64_state_t state64;
    XXH3_state_t  state3;

    /* Init */
    (void)XXH32_reset(&state32, XXHSUM32_DEFAULT_SEED);
    (void)XXH64_reset(&state64, XXHSUM64_DEFAULT_SEED);
    (void)XXH3_128bits_reset(&state3);

    /* Load file & update hash */
    {   size_t readSize;
        while ((readSize = fread(buffer, 1, blockSize, inFile)) > 0) {
            switch(hashType)
            {
            case algo_xxh32:
                (void)XXH32_update(&state32, buffer, readSize);
                break;
            case algo_xxh64:
                (void)XXH64_update(&state64, buffer, readSize);
                break;
            case algo_xxh128:
                (void)XXH3_128bits_update(&state3, buffer, readSize);
                break;
            case algo_xxh3:
                (void)XXH3_64bits_update(&state3, buffer, readSize);
                break;
            default:
                assert(0);
            }
        }
        if (ferror(inFile)) {
            XSUM_log("Error: a failure occurred reading the input file.\n");
            exit(1);
    }   }

    {   Multihash finalHash = {0};
        switch(hashType)
        {
        case algo_xxh32:
            finalHash.hash32 = XXH32_digest(&state32);
            break;
        case algo_xxh64:
            finalHash.hash64 = XXH64_digest(&state64);
            break;
        case algo_xxh128:
            finalHash.hash128 = XXH3_128bits_digest(&state3);
            break;
        case algo_xxh3:
            finalHash.hash64 = XXH3_64bits_digest(&state3);
            break;
        default:
            assert(0);
        }
        return finalHash;
    }
}

                                       /* algo_xxh32, algo_xxh64, algo_xxh128 */
static const char* XSUM_algoName[] =    { "XXH32",    "XXH64",    "XXH128",    "XXH3" };
static const char* XSUM_algoLE_name[] = { "XXH32_LE", "XXH64_LE", "XXH128_LE", "XXH3_LE" };
static const size_t XSUM_algoLength[] = { 4,          8,          16,          8 };

#define XSUM_TABLE_ELT_SIZE(table)   (sizeof(table) / sizeof(*table))

typedef void (*XSUM_displayHash_f)(const void*, size_t);  /* display function signature */

static void XSUM_printLine_BSD_internal(const char* filename,
                                        const void* canonicalHash, const AlgoSelected hashType,
                                        const char* algoString[],
                                        XSUM_displayHash_f f_displayHash)
{
    assert(0 <= hashType && (size_t)hashType <= XSUM_TABLE_ELT_SIZE(XSUM_algoName));
    {   const char* const typeString = algoString[hashType];
        const size_t hashLength = XSUM_algoLength[hashType];
        const int needsEscape = XSUM_filenameNeedsEscape(filename);
        if (needsEscape) {
            XSUM_output("%c", '\\');
        }
        XSUM_output("%s (", typeString);
        XSUM_printFilename(filename, needsEscape);
        XSUM_output(") = ");
        f_displayHash(canonicalHash, hashLength);
        XSUM_output("\n");
}   }

static void XSUM_printLine_BSD_LE(const char* filename, const void* canonicalHash, const AlgoSelected hashType)
{
    XSUM_printLine_BSD_internal(filename, canonicalHash, hashType, XSUM_algoLE_name, XSUM_display_LittleEndian);
}

static void XSUM_printLine_BSD(const char* filename, const void* canonicalHash, const AlgoSelected hashType)
{
    XSUM_printLine_BSD_internal(filename, canonicalHash, hashType, XSUM_algoName, XSUM_display_BigEndian);
}

static void XSUM_displayPrefix(const AlgoSelected hashType)
{
    assert(0 <= hashType && (size_t)hashType <= XSUM_TABLE_ELT_SIZE(XSUM_algoName));
    if (hashType == algo_xxh3) {
        XSUM_output("XXH3_");
    }
}

static void XSUM_printLine_GNU_internal(const char* filename,
                               const void* canonicalHash, const AlgoSelected hashType,
                               XSUM_displayHash_f f_displayHash)
{
    assert(0 <= hashType && (size_t)hashType <= XSUM_TABLE_ELT_SIZE(XSUM_algoName));
    {   const size_t hashLength = XSUM_algoLength[hashType];
        const int needsEscape = XSUM_filenameNeedsEscape(filename);
        if (needsEscape) {
            XSUM_output("%c", '\\');
        }
        XSUM_displayPrefix(hashType);
        f_displayHash(canonicalHash, hashLength);
        XSUM_output("  ");
        XSUM_printFilename(filename, needsEscape);
        XSUM_output("\n");
}   }

static void XSUM_printLine_GNU(const char* filename,
                               const void* canonicalHash, const AlgoSelected hashType)
{
    XSUM_printLine_GNU_internal(filename, canonicalHash, hashType, XSUM_display_BigEndian);
}

static void XSUM_printLine_GNU_LE(const char* filename,
                                  const void* canonicalHash, const AlgoSelected hashType)
{
    XSUM_printLine_GNU_internal(filename, canonicalHash, hashType, XSUM_display_LittleEndian);
}

typedef enum { big_endian, little_endian} Display_endianness;

typedef enum { display_gnu, display_bsd } Display_convention;

typedef void (*XSUM_displayLine_f)(const char*, const void*, AlgoSelected);  /* line display signature */

typedef enum {
    LineStatus_hashOk,
    LineStatus_hashFailed,
    LineStatus_failedToOpen,
    LineStatus_isDirectory,
    LineStatus_memoryError
} LineStatus;

static XSUM_displayLine_f XSUM_kDisplayLine_fTable[2][2] = {
    { XSUM_printLine_GNU, XSUM_printLine_GNU_LE },
    { XSUM_printLine_BSD, XSUM_printLine_BSD_LE }
};

static LineStatus XSUM_hashFile(const char* fileName,
                         const AlgoSelected hashType,
                         const Display_endianness displayEndianness,
                         const Display_convention convention)
{
    size_t const blockSize = 64 KB;
    XSUM_displayLine_f const f_displayLine = XSUM_kDisplayLine_fTable[convention][displayEndianness];
    FILE* inFile;
    Multihash hashValue;
    assert(displayEndianness==big_endian || displayEndianness==little_endian);
    assert(convention==display_gnu || convention==display_bsd);

    /* Check file existence */
    if (fileName == stdinName) {
        inFile = stdin;
        fileName = stdinFileName; /* "stdin" */
        XSUM_setBinaryMode(stdin);
    } else {
        if (XSUM_isDirectory(fileName)) {
            return LineStatus_isDirectory;
        }
        inFile = XSUM_fopen( fileName, "rb" );
        if (inFile==NULL) {
            XSUM_log("Error: unable to open input\n");
            return LineStatus_failedToOpen;
    }   }

    /* Memory allocation & streaming */
    {   void* const buffer = malloc(blockSize);
        if (buffer == NULL) {
            XSUM_log("\nError: Out of memory.\n");
            fclose(inFile);
            return LineStatus_memoryError;
        }

        /* Stream file & update hash */
        hashValue = XSUM_hashStream(inFile, hashType, buffer, blockSize);

        fclose(inFile);
        free(buffer);
    }

    /* display Hash value in selected format */
    switch(hashType)
    {
    case algo_xxh32:
        {   XXH32_canonical_t hcbe32;
            (void)XXH32_canonicalFromHash(&hcbe32, hashValue.hash32);
            f_displayLine(fileName, &hcbe32, hashType);
            break;
        }
    case algo_xxh64:
        {   XXH64_canonical_t hcbe64;
            (void)XXH64_canonicalFromHash(&hcbe64, hashValue.hash64);
            f_displayLine(fileName, &hcbe64, hashType);
            break;
        }
    case algo_xxh128:
        {   XXH128_canonical_t hcbe128;
            (void)XXH128_canonicalFromHash(&hcbe128, hashValue.hash128);
            f_displayLine(fileName, &hcbe128, hashType);
            break;
        }
    case algo_xxh3:
        {   XXH64_canonical_t hcbe64;
            (void)XXH64_canonicalFromHash(&hcbe64, hashValue.hash64);
            f_displayLine(fileName, &hcbe64, hashType);
            break;
        }
    default:
        assert(0);  /* not possible */
    }

    return LineStatus_hashOk;
}


/*
 * XSUM_hashFiles:
 * If fnTotal==0, read from stdin instead.
 */
static int XSUM_hashFiles(const char* fnList[], int fnTotal,
                          AlgoSelected hashType,
                          Display_endianness displayEndianness,
                          Display_convention convention)
{
    int fnNb;
    int result = 0;

    if (fnTotal == 0)
    {
        LineStatus filestatus = XSUM_hashFile(stdinName, hashType, displayEndianness, convention);
        switch (filestatus)
        {
        case LineStatus_hashOk:
        case LineStatus_hashFailed:
            break;
        case LineStatus_isDirectory:
            XSUM_log("xxhsum: %s: Is a directory \n", stdinName);
            break;
        case LineStatus_failedToOpen:
            XSUM_log("Error: Could not open '%s': %s. \n", stdinName, strerror(errno));
            break;
        case LineStatus_memoryError:
            XSUM_log("\nError: Out of memory.\n");
            break;
        }

        if (filestatus != LineStatus_hashOk)
            result = 1;
    }


    for (fnNb = 0; fnNb < fnTotal; fnNb++)
    {
        LineStatus filestatus = XSUM_hashFile(fnList[fnNb], hashType, displayEndianness, convention);
        switch (filestatus)
        {
        case LineStatus_hashOk:
        case LineStatus_hashFailed:
            break;
        case LineStatus_isDirectory:
            XSUM_log("xxhsum: %s: Is a directory \n", fnList[fnNb]);
            break;
        case LineStatus_failedToOpen:
            XSUM_log("Error: Could not open '%s': %s. \n", fnList[fnNb], strerror(errno));
            break;
        case LineStatus_memoryError:
            XSUM_log("\nError: Out of memory.\n");
            break;
        }

        if (filestatus != LineStatus_hashOk)
            result = 1;
    }

    return result;
}


typedef enum {
    GetLine_ok,
    GetLine_comment,
    GetLine_eof,
    GetLine_exceedMaxLineLength,
    GetLine_outOfMemory
} GetLineResult;

typedef enum {
    CanonicalFromString_ok,
    CanonicalFromString_invalidFormat
} CanonicalFromStringResult;

typedef enum {
    ParseLine_ok,
    ParseLine_invalidFormat
} ParseLineResult;

typedef union {
    XXH32_canonical_t xxh32;
    XXH64_canonical_t xxh64;
    XXH128_canonical_t xxh128;
} Canonical;

typedef struct {
    Canonical    canonical;
    const char*  filename;
    AlgoSelected algo;
} ParsedLine;

typedef struct {
    unsigned long   nProperlyFormattedLines;
    unsigned long   nImproperlyFormattedLines;
    unsigned long   nMismatchedChecksums;
    unsigned long   nMatchedChecksums;
    unsigned long   nOpenOrReadFailures;
    unsigned long   nMixedFormatLines;
    unsigned long   nMissing;
    int             quit;
} ParseFileReport;

typedef struct {
    const char*  inFileName;
    FILE*        inFile;
    int          lineMax;
    char*        lineBuf;
    size_t       blockSize;
    char*        blockBuf;
    int          strictMode;
    int          statusOnly;
    int          ignoreMissing;
    int          warn;
    int          quiet;
    XSUM_U32     algoBitmask;
    ParseFileReport report;
} ParseFileArg;


/*
 * Reads a line from stream `inFile`.
 * Returns GetLine_ok, if it reads line successfully.
 * Returns GetLine_comment, if the line is beginning with '#'.
 * Returns GetLine_eof, if stream reaches EOF.
 * Returns GetLine_exceedMaxLineLength, if line length is longer than MAX_LINE_LENGTH.
 * Returns GetLine_outOfMemory, if line buffer memory allocation failed.
 */
static GetLineResult XSUM_getLine(char** lineBuf, int* lineMax, FILE* inFile)
{
    GetLineResult result = GetLine_ok;
    size_t len = 0;

    if ((*lineBuf == NULL) || (*lineMax<1)) {
        free(*lineBuf);  /* in case it's != NULL */
        *lineMax = 0;
        *lineBuf = (char*)malloc(DEFAULT_LINE_LENGTH);
        if(*lineBuf == NULL) return GetLine_outOfMemory;
        *lineMax = DEFAULT_LINE_LENGTH;
    }

    for (;;) {
        const int c = fgetc(inFile);
        if (c == EOF) {
            /*
             * If we meet EOF before first character, returns GetLine_eof,
             * otherwise GetLine_ok.
             */
            if (len == 0) result = GetLine_eof;
            break;
        }

        /* Make enough space for len+1 (for final NUL) bytes. */
        if (len+1 >= (size_t)*lineMax) {
            char* newLineBuf = NULL;
            size_t newBufSize = (size_t)*lineMax;

            newBufSize += (newBufSize/2) + 1; /* x 1.5 */
            if (newBufSize > MAX_LINE_LENGTH) newBufSize = MAX_LINE_LENGTH;
            if (len+1 >= newBufSize) return GetLine_exceedMaxLineLength;

            newLineBuf = (char*) realloc(*lineBuf, newBufSize);
            if (newLineBuf == NULL) return GetLine_outOfMemory;

            *lineBuf = newLineBuf;
            *lineMax = (int)newBufSize;
        }

        if (c == '\n') break;
        (*lineBuf)[len++] = (char) c;
    }

    (*lineBuf)[len] = '\0';

    /* Ignore comment lines, which begin with a '#' character. */
    if (result == GetLine_ok && len > 0 && ((*lineBuf)[0] == '#')) {
        result = GetLine_comment;
    }

    return result;
}


/*
 * Converts one hexadecimal character to integer.
 * Returns -1 if the given character is not hexadecimal.
 */
static int charToHex(char c)
{
    int result = -1;
    if (c >= '0' && c <= '9') {
        result = (int) (c - '0');
    } else if (c >= 'A' && c <= 'F') {
        result = (int) (c - 'A') + 0x0a;
    } else if (c >= 'a' && c <= 'f') {
        result = (int) (c - 'a') + 0x0a;
    }
    return result;
}


/*
 * Converts canonical ASCII hexadecimal string `hashStr`
 * to the big endian binary representation in unsigned char array `dst`.
 *
 * Returns CanonicalFromString_invalidFormat if hashStr is not well formatted.
 * Returns CanonicalFromString_ok if hashStr is parsed successfully.
 */
static CanonicalFromStringResult XSUM_canonicalFromString(unsigned char* dst,
                                                          size_t dstSize,
                                                          const char* hashStr,
                                                          int reverseBytes)
{
    size_t i;
    for (i = 0; i < dstSize; ++i) {
        int h0, h1;
        size_t j = reverseBytes ? dstSize - i - 1 : i;

        h0 = charToHex(hashStr[j*2 + 0]);
        if (h0 < 0) return CanonicalFromString_invalidFormat;

        h1 = charToHex(hashStr[j*2 + 1]);
        if (h1 < 0) return CanonicalFromString_invalidFormat;

        dst[i] = (unsigned char) ((h0 << 4) | h1);
    }
    return CanonicalFromString_ok;
}


/*
 * Parse single line of xxHash checksum file.
 * Returns ParseLine_invalidFormat if the line is not well formatted.
 * Returns ParseLine_ok if the line is parsed successfully.
 * And members of XSUM_parseLine will be filled by parsed values.
 *
 *  - line must be terminated with '\0' without a trailing newline.
 *  - Since parsedLine.filename will point within given argument `line`,
 *    users must keep `line`s content when they are using parsedLine.
 *  - The line may be modified to carve up the information it contains.
 *
 * xxHash checksum lines should have the following format:
 *
 *      <8, 16, or 32 hexadecimal char> <space> <space> <filename...> <'\0'>
 *
 * or:
 *
 *      <algorithm> <' ('> <filename> <') = '> <hexstring> <'\0'>
 */
static ParseLineResult XSUM_parseLine1(ParsedLine* parsedLine, char* line, int rev, int needsUnescape, XSUM_U32 algoBitmask)
{
    char* const firstSpace = strchr(line, ' ');
    const char* hash_ptr;
    size_t hash_len;

    parsedLine->filename = NULL;
    parsedLine->algo = algo_xxh64; /* default - will be overwritten */
    if (firstSpace == NULL || !firstSpace[1]) return ParseLine_invalidFormat;

    if (firstSpace[1] == '(') {
        char* lastSpace = strrchr(line, ' ');
        if (lastSpace - firstSpace < 5) return ParseLine_invalidFormat;
        if (lastSpace[-1] != '=' || lastSpace[-2] != ' ' || lastSpace[-3] != ')') return ParseLine_invalidFormat;
        lastSpace[-3] = '\0'; /* Terminate the filename */
        *firstSpace = '\0';
        rev = strstr(line, "_LE") != NULL; /* was output little-endian */
        hash_ptr = lastSpace + 1;
        hash_len = strlen(hash_ptr);
        if (!memcmp(line, "XXH3", 4)) parsedLine->algo = algo_xxh3;
        if (!memcmp(line, "XXH32", 5)) parsedLine->algo = algo_xxh32;
        if (!memcmp(line, "XXH64", 5)) parsedLine->algo = algo_xxh64;
        if (!memcmp(line, "XXH128", 6)) parsedLine->algo = algo_xxh128;
    } else {
        hash_ptr = line;
        hash_len = (size_t)(firstSpace - line);
        if (hash_len==8) parsedLine->algo = algo_xxh32;
        if (hash_len==16) parsedLine->algo = algo_xxh64;
        if (hash_len==21) parsedLine->algo = algo_xxh3;
        if (hash_len==32) parsedLine->algo = algo_xxh128;
    }

    /* Check current CLI accepts the algorithm or not */
    if(! XSUM_algoBitmask_Accepts(algoBitmask, parsedLine->algo)) {
        return ParseLine_invalidFormat;
    }

    switch (hash_len)
    {
    case 8:
        if (parsedLine->algo != algo_xxh32) return ParseLine_invalidFormat;
        {   XXH32_canonical_t* xxh32c = &parsedLine->canonical.xxh32;
            if (XSUM_canonicalFromString(xxh32c->digest, sizeof(xxh32c->digest), hash_ptr, rev)
                != CanonicalFromString_ok) {
                return ParseLine_invalidFormat;
            }
            break;
        }

    case 16:
        if (parsedLine->algo != algo_xxh64 && parsedLine->algo != algo_xxh3) return ParseLine_invalidFormat;
        {   XXH64_canonical_t* xxh64c = &parsedLine->canonical.xxh64;
            if (XSUM_canonicalFromString(xxh64c->digest, sizeof(xxh64c->digest), hash_ptr, rev)
                != CanonicalFromString_ok) {
                return ParseLine_invalidFormat;
            }
            break;
        }

    case 21:
        if (parsedLine->algo != algo_xxh3) return ParseLine_invalidFormat;
        /* check XXH3 prefix*/
        if (memcmp(hash_ptr, "XXH3_", 5) != 0) return ParseLine_invalidFormat;
        {   XXH64_canonical_t* xxh64c = &parsedLine->canonical.xxh64;
            if (XSUM_canonicalFromString(xxh64c->digest, sizeof(xxh64c->digest), (const char*)hash_ptr+5, rev)
                != CanonicalFromString_ok) {
                return ParseLine_invalidFormat;
            }
            break;
        }

    case 32:
        if (parsedLine->algo != algo_xxh128) return ParseLine_invalidFormat;
        {   XXH128_canonical_t* xxh128c = &parsedLine->canonical.xxh128;
            if (XSUM_canonicalFromString(xxh128c->digest, sizeof(xxh128c->digest), hash_ptr, rev)
                != CanonicalFromString_ok) {
                return ParseLine_invalidFormat;
            }
            break;
        }

    default:
            return ParseLine_invalidFormat;
            break;
    }

    /* note : skipping second separation character, which can be anything,
     * allowing insertion of custom markers such as '*' */
    {
        char* const filename = firstSpace + 2;
        const size_t filenameLen = strlen(filename);
        if (needsUnescape) {
            char* const result = XSUM_filenameUnescape(filename, filenameLen);
            if (result == NULL) {
                return ParseLine_invalidFormat;
            }
        }
        parsedLine->filename = filename;
    }
    return ParseLine_ok;
}

static ParseLineResult XSUM_parseLine(ParsedLine* parsedLine, char* line, int rev, XSUM_U32 algoBitmask) {
    const int needsUnescape = XSUM_lineNeedsUnescape(line);
    if (needsUnescape) {
        ++line;
    }
    return XSUM_parseLine1(parsedLine, line, rev, needsUnescape, algoBitmask);
}



/*!
 * Parse xxHash checksum file.
 */
static void XSUM_parseFile1(ParseFileArg* XSUM_parseFileArg, int rev)
{
    const char* const inFileName = XSUM_parseFileArg->inFileName;
    ParseFileReport* const report = &XSUM_parseFileArg->report;

    unsigned long lineNumber = 0;
    memset(report, 0, sizeof(*report));

    while (!report->quit) {
        LineStatus lineStatus = LineStatus_hashFailed;
        ParsedLine parsedLine;
        memset(&parsedLine, 0, sizeof(parsedLine));

        lineNumber++;
        if (lineNumber == 0) {
            /* This is unlikely happen, but md5sum.c has this error check. */
            XSUM_log("%s: Error: Too many checksum lines\n", inFileName);
            report->quit = 1;
            break;
        }

        {   GetLineResult const XSUM_getLineResult =
                XSUM_getLine(&XSUM_parseFileArg->lineBuf,
                    &XSUM_parseFileArg->lineMax,
                    XSUM_parseFileArg->inFile);

            /* Ignore comment lines */
            if (XSUM_getLineResult == GetLine_comment) {
                continue;
            }

            if (XSUM_getLineResult != GetLine_ok) {
                if (XSUM_getLineResult == GetLine_eof) break;

                switch (XSUM_getLineResult)
                {
                case GetLine_ok:
                case GetLine_comment:
                case GetLine_eof:
                    /* These cases never happen.  See above XSUM_getLineResult related "if"s.
                       They exist just for make gcc's -Wswitch-enum happy. */
                    assert(0);
                    break;

                default:
                    XSUM_log("%s:%lu: Error: Unknown error.\n", inFileName, lineNumber);
                    break;

                case GetLine_exceedMaxLineLength:
                    XSUM_log("%s:%lu: Error: Line too long.\n", inFileName, lineNumber);
                    break;

                case GetLine_outOfMemory:
                    XSUM_log("%s:%lu: Error: Out of memory.\n", inFileName, lineNumber);
                    break;
                }
                report->quit = 1;
                break;
        }   }

        if (XSUM_parseLine(&parsedLine, XSUM_parseFileArg->lineBuf, rev, XSUM_parseFileArg->algoBitmask) != ParseLine_ok) {
            report->nImproperlyFormattedLines++;
            if (XSUM_parseFileArg->warn) {
                XSUM_log("%s:%lu: Error: Improperly formatted checksum line.\n",
                        inFileName, lineNumber);
            }
            continue;
        }

        report->nProperlyFormattedLines++;

        do {
            int const fnameIsStdin = (strcmp(parsedLine.filename, stdinFileName) == 0); /* "stdin" */
            FILE* const fp = fnameIsStdin ? stdin : XSUM_fopen(parsedLine.filename, "rb");
            if (fp == stdin) {
                XSUM_setBinaryMode(stdin);
            }
            if (fp == NULL) {
                lineStatus = LineStatus_failedToOpen;
                break;
            }
            lineStatus = LineStatus_hashFailed;
            {   Multihash const xxh = XSUM_hashStream(fp, parsedLine.algo, XSUM_parseFileArg->blockBuf, XSUM_parseFileArg->blockSize);
                switch (parsedLine.algo)
                {
                case algo_xxh32:
                    if (xxh.hash32 == XXH32_hashFromCanonical(&parsedLine.canonical.xxh32)) {
                        lineStatus = LineStatus_hashOk;
                    }
                    break;

                case algo_xxh64:
                case algo_xxh3:
                    if (xxh.hash64 == XXH64_hashFromCanonical(&parsedLine.canonical.xxh64)) {
                        lineStatus = LineStatus_hashOk;
                    }
                    break;

                case algo_xxh128:
                    if (XXH128_isEqual(xxh.hash128, XXH128_hashFromCanonical(&parsedLine.canonical.xxh128))) {
                        lineStatus = LineStatus_hashOk;
                    }
                    break;

                default:
                    break;
                }
            }
            if (fp != stdin) fclose(fp);
        } while (0);

        switch (lineStatus)
        {
        default:
            XSUM_log("%s: Error: Unknown error.\n", inFileName);
            report->quit = 1;
            break;

        case LineStatus_memoryError:
        case LineStatus_isDirectory:
            assert(0); /* Never happens on these paths */
            break;

        case LineStatus_failedToOpen:
            if (XSUM_parseFileArg->ignoreMissing) {
                report->nMissing++;
            } else {
                report->nOpenOrReadFailures++;
                if (!XSUM_parseFileArg->statusOnly) {
                    XSUM_output("%s:%lu: Could not open or read '%s': %s.\n",
                        inFileName, lineNumber, parsedLine.filename, strerror(errno));
                }
            }
            break;

        case LineStatus_hashOk:
        case LineStatus_hashFailed:
            {   int b = 1;
                if (lineStatus == LineStatus_hashOk) {
                    report->nMatchedChecksums++;
                    /* If --quiet is specified, don't display "OK" */
                    if (XSUM_parseFileArg->quiet) b = 0;
                } else {
                    report->nMismatchedChecksums++;
                }

                if (b && !XSUM_parseFileArg->statusOnly) {
                    const int needsEscape = XSUM_filenameNeedsEscape(parsedLine.filename);
                    if (needsEscape) {
                        XSUM_output("%c", '\\');
                    }
                    XSUM_printFilename(parsedLine.filename, needsEscape);
                    XSUM_output(": %s\n", lineStatus == LineStatus_hashOk ? "OK" : "FAILED");
            }   }
            break;
        }
    }   /* while (!report->quit) */
}


/*  Parse xxHash checksum file.
 *  Returns 1, if all procedures were succeeded.
 *  Returns 0, if any procedures was failed.
 *
 *  If strictMode != 0, return error code if any line is invalid.
 *  If statusOnly != 0, don't generate any output.
 *  If ignoreMissing != 0, ignore missing file.  But if no file was verified, returns 0 (failed).
 *  If warn != 0, print a warning message to stderr.
 *  If quiet != 0, suppress "OK" line.
 *
 *  "All procedures are succeeded" means:
 *    - Checksum file contains at least one line and less than SIZE_T_MAX lines.
 *    - All files are properly opened and read.
 *    - All hash values match with its content.
 *    - (strict mode) All lines in checksum file are consistent and well formatted.
 */
static int XSUM_checkFile(const char* inFileName,
                          const Display_endianness displayEndianness,
                          int strictMode,
                          int statusOnly,
                          int ignoreMissing,
                          int warn,
                          int quiet,
                          XSUM_U32 algoBitmask)
{
    int result = 0;
    FILE* inFile = NULL;
    ParseFileArg XSUM_parseFileArgBody;
    ParseFileArg* const XSUM_parseFileArg = &XSUM_parseFileArgBody;
    ParseFileReport* const report = &XSUM_parseFileArg->report;

    /* note: stdinName is special constant pointer.  It is not a string. */
    if (inFileName == stdinName) {
        /*
         * Note: Since we expect text input for xxhash -c mode,
         * we don't set binary mode for stdin.
         */
        inFileName = stdinFileName; /* "stdin" */
        inFile = stdin;
    } else {
        inFile = XSUM_fopen( inFileName, "rt" );
    }

    if (inFile == NULL) {
        XSUM_log("Error: Could not open '%s': %s\n", inFileName, strerror(errno));
        return 0;
    }

    XSUM_parseFileArg->inFileName  = inFileName;
    XSUM_parseFileArg->inFile      = inFile;
    XSUM_parseFileArg->lineMax     = DEFAULT_LINE_LENGTH;
    XSUM_parseFileArg->lineBuf     = (char*) malloc((size_t)XSUM_parseFileArg->lineMax);
    XSUM_parseFileArg->blockSize   = 64 * 1024;
    XSUM_parseFileArg->blockBuf    = (char*) malloc(XSUM_parseFileArg->blockSize);
    XSUM_parseFileArg->strictMode  = strictMode;
    XSUM_parseFileArg->statusOnly  = statusOnly;
    XSUM_parseFileArg->ignoreMissing = ignoreMissing;
    XSUM_parseFileArg->warn        = warn;
    XSUM_parseFileArg->quiet       = quiet;
    XSUM_parseFileArg->algoBitmask = algoBitmask;

    if ( (XSUM_parseFileArg->lineBuf == NULL)
      || (XSUM_parseFileArg->blockBuf == NULL) ) {
        XSUM_log("Error: : memory allocation failed \n");
        exit(1);
    }
    XSUM_parseFile1(XSUM_parseFileArg, displayEndianness != big_endian);

    free(XSUM_parseFileArg->blockBuf);
    free(XSUM_parseFileArg->lineBuf);

    if (inFile != stdin) fclose(inFile);

    /* Show error/warning messages.  All messages are copied from md5sum.c
     */
    if (report->nProperlyFormattedLines == 0) {
        XSUM_log("%s: no properly formatted xxHash checksum lines found\n", inFileName);
    } else if (!statusOnly) {
        if (report->nImproperlyFormattedLines) {
            XSUM_output("%lu %s improperly formatted\n"
                , report->nImproperlyFormattedLines
                , report->nImproperlyFormattedLines == 1 ? "line is" : "lines are");
        }
        if (report->nOpenOrReadFailures) {
            XSUM_output("%lu listed %s could not be read\n"
                , report->nOpenOrReadFailures
                , report->nOpenOrReadFailures == 1 ? "file" : "files");
        }
        if (report->nMismatchedChecksums) {
            XSUM_output("%lu computed %s did NOT match\n"
                , report->nMismatchedChecksums
                , report->nMismatchedChecksums == 1 ? "checksum" : "checksums");
    }   }

    /* Result (exit) code logic is copied from
     * gnu coreutils/src/md5sum.c digest_check() */
    result =   report->nProperlyFormattedLines != 0
            && report->nMismatchedChecksums == 0
            && report->nOpenOrReadFailures == 0
            && (!strictMode || report->nImproperlyFormattedLines == 0)
            && report->quit == 0;

    /* If "--ignore-missing" is enabled and there's no matched checksum, report it as error.
     * See https://github.com/coreutils/coreutils/blob/2f1cffe07ab0f0b4135a52d95f1689d7fc7f26c9/src/digest.c#L1325-L1328 */
    if (ignoreMissing && report->nMatchedChecksums == 0) {
        XSUM_output("%s: no file was verified\n", inFileName);
        result = 0;
    }

    return result;
}


static int XSUM_checkFiles(const char* fnList[], int fnTotal,
                           const Display_endianness displayEndianness,
                           int strictMode,
                           int statusOnly,
                           int ignoreMissing,
                           int warn,
                           int quiet,
                           XSUM_U32 algoBitmask)
{
    int ok = 1;

    /* Special case for stdinName "-",
     * note: stdinName is not a string.  It's special pointer. */
    if (fnTotal==0) {
        ok &= XSUM_checkFile(stdinName, displayEndianness, strictMode, statusOnly, ignoreMissing, warn, quiet, algoBitmask);
    } else {
        int fnNb;
        for (fnNb=0; fnNb<fnTotal; fnNb++)
            ok &= XSUM_checkFile(fnList[fnNb], displayEndianness, strictMode, statusOnly, ignoreMissing, warn, quiet, algoBitmask);
    }
    return ok ? 0 : 1;
}


/*
*
* Parse single filename from list to generate hashes for.
* Returns ParseLine_invalidFormat if the filename is not well formatted.
* Returns ParseLine_ok if the filename is parsed successfully.
*/
static ParseLineResult XSUM_parseGenLine(ParsedLine * parsedLine,
    char* filename)
{
    if (XSUM_lineNeedsUnescape(filename)) {
        size_t filenameLen;
        ++filename;
        filenameLen = strlen(filename);

        if (XSUM_filenameUnescape(filename, filenameLen) == NULL) {
            parsedLine->filename = NULL;
            return ParseLine_invalidFormat;
        }
    }

    parsedLine->filename = filename;

    return ParseLine_ok;
}

/*
 * Parse gen source file.
 */
static void XSUM_parseGenFile1(ParseFileArg* XSUM_parseGenArg,
    AlgoSelected hashType,
    Display_endianness displayEndianness,
    Display_convention convention)
{
    const char* const inFileName = XSUM_parseGenArg->inFileName;
    ParseFileReport* const report = &XSUM_parseGenArg->report;

    unsigned long lineNumber = 0;
    memset(report, 0, sizeof(*report));

    while (!report->quit) {
        LineStatus lineStatus = LineStatus_hashFailed;
        ParsedLine parsedLine;
        memset(&parsedLine, 0, sizeof(parsedLine));

        lineNumber++;
        if (lineNumber == 0) {
            /* This is unlikely happen, but md5sum.c has this error check. */
            XSUM_log("%s: Error: Too many generate lines\n", inFileName);
            report->quit = 1;
            break;
        }

        {   GetLineResult const XSUM_getLineResult = XSUM_getLine(&XSUM_parseGenArg->lineBuf,
            &XSUM_parseGenArg->lineMax,
            XSUM_parseGenArg->inFile);

        /* Ignore comment lines */
        if (XSUM_getLineResult == GetLine_comment) {
            continue;
        }

        if (XSUM_getLineResult != GetLine_ok) {
            if (XSUM_getLineResult == GetLine_eof) break;

            switch (XSUM_getLineResult)
            {
            case GetLine_ok:
            case GetLine_comment:
            case GetLine_eof:
                /* These cases never happen.  See above XSUM_getLineResult related "if"s.
                   They exist just for make gcc's -Wswitch-enum happy. */
                assert(0);
                break;

            default:
                XSUM_log("%s:%lu: Error: Unknown error.\n", inFileName, lineNumber);
                break;

            case GetLine_exceedMaxLineLength:
                XSUM_log("%s:%lu: Error: Line too long.\n", inFileName, lineNumber);
                break;

            case GetLine_outOfMemory:
                XSUM_log("%s:%lu: Error: Out of memory.\n", inFileName, lineNumber);
                break;
            }
            report->quit = 1;
            break;
        }   }

        if (XSUM_parseGenLine(&parsedLine, XSUM_parseGenArg->lineBuf) != ParseLine_ok) {
            report->nImproperlyFormattedLines++;
            if (XSUM_parseGenArg->warn) {
                XSUM_log("%s:%lu: Error: Improperly formatted line.\n",
                    inFileName, lineNumber);
            }
            continue;
        }

        report->nProperlyFormattedLines++;

        lineStatus = XSUM_hashFile(parsedLine.filename, hashType, displayEndianness, convention);

        switch (lineStatus)
        {
        default:
            XSUM_log("%s: Error: Unknown error.\n", parsedLine.filename);
            report->quit = 1;
            break;

        case LineStatus_memoryError:
            XSUM_log("\nError: Out of memory.\n");
            break;

        case LineStatus_failedToOpen:
        case LineStatus_isDirectory:
            if (XSUM_parseGenArg->ignoreMissing) {
                report->nMissing++;
            }
            else {
                report->nOpenOrReadFailures++;
                if (!XSUM_parseGenArg->statusOnly) {
                    XSUM_output(
                        lineStatus == LineStatus_failedToOpen ?
                            "%s:%lu: Could not open or read '%s': %s.\n" :
                            "%s:%lu: Target is a directory '%s'.\n", /* Leaves errno argument unconsumed */
                        inFileName, lineNumber, parsedLine.filename, strerror(errno));
                }
            }
            break;

        case LineStatus_hashOk:
        case LineStatus_hashFailed:
        break;
        }
    }   /* while (!report->quit) */
}


/*  Parse text file for list of targets.
 */
static int XSUM_generateFile(const char* inFileName,
    AlgoSelected hashType,
    Display_endianness displayEndianness,
    Display_convention convention,
    int statusOnly,
    int ignoreMissing,
    int warn)
{
    int result = 0;
    FILE* inFile = NULL;
    ParseFileArg XSUM_parseGenArgBody;
    ParseFileArg* const XSUM_parseGenArg = &XSUM_parseGenArgBody;
    ParseFileReport* const report = &XSUM_parseGenArg->report;

    /* note: stdinName is special constant pointer.  It is not a string. */
    if (inFileName == stdinName) {
        /*
         * Note: Since we expect text input for xxhash -c mode,
         * we don't set binary mode for stdin.
         */
        inFileName = stdinFileName; /* "stdin" */
        inFile = stdin;
    }
    else {
        inFile = XSUM_fopen(inFileName, "rt");
    }

    if (inFile == NULL) {
        XSUM_log("Error: Could not open '%s': %s\n", inFileName, strerror(errno));
        return 0;
    }

    XSUM_parseGenArg->inFileName = inFileName;
    XSUM_parseGenArg->inFile = inFile;
    XSUM_parseGenArg->lineMax = DEFAULT_LINE_LENGTH;
    XSUM_parseGenArg->lineBuf = (char*)malloc((size_t)XSUM_parseGenArg->lineMax);
    XSUM_parseGenArg->blockSize = 64 * 1024;
    XSUM_parseGenArg->blockBuf = (char*)malloc(XSUM_parseGenArg->blockSize);
    XSUM_parseGenArg->statusOnly = statusOnly;
    XSUM_parseGenArg->ignoreMissing = ignoreMissing;
    XSUM_parseGenArg->warn = warn;

    if ((XSUM_parseGenArg->lineBuf == NULL)
        || (XSUM_parseGenArg->blockBuf == NULL)) {
        XSUM_log("Error: : memory allocation failed \n");
        exit(1);
    }
    XSUM_parseGenFile1(XSUM_parseGenArg, hashType, displayEndianness, convention);

    free(XSUM_parseGenArg->blockBuf);
    free(XSUM_parseGenArg->lineBuf);

    if (inFile != stdin) fclose(inFile);

    /* Show error/warning messages.  All messages are copied from md5sum.c
     */
    if (report->nProperlyFormattedLines == 0) {
        XSUM_log("%s: no properly formatted filename lines found\n", inFileName);
    }
    if (report->nImproperlyFormattedLines) {
        XSUM_output("%lu %s improperly formatted\n"
            , report->nImproperlyFormattedLines
            , report->nImproperlyFormattedLines == 1 ? "line is" : "lines are");
    }
    if (report->nOpenOrReadFailures) {
        XSUM_output("%lu listed %s could not be read\n"
            , report->nOpenOrReadFailures
            , report->nOpenOrReadFailures == 1 ? "file" : "files");
    }
    /* Result (exit) code logic is copied from
     * gnu coreutils/src/md5sum.c digest_check() */
    result = report->nProperlyFormattedLines != 0
        && report->nOpenOrReadFailures == 0
        && (report->nImproperlyFormattedLines == 0)
        && report->quit == 0;

    return result;
}

static int XSUM_generateFiles(const char* fnList[], int fnTotal,
    AlgoSelected hashType,
    Display_endianness displayEndianness,
    Display_convention convention,
    int statusOnly,
    int ignoreMissing,
    int warn)
{
    int ok = 1;

    /* Special case for stdinName "-",
     * note: stdinName is not a string.  It's special pointer. */
    if (fnTotal == 0) {
        ok &= XSUM_generateFile(stdinName, hashType, displayEndianness, convention, statusOnly, ignoreMissing, warn);
    }
    else {
        int fnNb;
        for (fnNb = 0; fnNb < fnTotal; fnNb++)
            ok &= XSUM_generateFile(fnList[fnNb], hashType, displayEndianness, convention, statusOnly, ignoreMissing, warn);
    }
    return ok ? 0 : 1;
}


/* ********************************************************
*  Main
**********************************************************/

static int XSUM_usage(const char* exename)
{
    XSUM_log( WELCOME_MESSAGE(exename) );
    XSUM_log( "Create or verify checksums using fast non-cryptographic algorithm xxHash \n\n" );
    XSUM_log( "Usage: %s [options] [files] \n\n", exename);
    XSUM_log( "When no filename provided or when '-' is provided, uses stdin as input. \n");
    XSUM_log( "\nOptions: \n");
    XSUM_log( "  -H#                  select an xxhash algorithm (default: %i) \n", (int)g_defaultAlgo);
    XSUM_log( "                       0: XXH32 \n");
    XSUM_log( "                       1: XXH64 \n");
    XSUM_log( "                       2: XXH128 (also called XXH3_128bits) \n");
    XSUM_log( "                       3: XXH3 (also called XXH3_64bits) \n");
    XSUM_log( "  -c, --check          read xxHash checksum from [files] and check them \n");
    XSUM_log( "      --files-from     generate hashes for files listed in [files] \n");
    XSUM_log( "      --filelist       generate hashes for files listed in [files] \n");
    XSUM_log( "  -                    forces stdin as input, even if it's the console \n");
    XSUM_log( "  -h, --help           display a long help page about advanced options \n");
    return 0;
}

static int XSUM_usage_advanced(const char* exename)
{
    XSUM_usage(exename);
    XSUM_log( "\nAdvanced: \n");
    XSUM_log( "  -V, --version        Display version information \n");
    XSUM_log( "      --tag            Produce BSD-style checksum lines \n");
    XSUM_log( "      --little-endian  Checksum values use little endian convention (default: big endian) \n");
    XSUM_log( "      --binary         Read in binary mode \n");
    XSUM_log( "  -b                   Run benchmark \n");
    XSUM_log( "  -b#                  Bench only algorithm variant # \n");
    XSUM_log( "  -i#                  Number of times to run the benchmark (default: %i) \n", NBLOOPS_DEFAULT);
    XSUM_log( "  -q, --quiet          Don't display version header in benchmark mode \n");
    XSUM_log( "\n");
    XSUM_log( "The following five options are useful only when using lists in [files] to verify or generate checksums: \n");
    XSUM_log( "  -q, --quiet          Don't print OK for each successfully verified hash \n");
    XSUM_log( "      --status         Don't output anything, status code shows success \n");
    XSUM_log( "      --strict         Exit non-zero for improperly formatted lines in [files] \n");
    XSUM_log( "      --warn           Warn about improperly formatted lines in [files] \n");
    XSUM_log( "      --ignore-missing Don't fail or report status for missing files \n");
    return 0;
}

static int XSUM_badusage(const char* exename)
{
    XSUM_log("Wrong parameters\n\n");
    XSUM_usage(exename);
    return 1;
}

static void errorOut(const char* msg)
{
    XSUM_log("%s \n", msg);
    exit(1);
}

static const char* XSUM_lastNameFromPath(const char* path)
{
    const char* name = path;
    if (strrchr(name, '/')) name = strrchr(name, '/') + 1;
    if (strrchr(name, '\\')) name = strrchr(name, '\\') + 1; /* windows */
    return name;
}

/*!
 * XSUM_readU32FromCharChecked():
 * @return 0 if success, and store the result in *value.
 * Allows and interprets K, KB, KiB, M, MB and MiB suffix.
 * Will also modify `*stringPtr`, advancing it to position where it stopped reading.
 * @return 1 if an overflow error occurs
 */
static int XSUM_readU32FromCharChecked(const char** stringPtr, XSUM_U32* value)
{
    static const XSUM_U32 max = (((XSUM_U32)(-1)) / 10) - 1;
    XSUM_U32 result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9')) {
        if (result > max) return 1; /* overflow error */
        result *= 10;
        result += (XSUM_U32)(**stringPtr - '0');
        (*stringPtr)++ ;
    }
    if ((**stringPtr=='K') || (**stringPtr=='M')) {
        XSUM_U32 const maxK = ((XSUM_U32)(-1)) >> 10;
        if (result > maxK) return 1; /* overflow error */
        result <<= 10;
        if (**stringPtr=='M') {
            if (result > maxK) return 1; /* overflow error */
            result <<= 10;
        }
        (*stringPtr)++;  /* skip `K` or `M` */
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    *value = result;
    return 0;
}

/*!
 * XSUM_readU32FromChar():
 * @return: unsigned integer value read from input in `char` format.
 *  allows and interprets K, KB, KiB, M, MB and MiB suffix.
 *  Will also modify `*stringPtr`, advancing it to position where it stopped reading.
 *  Note: function will exit() program if digit sequence overflows
 */
static XSUM_U32 XSUM_readU32FromChar(const char** stringPtr) {
    XSUM_U32 result;
    if (XSUM_readU32FromCharChecked(stringPtr, &result)) {
        static const char errorMsg[] = "Error: numeric value too large";
        errorOut(errorMsg);
    }
    return result;
}

XSUM_API int XSUM_main(int argc, const char* argv[])
{
    int i, filenamesStart = 0;
    const char* const exename = XSUM_lastNameFromPath(argv[0]);
    int benchmarkMode = 0;
    int fileCheckMode = 0;
    int readFilenamesMode = 0;
    int strictMode    = 0;
    int statusOnly    = 0;
    int warn          = 0;
    int ignoreMissing = 0;
    XSUM_U32 algoBitmask   = algo_bitmask_all;
    int explicitStdin = 0;
    XSUM_U32 selectBenchIDs= 0;  /* 0 == use default k_testIDs_default, kBenchAll == bench all */
    static const XSUM_U32 kBenchAll = 99;
    size_t keySize    = XSUM_DEFAULT_SAMPLE_SIZE;
    AlgoSelected algo     = g_defaultAlgo;
    Display_endianness displayEndianness = big_endian;
    Display_convention convention = display_gnu;
    int nbIterations = NBLOOPS_DEFAULT;

    /* special case: xxhNNsum default to NN bits checksum */
    if (strstr(exename,  "xxh32sum") != NULL) { algo = g_defaultAlgo = algo_xxh32;  algoBitmask = algo_bitmask_xxh32;  }
    if (strstr(exename,  "xxh64sum") != NULL) { algo = g_defaultAlgo = algo_xxh64;  algoBitmask = algo_bitmask_xxh64;  }
    if (strstr(exename, "xxh128sum") != NULL) { algo = g_defaultAlgo = algo_xxh128; algoBitmask = algo_bitmask_xxh128; }
    if (strstr(exename,   "xxh3sum") != NULL) { algo = g_defaultAlgo = algo_xxh3;   algoBitmask = algo_bitmask_xxh3;  }

    for (i=1; i<argc; i++) {
        const char* argument = argv[i];
        assert(argument != NULL);

        if (!strcmp(argument, "--check")) { fileCheckMode = 1; continue; }
        if (!strcmp(argument, "--files-from")) { readFilenamesMode = 1; continue; }
        if (!strcmp(argument, "--filelist")) { readFilenamesMode = 1; continue; }
        if (!strcmp(argument, "--benchmark-all")) { benchmarkMode = 1; selectBenchIDs = kBenchAll; continue; }
        if (!strcmp(argument, "--bench-all")) { benchmarkMode = 1; selectBenchIDs = kBenchAll; continue; }
        if (!strcmp(argument, "--quiet")) { XSUM_logLevel--; continue; }
        if (!strcmp(argument, "--little-endian")) { displayEndianness = little_endian; continue; }
        if (!strcmp(argument, "--strict")) { strictMode = 1; continue; }
        if (!strcmp(argument, "--status")) { statusOnly = 1; continue; }
        if (!strcmp(argument, "--warn")) { warn = 1; continue; }
        if (!strcmp(argument, "--binary")) { continue; } /* Just ignore it. See https://github.com/Cyan4973/xxHash/issues/812 */
        if (!strcmp(argument, "--ignore-missing")) { ignoreMissing = 1; continue; }
        if (!strcmp(argument, "--help")) { return XSUM_usage_advanced(exename); }
        if (!strcmp(argument, "--version")) { XSUM_log(FULL_WELCOME_MESSAGE(exename)); XSUM_sanityCheck(); return 0; }
        if (!strcmp(argument, "--tag")) { convention = display_bsd; continue; }

        if (!strcmp(argument, "--")) {
            if (filenamesStart==0 && i!=argc-1) filenamesStart=i+1; /* only supports a continuous list of filenames */
            break;  /* treat rest of arguments as strictly file names */
        }
        if (*argument != '-') {
            if (filenamesStart==0) filenamesStart=i;   /* only supports a continuous list of filenames */
            break;  /* treat rest of arguments as strictly file names */
        }

        /* command selection */
        argument++;   /* note: argument[0] was =='-' */
        if (*argument == 0) explicitStdin = 1;

        while (*argument != 0) {
            switch(*argument)
            {
            /* Display version */
            case 'V':
                XSUM_log(FULL_WELCOME_MESSAGE(exename));
                XSUM_sanityCheck();
                return 0;

            /* Display help on XSUM_usage */
            case 'h':
                return XSUM_usage_advanced(exename);

            /* select hash algorithm */
            case 'H': argument++;
                switch(XSUM_readU32FromChar(&argument)) {
                    case 0 :
                    case 32: algo = algo_xxh32; break;
                    case 1 :
                    case 64: algo = algo_xxh64; break;
                    case 2 :
                    case 128: algo = algo_xxh128; break;
                    case 3 :
                        algo = algo_xxh3; break;
                    default:
                        return XSUM_badusage(exename);
                }
                break;

            /* File check mode */
            case 'c':
                fileCheckMode=1;
                argument++;
                break;

            /* Generate hash mode (tar style short form of --files-from)
            case 'T':
                readFilenamesMode = 2;
                argument++;
                break; */

            /* Warning mode (file check mode only, alias of "--warning") */
            case 'w':
                warn=1;
                argument++;
                break;

            /* Trigger benchmark mode */
            case 'b':
                argument++;
                benchmarkMode = 1;
                do {
                    if (*argument == ',') argument++;
                    selectBenchIDs = XSUM_readU32FromChar(&argument); /* select one specific test */
                    if ((int)selectBenchIDs < g_nbTestFunctions) {
                        g_testIDs[selectBenchIDs] = 1;
                    } else {
                        selectBenchIDs = kBenchAll;
                    }
                } while (*argument == ',');
                break;

            /* Modify Nb Iterations (benchmark only) */
            case 'i':
                argument++;
                nbIterations = (int)XSUM_readU32FromChar(&argument);
                break;

            /* Modify Block size (benchmark only) */
            case 'B':
                argument++;
                keySize = XSUM_readU32FromChar(&argument);
                break;

            /* Modify verbosity of benchmark output (hidden option) */
            case 'q':
                argument++;
                XSUM_logLevel--;
                break;

            default:
                return XSUM_badusage(exename);
            }
        }
    }   /* for(i=1; i<argc; i++) */

    /* Check benchmark mode */
    if (benchmarkMode) {
        XSUM_logVerbose(2, FULL_WELCOME_MESSAGE(exename) );
        XSUM_sanityCheck();
        g_nbIterations = nbIterations;
        if (selectBenchIDs == 0) memcpy(g_testIDs, k_testIDs_default, (size_t)g_nbTestFunctions);
        if (selectBenchIDs == kBenchAll) memset(g_testIDs, 1, (size_t)g_nbTestFunctions);
        if (filenamesStart==0) return XSUM_benchInternal(keySize);
        return XSUM_benchFiles(argv+filenamesStart, argc-filenamesStart);
    }

    /* Check if input is defined as console; trigger an error in this case */
    if ( (filenamesStart==0) && XSUM_isConsole(stdin) && !explicitStdin) {
        XSUM_log("No input provided \n");
        return 1;
    }

    if (filenamesStart==0) filenamesStart = argc;

    if (fileCheckMode) {
        return XSUM_checkFiles(argv+filenamesStart, argc-filenamesStart,
                          displayEndianness, strictMode, statusOnly, ignoreMissing, warn, (XSUM_logLevel < 2) /*quiet*/, algoBitmask);
    }

    if (readFilenamesMode) {
        return XSUM_generateFiles(argv + filenamesStart, argc - filenamesStart, algo, displayEndianness, convention, statusOnly, ignoreMissing, warn);
    }

    return XSUM_hashFiles(argv+filenamesStart, argc-filenamesStart, algo, displayEndianness, convention);

}
