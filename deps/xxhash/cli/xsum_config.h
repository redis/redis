/*
 * xxhsum - Command line interface for xxhash algorithms
 * Copyright (C) 2013-2021 Yann Collet
 *
 * GPL v2 License
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * You can contact the author at:
 *   - xxHash homepage: https://www.xxhash.com
 *   - xxHash source repository: https://github.com/Cyan4973/xxHash
 */

/*
 * This contains various configuration parameters and feature detection for
 * xxhsum.
 *
 * Similar to config.h in Autotools, this should be the first header included.
 */

#ifndef XSUM_CONFIG_H
#define XSUM_CONFIG_H


/* ************************************
 *  Compiler Options
 **************************************/
/*
 * Disable Visual C's warnings when using the "insecure" CRT functions instead
 * of the "secure" _s functions.
 *
 * These functions are not portable, and aren't necessary if you are using the
 * original functions properly.
 */
#if defined(_MSC_VER) || defined(_WIN32)
#  ifndef _CRT_SECURE_NO_WARNINGS
#    define _CRT_SECURE_NO_WARNINGS
#  endif
#endif

#if defined(_MSC_VER)
#  pragma warning(disable : 4127) /* disable: C4127: conditional expression is constant */
#endif

/* Under Linux at least, pull in the *64 commands */
#ifndef _LARGEFILE64_SOURCE
#  define _LARGEFILE64_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS 64
#endif

/*
 * So we can use __attribute__((__format__))
 */
#ifdef __GNUC__
#  define XSUM_ATTRIBUTE(x) __attribute__(x)
#else
#  define XSUM_ATTRIBUTE(x)
#endif

#if !defined(_WIN32) && (defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__)) /* UNIX-like OS */ \
   || defined(__midipix__) || defined(__VMS))
#  if (defined(__APPLE__) && defined(__MACH__)) || defined(__SVR4) || defined(_AIX) || defined(__hpux) /* POSIX.1-2001 (SUSv3) conformant */ \
     || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)  /* BSD distros */
#    define XSUM_PLATFORM_POSIX_VERSION 200112L
#  else
#    if defined(__linux__) || defined(__linux)
#      ifndef _POSIX_C_SOURCE
#        define _POSIX_C_SOURCE 200112L  /* use feature test macro */
#      endif
#    endif
#    include <unistd.h>  /* declares _POSIX_VERSION */
#    if defined(_POSIX_VERSION)  /* POSIX compliant */
#      define XSUM_PLATFORM_POSIX_VERSION _POSIX_VERSION
#    else
#      define XSUM_PLATFORM_POSIX_VERSION 0
#    endif
#  endif
#endif
#if !defined(XSUM_PLATFORM_POSIX_VERSION)
#  define XSUM_PLATFORM_POSIX_VERSION -1
#endif

#if !defined(S_ISREG)
#  define S_ISREG(x) (((x) & S_IFMT) == S_IFREG)
#endif


/* ************************************
 * Windows helpers
 **************************************/

/*
 * Whether to use the Windows UTF-16 APIs instead of the portable libc 8-bit
 * ("ANSI") APIs.
 *
 * Windows is not UTF-8 clean by default, and the only way to access every file
 * on the OS is to use UTF-16.
 *
 * Do note that xxhsum uses UTF-8 internally and only uses UTF-16 for command
 * line arguments, console I/O, and opening files.
 *
 * Additionally, this guarantees all piped output is UTF-8.
 */
#if defined(XSUM_WIN32_USE_WCHAR) && !defined(_WIN32)
/* We use Windows APIs, only use this on Windows. */
#  undef XSUM_WIN32_USE_WCHAR
#endif

#ifndef XSUM_WIN32_USE_WCHAR
#  if defined(_WIN32)
#    include <wchar.h>
#    if WCHAR_MAX == 0xFFFFU /* UTF-16 wchar_t */
#       define XSUM_WIN32_USE_WCHAR 1
#    else
#       define XSUM_WIN32_USE_WCHAR 0
#    endif
#  else
#    define XSUM_WIN32_USE_WCHAR 0
#  endif
#endif

#if !XSUM_WIN32_USE_WCHAR
/*
 * It doesn't make sense to have one without the other.
 * Due to XSUM_WIN32_USE_WCHAR being undef'd, this also handles
 * non-WIN32 platforms.
 */
#  undef  XSUM_WIN32_USE_WMAIN
#  define XSUM_WIN32_USE_WMAIN 0
#else
/*
 * Whether to use wmain() or main().
 *
 * wmain() is preferred because we don't have to mess with internal hidden
 * APIs.
 *
 * It always works on MSVC, but in MinGW, it only works on MinGW-w64 with the
 * -municode flag.
 *
 * Therefore we have to use main() -- there is no better option.
 */
#  ifndef XSUM_WIN32_USE_WMAIN
#    if defined(_UNICODE) || defined(UNICODE) /* MinGW -municode */ \
        || defined(_MSC_VER) /* MSVC */
#      define XSUM_WIN32_USE_WMAIN 1
#    else
#      define XSUM_WIN32_USE_WMAIN 0
#    endif
#  endif
/*
 * It is always good practice to define these to prevent accidental use of the
 * ANSI APIs, even if the program primarily uses UTF-8.
 */
#  ifndef _UNICODE
#    define _UNICODE
#  endif
#  ifndef UNICODE
#    define UNICODE
#  endif
#endif /* XSUM_WIN32_USE_WCHAR */

#ifndef XSUM_API
#  define XSUM_API
#endif

#ifndef XSUM_NO_TESTS
#  define XSUM_NO_TESTS 0
#endif

/* ***************************
 * Basic types
 * ***************************/

#if defined(__cplusplus) /* C++ */ \
 || (defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)  /* C99 */
#  include <stdint.h>
    typedef uint8_t  XSUM_U8;
    typedef uint32_t XSUM_U32;
    typedef uint64_t XSUM_U64;
# else
#   include <limits.h>
    typedef unsigned char      XSUM_U8;
#   if UINT_MAX == 0xFFFFFFFFUL
      typedef unsigned int     XSUM_U32;
#   else
      typedef unsigned long    XSUM_U32;
#   endif
    typedef unsigned long long XSUM_U64;
#endif /* not C++/C99 */

/* ***************************
 * Common constants
 * ***************************/

#define KB *( 1<<10)
#define MB *( 1<<20)
#define GB *(1U<<30)


#endif /* XSUM_CONFIG_H */
