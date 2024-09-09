/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#ifndef _REDIS_FMACRO_H
#define _REDIS_FMACRO_H

#define _BSD_SOURCE

#if defined(__linux__)
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#endif

#if defined(_AIX)
#define _ALL_SOURCE
#endif

#if defined(__linux__) || defined(__OpenBSD__)
#define _XOPEN_SOURCE 700
/*
 * On NetBSD, _XOPEN_SOURCE undefines _NETBSD_SOURCE and
 * thus hides inet_aton etc.
 */
#elif !defined(__NetBSD__)
#define _XOPEN_SOURCE
#endif

#if defined(__sun)
#define _POSIX_C_SOURCE 199506L
#endif

#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64

/* deprecate unsafe functions
 *
 * NOTE: We do not use the poison pragma since it
 * will error on stdlib definitions in files as well*/
#if (__GNUC__ && __GNUC__ >= 4) && !defined __APPLE__
int sprintf(char *str, const char *format, ...) __attribute__((deprecated("please avoid use of unsafe C functions. prefer use of snprintf instead")));
char *strcpy(char *restrict dest, const char *src) __attribute__((deprecated("please avoid use of unsafe C functions. prefer use of redis_strlcpy instead")));
char *strcat(char *restrict dest, const char *restrict src) __attribute__((deprecated("please avoid use of unsafe C functions. prefer use of redis_strlcat instead")));
#endif

#ifdef __linux__
/* features.h uses the defines above to set feature specific defines.  */
#include <features.h>
#endif

#endif
