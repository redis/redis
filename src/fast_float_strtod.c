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
        int diff = (s1[i] | 0x20) - (s2[i] | 0x20);
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
            if (remaining >= 8 && strncasecmp_local(p, "inity", 5) == 0) {
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
    if (mantissa > MAX_MANTISSA_FAST_PATH) return 0;
    
    /* Fast path: direct conversion */
    double value = (double)mantissa;

    if (exponent < 0) {
        value = value / powers_of_ten[-exponent];
    } else if (exponent > 0) {
        value = value * powers_of_ten[exponent];
    }

    if (negative) {
        value = -value;
    }

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
        return result;
    }
    
    /* Fall back to strtod for complex cases:
     * - Very large or very small exponents
     * - Too many digits (need precise rounding)
     * This ensures we get correctly-rounded results for edge cases. */
    return fast_float_strtod_fallback(nptr, len, endptr);
}

#define REDIS_TEST
#ifdef REDIS_TEST
#include <stdio.h>
#include "testhelp.h"

#define UNUSED(x) (void)(x)
#define COUNTOF(arr) (int)(sizeof(arr) / sizeof((arr)[0]))

typedef struct {
    const char *input;
    double expected;
    int failed;
} ff_testcase;

static int ff_eq(double a, double b) {
    if (isnan(a)) return isnan(b);
    if (isinf(a)) return isinf(b);
    return a == b;
}

static void run_ff_tests(ff_testcase *cases, int n) {
    for (int i = 0; i < n; i++) {
        const char *s = cases[i].input;
        size_t len = strlen(s);
        char *eptr; double d = fast_float_strtod(s, len, &eptr);
        int failed = ((size_t)(eptr - s) != len);
        int ok = (cases[i].failed == failed) && ff_eq(d, cases[i].expected);
        char descr[128];
        if (ok)
            snprintf(descr, sizeof(descr), "\"%s\" -> expect %s(%.17g)",
                     s, cases[i].failed ? "fail" : "ok", cases[i].expected);
        else
            snprintf(descr, sizeof(descr), "\"%s\" -> expect %s(%.17g) but got %s(%.17g)",
                     s, cases[i].failed ? "fail" : "ok", cases[i].expected, failed ? "fail" : "ok", d);
        test_cond(descr, ok);
    }
}

int fastFloatTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ff_testcase inf_valid[] = {
        {"inf", INFINITY, 0},
        {"INF", INFINITY, 0},
        {"Inf", INFINITY, 0},
        {"infinity", INFINITY, 0},
        {"INFINITY", INFINITY, 0},
        {"Infinity", INFINITY, 0},
        {"+inf", INFINITY, 0},
        {"-inf", -INFINITY, 0},
        {"+infinity", INFINITY, 0},
        {"-INFINITY", -INFINITY, 0},
    };
    run_ff_tests(inf_valid, COUNTOF(inf_valid));

    ff_testcase inf_invalid[] = {
        {"in", 0, 1},
        {"infin", INFINITY, 1},
        {"infini1", INFINITY, 1},
        {"infinitx", INFINITY, 1},
        {"infinityy", INFINITY, 1},
        {"info", INFINITY, 1},
        {"ina", 0, 1},
        {"INFI", INFINITY, 1},
        {"iNf0", INFINITY, 1},
    };
    run_ff_tests(inf_invalid, COUNTOF(inf_invalid));

    test_report();
    return 0;
}
#endif