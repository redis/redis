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
 * Checks for predefined macros by the compiler to try and get both the arch
 * and the compiler version.
 */
#ifndef XSUM_ARCH_H
#define XSUM_ARCH_H

#include "xsum_config.h"

#define XSUM_LIB_VERSION XXH_VERSION_MAJOR.XXH_VERSION_MINOR.XXH_VERSION_RELEASE
#define XSUM_QUOTE(str) #str
#define XSUM_EXPAND_AND_QUOTE(str) XSUM_QUOTE(str)
#define XSUM_PROGRAM_VERSION XSUM_EXPAND_AND_QUOTE(XSUM_LIB_VERSION)


/* Show compiler versions in WELCOME_MESSAGE. XSUM_CC_VERSION_FMT will return the printf specifiers,
 * and VERSION will contain the comma separated list of arguments to the XSUM_CC_VERSION_FMT string. */
#if defined(__clang_version__)
/* Clang does its own thing. */
#  ifdef __apple_build_version__
#    define XSUM_CC_VERSION_FMT "Apple Clang %s"
#  else
#    define XSUM_CC_VERSION_FMT "Clang %s"
#  endif
#  define XSUM_CC_VERSION  __clang_version__
#elif defined(__VERSION__)
/* GCC and ICC */
#  define XSUM_CC_VERSION_FMT "%s"
#  ifdef __INTEL_COMPILER /* icc adds its prefix */
#    define XSUM_CC_VERSION __VERSION__
#  else /* assume GCC */
#    define XSUM_CC_VERSION "GCC " __VERSION__
#  endif
#elif defined(_MSC_FULL_VER) && defined(_MSC_BUILD)
/*
 * MSVC
 *  "For example, if the version number of the Visual C++ compiler is
 *   15.00.20706.01, the _MSC_FULL_VER macro evaluates to 150020706."
 *
 *   https://docs.microsoft.com/en-us/cpp/preprocessor/predefined-macros?view=vs-2017
 */
#  define XSUM_CC_VERSION_FMT "MSVC %02i.%02i.%05i.%02i"
#  define XSUM_CC_VERSION  _MSC_FULL_VER / 10000000 % 100, _MSC_FULL_VER / 100000 % 100, _MSC_FULL_VER % 100000, _MSC_BUILD
#elif defined(_MSC_VER) /* old MSVC */
#  define XSUM_CC_VERSION_FMT "MSVC %02i.%02i"
#  define XSUM_CC_VERSION _MSC_VER / 100, _MSC_VER % 100
#elif defined(__TINYC__)
/* tcc stores its version in the __TINYC__ macro. */
#  define XSUM_CC_VERSION_FMT "tcc %i.%i.%i"
#  define XSUM_CC_VERSION __TINYC__ / 10000 % 100, __TINYC__ / 100 % 100, __TINYC__ % 100
#else
#  define XSUM_CC_VERSION_FMT "%s"
#  define XSUM_CC_VERSION "unknown compiler"
#endif

/* makes the next part easier */
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC)
#   define XSUM_ARCH_X64 1
#   define XSUM_ARCH_X86 "x86_64"
#elif defined(__i386__) || defined(_M_IX86) || defined(_M_IX86_FP)
#   define XSUM_ARCH_X86 "i386"
#endif

/* Try to detect the architecture. */
#if defined(XSUM_ARCH_X86)
#  if defined(XXHSUM_DISPATCH)
     const char* XSUM_autox86(void);
#    define XSUM_ARCH XSUM_autox86()
#  elif defined(__AVX512F__)
#    define XSUM_ARCH XSUM_ARCH_X86 " + AVX512"
#  elif defined(__AVX2__)
#    define XSUM_ARCH XSUM_ARCH_X86 " + AVX2"
#  elif defined(__AVX__)
#    define XSUM_ARCH XSUM_ARCH_X86 " + AVX"
#  elif defined(_M_X64) || defined(__x86_64__) \
      || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP == 2)
#     define XSUM_ARCH XSUM_ARCH_X86 " + SSE2"
#  else
#     define XSUM_ARCH XSUM_ARCH_X86
#  endif
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
#  if defined(__ARM_FEATURE_SVE)
#    define XSUM_ARCH "aarch64 + SVE"
#  else
#    define XSUM_ARCH "aarch64 + NEON"
#  endif
#elif defined(__arm__) || defined(__thumb__) || defined(__thumb2__) || defined(_M_ARM)
/* ARM has a lot of different features that can change xxHash significantly. */
#  ifdef __ARM_ARCH
#    define XSUM_ARCH_ARM_VER XSUM_EXPAND_AND_QUOTE(__ARM_ARCH)
#  else
#    define XSUM_ARCH_ARM_VER XSUM_EXPAND_AND_QUOTE(_M_ARM)
#  endif
#  if defined(_M_ARM) /* windows arm is always thumb-2 */ \
    || defined(__thumb2__) || (defined(__thumb__) && (__thumb__ == 2 || __ARM_ARCH >= 7))
#    define XSUM_ARCH_THUMB " Thumb-2"
#  elif defined(__thumb__)
#    define XSUM_ARCH_THUMB " Thumb-1"
#  else
#    define XSUM_ARCH_THUMB ""
#  endif
/* ARMv7 has unaligned by default */
#  if defined(__ARM_FEATURE_UNALIGNED) || __ARM_ARCH >= 7 || defined(_M_ARM)
#    define XSUM_ARCH_UNALIGNED " + unaligned"
#  else
#    define XSUM_ARCH_UNALIGNED ""
#  endif
#  if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM)
#    define XSUM_ARCH_NEON " + NEON"
#  else
#    define XSUM_ARCH_NEON ""
#  endif
#  define XSUM_ARCH "ARMv" XSUM_ARCH_ARM_VER XSUM_ARCH_THUMB XSUM_ARCH_NEON XSUM_ARCH_UNALIGNED
#elif defined(__powerpc64__) || defined(__ppc64__) || defined(__PPC64__)
#  if defined(__GNUC__) && defined(__POWER9_VECTOR__)
#    define XSUM_ARCH "ppc64 + POWER9 vector"
#  elif defined(__GNUC__) && defined(__POWER8_VECTOR__)
#    define XSUM_ARCH "ppc64 + POWER8 vector"
#  else
#    define XSUM_ARCH "ppc64"
#  endif
#elif defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
#  define XSUM_ARCH "ppc"
#elif defined(__AVR)
#  define XSUM_ARCH "AVR"
#elif defined(__mips64)
#  define XSUM_ARCH "mips64"
#elif defined(__mips)
#  define XSUM_ARCH "mips"
#elif defined(__s390x__)
#  define XSUM_ARCH "s390x"
#elif defined(__s390__)
#  define XSUM_ARCH "s390"
#elif defined(__wasm__) || defined(__asmjs__) || defined(__EMSCRIPTEN__)
#  if defined(__wasm_simd128__)
#    define XSUM_ARCH "wasm/asmjs + simd128"
#  else
#    define XSUM_ARCH "wasm/asmjs"
#  endif
#elif defined(__riscv)
#    define XSUM_ARCH "riscv"
#elif defined(__loongarch_lp64)
#  if defined(__loongarch_asx)
#    define XSUM_ARCH "loongarch64 + lasx"
#  elif defined(__loongarch_sx)
#    define XSUM_ARCH "loongarch64 + lsx"
#  else
#    define XSUM_ARCH "loongarch64"
#  endif
#else
#  define XSUM_ARCH "unknown"
#endif


#endif /* XSUM_ARCH_H */
