#include "monotonic.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "redisassert.h"
#include <string.h>

/* The function pointer for clock retrieval.  */
monotime (*getMonotonicUs)(void) = NULL;

static char monotonic_info_string[32];


/* Using the processor clock (aka TSC on x86) can provide improved performance
 * throughout Redis wherever the monotonic clock is used.  The processor clock
 * is significantly faster than calling 'clock_gettime' (POSIX).  While this is
 * generally safe on modern systems, this link provides additional information
 * about use of the x86 TSC: http://oliveryang.net/2015/09/pitfalls-of-TSC-usage
 *
 * The hardware clock is enabled by default on x86-64 Linux and ARM aarch64:
 *   - aarch64: the ARM Generic Timer is architecturally guaranteed to be
 *     available and monotonic on all ARMv8-A processors (see “The Generic Timer
 *     in AArch64 state” in the Arm Architecture Reference Manual for Armv8-A).
 *   - x86-64: the TSC is used only when the CPU advertises an invariant TSC
 *     (constant_tsc + nonstop_tsc); otherwise we fall back to POSIX.  That
 *     runtime guard, plus the POSIX fallback, is what makes it safe to enable by
 *     default and brings x86 into line with aarch64.  Previously the x86 path
 *     was compiled in only under USE_PROCESSOR_CLOCK which, being off by
 *     default, meant stock x86 builds never used the TSC at all — and the old
 *     rate detection parsed the Intel-only "model name ... @ X.XGHz" brand
 *     string, which never matched AMD CPUs regardless.
 *
 * To use the processor clock on other architectures (e.g. RISC-V), either
 * uncomment this line, or build with
 *   CFLAGS="-DUSE_PROCESSOR_CLOCK"
#define USE_PROCESSOR_CLOCK
 */


#if defined(__x86_64__) && defined(__linux__)
#include <regex.h>
#include <x86intrin.h>

static long mono_ticksPerMicrosecond = 0;

static monotime getMonotonicUs_x86(void) {
    return __rdtsc() / mono_ticksPerMicrosecond;
}

/* Determine the number of TSC ticks in a micro-second by calibrating the TSC
 * against CLOCK_MONOTONIC.  An invariant TSC advances at a constant rate
 * regardless of the current core frequency, so a short one-time calibration is
 * accurate.  This is vendor neutral, unlike parsing the marketed base clock out
 * of the "model name" brand string (which is Intel-specific and biased).
 * Returns the tick rate in ticks/us, or 0 on failure. */
static long calibrateTscTicksPerMicrosecond(void) {
    struct timespec ts0, ts1;
    uint64_t c0, c1;
    long elapsed_ns;
    const long calib_ns = 20L * 1000 * 1000; /* 20 ms */

    clock_gettime(CLOCK_MONOTONIC, &ts0);
    c0 = __rdtsc();
    do {
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        elapsed_ns = (ts1.tv_sec - ts0.tv_sec) * 1000000000L +
                     (ts1.tv_nsec - ts0.tv_nsec);
    } while (elapsed_ns < calib_ns);
    c1 = __rdtsc();

    double ticks_per_ns = (double)(c1 - c0) / (double)elapsed_ns;
    return (long)(ticks_per_ns * 1000.0 + 0.5);
}

static void monotonicInit_x86linux(void) {
    /* The /proc/cpuinfo "flags" line can be long on modern CPUs (well over 1 KB
     * on recent x86 parts), so size the buffer generously — a truncated flags
     * line could otherwise hide the constant_tsc/nonstop_tsc tokens. */
    const int bufflen = 4096;
    char buf[bufflen];
    regex_t constTscRegex, nonstopTscRegex;
    const size_t nmatch = 2;
    regmatch_t pmatch[nmatch];
    int constantTsc = 0;
    int rc;

    /* Require an invariant TSC: both the constant_tsc flag (rate does not vary
     * with the core frequency) and the nonstop_tsc flag (TSC does not halt in
     * deep C-states) must be present.  Unlike the previous "model name : ... @
     * X.XGHz" parse, this is vendor neutral and matches AMD CPUs, whose brand
     * string carries no GHz token. */
    rc = regcomp(&constTscRegex, "^flags\\s+:.* constant_tsc", REG_EXTENDED);
    assert(rc == 0);
    rc = regcomp(&nonstopTscRegex, "^flags\\s+:.* nonstop_tsc", REG_EXTENDED);
    assert(rc == 0);

    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
        while (fgets(buf, bufflen, cpuinfo) != NULL) {
            if (regexec(&constTscRegex, buf, nmatch, pmatch, 0) == 0 &&
                regexec(&nonstopTscRegex, buf, nmatch, pmatch, 0) == 0) {
                constantTsc = 1;
                break;
            }
        }
        fclose(cpuinfo);
    }
    regfree(&constTscRegex);
    regfree(&nonstopTscRegex);

    if (!constantTsc) {
        fprintf(stderr, "monotonic: x86 linux, invariant TSC not present\n");
        return;
    }

    mono_ticksPerMicrosecond = calibrateTscTicksPerMicrosecond();
    if (mono_ticksPerMicrosecond <= 0) {
        fprintf(stderr, "monotonic: x86 linux, TSC calibration failed\n");
        return;
    }

    snprintf(monotonic_info_string, sizeof(monotonic_info_string),
            "X86 TSC @ %ld ticks/us (calibrated)", mono_ticksPerMicrosecond);
    getMonotonicUs = getMonotonicUs_x86;
}
#endif

#if defined(__aarch64__)
static long mono_ticksPerMicrosecond = 0;

/* Read the clock value.
 * CNTVCT_EL0 is a system counter register, that provides the monotonic
 * timestamp as a 64-bit count value. */
static inline uint64_t __cntvct(void) {
    uint64_t virtual_timer_value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(virtual_timer_value));
    return virtual_timer_value;
}

/* Read the Count-timer Frequency.
 * CNTFRQ_EL0 is a system counter register that provides the frequency (in Hz)
 * needed to convert ticks to microseconds. Together with CNTVCT_EL0, this enables
 * high-performance monotonic time measurement without system calls. */
static inline uint32_t cntfrq_hz(void) {
    uint64_t virtual_freq_value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(virtual_freq_value));
    return (uint32_t)virtual_freq_value;    /* top 32 bits are reserved */
}

static monotime getMonotonicUs_aarch64(void) {
    return __cntvct() / mono_ticksPerMicrosecond;
}

static void monotonicInit_aarch64(void) {
    mono_ticksPerMicrosecond = (long)cntfrq_hz() / 1000L / 1000L;
    if (mono_ticksPerMicrosecond == 0) {
        fprintf(stderr, "monotonic: aarch64, unable to determine clock rate\n");
        return;
    }

    snprintf(monotonic_info_string, sizeof(monotonic_info_string),
            "ARM CNTVCT @ %ld ticks/us", mono_ticksPerMicrosecond);
    getMonotonicUs = getMonotonicUs_aarch64;
}
#endif


#if defined(USE_PROCESSOR_CLOCK) && defined(__riscv) && defined(__linux__)
static long mono_ticksPerMicrosecond = 0;

static inline uint64_t read_mtime(void) {
    uint64_t val;
    asm volatile("csrr %0, time" : "=r"(val));
    return val;
}

/* Read RISC-V timebase-frequency, which may be stored as either a 64-bit
 * or 32-bit big-endian integer in the device tree.  */
static uint64_t get_timebase_frequency(void) {
    uint64_t freq = 0;
    FILE *fp = fopen("/proc/device-tree/cpus/timebase-frequency", "rb");
    if (!fp)
        return 0;

    uint8_t buf[8] = {0};
    size_t cnt = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    if (cnt == 8) {
        uint64_t be64 = 0;
        memcpy(&be64, buf, sizeof(be64));
        /* Convert be64 from big-endian to little-endian.  */
        freq = __builtin_bswap64(be64);
    } else if (cnt == 4) {
        uint32_t be32 = 0;
        memcpy(&be32, buf, sizeof(be32));
        /* Convert be32 from big-endian to little-endian.  */
        freq = __builtin_bswap32(be32);
    } else {
        /* Unable to read timebase-frequency.  */
        return 0;
    }

    return freq;
}

static monotime getMonotonicUs_riscv(void) {
    return read_mtime() / mono_ticksPerMicrosecond;
}

static void monotonicInit_riscv(void) {
    mono_ticksPerMicrosecond = (long)get_timebase_frequency() / 1000L / 1000L;
    if (mono_ticksPerMicrosecond == 0) {
        fprintf(stderr, "monotonic: riscv, unable to determine clock rate\n");
        return;
    }
    snprintf(monotonic_info_string, sizeof(monotonic_info_string),
            "RISC-V mtime @ %ld ticks/us", mono_ticksPerMicrosecond);
    getMonotonicUs = getMonotonicUs_riscv;
}
#endif

static monotime getMonotonicUs_posix(void) {
    /* clock_gettime() is specified in POSIX.1b (1993).  Even so, some systems
     * did not support this until much later.  CLOCK_MONOTONIC is technically
     * optional and may not be supported - but it appears to be universal.
     * If this is not supported, provide a system-specific alternate version.  */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

static void monotonicInit_posix(void) {
    /* Ensure that CLOCK_MONOTONIC is supported.  This should be supported
     * on any reasonably current OS.  If the assertion below fails, provide
     * an appropriate alternate implementation.  */
    struct timespec ts;
    int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0);

    snprintf(monotonic_info_string, sizeof(monotonic_info_string),
            "POSIX clock_gettime");
    getMonotonicUs = getMonotonicUs_posix;
}



const char * monotonicInit(void) {
    #if defined(__x86_64__) && defined(__linux__)
    if (getMonotonicUs == NULL) monotonicInit_x86linux();
    #endif

    #if defined(__aarch64__)
    if (getMonotonicUs == NULL) monotonicInit_aarch64();
    #endif

    #if defined(USE_PROCESSOR_CLOCK) && defined(__riscv) && defined(__linux__)
    if (getMonotonicUs == NULL) monotonicInit_riscv();
    #endif

    if (getMonotonicUs == NULL) monotonicInit_posix();

    return monotonic_info_string;
}

const char *monotonicInfoString(void) {
    return monotonic_info_string;
}

monotonic_clock_type monotonicGetType(void) {
    if (getMonotonicUs == getMonotonicUs_posix)
        return MONOTONIC_CLOCK_POSIX;
    return MONOTONIC_CLOCK_HW;
}
