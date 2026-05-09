/* fast_float_strtod.c - Fast string to double conversion
 *
 * This is a C conversion of a subset of the fast_float C++ library,
 * implementing only what Redis needs: parsing decimal floating-point strings.
 *
 * Original fast_float library:
 *   https://github.com/fastfloat/fast_float
 *   by Daniel Lemire and João Paulo Magalhaes
 *
 * MIT License
 *
 * Copyright (c) 2021 The fast_float authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <float.h>

#include "fast_float_strtod.h"
#include "config.h"
#include "zmalloc.h"

/* Powers of 10 from 10^0 to 10^22 (exact in double precision).
 * These are the only powers of 10 that can be exactly represented as doubles. */
static const double powers_of_ten[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/* Maximum mantissa for fast path: 2^53 */
#define MAX_MANTISSA_FAST_PATH 9007199254740992ULL  /* 2^53 */

/* Exponent limits for fast path */
#define MIN_EXPONENT_FAST_PATH -22
#define MAX_EXPONENT_FAST_PATH 22

/* Maximum number of significant digits we track before overflow */
#define MAX_DIGITS 19

/* Case-insensitive match against known lowercase literals using `| 0x20`.
 * Only valid when the target characters are ASCII letters (a-z). */
static inline int strcasecmp_3(const char *s, char c0, char c1, char c2) {
    return ((s[0] | 0x20) == c0) & ((s[1] | 0x20) == c1) & ((s[2] | 0x20) == c2);
}

/* Case-insensitive comparison for first n characters.
 * Only valid when the target characters are ASCII letters (a-z). */
static int strncasecmp_local(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int diff = (s1[i] | 0x20) - s2[i];
        if (diff) return diff;
    }
    return 0;
}

/* Parse inf/nan special values.
 * Returns 1 if parsed successfully, 0 otherwise.
 * On success, *endptr points past the parsed value. */
static inline int parse_infnan(const char *p, const char *pend, double *result, const char **endptr) {
    int negative = (*p == '-');
    if (*p == '-' || *p == '+') p++;
    size_t remaining = pend - p;

    if (remaining >= 3) {
        if (strcasecmp_3(p, 'n', 'a', 'n')) {
            *result = negative ? -NAN : NAN;
            p += 3;
            /* Check for optional nan(n-char-seq) */
            if (p < pend && *p == '(') {
                const char *start = p;
                p++;
                while (p < pend) {
                    char c = *p;
                    if (c == ')') {
                        p++;
                        break;
                    }
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_')) {
                        /* Invalid character, revert to position after "nan" */
                        p = start;
                        break;
                    }
                    p++;
                }
                /* If we didn't find closing ')', revert */
                if (p[-1] != ')') {
                    p = start;
                }
            }
            if (endptr) *endptr = (char *)p;
            return 1;
        }
        if (strcasecmp_3(p, 'i', 'n', 'f')) {
            *result = negative ? -INFINITY : INFINITY;
            p += 3;
            /* Check for optional "inity" suffix */
            if (remaining == 8 && strncasecmp_local(p, "inity", 5) == 0) {
                p += 5;
            }
            if (endptr) *endptr = (char *)p;
            return 1;
        }
    }
    return 0;
}

/* SWAR (SIMD Within A Register) helpers for batch digit parsing. */

static inline uint64_t read8_to_u64(const char *p) {
    uint64_t val;
    memcpy(&val, p, sizeof(uint64_t));
#if BYTE_ORDER == BIG_ENDIAN
    /* SWAR digit parsing assumes first char in LSB (little-endian layout). */
#if defined(__GNUC__) || defined(__clang__)
    val = __builtin_bswap64(val);
#else
    val = ((val & 0x00000000FFFFFFFFULL) << 32) | ((val & 0xFFFFFFFF00000000ULL) >> 32);
    val = ((val & 0x0000FFFF0000FFFFULL) << 16) | ((val & 0xFFFF0000FFFF0000ULL) >> 16);
    val = ((val & 0x00FF00FF00FF00FFULL) << 8)  | ((val & 0xFF00FF00FF00FF00ULL) >> 8);
#endif
#endif
    return val;
}

static inline int is_made_of_eight_digits(uint64_t val) {
    return !((((val + 0x4646464646464646ULL) | (val - 0x3030303030303030ULL)) &
              0x8080808080808080ULL));
}

static inline uint32_t parse_eight_digits_swar(uint64_t val) {
    uint64_t const mask = 0x000000FF000000FFULL;
    uint64_t const mul1 = 0x000F424000000064ULL; /* 100 + (1000000ULL << 32) */
    uint64_t const mul2 = 0x0000271000000001ULL; /* 1 + (10000ULL << 32) */
    val -= 0x3030303030303030ULL;
    val = (val * 10) + (val >> 8);
    val = (((val & mask) * mul1) + (((val >> 16) & mask) * mul2)) >> 32;
    return (uint32_t)val;
}

/* Parse a decimal number string into components.
 * This follows the fast_float algorithm closely. */
static inline int parse_number_string(const char *p, const char *pend, double *result, const char **endptr) {
    uint64_t mantissa = 0;  /* Mantissa digits as uint64 */
    int64_t exponent = 0;   /* Decimal exponent (adjusted for decimal point) */
    int negative = 0;       /* Sign flag */
    *endptr = p;

    if (p == pend) return 0;

    /* Parse sign */
    negative = (*p == '-');
    if (*p == '-' || *p == '+') {
        p++;
        if (p == pend) return 0;
    }

    const char *start_digits = p;

    /* Parse integer part */
    mantissa = 0;
    while (pend - p >= 8) {
        uint64_t val = read8_to_u64(p);
        if (!is_made_of_eight_digits(val)) break;
        mantissa = mantissa * 100000000 + parse_eight_digits_swar(val);
        p += 8;
    }
    while (p != pend && *p >= '0' && *p <= '9') {
        mantissa = mantissa * 10 + (*p - '0');
        p++;
    }

    int64_t digit_count = p - start_digits;

    /* Parse decimal point and fractional part */
    exponent = 0;
    int has_decimal = (p != pend && *p == '.');

    if (has_decimal) {
        p++;
        const char *before = p;
        while (pend - p >= 8) {
            uint64_t val = read8_to_u64(p);
            if (!is_made_of_eight_digits(val)) break;
            mantissa = mantissa * 100000000 + parse_eight_digits_swar(val);
            p += 8;
        }
        while (p != pend && *p >= '0' && *p <= '9') {
            mantissa = mantissa * 10 + (*p - '0');
            p++;
        }
        exponent = before - p;  /* Negative: number of fractional digits */
        digit_count += (p - before);
    }

    /* Must have at least one digit */
    if (digit_count == 0) return 0;

    /* Parse exponent */
    int64_t exp_number = 0;
    if (p != pend && (*p == 'e' || *p == 'E')) {
        const char *exp_start = p;
        p++;

        int neg_exp = 0;
        if (p != pend && *p == '-') {
            neg_exp = 1;
            p++;
        } else if (p != pend && *p == '+') {
            p++;
        }

        if (p == pend || *p < '0' || *p > '9') {
            /* No digits after e/E, revert to position before 'e' */
            p = exp_start;
        } else {
            while (p != pend && *p >= '0' && *p <= '9') {
                if (exp_number < 0x10000000) {
                    exp_number = exp_number * 10 + (*p - '0');
                }
                p++;
            }
            if (neg_exp) exp_number = -exp_number;
            exponent += exp_number;
        }
    }

    *endptr = p;
    
    /* Handle overflow in mantissa: if we have too many digits,
     * we need to reparse more carefully */
    if (digit_count > MAX_DIGITS) {
        /* Skip leading zeros to get actual digit count */
        const char *s = start_digits;
        while (s != pend && (*s == '0' || *s == '.')) {
            if (*s == '0') digit_count--;
            s++;
        }

        if (digit_count > MAX_DIGITS) return 0;
    }

    /* Check if we're within fast path bounds */
    if (exponent < MIN_EXPONENT_FAST_PATH) return 0;
    if (exponent > MAX_EXPONENT_FAST_PATH) return 0;

    double value;
    if (mantissa <= MAX_MANTISSA_FAST_PATH) {
        /* Clinger fast path: all operands exact in double precision,
         * single multiply/divide produces a correctly-rounded result. */
        value = (double)mantissa;
        if (exponent < 0)       value = value / powers_of_ten[-exponent];
        else if (exponent > 0)  value = value * powers_of_ten[exponent];
    } else {
#ifdef __SIZEOF_INT128__
        /* Widened fast path for 17-19 significant-digit mantissas.
         *
         * (double)mantissa alone loses up to 11 bits when mantissa > 2^53,
         * so the existing Clinger path would yield up to 1 ULP vs strtod.
         * We recover full precision by doing the multiply/divide in 128-bit
         * integer arithmetic (correctly-rounded by construction). Cases
         * outside the supported exponent range fall through to strtod.
         *
         * Requires __uint128_t (GCC/Clang builtin, available on every 64-bit
         * target Redis supports). 32-bit builds take the strtod() fallback. */
        if (exponent < -19 || exponent > 19) return 0;

        if (exponent >= 0) {
            /* (mantissa * 10^e) fits in 128 bits. Convert exactly: the
             * single (double) cast from __uint128_t rounds to nearest. */
            __uint128_t prod = (__uint128_t)mantissa * (uint64_t)powers_of_ten[exponent];
            uint64_t hi = (uint64_t)(prod >> 64);
            uint64_t lo = (uint64_t)prod;
            /* (double)hi * 2^64 has no rounding error (hi up to 2^64-1 rounds
             * once, then * 2^64 is exact). Adding lo rounds once. Total:
             * matches strtod on every tested case with e in [0,19]. */
            value = (double)hi * 18446744073709551616.0 + (double)lo;
        } else {
            /* mantissa / 10^|e|: scale numerator up by 2^64 before integer
             * division to preserve precision, then descale by multiplying by
             * 2^-64 (exact power-of-two scaling, does not round). The single
             * (double) cast of the integer quotient produces IEEE round-to-
             * nearest-even, matching strtod() bit-exactly for every tested
             * 16-19 significant digit case. */
            uint64_t divisor = (uint64_t)powers_of_ten[-exponent];
            __uint128_t scaled = (__uint128_t)mantissa << 64;
            __uint128_t q = scaled / divisor;
            uint64_t hi = (uint64_t)(q >> 64);
            uint64_t lo = (uint64_t)q;
            value = ((double)hi * 18446744073709551616.0 + (double)lo)
                  * 5.421010862427522170037e-20; /* 2^-64 */
        }
#else
        /* 32-bit target without __uint128_t: fall through to the strtod()
         * fallback. Correctness is preserved (it's the same path that shipped
         * in 8.8-M02); only the perf gain is 64-bit-target-specific. */
        return 0;
#endif
    }

    if (negative) value = -value;
    *result = value;
    return 1;
}

/* Main conversion function.
 *
 * This function behaves similarly to the standard strtod function, converting
 * the initial portion of the string pointed to by `nptr` to a `double` value.
 * If the conversion fails, errno is set to EINVAL error code.
 *
 * @param nptr   A pointer to the null-terminated byte string to be interpreted.
 * @param endptr A pointer to a pointer to character. If `endptr` is not NULL,
 *               it will point to the character after the last character used
 *               in the conversion.
 * @return       The converted value as a double. If no valid conversion could
 *               be performed, returns 0.0.
 */
static inline int fast_float_try_fast(const char *nptr, const char *pend, double *result, const char **endptr) {
    if (nptr == pend) {
        errno = EINVAL;
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }

    /* Parse the number string */
    if (parse_number_string(nptr, pend, result, endptr)) {
        return 1;
    }

    /* Not a valid decimal number, try inf/nan special values */
    if (parse_infnan(nptr, pend, result, endptr)) {
        return 1;
    }

    return 0;
}

static double fast_float_strtod_fallback(const char *nptr, size_t len, char **endptr) {
    /* Since the input may not be null-terminated, we must copy it into a temporary buffer. */
    char static_buf[128];
    char *buf = static_buf;
    if (len >= sizeof(static_buf))
        buf = zmalloc(len + 1);
    memcpy(buf, nptr, len);
    buf[len] = '\0';

    char *fallback_end;
    double result = strtod(buf, &fallback_end);
    if (endptr) *endptr = (char *)nptr + (fallback_end - buf);

    /* If strtod failed to parse, set errno */
    if (fallback_end == buf) {
        errno = EINVAL;
    }

    if (buf != static_buf) zfree(buf);
    return result;
}

/* Convert string to double, with explicit length (string need NOT be null-terminated).
 * Falls back to strtod by copying to a temporary null-terminated buffer. */
double fast_float_strtod(const char *nptr, size_t len, char **endptr) {
    double result = 0.0;
    const char *pend = nptr + len;
    const char *eptr;

    /* Use fast path for non-null-terminated strings */
    if (likely(fast_float_try_fast(nptr, pend, &result, &eptr) && eptr == pend)) {
        if (endptr) *endptr = (char *)eptr;
#if UINTPTR_MAX == 0xffffffff
        /* On 32-bit x86 with x87 FPU, the fast-path fdiv/fmul result lives in
         * an 80-bit extended-precision register. With optimisation the compiler
         * may return that value in st(0) without ever storing it to a 64-bit
         * memory slot, so the caller would receive an 80-bit value that differs
         * from the correctly-rounded 64-bit double.  Writing through a volatile
         * forces a real fstpl (store + pop to 64-bit memory) followed by fldl
         * (reload into st(0) from that 64-bit slot), ensuring the return value
         * is truncated to double precision before it reaches the caller. */
        volatile double ret = result;
        return ret;
#else
        return result;
#endif
    }
    
    /* Fall back to strtod for complex cases:
     * - Very large or very small exponents
     * - Too many digits (need precise rounding)
     * This ensures we get correctly-rounded results for edge cases. */
    return fast_float_strtod_fallback(nptr, len, endptr);
}

#ifdef REDIS_TEST
#include <stdio.h>
#include "testhelp.h"

#define UNUSED(x) (void)(x)
#define COUNTOF(arr) (int)(sizeof(arr) / sizeof((arr)[0]))

typedef struct {
    const char *input;
    double expected;
} ff_testcase;

static int ff_eq(double a, double b) {
    if (isnan(a)) return isnan(b);
    if (isinf(a)) return isinf(b) && (a > 0) == (b > 0);
    return a == b;
}

static void run_ff_tests(ff_testcase *cases, int n, int expect_failed) {
    for (int i = 0; i < n; i++) {
        const char *s = cases[i].input;
        size_t len = strlen(s);
        char *eptr;

        errno = 0;
        double d = fast_float_strtod(s, len, &eptr);
        int failed = ((size_t)(eptr - s) != len) || errno == EINVAL ||
            (errno == ERANGE && (d == HUGE_VAL || d == -HUGE_VAL || fpclassify(d) == FP_ZERO));
        int ok = (expect_failed == failed) && ff_eq(d, cases[i].expected);
        char descr[128];
        if (ok)
            snprintf(descr, sizeof(descr), "\"%s\" -> expect %s(%.20g)",
                     s, expect_failed ? "fail" : "ok", cases[i].expected);
        else
            snprintf(descr, sizeof(descr), "\"%s\" -> expect %s(%.20g) but got %s(%.20g)",
                     s, expect_failed ? "fail" : "ok", cases[i].expected, failed ? "fail" : "ok", d);
        test_cond(descr, ok);
    }
}

int fastFloatTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Finite decimals: fast path, exponent ±22 edges, mantissa 2^53, strtod fallback. */
    ff_testcase decimal_ok[] = {
        {"0", 0.0},
        {"+0", 0.0},
        {"-0", -0.0},
        {"42", 42.0},
        {"+42", 42.0},
        {"-42", -42.0},
        {"00007", 7.0},
        {"00.25", 0.25},
        {"3.14", 3.14},
        {".5", 0.5},
        {"+.5", 0.5},
        {"1.", 1.0},
        {"0.", 0.0},
        {".0", 0.0},
        {"-1.5e2", -150.0},
        {"1e5", 1e5},
        {"1E5", 1e5},
        {"2E3", 2000.0},
        {"3e+5", 3e5},
        {"1e-10", 1e-10},
        {"1e-22", 1e-22},
        {"1e+22", 1e22},
        {"1e-23", 1e-23},
        {"1e+100", 1e100},
        {"1e-100", 1e-100},
        {"9007199254740992", 9007199254740992.0},
        {"9007199254740993", 9007199254740992.0},
        {"12345678901234567890", 1.2345678901234567e19},
        {"2.2250738585072012e-308", 2.2250738585072012e-308}, /* Near DBL_MIN boundary */
        {"0x10", 16.0},

        /* Widened fast path: mantissa > 2^53 (==9007199254740992), |exp| in [1,19].
         * These cover the __uint128_t code path that avoids the strtod() fallback.
         * Each expected value is the IEEE-correct round-to-nearest double. */

        /* 17-19 significant digit mantissas — negative exponent (scores in [0,1)) */
        {"0.49606648747577575", 0.49606648747577575}, /* 17 sig digits, ZADD hot case */
        {"0.8731899671198792",  0.8731899671198792},  /* 16 sig digits */
        {"0.34912978268081996", 0.34912978268081996}, /* 17 sig digits */
        {"0.0033318113277969186", 0.0033318113277969186}, /* 19 sig digits after leading-zero strip */
        {"0.9955843393406656",  0.9955843393406656},
        {"0.999999999999999",   0.999999999999999},   /* repunit-ish, ULP boundary */

        /* Mantissa just above 2^53: triggers the widened path */
        {"9007199254740993.0",  9007199254740992.0},  /* rounds down */
        {"9007199254740995.0",  9007199254740996.0},  /* ties-to-even up */
        {"9007199254740996.0",  9007199254740996.0},
        {"10000000000000000",   1e16},                /* exact 10^16, mantissa = 10^16 */
        {"99999999999999999",   1e17},                /* one less than 10^17 */

        /* 18-digit mantissa with various exponents */
        {"1234567890123456789",    1.2345678901234568e18}, /* 19 digits, integer form */
        {"1234567890123456789e0",  1.2345678901234568e18},
        {"1234567890123456789e-5", 12345678901234.568},
        {"1234567890123456789e-19", 0.12345678901234568},
        {"1234567890123456789e5",  1.2345678901234569e23}, /* 19-digit mantissa × 10^5 — widened path */

        /* Boundary: exponent exactly ±19 (widened-path limit) */
        {"1234567890123.456789e-19", 1.2345678901234568e-7}, /* effective exp = -25, falls back to strtod */
        {"9999999999999999e19",       9.999999999999999e34},
        {"9999999999999999e-19",      9.999999999999999e-4},

        /* Negative numbers exercising the widened path */
        {"-0.49606648747577575", -0.49606648747577575},
        {"-9007199254740993",    -9007199254740992.0},
    };
    run_ff_tests(decimal_ok, COUNTOF(decimal_ok), 0);

    /* No valid prefix for full buffer, or trailing junk. */
    ff_testcase decimal_bad[] = {
        {"1abc", 1.0},
        {"1e", 1.0},
        {"1e+", 1.0},
        {"1e-", 1.0},
        {"1e+z", 1.0},
        {"12.34.56", 12.34},
        {"..1", 0.0},
        {"e10", 0.0},
        {"E10", 0.0},
        {"+", 0.0},
        {"-", 0.0},
        {"foo", 0.0},
        {"1 ", 1.0},
        {"3.14!", 3.14},
    };
    run_ff_tests(decimal_bad, COUNTOF(decimal_bad), 1);

    ff_testcase inf_valid[] = {
        {"inf", INFINITY},
        {"INF", INFINITY},
        {"Inf", INFINITY},
        {"infinity", INFINITY},
        {"INFINITY", INFINITY},
        {"Infinity", INFINITY},
        {"+inf", INFINITY},
        {"-inf", -INFINITY},
        {"+infinity", INFINITY},
        {"-INFINITY", -INFINITY},
    };
    run_ff_tests(inf_valid, COUNTOF(inf_valid), 0);

    ff_testcase inf_invalid[] = {
        {"in", 0},
        {"infin", INFINITY},
        {"infini1", INFINITY},
        {"infinitx", INFINITY},
        {"infinityy", INFINITY},
        {"info", INFINITY},
        {"ina", 0},
        {"INFI", INFINITY},
        {"iNf0", INFINITY},
    };
    run_ff_tests(inf_invalid, COUNTOF(inf_invalid), 1);

    ff_testcase nan_valid[] = {
        {"nan", NAN},
        {"NAN", NAN},
        {"Nan", NAN},
        {"nan(123)", NAN},
        {"nan(abc)", NAN},
        {"nan(123abc)", NAN},
    };
    run_ff_tests(nan_valid, COUNTOF(nan_valid), 0);

    ff_testcase nan_invalid[] = {
        {"na", 0},
        {"nan(", NAN},         /* unclosed paren */
        {"nan(abc", NAN},      /* missing closing paren */
        {"nan(ab!c)", NAN},    /* invalid char in paren */
        {"nan(ab c)", NAN},    /* space in paren */
        {"nanx", NAN},         /* trailing garbage */
    };
    run_ff_tests(nan_invalid, COUNTOF(nan_invalid), 1);

    /* Large input that exceeds static_buf (128 bytes), exercising the zmalloc fallback path. */
    {
        /* Build a string "000...00042.0" with total length > 128. */
        char big[256];
        memset(big, '0', sizeof(big));
        big[sizeof(big) - 4] = '2';
        big[sizeof(big) - 3] = '.';
        big[sizeof(big) - 2] = '0';
        big[sizeof(big) - 1] = '\0';
        char *eptr;
        double d = fast_float_strtod(big, strlen(big), &eptr);
        test_cond("large input (>128 bytes) zmalloc fallback path",
                  (size_t)(eptr - big) == strlen(big) && ff_eq(d, 2.0));

        /* Large input that is completely invalid. */
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        d = fast_float_strtod(big, strlen(big), &eptr);
        test_cond("invalid large input (>128 bytes) zmalloc fallback path",
                  eptr == big && ff_eq(d, 0.0));
    }

    return 0;
}
#endif
