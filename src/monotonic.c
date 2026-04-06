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
 * On x86_64 Linux systems, the hardware clock is enabled by default when the
 * CPU advertises the 'constant_tsc' flag in /proc/cpuinfo.  The TSC frequency
 * is determined by runtime calibration (measuring RDTSC ticks over a known
 * wall-clock interval) which is more robust than parsing the "model name" GHz
 * string, since not all CPUs include a frequency in that field.
 *
 * On ARM aarch64 systems, the hardware clock is enabled by default because the
 * ARM Generic Timer is architecturally guaranteed to be available and monotonic
 * on all ARMv8-A processors (see the "The Generic Timer in AArch64 state"
 * section of the Arm Architecture Reference Manual for Armv8-A).
 *
 * To use the processor clock on other architectures, either uncomment this line,
 * or build with
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

static void monotonicInit_x86linux(void) {
    const int bufflen = 256;
    char buf[bufflen];
    regex_t constTscRegex;
    int constantTsc = 0;
    int rc;

    /* Check that the constant_tsc flag is present.  This ensures the TSC
     * runs at a fixed rate regardless of CPU frequency scaling.  Without it,
     * the TSC is unreliable for timekeeping.  */
    rc = regcomp(&constTscRegex, "^flags\\s+:.* constant_tsc", REG_EXTENDED);
    assert(rc == 0);

    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
        while (fgets(buf, bufflen, cpuinfo) != NULL) {
            if (regexec(&constTscRegex, buf, 0, NULL, 0) == 0) {
                constantTsc = 1;
                break;
            }
        }
        fclose(cpuinfo);
    }
    regfree(&constTscRegex);

    if (!constantTsc) {
        fprintf(stderr, "monotonic: x86 linux, 'constant_tsc' flag not present\n");
        return;
    }

    /* Calibrate TSC frequency by measuring RDTSC ticks over a wall-clock
     * interval.  This is more robust than parsing the CPU model name for a
     * GHz string, which may be absent on some processors.  */
    struct timespec ts_start, ts_end;
    uint64_t tsc_start, tsc_end;

    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    tsc_start = __rdtsc();

    /* Sleep ~10 ms to accumulate enough ticks for an accurate measurement. */
    struct timespec req = {0, 10000000};
    nanosleep(&req, NULL);

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    tsc_end = __rdtsc();

    long long elapsed_ns = (long long)(ts_end.tv_sec - ts_start.tv_sec) * 1000000000LL
                         + (ts_end.tv_nsec - ts_start.tv_nsec);
    if (elapsed_ns <= 0) {
        fprintf(stderr, "monotonic: x86 linux, calibration elapsed time <= 0\n");
        return;
    }

    /* ticks_per_us = total_ticks / total_microseconds
     * Multiply first to preserve precision, then divide. */
    mono_ticksPerMicrosecond = (long)((tsc_end - tsc_start) * 1000 / elapsed_ns);

    if (mono_ticksPerMicrosecond == 0) {
        fprintf(stderr, "monotonic: x86 linux, unable to determine clock rate\n");
        return;
    }

    snprintf(monotonic_info_string, sizeof(monotonic_info_string),
            "X86 TSC @ %ld ticks/us", mono_ticksPerMicrosecond);
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
