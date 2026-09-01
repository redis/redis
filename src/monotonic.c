#include "monotonic.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "redisassert.h"
#include <string.h>
#include <errno.h>
#include <limits.h>

/* The function pointer for clock retrieval.  */
monotime (*getMonotonicUs)(void) = NULL;

static char monotonic_info_string[32];

/* Optional log callback, set via monotonicInit(). */
static void (*monotonic_logger)(const char *fmt, ...) __attribute__((format(printf, 1, 2))) = NULL;
#define monotonicLog(...) do { \
    if (monotonic_logger) monotonic_logger(__VA_ARGS__); \
} while (0)

/* Using the processor clock (aka TSC on x86) can provide improved performance
 * throughout Redis wherever the monotonic clock is used.  The processor clock
 * is significantly faster than calling 'clock_gettime' (POSIX).  While this is
 * generally safe on modern systems, this link provides additional information
 * about use of the x86 TSC: http://oliveryang.net/2015/09/pitfalls-of-TSC-usage
 *
 * On x86_64 Linux the hardware clock is enabled by default, with two safety
 * gates and a layered frequency-detection chain.  The reasoning, for future
 * generations:
 *
 * Reliability: rather than replicating the kernel's knowledge of broken TSCs
 * (known-bad CPU quirk lists, boot-time sync tests, the clocksource watchdog
 * that demotes a TSC that drifts at runtime), we simply require that the
 * kernel's ACTIVE clocksource is "tsc".  Machines where Linux distrusts the
 * TSC never satisfy that, so they transparently stay on the POSIX clock.
 * 'constant_tsc' in /proc/cpuinfo is additionally required (fixed tick rate
 * regardless of frequency scaling).
 *
 * Speed vs the VDSO: clock_gettime(CLOCK_MONOTONIC) on a tsc clocksource is
 * a fast VDSO call (no context switch), but it still costs ~2-3x a raw
 * RDTSC: the seqlock-protected read of the timekeeper data, the mult/shift
 * conversion, ns scaling and the libc call.  The monotonic clock is read
 * several times per command, so the difference is measurable end-to-end
 * once the network stops being the bottleneck (measured on bare-metal
 * Sapphire Rapids at 1KiB SET/GET, 2000 connections: +7-9% throughput at
 * 8-16 io-threads; flat at 0-4 io-threads, which are network-bound).
 *
 * Tick rate: the frequency advertised in the "model name" cpuinfo string is
 * the marketing value and can differ from the real TSC rate by a few tenths
 * of a percent (e.g. a "2.30GHz" part whose TSC ticks at ~2294 MHz) — a rate
 * error that size skews every measured duration and accumulates as drift.
 * The kernel measures the true rate at boot, but does not expose it to
 * userspace on mainline; the tsc_freq_khz sysfs file IS that kernel-measured
 * value on kernels that carry the patch.  Calibrating RDTSC against
 * CLOCK_MONOTONIC recovers the same kernel-measured rate indirectly, because
 * with a tsc clocksource CLOCK_MONOTONIC itself advances at the kernel's
 * calibrated TSC frequency.  Hence the chain: model-name parse, validated
 * against one measured sample (calibration wins when they disagree beyond
 * noise), then tsc_freq_khz, then median-of-3 calibration.
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

/* One calibration measurement: RDTSC ticks across a ~10ms nanosleep, bounded
 * by CLOCK_MONOTONIC readings.  Returns ticks-per-microsecond, or 0 on any
 * failure (clock error, non-monotonic TSC sample pair).  */
static long monotonicCalibrateOnce_x86linux(void) {
    struct timespec ts_start, ts_end;
    uint64_t tsc_start, tsc_end;

    if (clock_gettime(CLOCK_MONOTONIC, &ts_start) != 0) return 0;
    tsc_start = __rdtsc();

    /* Sleep ~10 ms to accumulate enough ticks for an accurate measurement.
     * Retry on EINTR to ensure we get a meaningful interval. */
    struct timespec req = {0, 10000000}, rem;
    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) return 0;
        req = rem;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &ts_end) != 0) return 0;
    tsc_end = __rdtsc();

    long long elapsed_ns = (long long)(ts_end.tv_sec - ts_start.tv_sec) * 1000000000LL
                         + (ts_end.tv_nsec - ts_start.tv_nsec);
    if (elapsed_ns <= 0) return 0;

    /* Invariant TSC on modern x86 is guaranteed to be monotonic across a
     * single core's context, but migration across sockets/cores with
     * misaligned TSC, virtualisation, or firmware quirks can still produce
     * a non-monotonic sample pair. Subtracting uint64_t in that case would
     * wrap to a huge value and yield a nonsense tick rate, so reject the
     * sample. */
    if (tsc_end <= tsc_start) return 0;

    /* ticks_per_us = total_ticks / total_microseconds
     * Multiply first to preserve precision, then divide. */
    return (long)((tsc_end - tsc_start) * 1000 / elapsed_ns);
}

static int longcmp(const void *a, const void *b) {
    long la = *(const long *)a, lb = *(const long *)b;
    return (la > lb) - (la < lb);
}

static void monotonicInit_x86linux(void) {
    const int bufflen = 256;
    char buf[bufflen];
    regex_t cpuGhzRegex, constTscRegex;
    const size_t nmatch = 2;
    regmatch_t pmatch[nmatch];
    int constantTsc = 0;
    long nominal_model = 0;
    int rc;

    /* Only use the TSC directly when the kernel itself trusts it: the active
     * clocksource is "tsc" exactly when Linux has verified the TSC is stable
     * on this machine (CPUs with known-unreliable TSCs, or a TSC the kernel
     * watchdog has marked unstable, never get it).  This defers the
     * reliability decision to the kernel instead of re-deriving it here.  */
    FILE *cs = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
    if (cs == NULL || fgets(buf, bufflen, cs) == NULL || strncmp(buf, "tsc", 3) != 0) {
        if (cs) fclose(cs);
        monotonicLog("x86 linux, kernel clocksource is not 'tsc'");
        return;
    }
    fclose(cs);

    /* Determine the number of TSC ticks in a micro-second from the CPU model
     * name.  This is a constant value matching the standard speed of the
     * processor.  On modern processors, this speed remains constant even
     * though the actual clock speed varies dynamically for each core.  */
    rc = regcomp(&cpuGhzRegex, "^model name\\s+:.*@ ([0-9.]+)GHz", REG_EXTENDED);
    assert(rc == 0);

    /* Also check that the constant_tsc flag is present.  This ensures the TSC
     * runs at a fixed rate regardless of CPU frequency scaling.  Without it,
     * the TSC is unreliable for timekeeping.  */
    rc = regcomp(&constTscRegex, "^flags\\s+:.* constant_tsc", REG_EXTENDED);
    assert(rc == 0);

    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
        while (fgets(buf, bufflen, cpuinfo) != NULL) {
            if (nominal_model == 0 &&
                regexec(&cpuGhzRegex, buf, nmatch, pmatch, 0) == 0) {
                buf[pmatch[1].rm_eo] = '\0';
                double ghz = atof(&buf[pmatch[1].rm_so]);
                nominal_model = (long)(ghz * 1000);
            }
            if (!constantTsc &&
                regexec(&constTscRegex, buf, 0, NULL, 0) == 0) {
                constantTsc = 1;
            }
            if (nominal_model != 0 && constantTsc) break;
        }
        fclose(cpuinfo);
    }
    regfree(&cpuGhzRegex);
    regfree(&constTscRegex);

    if (!constantTsc) {
        monotonicLog("x86 linux, 'constant_tsc' flag not present");
        return;
    }

    /* Source 1: the kernel-measured TSC frequency, on kernels that expose it.
     * This is the ground-truth rate (what the kernel itself uses for
     * timekeeping), so when present it wins outright and needs no
     * validation.  */
    FILE *khz = fopen("/sys/devices/system/cpu/cpu0/tsc_freq_khz", "r");
    if (khz != NULL) {
        long tsc_khz = 0;
        if (fscanf(khz, "%ld", &tsc_khz) == 1 && tsc_khz > 0)
            mono_ticksPerMicrosecond = tsc_khz / 1000;
        fclose(khz);
    }

    /* Source 2: the frequency advertised in the model name, cross-checked
     * against one measurement.  The advertised frequency is the marketing
     * value and the real TSC rate can differ by a few tenths of a percent
     * (e.g. a "2.30GHz" part whose TSC ticks at ~2294 MHz); a rate error
     * that size makes every measured duration proportionally wrong and
     * accumulates as drift.  The nominal value is used only when a measured
     * sample confirms it within calibration noise; on disagreement OR when
     * no valid measurement could be taken, fall through to full
     * calibration.  */
    if (mono_ticksPerMicrosecond == 0 && nominal_model != 0) {
        long measured = monotonicCalibrateOnce_x86linux();
        /* measured <= 0 means calibration itself failed (non-monotonic
         * sample pair) -- treat that the same as "unconfirmed" and fall
         * through, rather than encoding failure as a sentinel that then
         * participates in the diff*1000 multiplication below (which can
         * overflow a signed long and is undefined behavior in C). */
        if (measured > 0 && labs(measured - nominal_model) * 1000 <= nominal_model) { /* within 0.1% */
            mono_ticksPerMicrosecond = nominal_model;
        } else {
            monotonicLog("x86 linux, advertised clock rate "
                    "(%ld ticks/us) unconfirmed by the measured rate "
                    "(%ld ticks/us), using calibration",
                    nominal_model, measured);
        }
    }

    /* Source 3 (last resort): runtime calibration — measure RDTSC ticks over a known
     * CLOCK_MONOTONIC interval.  A single measurement can be perturbed by a
     * context switch between the clock read and the TSC read, so take three
     * and use the median.  */
    if (mono_ticksPerMicrosecond == 0) {
        long samples[3];
        int valid = 0;
        for (int i = 0; i < 3; i++) {
            long s = monotonicCalibrateOnce_x86linux();
            if (s > 0) samples[valid++] = s;
        }
        if (valid > 0) {
            qsort(samples, valid, sizeof(long), longcmp);
            /* Median for odd counts; mean of the two central samples when an
             * even number survived (with exactly two, samples[valid/2] alone
             * would systematically pick the higher one). */
            if (valid % 2 == 0)
                mono_ticksPerMicrosecond =
                    (samples[valid/2 - 1] + samples[valid/2]) / 2;
            else
                mono_ticksPerMicrosecond = samples[valid/2];
        }
    }

    if (mono_ticksPerMicrosecond == 0) {
        monotonicLog("x86 linux, unable to determine clock rate");
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
        monotonicLog("aarch64, unable to determine clock rate");
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
        monotonicLog("riscv, unable to determine clock rate");
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



const char * monotonicInit(void (*logger)(const char *fmt, ...)) {
    if (getMonotonicUs == NULL) monotonic_logger = logger;

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
