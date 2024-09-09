/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
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

/* The maximum number of characters needed to for d2string/fpconv_dtoa call.
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
int string2ul_base16_async_signal_safe(const char *src, size_t slen, unsigned long *result_output);
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
int reclaimFilePageCache(int fd, size_t offset, size_t length);
char *fgets_async_signal_safe(char *dest, int buff_size, int fd);
int vsnprintf_async_signal_safe(char *to, size_t size, const char *format, va_list ap);
#ifdef __GNUC__
int snprintf_async_signal_safe(char *to, size_t n, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#else
int snprintf_async_signal_safe(char *to, size_t n, const char *fmt, ...);
#endif
size_t redis_strlcpy(char *dst, const char *src, size_t dsize);
size_t redis_strlcat(char *dst, const char *src, size_t dsize);

#ifdef REDIS_TEST
int utilTest(int argc, char **argv, int flags);
#endif

#endif
