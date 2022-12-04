/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __REDIS_UTIL_H
#define __REDIS_UTIL_H

#include <stdint.h>
#include "sds.h"

/* The maximum number of characters needed to represent a long double
 * as a string (long double has a huge range of some 4952 chars, see LDBL_MAX).
 * This should be the size of the buffer given to ld2string */
#define MAX_LONG_DOUBLE_CHARS 5*1024

/* The maximum number of characters needed to represent a double
 * as a string (double has a huge range of some 328 chars, see DBL_MAX).
 * This should be the size of the buffer for sprintf with %f */
#define MAX_DOUBLE_CHARS 400

/* The maximum number of characters needed to for d2string call.
 * Since it uses %g and not %f, some 40 chars should be enough. */
#define MAX_D2STRING_CHARS 128

/* Bytes needed for long -> str + '\0' */
#define LONG_STR_SIZE      21

/* long double to string conversion options */
typedef enum {
    LD_STR_AUTO,     /* %.17Lg */
    LD_STR_HUMAN,    /* %.17Lf + Trimming of trailing zeros */
    LD_STR_HEX       /* %La */
} ld2string_mode;

int stringmatchlen(const char *p, int plen, const char *s, int slen, int nocase);
int stringmatch(const char *p, const char *s, int nocase);
int stringmatchlen_fuzz_test(void);
unsigned long long memtoull(const char *p, int *err);
const char *mempbrk(const char *s, size_t len, const char *chars, size_t charslen);
char *memmapchars(char *s, size_t len, const char *from, const char *to, size_t setlen);
uint32_t digits10(uint64_t v);
uint32_t sdigits10(int64_t v);
int ll2string(char *s, size_t len, long long value);
int ull2string(char *s, size_t len, unsigned long long value);
int string2ll(const char *s, size_t slen, long long *value);
int string2ull(const char *s, unsigned long long *value);
int string2l(const char *s, size_t slen, long *value);
int string2ld(const char *s, size_t slen, long double *dp);
int string2d(const char *s, size_t slen, double *dp);
int trimDoubleString(char *buf, size_t len);
int d2string(char *buf, size_t len, double value);
int fixedpoint_d2string(char *dst, size_t dstlen, double dvalue, int fractional_digits);
int ld2string(char *buf, size_t len, long double value, ld2string_mode mode);
int double2ll(double d, long long *out);
int yesnotoi(char *s);
sds getAbsolutePath(char *filename);
long getTimeZone(void);
int pathIsBaseName(char *path);
int dirCreateIfMissing(char *dname);
int dirExists(char *dname);
int dirRemove(char *dname);
int fileExist(char *filename);
sds makePath(char *path, char *filename);
int fsyncFileDir(const char *filename);

#ifdef REDIS_TEST
int utilTest(int argc, char **argv, int flags);
#endif

#endif
