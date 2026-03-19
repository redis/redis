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

#if __GNUC__ >= 3
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

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

/* Case-insensitive comparison for first n characters */
static int strncasecmp_local(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c1 = s1[i];
        char c2 = s2[i];
        /* Convert to lowercase */
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
    }
    return 0;
}

/* Parse inf/nan special values.
 * Returns 1 if parsed successfully, 0 otherwise.
 * On success, *endptr points past the parsed value. */
static inline int parse_infnan(const char *p, const char *pend, double *result, char **endptr) {
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

/* Parsed number structure */
typedef struct {
    uint64_t mantissa;      /* Mantissa digits as uint64 */
    int64_t exponent;       /* Decimal exponent (adjusted for decimal point) */
    int negative;           /* Sign flag */
    int valid;              /* Parse success flag */
    int too_many_digits;    /* More than 19 significant digits */
    const char *lastmatch;  /* Position after last parsed character */
} parsed_number_t;

/* Parse a decimal number string into components.
 * This follows the fast_float algorithm closely. */
static inline parsed_number_t parse_number_string(const char *p, const char *pend) {
    parsed_number_t result;
    result.mantissa = 0;
    result.exponent = 0;
    result.negative = 0;
    result.valid = 0;
    result.too_many_digits = 0;
    result.lastmatch = p;

    if (p == pend) return result;

    /* Parse sign */
    result.negative = (*p == '-');
    if (*p == '-' || *p == '+') {
        p++;
        if (p == pend) return result;
    }

    const char *start_digits = p;

    /* Parse integer part */
    uint64_t mantissa = 0;
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
    int64_t exponent = 0;
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
    if (digit_count == 0) return result;

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

    result.lastmatch = p;
    result.valid = 1;

    /* Handle overflow in mantissa: if we have too many digits,
     * we need to reparse more carefully */
    if (digit_count > MAX_DIGITS) {
        /* Skip leading zeros to get actual digit count */
        const char *s = start_digits;
        while (s != pend && (*s == '0' || *s == '.')) {
            if (*s == '0') digit_count--;
            s++;
        }

        if (digit_count > MAX_DIGITS) result.too_many_digits = 1;
    }

    result.mantissa = mantissa;
    result.exponent = exponent;
    return result;
}

/* Convert parsed number to double using fast path.
 * Returns 1 if fast path succeeded, 0 if fallback needed. */
static inline int compute_float_fast(parsed_number_t *pns, double *result) {
    /* Check if we're within fast path bounds */
    if (pns->too_many_digits) return 0;
    if (pns->exponent < MIN_EXPONENT_FAST_PATH) return 0;
    if (pns->exponent > MAX_EXPONENT_FAST_PATH) return 0;
    if (pns->mantissa > MAX_MANTISSA_FAST_PATH) return 0;

    /* Fast path: direct conversion */
    double value = (double)pns->mantissa;

    if (pns->exponent < 0) {
        value = value / powers_of_ten[-pns->exponent];
    } else if (pns->exponent > 0) {
        value = value * powers_of_ten[pns->exponent];
    }

    if (pns->negative) {
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
static inline int fast_float_try_fast(const char *nptr, const char *pend, double *result, char **endptr) {
    if (nptr == pend) {
        errno = EINVAL;
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }

    /* Parse the number string */
    parsed_number_t pns = parse_number_string(nptr, pend);
    if (pns.valid) {
        /* Try fast path first */
        if (compute_float_fast(&pns, result)) {
            if (endptr) *endptr = (char *)pns.lastmatch;
            return 1;
        }
    }

    /* Parse inf/nan special values */
    if (parse_infnan(nptr, pend, result, endptr)) {
        return 1;
    }

    return 0;
}

/* Convert string to double, with explicit length (string need NOT be null-terminated).
 * Falls back to strtod by copying to a temporary null-terminated buffer. */
double fast_float_strtod_n(const char *nptr, size_t len, char **endptr) {
    double result = 0.0;

    /* Use fast path for non-null-terminated strings */
    if (likely(fast_float_try_fast(nptr, nptr + len, &result, endptr)))
        return result;
    
    /* Fall back to strtod for complex cases. Since the input may not be
     * null-terminated, we must copy it into a temporary buffer. */
    char buf[4096];
    if (len > sizeof(buf) - 1)
        len = sizeof(buf) - 1;
    memcpy(buf,nptr,len);
    buf[len] = '\0';

    /* Fall back to strtod for complex cases:
     * - Very large or very small exponents
     * - Too many digits (need precise rounding)
     * This ensures we get correctly-rounded results for edge cases. */
    char *fallback_end;
    result = strtod(buf, &fallback_end);
    if (endptr) *endptr = (char *)nptr + (fallback_end - buf);

    /* If strtod failed to parse, set errno */
    if (fallback_end == buf) {
        errno = EINVAL;
    }

    return result;
}

/* Convert null-terminated string to double (no length needed).
 * Falls back to strtod directly since the string is already null-terminated. */
double fast_float_strtod(const char *nptr, char **endptr) {
    double result = 0.0;

    /* Use fast path for null-terminated strings */
    if (fast_float_try_fast(nptr, nptr + strlen(nptr), &result, endptr))
        return result;

    /* Fall back to strtod for complex cases:
     * - Very large or very small exponents
     * - Too many digits (need precise rounding)
     * This ensures we get correctly-rounded results for edge cases. */
    char *fallback_end;
    result = strtod(nptr, &fallback_end);
    if (endptr) *endptr = fallback_end;

    /* If strtod failed to parse, set errno */
    if (fallback_end == nptr) {
        errno = EINVAL;
    }

    return result;
}
