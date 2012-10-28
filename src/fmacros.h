#ifndef _REDIS_FMACRO_H
#define _REDIS_FMACRO_H

#define _BSD_SOURCE

#if defined(__linux__)
#define _GNU_SOURCE
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

#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64

#endif
