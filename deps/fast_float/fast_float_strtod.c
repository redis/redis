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
        if (strncasecmp_local(p, "nan", 3) == 0) {
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
        if (strncasecmp_local(p, "inf", 3) == 0) {
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
double fast_float_strtod(const char *nptr, size_t len, char **endptr) {
    double result = 0.0;
    const char *p = nptr;
    const char *pend = nptr + len;

    if (p == pend) {
        errno = EINVAL;
        if (endptr) *endptr = (char *)nptr;
        return 0.0;
    }

    /* Parse the number string */
    parsed_number_t pns = parse_number_string(p, pend);

    if (pns.valid) {
        /* Try fast path first */
        if (compute_float_fast(&pns, &result)) {
            if (endptr) *endptr = (char *)pns.lastmatch;
            return result;
        }
    } else {
         if (parse_infnan(nptr, pend, &result, endptr)) {
            return result; 
        }
    }

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
