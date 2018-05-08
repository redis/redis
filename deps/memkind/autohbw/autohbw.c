/*
 * Copyright (C) 2015 - 2016 Intel Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice(s),
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice(s),
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
 * EVENT SHALL THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

///////////////////////////////////////////////////////////////////////////
// File   : autohbw.c
// Purpose: Library to automatically allocate HBW (MCDRAM)
// Author : Ruchira Sasanka (ruchira DOT sasanka AT intel DOT com)
// Date   : Jan 30, 2015
///////////////////////////////////////////////////////////////////////////

#include <memkind.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AUTOHBW_EXPORT __attribute__((visibility("default")))
#define AUTOHBW_INIT __attribute__((constructor))

//-2 = nothing is printed
//-1 = critical messages are printed
// 0 = no log messages for allocations are printed but INFO messages are printed
// 1 = a log message is printed for each allocation (Default)
// 2 = a log message is printed for each allocation with a backtrace
enum {
    ALWAYS = -1,
    INFO,
    ALLOC,
    VERBOSE
};

// Default is to print allocations
static int LogLevel = ALLOC;

// Allocations of size greater than low limit promoted to HBW memory.
// If there is a high limit specified, allocations larger than this limit
// will not be allocated in HBW.
static size_t HBWLowLimit = 1 * 1024 * 1024;
static size_t HBWHighLimit = -1ull;

// Whether we have initialized HBW arena of memkind library -- by making
// a dummy call to it. HBW arena (and hence any memkind_* call with kind
// HBW) must NOT be used until this flag is set true.
static bool MemkindInitDone = false;

// Following is the type of HBW memory that is allocated using memkind.
// By changing this type, this library can be used to allocate other
// types of memory types (e.g., MEMKIND_HUGETLB, MEMKIND_GBTLB,
// MEMKIND_HBW_HUGETLB etc.)
static memkind_t hbw_kind;

// API control for HBW allocations.
static bool isAutoHBWEnabled = true;

#define LOG(level, ...)                                                                            \
    do {                                                                                           \
        if (LogLevel >= level) {                                                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
        }                                                                                          \
    } while (0)

static bool isAllocInHBW(size_t size)
{
    if (!MemkindInitDone)
        return false;

    if (!isAutoHBWEnabled)
        return false;

    if (size < HBWLowLimit)
        return false;

    if (size > HBWHighLimit)
        return false;

    return true;
}

// Returns the limit in bytes using a limit value and a multiplier
// character like K, M, G
static size_t getLimit(size_t limit, char lchar)
{
    // Now read the trailing character (e.g., K, M, G)
    // Based on the character, determine the multiplier
    if ((limit > 0) && isalpha(lchar)) {
        long mult = 1;

        switch (toupper(lchar)) {
        case 'G':
            mult *= 1024;
        case 'M':
            mult *= 1024;
        case 'K':
            mult *= 1024;
        }

        // check for overflow, saturate at max
        if (limit >= -1ull / mult)
            return -1ull;

        return limit * mult;
    }

    return limit;
}

// Once HBWLowLimit (and HBWHighLimit) are set, call this method to
// inform the user about the size range of arrays that will be allocated
// in HBW
static void printLimits()
{
    // Inform according to the limits set
    if ((HBWLowLimit > 0) && (HBWHighLimit < -1ull)) {
        // if both high and low limits are specified, we use a range
        LOG(INFO, "INFO: Allocations between %ldK - %ldK will be allocated in "
                  "HBW. Set AUTO_HBW_SIZE=X:Y to change this limit.\n",
            HBWLowLimit / 1024, HBWHighLimit / 1024);
    } else if (HBWLowLimit > 0) {
        // if only a low limit is provided, use that
        LOG(INFO, "INFO: Allocations greater than %ldK will be allocated in HBW."
                  " Set AUTO_HBW_SIZE=X:Y to change this limit.\n",
            HBWLowLimit / 1024);
    } else if (HBWHighLimit < -1ull) {
        // if only a high limit is provided, use that
        LOG(INFO, "INFO: Allocations smaller than %ldK will be allocated in HBW. "
                  "Set AUTO_HBW_SIZE=X:Y to change this limit.\n",
            HBWHighLimit / 1024);
    } else {
        // none of limits is set to non-edge value, everything goes to HBW
        LOG(INFO, "INFO: All allocation will be done in HBW.");
    }
}

struct kind_name_t {
    memkind_t* kind;
    const char* name;
};

static struct kind_name_t named_kinds[] = {
    { &MEMKIND_DEFAULT, "memkind_default" },
    { &MEMKIND_HUGETLB, "memkind_hugetlb" },
    { &MEMKIND_INTERLEAVE, "memkind_interleave" },
    { &MEMKIND_HBW, "memkind_hbw" },
    { &MEMKIND_HBW_PREFERRED, "memkind_hbw_preferred" },
    { &MEMKIND_HBW_HUGETLB, "memkind_hbw_hugetlb" },
    { &MEMKIND_HBW_PREFERRED_HUGETLB, "memkind_hbw_preferred_hugetlb" },
    { &MEMKIND_HBW_GBTLB, "memkind_hbw_gbtlb" },
    { &MEMKIND_HBW_PREFERRED_GBTLB, "memkind_hbw_preferred_gbtlb" },
    { &MEMKIND_GBTLB, "memkind_gbtlb" },
    { &MEMKIND_HBW_INTERLEAVE, "memkind_hbw_interleave" },
};

static memkind_t get_kind_by_name(const char* name)
{
    int i;
    for (i = 0; i < sizeof(named_kinds) / sizeof(named_kinds[0]); ++i)
        if (strcasecmp(named_kinds[i].name, name) == 0)
            return *named_kinds[i].kind;

    return 0;
}

// Read from the environment and sets global variables
// Env variables are:
//   AUTO_HBW_SIZE = gives the size for auto HBW allocation
//   AUTO_HBW_LOG = gives logging level
static void setEnvValues()
{
    // STEP: Read the log level from the env variable. Do this early because
    //       printing depends on this
    char* log_str = getenv("AUTO_HBW_LOG");
    if (log_str && strlen(log_str)) {
        int level = atoi(log_str);
        LogLevel = level;
        LOG(ALWAYS, "INFO: Setting log level to %d\n", LogLevel);
    }

    if (LogLevel == INFO) {
        LOG(INFO, "INFO: HBW allocation stats will not be printed. "
                  "Set AUTO_HBW_LOG to enable.\n");
    } else if (LogLevel == ALLOC) {
        LOG(INFO, "INFO: Only HBW allocations will be printed. "
                  "Set AUTO_HBW_LOG to disable/enable.\n");
    } else if (LogLevel == VERBOSE) {
        LOG(INFO, "INFO: HBW allocation with backtrace info will be printed. "
                  "Set AUTO_HBW_LOG to disable.\n");
    }

    // Set the memory type allocated by this library. By default, it is
    // MEMKIND_HBW, but we can use this library to allocate other memory
    // types
    const char* memtype_str = getenv("AUTO_HBW_MEM_TYPE");
    if (memtype_str && strlen(memtype_str)) {
        // Find the memkind_t using the name the user has provided in the env variable
        memkind_t mty = get_kind_by_name(memtype_str);
        if (mty != 0) {
            hbw_kind = mty;
            LOG(INFO, "INFO: Setting HBW memory type to %s\n", memtype_str);
        } else {
            LOG(ALWAYS, "WARN: Memory type %s not recognized. Using default type\n", memtype_str);
        }
    }

    // STEP: Set the size limits (thresholds) for HBW allocation
    //
    // Reads the environment variable
    const char* size_str = getenv("AUTO_HBW_SIZE");
    if (size_str) {
        size_t lowlim = HBWLowLimit / 1024;
        size_t highlim = HBWHighLimit / 1024;
        char lowC = 'K', highC = 'K';

        if (size_str) {
            char* ptr = (char*)size_str;
            lowlim = strtoll(ptr, &ptr, 10);
            if (*ptr != 0 && *ptr != ':')
                lowC = *ptr++;
            else
                lowC = ' ';

            if (*ptr++ == ':') {
                highlim = strtoll(ptr, &ptr, 10);
                if (*ptr)
                    highC = *ptr;
                else
                    highC = ' ';
            }

            LOG(INFO, "INFO: lowlim=%zu(%c), highlim=%zu(%c)\n", lowlim, lowC, highlim, highC);
        }

        HBWLowLimit = getLimit(lowlim, lowC);
        HBWHighLimit = getLimit(highlim, highC);

        if (HBWLowLimit >= HBWHighLimit) {
            LOG(ALWAYS, "WARN: In AUTO_HBW_SIZE=X:Y, X cannot be greater or equal to Y. "
                        "None of allocations will use HBW memory.\n");
        }
    } else {
        // if the user did not specify any limits, inform that we are using
        // default limits
        LOG(INFO, "INFO: Using default values for array size thresholds. "
                  "Set AUTO_HBW_SIZE=X:Y to change.\n");
    }

    // inform the user about limits
    printLimits();
}

// This function is executed at library load time.
// Initialize HBW arena by making a dummy allocation/free at library load
// time. Until HBW initialization is complete, we must not call any
// allocation routines with HBW as kind.
static void AUTOHBW_INIT autohbw_load(void)
{
    // First set the default memory type this library allocates. This can
    // be overridden by env variable
    // Note: 'memkind_hbw_preferred' will allow falling back to DDR but
    //       'memkind_hbw will not'
    // Note: If HBM is not installed on a system, memkind_hbw_preferred call
    //       would fail. Therefore, we need to check for availability first.
    if (memkind_check_available(MEMKIND_HBW) != 0) {
        LOG(ALWAYS, "WARN: *** No HBM found in system. Will use default (DDR) "
                    "OR user specified type ***\n");
        hbw_kind = MEMKIND_DEFAULT;
    } else {
        hbw_kind = MEMKIND_HBW_PREFERRED;
    }

    // Read any env variables. This has to be done first because DbgLevel
    // is set using env variables and debug printing is used below
    setEnvValues(); // read any env variables

    LOG(INFO, "INFO: autohbw.so loaded!\n");

    // dummy HBW call to initialize HBW arena
    void* pp = memkind_malloc(hbw_kind, 16);
    if (pp == 0) {
        LOG(ALWAYS, "\t-HBW init call FAILED. "
                    "Is required memory type present on your system?\n");
        abort();
    }

    LOG(ALWAYS, "\t-HBW int call succeeded\n");
    memkind_free(hbw_kind, pp);

    MemkindInitDone = true; // enable HBW allocation
}

static void* MemkindMalloc(size_t size)
{
    LOG(VERBOSE, "In my memkind malloc sz:%ld ... ", size);

    bool useHbw = isAllocInHBW(size);
    memkind_t kind = useHbw ? hbw_kind : MEMKIND_DEFAULT;

    if (useHbw)
        LOG(VERBOSE, "\tHBW");

    void* ptr = memkind_malloc(kind, size);

    LOG(VERBOSE, "\tptr:%p\n", ptr);
    return ptr;
}

static void* MemkindCalloc(size_t nmemb, size_t size)
{
    LOG(VERBOSE, "In my memkind calloc sz:%ld ..", size * nmemb);

    bool useHbw = isAllocInHBW(size);
    memkind_t kind = useHbw ? hbw_kind : MEMKIND_DEFAULT;

    if (useHbw)
        LOG(VERBOSE, "\tHBW");

    void* ptr = memkind_calloc(kind, nmemb, size);

    LOG(VERBOSE, "\tptr:%p\n", ptr);
    return ptr;
}

static void* MemkindRealloc(void* ptr, size_t size)
{
    LOG(VERBOSE, "In my memkind realloc sz:%ld, p1:%p ..", size, ptr);

    bool useHbw = isAllocInHBW(size);
    memkind_t kind = useHbw ? hbw_kind : MEMKIND_DEFAULT;

    if (useHbw)
        LOG(VERBOSE, "\tHBW");

    void* nptr = memkind_realloc(kind, ptr, size);

    LOG(VERBOSE, "\tptr=%p\n", nptr);
    return nptr;
}

static int MemkindAlign(void** memptr, size_t alignment, size_t size)
{
    LOG(VERBOSE, "In my memkind align sz:%ld .. ", size);

    bool useHbw = isAllocInHBW(size);
    memkind_t kind = useHbw ? hbw_kind : MEMKIND_DEFAULT;

    if (useHbw)
        LOG(VERBOSE, "\tHBW");

    int ret = memkind_posix_memalign(kind, memptr, alignment, size);

    LOG(VERBOSE, "\tptr:%p\n", *memptr);
    return ret;
}

// memkind_free does not need the exact kind, if kind is 0. Then
// the library can figure out the proper kind itself.
static void MemkindFree(void* ptr)
{
    // avoid to many useless logs
    if (ptr)
        LOG(VERBOSE, "In my memkind free, ptr:%p\n", ptr);
    memkind_free(0, ptr);
}

//--------------------------------------------------------------------------
// ------------------ Public API of autohbw           ----------------------
//--------------------------------------------------------------------------

AUTOHBW_EXPORT void enableAutoHBW()
{
    isAutoHBWEnabled = true;
    LOG(INFO, "INFO: HBW allocations enabled by application (for this rank)\n");
}

AUTOHBW_EXPORT void disableAutoHBW()
{
    isAutoHBWEnabled = false;
    LOG(INFO, "INFO: HBW allocations disabled by application (for this rank)\n");
}

AUTOHBW_EXPORT void* malloc(size_t size)
{
    return MemkindMalloc(size);
}

AUTOHBW_EXPORT void* calloc(size_t nmemb, size_t size)
{
    return MemkindCalloc(nmemb, size);
}

AUTOHBW_EXPORT void* realloc(void* ptr, size_t size)
{
    return MemkindRealloc(ptr, size);
}

AUTOHBW_EXPORT int posix_memalign(void** memptr, size_t alignment, size_t size)
{
    return MemkindAlign(memptr, alignment, size);
}

// Warn about deprecated function usage.
AUTOHBW_EXPORT void* valloc(size_t size)
{
    LOG(ALWAYS, "use of deprecated valloc. Use posix_memalign instead\n");
    void* memptr = 0;
    size_t boundary = sysconf(_SC_PAGESIZE);
    int status = MemkindAlign(&memptr, boundary, size);
    if (status == 0 && memptr != 0)
        return memptr;

    return 0;
}

// Warn about deprecated function usage.
AUTOHBW_EXPORT void* memalign(size_t boundary, size_t size)
{
    LOG(ALWAYS, "use of deprecated memalign. Use posix_memalign instead\n");
    void* memptr = 0;
    int status = MemkindAlign(&memptr, boundary, size);
    if (status == 0 && memptr != 0)
        return memptr;

    return 0;
}

AUTOHBW_EXPORT void free(void* ptr)
{
    return MemkindFree(ptr);
}
