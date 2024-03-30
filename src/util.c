/*
 * Copyright (c) 2009-current, Redis Ltd.
 * Copyright (c) 2012, Twitter, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include "fpconv_dtoa.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <unistd.h>
#include <sys/time.h>
#include <float.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>

#include "util.h"
#include "sha256.h"
#include "config.h"

#define UNUSED(x) ((void)(x))

/* Glob-style pattern matching. */
static int stringmatchlen_impl(const char *pattern, int patternLen,
        const char *string, int stringLen, int nocase, int *skipLongerMatches)
{
    while(patternLen && stringLen) {
        switch(pattern[0]) {
        case '*':
            while (patternLen && pattern[1] == '*') {
                pattern++;
                patternLen--;
            }
            if (patternLen == 1)
                return 1; /* match */
            while(stringLen) {
                if (stringmatchlen_impl(pattern+1, patternLen-1,
                            string, stringLen, nocase, skipLongerMatches))
                    return 1; /* match */
                if (*skipLongerMatches)
                    return 0; /* no match */
                string++;
                stringLen--;
            }
            /* There was no match for the rest of the pattern starting
             * from anywhere in the rest of the string. If there were
             * any '*' earlier in the pattern, we can terminate the
             * search early without trying to match them to longer
             * substrings. This is because a longer match for the
             * earlier part of the pattern would require the rest of the
             * pattern to match starting later in the string, and we
             * have just determined that there is no match for the rest
             * of the pattern starting from anywhere in the current
             * string. */
            *skipLongerMatches = 1;
            return 0; /* no match */
            break;
        case '?':
            string++;
            stringLen--;
            break;
        case '[':
        {
            int not, match;

            pattern++;
            patternLen--;
            not = pattern[0] == '^';
            if (not) {
                pattern++;
                patternLen--;
            }
            match = 0;
            while(1) {
                if (pattern[0] == '\\' && patternLen >= 2) {
                    pattern++;
                    patternLen--;
                    if (pattern[0] == string[0])
                        match = 1;
                } else if (pattern[0] == ']') {
                    break;
                } else if (patternLen == 0) {
                    pattern--;
                    patternLen++;
                    break;
                } else if (patternLen >= 3 && pattern[1] == '-') {
                    int start = pattern[0];
                    int end = pattern[2];
                    int c = string[0];
                    if (start > end) {
                        int t = start;
                        start = end;
                        end = t;
                    }
                    if (nocase) {
                        start = tolower(start);
                        end = tolower(end);
                        c = tolower(c);
                    }
                    pattern += 2;
                    patternLen -= 2;
                    if (c >= start && c <= end)
                        match = 1;
                } else {
                    if (!nocase) {
                        if (pattern[0] == string[0])
                            match = 1;
                    } else {
                        if (tolower((int)pattern[0]) == tolower((int)string[0]))
                            match = 1;
                    }
                }
                pattern++;
                patternLen--;
            }
            if (not)
                match = !match;
            if (!match)
                return 0; /* no match */
            string++;
            stringLen--;
            break;
        }
        case '\\':
            if (patternLen >= 2) {
                pattern++;
                patternLen--;
            }
            /* fall through */
        default:
            if (!nocase) {
                if (pattern[0] != string[0])
                    return 0; /* no match */
            } else {
                if (tolower((int)pattern[0]) != tolower((int)string[0]))
                    return 0; /* no match */
            }
            string++;
            stringLen--;
            break;
        }
        pattern++;
        patternLen--;
        if (stringLen == 0) {
            while(*pattern == '*') {
                pattern++;
                patternLen--;
            }
            break;
        }
    }
    if (patternLen == 0 && stringLen == 0)
        return 1;
    return 0;
}

int stringmatchlen(const char *pattern, int patternLen,
        const char *string, int stringLen, int nocase) {
    int skipLongerMatches = 0;
    return stringmatchlen_impl(pattern,patternLen,string,stringLen,nocase,&skipLongerMatches);
}

int stringmatch(const char *pattern, const char *string, int nocase) {
    return stringmatchlen(pattern,strlen(pattern),string,strlen(string),nocase);
}

/* Fuzz stringmatchlen() trying to crash it with bad input. */
int stringmatchlen_fuzz_test(void) {
    char str[32];
    char pat[32];
    int cycles = 10000000;
    int total_matches = 0;
    while(cycles--) {
        int strlen = rand() % sizeof(str);
        int patlen = rand() % sizeof(pat);
        for (int j = 0; j < strlen; j++) str[j] = rand() % 128;
        for (int j = 0; j < patlen; j++) pat[j] = rand() % 128;
        total_matches += stringmatchlen(pat, patlen, str, strlen, 0);
    }
    return total_matches;
}


/* Convert a string representing an amount of memory into the number of
 * bytes, so for instance memtoull("1Gb") will return 1073741824 that is
 * (1024*1024*1024).
 *
 * On parsing error, if *err is not NULL, it's set to 1, otherwise it's
 * set to 0. On error the function return value is 0, regardless of the
 * fact 'err' is NULL or not. */
unsigned long long memtoull(const char *p, int *err) {
    const char *u;
    char buf[128];
    long mul; /* unit multiplier */
    unsigned long long val;
    unsigned int digits;

    if (err) *err = 0;

    /* Search the first non digit character. */
    u = p;
    if (*u == '-') {
        if (err) *err = 1;
        return 0;
    }
    while(*u && isdigit(*u)) u++;
    if (*u == '\0' || !strcasecmp(u,"b")) {
        mul = 1;
    } else if (!strcasecmp(u,"k")) {
        mul = 1000;
    } else if (!strcasecmp(u,"kb")) {
        mul = 1024;
    } else if (!strcasecmp(u,"m")) {
        mul = 1000*1000;
    } else if (!strcasecmp(u,"mb")) {
        mul = 1024*1024;
    } else if (!strcasecmp(u,"g")) {
        mul = 1000L*1000*1000;
    } else if (!strcasecmp(u,"gb")) {
        mul = 1024L*1024*1024;
    } else {
        if (err) *err = 1;
        return 0;
    }

    /* Copy the digits into a buffer, we'll use strtoll() to convert
     * the digit (without the unit) into a number. */
    digits = u-p;
    if (digits >= sizeof(buf)) {
        if (err) *err = 1;
        return 0;
    }
    memcpy(buf,p,digits);
    buf[digits] = '\0';

    char *endptr;
    errno = 0;
    val = strtoull(buf,&endptr,10);
    if ((val == 0 && errno == EINVAL) || *endptr != '\0') {
        if (err) *err = 1;
        return 0;
    }
    return val*mul;
}

/* Search a memory buffer for any set of bytes, like strpbrk().
 * Returns pointer to first found char or NULL.
 */
const char *mempbrk(const char *s, size_t len, const char *chars, size_t charslen) {
    for (size_t j = 0; j < len; j++) {
        for (size_t n = 0; n < charslen; n++)
            if (s[j] == chars[n]) return &s[j];
    }

    return NULL;
}

/* Modify the buffer replacing all occurrences of chars from the 'from'
 * set with the corresponding char in the 'to' set. Always returns s.
 */
char *memmapchars(char *s, size_t len, const char *from, const char *to, size_t setlen) {
    for (size_t j = 0; j < len; j++) {
        for (size_t i = 0; i < setlen; i++) {
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

/* Return the number of digits of 'v' when converted to string in radix 10.
 * See ll2string() for more information. */
uint32_t digits10(uint64_t v) {
    if (v < 10) return 1;
    if (v < 100) return 2;
    if (v < 1000) return 3;
    if (v < 1000000000000UL) {
        if (v < 100000000UL) {
            if (v < 1000000) {
                if (v < 10000) return 4;
                return 5 + (v >= 100000);
            }
            return 7 + (v >= 10000000UL);
        }
        if (v < 10000000000UL) {
            return 9 + (v >= 1000000000UL);
        }
        return 11 + (v >= 100000000000UL);
    }
    return 12 + digits10(v / 1000000000000UL);
}

/* Like digits10() but for signed values. */
uint32_t sdigits10(int64_t v) {
    if (v < 0) {
        /* Abs value of LLONG_MIN requires special handling. */
        uint64_t uv = (v != LLONG_MIN) ?
                      (uint64_t)-v : ((uint64_t) LLONG_MAX)+1;
        return digits10(uv)+1; /* +1 for the minus. */
    } else {
        return digits10(v);
    }
}

/* Convert a long long into a string. Returns the number of
 * characters needed to represent the number.
 * If the buffer is not big enough to store the string, 0 is returned. */
int ll2string(char *dst, size_t dstlen, long long svalue) {
    unsigned long long value;
    int negative = 0;

    /* The ull2string function with 64bit unsigned integers for simplicity, so
     * we convert the number here and remember if it is negative. */
    if (svalue < 0) {
        if (svalue != LLONG_MIN) {
            value = -svalue;
        } else {
            value = ((unsigned long long) LLONG_MAX)+1;
        }
        if (dstlen < 2)
            goto err;
        negative = 1;
        dst[0] = '-';
        dst++;
        dstlen--;
    } else {
        value = svalue;
    }

    /* Converts the unsigned long long value to string*/
    int length = ull2string(dst, dstlen, value);
    if (length == 0) return 0;
    return length + negative;

err:
    /* force add Null termination */
    if (dstlen > 0)
        dst[0] = '\0';
    return 0;
}

/* Convert a unsigned long long into a string. Returns the number of
 * characters needed to represent the number.
 * If the buffer is not big enough to store the string, 0 is returned.
 *
 * Based on the following article (that apparently does not provide a
 * novel approach but only publicizes an already used technique):
 *
 * https://www.facebook.com/notes/facebook-engineering/three-optimization-tips-for-c/10151361643253920 */
int ull2string(char *dst, size_t dstlen, unsigned long long value) {
    static const char digits[201] =
        "0001020304050607080910111213141516171819"
        "2021222324252627282930313233343536373839"
        "4041424344454647484950515253545556575859"
        "6061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";

    /* Check length. */
    uint32_t length = digits10(value);
    if (length >= dstlen) goto err;;

    /* Null term. */
    uint32_t next = length - 1;
    dst[next + 1] = '\0';
    while (value >= 100) {
        int const i = (value % 100) * 2;
        value /= 100;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
        next -= 2;
    }

    /* Handle last 1-2 digits. */
    if (value < 10) {
        dst[next] = '0' + (uint32_t) value;
    } else {
        int i = (uint32_t) value * 2;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
    }
    return length;
err:
    /* force add Null termination */
    if (dstlen > 0)
        dst[0] = '\0';
    return 0;
}

/* Convert a string into a long long. Returns 1 if the string could be parsed
 * into a (non-overflowing) long long, 0 otherwise. The value will be set to
 * the parsed value when appropriate.
 *
 * Note that this function demands that the string strictly represents
 * a long long: no spaces or other characters before or after the string
 * representing the number are accepted, nor zeroes at the start if not
 * for the string "0" representing the zero number.
 *
 * Because of its strictness, it is safe to use this function to check if
 * you can convert a string into a long long, and obtain back the string
 * from the number without any loss in the string representation. */
int string2ll(const char *s, size_t slen, long long *value) {
    const char *p = s;
    size_t plen = 0;
    int negative = 0;
    unsigned long long v;

    /* A string of zero length or excessive length is not a valid number. */
    if (plen == slen || slen >= LONG_STR_SIZE)
        return 0;

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0') {
        if (value != NULL) *value = 0;
        return 1;
    }

    /* Handle negative numbers: just set a flag and continue like if it
     * was a positive number. Later convert into negative. */
    if (p[0] == '-') {
        negative = 1;
        p++; plen++;

        /* Abort on only a negative sign. */
        if (plen == slen)
            return 0;
    }

    /* First digit should be 1-9, otherwise the string should just be 0. */
    if (p[0] >= '1' && p[0] <= '9') {
        v = p[0]-'0';
        p++; plen++;
    } else {
        return 0;
    }

    /* Parse all the other digits, checking for overflow at every step. */
    while (plen < slen && p[0] >= '0' && p[0] <= '9') {
        if (v > (ULLONG_MAX / 10)) /* Overflow. */
            return 0;
        v *= 10;

        if (v > (ULLONG_MAX - (p[0]-'0'))) /* Overflow. */
            return 0;
        v += p[0]-'0';

        p++; plen++;
    }

    /* Return if not all bytes were used. */
    if (plen < slen)
        return 0;

    /* Convert to negative if needed, and do the final overflow check when
     * converting from unsigned long long to long long. */
    if (negative) {
        if (v > ((unsigned long long)(-(LLONG_MIN+1))+1)) /* Overflow. */
            return 0;
        if (value != NULL) *value = -v;
    } else {
        if (v > LLONG_MAX) /* Overflow. */
            return 0;
        if (value != NULL) *value = v;
    }
    return 1;
}

/* Helper function to convert a string to an unsigned long long value.
 * The function attempts to use the faster string2ll() function inside
 * Redis: if it fails, strtoull() is used instead. The function returns
 * 1 if the conversion happened successfully or 0 if the number is
 * invalid or out of range. */
int string2ull(const char *s, unsigned long long *value) {
    long long ll;
    if (string2ll(s,strlen(s),&ll)) {
        if (ll < 0) return 0; /* Negative values are out of range. */
        *value = ll;
        return 1;
    }
    errno = 0;
    char *endptr = NULL;
    *value = strtoull(s,&endptr,10);
    if (errno == EINVAL || errno == ERANGE || !(*s != '\0' && *endptr == '\0'))
        return 0; /* strtoull() failed. */
    return 1; /* Conversion done! */
}

/* Convert a string into a long. Returns 1 if the string could be parsed into a
 * (non-overflowing) long, 0 otherwise. The value will be set to the parsed
 * value when appropriate. */
int string2l(const char *s, size_t slen, long *lval) {
    long long llval;

    if (!string2ll(s,slen,&llval))
        return 0;

    if (llval < LONG_MIN || llval > LONG_MAX)
        return 0;

    *lval = (long)llval;
    return 1;
}

/* return 1 if c>= start && c <= end, 0 otherwise*/
static int safe_is_c_in_range(char c, char start, char end) {
    if (c >= start && c <= end) return 1;
    return 0;
}

static int base_16_char_type(char c) {
    if (safe_is_c_in_range(c, '0', '9')) return 0;
    if (safe_is_c_in_range(c, 'a', 'f')) return 1;
    if (safe_is_c_in_range(c, 'A', 'F')) return 2;
    return -1;
}

/** This is an async-signal safe version of string2l to convert unsigned long to string.
 * The function translates @param src until it reaches a value that is not 0-9, a-f or A-F, or @param we read slen characters.
 * On successes writes the result to @param result_output and returns 1.
 * if the string represents an overflow value, return -1. */
int string2ul_base16_async_signal_safe(const char *src, size_t slen, unsigned long *result_output) {
    static char ascii_to_dec[] = {'0', 'a' - 10, 'A' - 10};

    int char_type = 0;
    size_t curr_char_idx = 0;
    unsigned long result = 0;
    int base = 16;
    while ((-1 != (char_type = base_16_char_type(src[curr_char_idx]))) &&
            curr_char_idx < slen) {
        unsigned long curr_val = src[curr_char_idx] - ascii_to_dec[char_type];
        if ((result > ULONG_MAX / base) || (result > (ULONG_MAX - curr_val)/base)) /* Overflow. */
            return -1;
        result = result * base + curr_val;
        ++curr_char_idx;
    }

    *result_output = result;
    return 1;
}

/* Convert a string into a double. Returns 1 if the string could be parsed
 * into a (non-overflowing) double, 0 otherwise. The value will be set to
 * the parsed value when appropriate.
 *
 * Note that this function demands that the string strictly represents
 * a double: no spaces or other characters before or after the string
 * representing the number are accepted. */
int string2ld(const char *s, size_t slen, long double *dp) {
    char buf[MAX_LONG_DOUBLE_CHARS];
    long double value;
    char *eptr;

    if (slen == 0 || slen >= sizeof(buf)) return 0;
    memcpy(buf,s,slen);
    buf[slen] = '\0';

    errno = 0;
    value = strtold(buf, &eptr);
    if (isspace(buf[0]) || eptr[0] != '\0' ||
        (size_t)(eptr-buf) != slen ||
        (errno == ERANGE &&
            (value == HUGE_VAL || value == -HUGE_VAL || fpclassify(value) == FP_ZERO)) ||
        errno == EINVAL ||
        isnan(value))
        return 0;

    if (dp) *dp = value;
    return 1;
}

/* Convert a string into a double. Returns 1 if the string could be parsed
 * into a (non-overflowing) double, 0 otherwise. The value will be set to
 * the parsed value when appropriate.
 *
 * Note that this function demands that the string strictly represents
 * a double: no spaces or other characters before or after the string
 * representing the number are accepted. */
int string2d(const char *s, size_t slen, double *dp) {
    errno = 0;
    char *eptr;
    *dp = strtod(s, &eptr);
    if (slen == 0 ||
        isspace(((const char*)s)[0]) ||
        (size_t)(eptr-(char*)s) != slen ||
        (errno == ERANGE &&
            (*dp == HUGE_VAL || *dp == -HUGE_VAL || fpclassify(*dp) == FP_ZERO)) ||
        isnan(*dp))
        return 0;
    return 1;
}

/* Returns 1 if the double value can safely be represented in long long without
 * precision loss, in which case the corresponding long long is stored in the out variable. */
int double2ll(double d, long long *out) {
#if (DBL_MANT_DIG >= 52) && (DBL_MANT_DIG <= 63) && (LLONG_MAX == 0x7fffffffffffffffLL)
    /* Check if the float is in a safe range to be casted into a
     * long long. We are assuming that long long is 64 bit here.
     * Also we are assuming that there are no implementations around where
     * double has precision < 52 bit.
     *
     * Under this assumptions we test if a double is inside a range
     * where casting to long long is safe. Then using two castings we
     * make sure the decimal part is zero. If all this is true we can use
     * integer without precision loss.
     *
     * Note that numbers above 2^52 and below 2^63 use all the fraction bits as real part,
     * and the exponent bits are positive, which means the "decimal" part must be 0.
     * i.e. all double values in that range are representable as a long without precision loss,
     * but not all long values in that range can be represented as a double.
     * we only care about the first part here. */
    if (d < (double)(-LLONG_MAX/2) || d > (double)(LLONG_MAX/2))
        return 0;
    long long ll = d;
    if (ll == d) {
        *out = ll;
        return 1;
    }
#endif
    return 0;
}

/* Convert a double to a string representation. Returns the number of bytes
 * required. The representation should always be parsable by strtod(3).
 * This function does not support human-friendly formatting like ld2string
 * does. It is intended mainly to be used inside t_zset.c when writing scores
 * into a listpack representing a sorted set. */
int d2string(char *buf, size_t len, double value) {
    if (isnan(value)) {
        /* Libc in some systems will format nan in a different way,
         * like nan, -nan, NAN, nan(char-sequence).
         * So we normalize it and create a single nan form in an explicit way. */
        len = snprintf(buf,len,"nan");
    } else if (isinf(value)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        if (value < 0)
            len = snprintf(buf,len,"-inf");
        else
            len = snprintf(buf,len,"inf");
    } else if (value == 0) {
        /* See: http://en.wikipedia.org/wiki/Signed_zero, "Comparisons". */
        if (1.0/value < 0)
            len = snprintf(buf,len,"-0");
        else
            len = snprintf(buf,len,"0");
    } else {
        long long lvalue;
        /* Integer printing function is much faster, check if we can safely use it. */
        if (double2ll(value, &lvalue))
            len = ll2string(buf,len,lvalue);
        else {
            len = fpconv_dtoa(value, buf);
            buf[len] = '\0';
        }
    }

    return len;
}

/* Convert a double into a string with 'fractional_digits' digits after the dot precision.
 * This is an optimized version of snprintf "%.<fractional_digits>f".
 * We convert the double to long and multiply it  by 10 ^ <fractional_digits> to shift
 * the decimal places.
 * Note that multiply it of input value by 10 ^ <fractional_digits> can overflow but on the scenario
 * that we currently use within redis this that is not possible.
 * After we get the long representation we use the logic from ull2string function on this file
 * which is based on the following article:
 * https://www.facebook.com/notes/facebook-engineering/three-optimization-tips-for-c/10151361643253920
 *
 * Input values:
 * char: the buffer to store the string representation
 * dstlen: the buffer length
 * dvalue: the input double
 * fractional_digits: the number of fractional digits after the dot precision. between 1 and 17
 *
 * Return values:
 * Returns the number of characters needed to represent the number.
 * If the buffer is not big enough to store the string, 0 is returned.
 */
int fixedpoint_d2string(char *dst, size_t dstlen, double dvalue, int fractional_digits) {
    if (fractional_digits < 1 || fractional_digits > 17)
        goto err;
    /* min size of 2 ( due to 0. ) + n fractional_digitits + \0 */
    if ((int)dstlen < (fractional_digits+3))
        goto err;
    if (dvalue == 0) {
        dst[0] = '0';
        dst[1] = '.';
        memset(dst + 2, '0', fractional_digits);
        dst[fractional_digits+2] = '\0';
        return fractional_digits + 2;
    }
    /* scale and round */
    static double powers_of_ten[] = {1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, 1000000.0,
    10000000.0, 100000000.0, 1000000000.0, 10000000000.0, 100000000000.0, 1000000000000.0,
    10000000000000.0, 100000000000000.0, 1000000000000000.0, 10000000000000000.0,
    100000000000000000.0 };
    long long svalue = llrint(dvalue * powers_of_ten[fractional_digits]);
    unsigned long long value;
    /* write sign */
    int negative = 0;
    if (svalue < 0) {
        if (svalue != LLONG_MIN) {
            value = -svalue;
        } else {
            value = ((unsigned long long) LLONG_MAX)+1;
        }
        if (dstlen < 2)
            goto err;
        negative = 1;
        dst[0] = '-';
        dst++;
        dstlen--;
    } else {
        value = svalue;
    }

    static const char digitsd[201] =
        "0001020304050607080910111213141516171819"
        "2021222324252627282930313233343536373839"
        "4041424344454647484950515253545556575859"
        "6061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";

    /* Check length. */
    uint32_t ndigits = digits10(value);
    if (ndigits >= dstlen) goto err;
    int integer_digits = ndigits - fractional_digits;
    /* Fractional only check to avoid representing 0.7750 as .7750.
     * This means we need to increment the length and store 0 as the first character.
     */
    if (integer_digits < 1) {
        dst[0] = '0';
        integer_digits = 1;
    }
    dst[integer_digits] = '.';
    int size = integer_digits + 1 + fractional_digits;
    /* fill with 0 from fractional digits until size */
    memset(dst + integer_digits + 1, '0', fractional_digits);
    int next = size - 1;
    while (value >= 100) {
        int const i = (value % 100) * 2;
        value /= 100;
        dst[next] = digitsd[i + 1];
        dst[next - 1] = digitsd[i];
        next -= 2;
        /* dot position */
        if (next == integer_digits) {
            next--;
        }
    }

    /* Handle last 1-2 digits. */
    if (value < 10) {
        dst[next] = '0' + (uint32_t) value;
    } else {
        int i = (uint32_t) value * 2;
        dst[next] = digitsd[i + 1];
        dst[next - 1] = digitsd[i];
    }
    /* Null term. */
    dst[size] = '\0';
    return size + negative;
err:
    /* force add Null termination */
    if (dstlen > 0)
        dst[0] = '\0';
    return 0;
}

/* Trims off trailing zeros from a string representing a double. */
int trimDoubleString(char *buf, size_t len) {
    if (strchr(buf,'.') != NULL) {
        char *p = buf+len-1;
        while(*p == '0') {
            p--;
            len--;
        }
        if (*p == '.') len--;
    }
    buf[len] = '\0';
    return len;
}

/* Create a string object from a long double.
 * If mode is humanfriendly it does not use exponential format and trims trailing
 * zeroes at the end (may result in loss of precision).
 * If mode is default exp format is used and the output of snprintf()
 * is not modified (may result in loss of precision).
 * If mode is hex hexadecimal format is used (no loss of precision)
 *
 * The function returns the length of the string or zero if there was not
 * enough buffer room to store it. */
int ld2string(char *buf, size_t len, long double value, ld2string_mode mode) {
    size_t l = 0;

    if (isinf(value)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        if (len < 5) goto err; /* No room. 5 is "-inf\0" */
        if (value > 0) {
            memcpy(buf,"inf",3);
            l = 3;
        } else {
            memcpy(buf,"-inf",4);
            l = 4;
        }
    } else if (isnan(value)) {
        /* Libc in some systems will format nan in a different way,
         * like nan, -nan, NAN, nan(char-sequence).
         * So we normalize it and create a single nan form in an explicit way. */
        if (len < 4) goto err; /* No room. 4 is "nan\0" */
        memcpy(buf, "nan", 3);
        l = 3;
    } else {
        switch (mode) {
        case LD_STR_AUTO:
            l = snprintf(buf,len,"%.17Lg",value);
            if (l+1 > len) goto err;; /* No room. */
            break;
        case LD_STR_HEX:
            l = snprintf(buf,len,"%La",value);
            if (l+1 > len) goto err; /* No room. */
            break;
        case LD_STR_HUMAN:
            /* We use 17 digits precision since with 128 bit floats that precision
             * after rounding is able to represent most small decimal numbers in a
             * way that is "non surprising" for the user (that is, most small
             * decimal numbers will be represented in a way that when converted
             * back into a string are exactly the same as what the user typed.) */
            l = snprintf(buf,len,"%.17Lf",value);
            if (l+1 > len) goto err; /* No room. */
            /* Now remove trailing zeroes after the '.' */
            if (strchr(buf,'.') != NULL) {
                char *p = buf+l-1;
                while(*p == '0') {
                    p--;
                    l--;
                }
                if (*p == '.') l--;
            }
            if (l == 2 && buf[0] == '-' && buf[1] == '0') {
                buf[0] = '0';
                l = 1;
            }
            break;
        default: goto err; /* Invalid mode. */
        }
    }
    buf[l] = '\0';
    return l;
err:
    /* force add Null termination */
    if (len > 0)
        buf[0] = '\0';
    return 0;
}

/* Get random bytes, attempts to get an initial seed from /dev/urandom and
 * the uses a one way hash function in counter mode to generate a random
 * stream. However if /dev/urandom is not available, a weaker seed is used.
 *
 * This function is not thread safe, since the state is global. */
void getRandomBytes(unsigned char *p, size_t len) {
    /* Global state. */
    static int seed_initialized = 0;
    static unsigned char seed[64]; /* 512 bit internal block size. */
    static uint64_t counter = 0; /* The counter we hash with the seed. */

    if (!seed_initialized) {
        /* Initialize a seed and use SHA1 in counter mode, where we hash
         * the same seed with a progressive counter. For the goals of this
         * function we just need non-colliding strings, there are no
         * cryptographic security needs. */
        FILE *fp = fopen("/dev/urandom","r");
        if (fp == NULL || fread(seed,sizeof(seed),1,fp) != 1) {
            /* Revert to a weaker seed, and in this case reseed again
             * at every call.*/
            for (unsigned int j = 0; j < sizeof(seed); j++) {
                struct timeval tv;
                gettimeofday(&tv,NULL);
                pid_t pid = getpid();
                seed[j] = tv.tv_sec ^ tv.tv_usec ^ pid ^ (long)fp;
            }
        } else {
            seed_initialized = 1;
        }
        if (fp) fclose(fp);
    }

    while(len) {
        /* This implements SHA256-HMAC. */
        unsigned char digest[SHA256_BLOCK_SIZE];
        unsigned char kxor[64];
        unsigned int copylen =
            len > SHA256_BLOCK_SIZE ? SHA256_BLOCK_SIZE : len;

        /* IKEY: key xored with 0x36. */
        memcpy(kxor,seed,sizeof(kxor));
        for (unsigned int i = 0; i < sizeof(kxor); i++) kxor[i] ^= 0x36;

        /* Obtain HASH(IKEY||MESSAGE). */
        SHA256_CTX ctx;
        sha256_init(&ctx);
        sha256_update(&ctx,kxor,sizeof(kxor));
        sha256_update(&ctx,(unsigned char*)&counter,sizeof(counter));
        sha256_final(&ctx,digest);

        /* OKEY: key xored with 0x5c. */
        memcpy(kxor,seed,sizeof(kxor));
        for (unsigned int i = 0; i < sizeof(kxor); i++) kxor[i] ^= 0x5C;

        /* Obtain HASH(OKEY || HASH(IKEY||MESSAGE)). */
        sha256_init(&ctx);
        sha256_update(&ctx,kxor,sizeof(kxor));
        sha256_update(&ctx,digest,SHA256_BLOCK_SIZE);
        sha256_final(&ctx,digest);

        /* Increment the counter for the next iteration. */
        counter++;

        memcpy(p,digest,copylen);
        len -= copylen;
        p += copylen;
    }
}

/* Generate the Redis "Run ID", a SHA1-sized random number that identifies a
 * given execution of Redis, so that if you are talking with an instance
 * having run_id == A, and you reconnect and it has run_id == B, you can be
 * sure that it is either a different instance or it was restarted. */
void getRandomHexChars(char *p, size_t len) {
    char *charset = "0123456789abcdef";
    size_t j;

    getRandomBytes((unsigned char*)p,len);
    for (j = 0; j < len; j++) p[j] = charset[p[j] & 0x0F];
}

/* Given the filename, return the absolute path as an SDS string, or NULL
 * if it fails for some reason. Note that "filename" may be an absolute path
 * already, this will be detected and handled correctly.
 *
 * The function does not try to normalize everything, but only the obvious
 * case of one or more "../" appearing at the start of "filename"
 * relative path. */
sds getAbsolutePath(char *filename) {
    char cwd[1024];
    sds abspath;
    sds relpath = sdsnew(filename);

    relpath = sdstrim(relpath," \r\n\t");
    if (relpath[0] == '/') return relpath; /* Path is already absolute. */

    /* If path is relative, join cwd and relative path. */
    if (getcwd(cwd,sizeof(cwd)) == NULL) {
        sdsfree(relpath);
        return NULL;
    }
    abspath = sdsnew(cwd);
    if (sdslen(abspath) && abspath[sdslen(abspath)-1] != '/')
        abspath = sdscat(abspath,"/");

    /* At this point we have the current path always ending with "/", and
     * the trimmed relative path. Try to normalize the obvious case of
     * trailing ../ elements at the start of the path.
     *
     * For every "../" we find in the filename, we remove it and also remove
     * the last element of the cwd, unless the current cwd is "/". */
    while (sdslen(relpath) >= 3 &&
           relpath[0] == '.' && relpath[1] == '.' && relpath[2] == '/')
    {
        sdsrange(relpath,3,-1);
        if (sdslen(abspath) > 1) {
            char *p = abspath + sdslen(abspath)-2;
            int trimlen = 1;

            while(*p != '/') {
                p--;
                trimlen++;
            }
            sdsrange(abspath,0,-(trimlen+1));
        }
    }

    /* Finally glue the two parts together. */
    abspath = sdscatsds(abspath,relpath);
    sdsfree(relpath);
    return abspath;
}

/*
 * Gets the proper timezone in a more portable fashion
 * i.e timezone variables are linux specific.
 */
long getTimeZone(void) {
#if defined(__linux__) || defined(__sun)
    return timezone;
#else
    struct timezone tz;

    gettimeofday(NULL, &tz);

    return tz.tz_minuteswest * 60L;
#endif
}

/* Return true if the specified path is just a file basename without any
 * relative or absolute path. This function just checks that no / or \
 * character exists inside the specified path, that's enough in the
 * environments where Redis runs. */
int pathIsBaseName(char *path) {
    return strchr(path,'/') == NULL && strchr(path,'\\') == NULL;
}

int fileExist(char *filename) {
    struct stat statbuf;
    return stat(filename, &statbuf) == 0 && S_ISREG(statbuf.st_mode);
}

int dirExists(char *dname) {
    struct stat statbuf;
    return stat(dname, &statbuf) == 0 && S_ISDIR(statbuf.st_mode);
}

int dirCreateIfMissing(char *dname) {
    if (mkdir(dname, 0755) != 0) {
        if (errno != EEXIST) {
            return -1;
        } else if (!dirExists(dname)) {
            errno = ENOTDIR;
            return -1;
        }
    }
    return 0;
}

int dirRemove(char *dname) {
    DIR *dir;
    struct stat stat_entry;
    struct dirent *entry;
    char full_path[PATH_MAX + 1];

    if ((dir = opendir(dname)) == NULL) {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        snprintf(full_path, sizeof(full_path), "%s/%s", dname, entry->d_name);

        int fd = open(full_path, O_RDONLY|O_NONBLOCK);
        if (fd == -1) {
            closedir(dir);
            return -1;
        }

        if (fstat(fd, &stat_entry) == -1) {
            close(fd);
            closedir(dir);
            return -1;
        }
        close(fd);

        if (S_ISDIR(stat_entry.st_mode) != 0) {
            if (dirRemove(full_path) == -1) {
                closedir(dir);
                return -1;
            }
            continue;
        }

        if (unlink(full_path) != 0) {
            closedir(dir);
            return -1;
        }
    }

    if (rmdir(dname) != 0) {
        closedir(dir);
        return -1;
    }

    closedir(dir);
    return 0;
}

sds makePath(char *path, char *filename) {
    return sdscatfmt(sdsempty(), "%s/%s", path, filename);
}

/* Given the filename, sync the corresponding directory.
 *
 * Usually a portable and safe pattern to overwrite existing files would be like:
 * 1. create a new temp file (on the same file system!)
 * 2. write data to the temp file
 * 3. fsync() the temp file
 * 4. rename the temp file to the appropriate name
 * 5. fsync() the containing directory */
int fsyncFileDir(const char *filename) {
#ifdef _AIX
    /* AIX is unable to fsync a directory */
    return 0;
#endif
    char temp_filename[PATH_MAX + 1];
    char *dname;
    int dir_fd;

    if (strlen(filename) > PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    /* In the glibc implementation dirname may modify their argument. */
    memcpy(temp_filename, filename, strlen(filename) + 1);
    dname = dirname(temp_filename);

    dir_fd = open(dname, O_RDONLY);
    if (dir_fd == -1) {
        /* Some OSs don't allow us to open directories at all, just
         * ignore the error in that case */
        if (errno == EISDIR) {
            return 0;
        }
        return -1;
    }
    /* Some OSs don't allow us to fsync directories at all, so we can ignore
     * those errors. */
    if (redis_fsync(dir_fd) == -1 && !(errno == EBADF || errno == EINVAL)) {
        int save_errno = errno;
        close(dir_fd);
        errno = save_errno;
        return -1;
    }

    close(dir_fd);
    return 0;
}

 /* free OS pages backed by file */
int reclaimFilePageCache(int fd, size_t offset, size_t length) {
#ifdef HAVE_FADVISE
    int ret = posix_fadvise(fd, offset, length, POSIX_FADV_DONTNEED);
    if (ret) return -1;
    return 0;
#else
    UNUSED(fd);
    UNUSED(offset);
    UNUSED(length);
    return 0;
#endif
}

/** An async signal safe version of fgets().
 * Has the same behaviour as standard fgets(): reads a line from fd and stores it into the dest buffer.
 * It stops when either (buff_size-1) characters are read, the newline character is read, or the end-of-file is reached,
 * whichever comes first.
 *
 * On success, the function returns the same dest parameter. If the End-of-File is encountered and no characters have
 * been read, the contents of dest remain unchanged and a null pointer is returned.
 * If an error occurs, a null pointer is returned. */
char *fgets_async_signal_safe(char *dest, int buff_size, int fd) {
    for (int i = 0; i < buff_size; i++) {
        /* Read one byte */
        ssize_t bytes_read_count = read(fd, dest + i, 1);
        /* On EOF or error return NULL */
        if (bytes_read_count < 1) {
            return NULL;
        }
        /* we found the end of the line. */
        if (dest[i] == '\n') {
            break;
        }
    }
    return dest;
}

static const char HEX[] = "0123456789abcdef";

static char *u2string_async_signal_safe(int _base, uint64_t val, char *buf) {
    uint32_t base = (uint32_t) _base;
    *buf-- = 0;
    do {
        *buf-- = HEX[val % base];
    } while ((val /= base) != 0);
    return buf + 1;
}

static char *i2string_async_signal_safe(int base, int64_t val, char *buf) {
    char *orig_buf = buf;
    const int32_t is_neg = (val < 0);
    *buf-- = 0;

    if (is_neg) {
        val = -val;
    }
    if (is_neg && base == 16) {
        int ix;
        val -= 1;
        for (ix = 0; ix < 16; ++ix)
            buf[-ix] = '0';
    }

    do {
        *buf-- = HEX[val % base];
    } while ((val /= base) != 0);

    if (is_neg && base == 10) {
        *buf-- = '-';
    }

    if (is_neg && base == 16) {
        int ix;
        buf = orig_buf - 1;
        for (ix = 0; ix < 16; ++ix, --buf) {
            /* *INDENT-OFF* */
            switch (*buf) {
            case '0': *buf = 'f'; break;
            case '1': *buf = 'e'; break;
            case '2': *buf = 'd'; break;
            case '3': *buf = 'c'; break;
            case '4': *buf = 'b'; break;
            case '5': *buf = 'a'; break;
            case '6': *buf = '9'; break;
            case '7': *buf = '8'; break;
            case '8': *buf = '7'; break;
            case '9': *buf = '6'; break;
            case 'a': *buf = '5'; break;
            case 'b': *buf = '4'; break;
            case 'c': *buf = '3'; break;
            case 'd': *buf = '2'; break;
            case 'e': *buf = '1'; break;
            case 'f': *buf = '0'; break;
            }
            /* *INDENT-ON* */
        }
    }
    return buf + 1;
}

static const char *check_longlong_async_signal_safe(const char *fmt, int32_t *have_longlong) {
    *have_longlong = 0;
    if (*fmt == 'l') {
        fmt++;
        if (*fmt != 'l') {
            *have_longlong = (sizeof(long) == sizeof(int64_t));
        } else {
            fmt++;
            *have_longlong = 1;
        }
    }
    return fmt;
}

int vsnprintf_async_signal_safe(char *to, size_t size, const char *format, va_list ap) {
    char *start = to;
    char *end = start + size - 1;
    for (; *format; ++format) {
        int32_t have_longlong = 0;
        if (*format != '%') {
            if (to == end) { /* end of buffer */
                break;
            }
            *to++ = *format; /* copy ordinary char */
            continue;
        }
        ++format; /* skip '%' */

        format = check_longlong_async_signal_safe(format, &have_longlong);

        switch (*format) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'p':
            {
                int64_t ival = 0;
                uint64_t uval = 0;
                if (*format == 'p')
                    have_longlong = (sizeof(void *) == sizeof(uint64_t));
                if (have_longlong) {
                    if (*format == 'u') {
                        uval = va_arg(ap, uint64_t);
                    } else {
                        ival = va_arg(ap, int64_t);
                    }
                } else {
                    if (*format == 'u') {
                        uval = va_arg(ap, uint32_t);
                    } else {
                        ival = va_arg(ap, int32_t);
                    }
                }

                {
                    char buff[22];
                    const int base = (*format == 'x' || *format == 'p') ? 16 : 10;

/* *INDENT-OFF* */
                    char *val_as_str = (*format == 'u') ?
                        u2string_async_signal_safe(base, uval, &buff[sizeof(buff) - 1]) :
                        i2string_async_signal_safe(base, ival, &buff[sizeof(buff) - 1]);
/* *INDENT-ON* */

                    /* Strip off "ffffffff" if we have 'x' format without 'll' */
                    if (*format == 'x' && !have_longlong && ival < 0) {
                        val_as_str += 8;
                    }

                    while (*val_as_str && to < end) {
                        *to++ = *val_as_str++;
                    }
                    continue;
                }
            }
        case 's':
            {
                const char *val = va_arg(ap, char *);
                if (!val) {
                    val = "(null)";
                }
                while (*val && to < end) {
                    *to++ = *val++;
                }
                continue;
            }
        }
    }
    *to = 0;
    return (int)(to - start);
}

int snprintf_async_signal_safe(char *to, size_t n, const char *fmt, ...) {
    int result;
    va_list args;
    va_start(args, fmt);
    result = vsnprintf_async_signal_safe(to, n, fmt, args);
    va_end(args);
    return result;
}

#ifdef REDIS_TEST
#include <assert.h>
#include <sys/mman.h>
#include "testhelp.h"

static void test_string2ll(void) {
    char buf[32];
    long long v;

    /* May not start with +. */
    redis_strlcpy(buf,"+1",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 0);

    /* Leading space. */
    redis_strlcpy(buf," 1",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 0);

    /* Trailing space. */
    redis_strlcpy(buf,"1 ",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 0);

    /* May not start with 0. */
    redis_strlcpy(buf,"01",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 0);

    redis_strlcpy(buf,"-1",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == -1);

    redis_strlcpy(buf,"0",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == 0);

    redis_strlcpy(buf,"1",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == 1);

    redis_strlcpy(buf,"99",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == 99);

    redis_strlcpy(buf,"-99",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == -99);

    redis_strlcpy(buf,"-9223372036854775808",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == LLONG_MIN);

    redis_strlcpy(buf,"-9223372036854775809",sizeof(buf)); /* overflow */
    assert(string2ll(buf,strlen(buf),&v) == 0);

    redis_strlcpy(buf,"9223372036854775807",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == LLONG_MAX);

    redis_strlcpy(buf,"9223372036854775808",sizeof(buf)); /* overflow */
    assert(string2ll(buf,strlen(buf),&v) == 0);
}

static void test_string2l(void) {
    char buf[32];
    long v;

    /* May not start with +. */
    redis_strlcpy(buf,"+1",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 0);

    /* May not start with 0. */
    redis_strlcpy(buf,"01",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 0);

    redis_strlcpy(buf,"-1",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == -1);

    redis_strlcpy(buf,"0",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == 0);

    redis_strlcpy(buf,"1",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == 1);

    redis_strlcpy(buf,"99",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == 99);

    redis_strlcpy(buf,"-99",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == -99);

#if LONG_MAX != LLONG_MAX
    redis_strlcpy(buf,"-2147483648",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == LONG_MIN);

    redis_strlcpy(buf,"-2147483649",sizeof(buf)); /* overflow */
    assert(string2l(buf,strlen(buf),&v) == 0);

    redis_strlcpy(buf,"2147483647",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == LONG_MAX);

    redis_strlcpy(buf,"2147483648",sizeof(buf)); /* overflow */
    assert(string2l(buf,strlen(buf),&v) == 0);
#endif
}

static void test_ll2string(void) {
    char buf[32];
    long long v;
    int sz;

    v = 0;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 1);
    assert(!strcmp(buf, "0"));

    v = -1;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 2);
    assert(!strcmp(buf, "-1"));

    v = 99;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 2);
    assert(!strcmp(buf, "99"));

    v = -99;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 3);
    assert(!strcmp(buf, "-99"));

    v = -2147483648;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 11);
    assert(!strcmp(buf, "-2147483648"));

    v = LLONG_MIN;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 20);
    assert(!strcmp(buf, "-9223372036854775808"));

    v = LLONG_MAX;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 19);
    assert(!strcmp(buf, "9223372036854775807"));
}

static void test_ld2string(void) {
    char buf[32];
    long double v;
    int sz;

    v = 0.0 / 0.0;
    sz = ld2string(buf, sizeof(buf), v, LD_STR_AUTO);
    assert(sz == 3);
    assert(!strcmp(buf, "nan"));
}

static void test_fixedpoint_d2string(void) {
    char buf[32];
    double v;
    int sz;
    v = 0.0;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 6);
    assert(!strcmp(buf, "0.0000"));
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    assert(sz == 3);
    assert(!strcmp(buf, "0.0"));
    /* set junk in buffer */
    memset(buf,'A',32);
    v = 0.0001;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 6);
    assert(buf[sz] == '\0');
    assert(!strcmp(buf, "0.0001"));
    /* set junk in buffer */
    memset(buf,'A',32);
    v = 6.0642951598391699e-05;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 6);
    assert(buf[sz] == '\0');
    assert(!strcmp(buf, "0.0001"));
    v = 0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 6);
    assert(!strcmp(buf, "0.0100"));
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    assert(sz == 3);
    assert(!strcmp(buf, "0.0"));
    v = -0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 7);
    assert(!strcmp(buf, "-0.0100"));
     v = -0.1;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    assert(sz == 4);
    assert(!strcmp(buf, "-0.1"));
    v = 0.1;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    assert(sz == 3);
    assert(!strcmp(buf, "0.1"));
    v = 0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 17);
    assert(sz == 19);
    assert(!strcmp(buf, "0.01000000000000000"));
    v = 10.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 7);
    assert(!strcmp(buf, "10.0100"));
    /* negative tests */
    sz = fixedpoint_d2string(buf, sizeof buf, v, 18);
    assert(sz == 0);
    sz = fixedpoint_d2string(buf, sizeof buf, v, 0);
    assert(sz == 0);
    sz = fixedpoint_d2string(buf, 1, v, 1);
    assert(sz == 0);
}

#if defined(__linux__)
/* Since fadvise and mincore is only supported in specific platforms like
 * Linux, we only verify the fadvise mechanism works in Linux */
static int cache_exist(int fd) {
    unsigned char flag;
    void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    assert(m);
    assert(mincore(m, 4096, &flag) == 0);
    munmap(m, 4096);
    /* the least significant bit of the byte will be set if the corresponding
     * page is currently resident in memory */
    return flag&1;
}

static void test_reclaimFilePageCache(void) {
    char *tmpfile = "/tmp/redis-reclaim-cache-test";
    int fd = open(tmpfile, O_RDWR|O_CREAT, 0644);
    assert(fd >= 0);

    /* test write file */
    char buf[4] = "foo";
    assert(write(fd, buf, sizeof(buf)) > 0);
    assert(cache_exist(fd));
    assert(redis_fsync(fd) == 0);
    assert(reclaimFilePageCache(fd, 0, 0) == 0);
    assert(!cache_exist(fd));

    /* test read file */
    assert(pread(fd, buf, sizeof(buf), 0) > 0);
    assert(cache_exist(fd));
    assert(reclaimFilePageCache(fd, 0, 0) == 0);
    assert(!cache_exist(fd));

    unlink(tmpfile);
    printf("reclaimFilePageCach test is ok\n");
}
#endif

int utilTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    test_string2ll();
    test_string2l();
    test_ll2string();
    test_ld2string();
    test_fixedpoint_d2string();
#if defined(__linux__)
    if (!(flags & REDIS_TEST_VALGRIND)) {
        test_reclaimFilePageCache();
    }
#endif
    printf("Done testing util\n");
    return 0;
}
#endif
