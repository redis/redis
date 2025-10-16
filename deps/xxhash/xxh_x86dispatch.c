/*
 * xxHash - Extremely Fast Hash algorithm
 * Copyright (C) 2020-2021 Yann Collet
 *
 * BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * You can contact the author at:
 *   - xxHash homepage: https://www.xxhash.com
 *   - xxHash source repository: https://github.com/Cyan4973/xxHash
 */


/*!
 * @file xxh_x86dispatch.c
 *
 * Automatic dispatcher code for the @ref XXH3_family on x86-based targets.
 *
 * Optional add-on.
 *
 * **Compile this file with the default flags for your target.**
 * Note that compiling with flags like `-mavx*`, `-march=native`, or `/arch:AVX*`
 * will make the resulting binary incompatible with cpus not supporting the requested instruction set.
 *
 * @defgroup dispatch x86 Dispatcher
 * @{
 */

#if defined (__cplusplus)
extern "C" {
#endif

#if !(defined(__x86_64__) || defined(__i386__) || defined(_M_IX86) || defined(_M_X64))
#  error "Dispatching is currently only supported on x86 and x86_64."
#endif

/*! @cond Doxygen ignores this part */
#ifndef XXH_HAS_INCLUDE
#  ifdef __has_include
/*
 * Not defined as XXH_HAS_INCLUDE(x) (function-like) because
 * this causes segfaults in Apple Clang 4.2 (on Mac OS X 10.7 Lion)
 */
#    define XXH_HAS_INCLUDE __has_include
#  else
#    define XXH_HAS_INCLUDE(x) 0
#  endif
#endif
/*! @endcond */

/*!
 * @def XXH_DISPATCH_SCALAR
 * @brief Enables/dispatching the scalar code path.
 *
 * If this is defined to 0, SSE2 support is assumed. This reduces code size
 * when the scalar path is not needed.
 *
 * This is automatically defined to 0 when...
 *   - SSE2 support is enabled in the compiler
 *   - Targeting x86_64
 *   - Targeting Android x86
 *   - Targeting macOS
 */
#ifndef XXH_DISPATCH_SCALAR
#  if defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) /* SSE2 on by default */ \
     || defined(__x86_64__) || defined(_M_X64) /* x86_64 */ \
     || defined(__ANDROID__) || defined(__APPLE__) /* Android or macOS */
#     define XXH_DISPATCH_SCALAR 0 /* disable */
#  else
#     define XXH_DISPATCH_SCALAR 1
#  endif
#endif
/*!
 * @def XXH_DISPATCH_AVX2
 * @brief Enables/disables dispatching for AVX2.
 *
 * This is automatically detected if it is not defined.
 *  - GCC 4.7 and later are known to support AVX2, but >4.9 is required for
 *    to get the AVX2 intrinsics and typedefs without -mavx -mavx2.
 *  - Visual Studio 2013 Update 2 and later are known to support AVX2.
 *  - The GCC/Clang internal header `<avx2intrin.h>` is detected. While this is
 *    not allowed to be included directly, it still appears in the builtin
 *    include path and is detectable with `__has_include`.
 *
 * @see XXH_AVX2
 */
#ifndef XXH_DISPATCH_AVX2
#  if (defined(__GNUC__) && (__GNUC__ > 4)) /* GCC 5.0+ */ \
   || (defined(_MSC_VER) && _MSC_VER >= 1900) /* VS 2015+ */ \
   || (defined(_MSC_FULL_VER) && _MSC_FULL_VER >= 180030501) /* VS 2013 Update 2 */ \
   || XXH_HAS_INCLUDE(<avx2intrin.h>) /* GCC/Clang internal header */
#    define XXH_DISPATCH_AVX2 1   /* enable dispatch towards AVX2 */
#  else
#    define XXH_DISPATCH_AVX2 0
#  endif
#endif /* XXH_DISPATCH_AVX2 */

/*!
 * @def XXH_DISPATCH_AVX512
 * @brief Enables/disables dispatching for AVX512.
 *
 * Automatically detected if one of the following conditions is met:
 *  - GCC 4.9 and later are known to support AVX512.
 *  - Visual Studio 2017  and later are known to support AVX2.
 *  - The GCC/Clang internal header `<avx512fintrin.h>` is detected. While this
 *    is not allowed to be included directly, it still appears in the builtin
 *    include path and is detectable with `__has_include`.
 *
 * @see XXH_AVX512
 */
#ifndef XXH_DISPATCH_AVX512
#  if (defined(__GNUC__) \
       && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 9))) /* GCC 4.9+ */ \
   || (defined(_MSC_VER) && _MSC_VER >= 1910) /* VS 2017+ */ \
   || XXH_HAS_INCLUDE(<avx512fintrin.h>) /* GCC/Clang internal header */
#    define XXH_DISPATCH_AVX512 1   /* enable dispatch towards AVX512 */
#  else
#    define XXH_DISPATCH_AVX512 0
#  endif
#endif /* XXH_DISPATCH_AVX512 */

/*!
 * @def XXH_TARGET_SSE2
 * @brief Allows a function to be compiled with SSE2 intrinsics.
 *
 * Uses `__attribute__((__target__("sse2")))` on GCC to allow SSE2 to be used
 * even with `-mno-sse2`.
 *
 * @def XXH_TARGET_AVX2
 * @brief Like @ref XXH_TARGET_SSE2, but for AVX2.
 *
 * @def XXH_TARGET_AVX512
 * @brief Like @ref XXH_TARGET_SSE2, but for AVX512.
 *
 */
#if defined(__GNUC__)
#  include <emmintrin.h> /* SSE2 */
#  if XXH_DISPATCH_AVX2 || XXH_DISPATCH_AVX512
#    include <immintrin.h> /* AVX2, AVX512F */
#  endif
#  define XXH_TARGET_SSE2 __attribute__((__target__("sse2")))
#  define XXH_TARGET_AVX2 __attribute__((__target__("avx2")))
#  define XXH_TARGET_AVX512 __attribute__((__target__("avx512f")))
#elif defined(__clang__) && defined(_MSC_VER) /* clang-cl.exe */
#  include <emmintrin.h> /* SSE2 */
#  if XXH_DISPATCH_AVX2 || XXH_DISPATCH_AVX512
#    include <immintrin.h> /* AVX2, AVX512F */
#    include <smmintrin.h>
#    include <avxintrin.h>
#    include <avx2intrin.h>
#    include <avx512fintrin.h>
#  endif
#  define XXH_TARGET_SSE2 __attribute__((__target__("sse2")))
#  define XXH_TARGET_AVX2 __attribute__((__target__("avx2")))
#  define XXH_TARGET_AVX512 __attribute__((__target__("avx512f")))
#elif defined(_MSC_VER)
#  include <intrin.h>
#  define XXH_TARGET_SSE2
#  define XXH_TARGET_AVX2
#  define XXH_TARGET_AVX512
#else
#  error "Dispatching is currently not supported for your compiler."
#endif

/*! @cond Doxygen ignores this part */
#ifdef XXH_DISPATCH_DEBUG
/* debug logging */
#  include <stdio.h>
#  define XXH_debugPrint(str) { fprintf(stderr, "DEBUG: xxHash dispatch: %s \n", str); fflush(NULL); }
#else
#  define XXH_debugPrint(str) ((void)0)
#  undef NDEBUG /* avoid redefinition */
#  define NDEBUG
#endif
/*! @endcond */
#include <assert.h>

#ifndef XXH_DOXYGEN
#define XXH_INLINE_ALL
#define XXH_X86DISPATCH
#include "xxhash.h"
#endif

/*! @cond Doxygen ignores this part */
#ifndef XXH_HAS_ATTRIBUTE
#  ifdef __has_attribute
#    define XXH_HAS_ATTRIBUTE(...) __has_attribute(__VA_ARGS__)
#  else
#    define XXH_HAS_ATTRIBUTE(...) 0
#  endif
#endif
/*! @endcond */

/*! @cond Doxygen ignores this part */
#if XXH_HAS_ATTRIBUTE(constructor)
#  define XXH_CONSTRUCTOR __attribute__((constructor))
#  define XXH_DISPATCH_MAYBE_NULL 0
#else
#  define XXH_CONSTRUCTOR
#  define XXH_DISPATCH_MAYBE_NULL 1
#endif
/*! @endcond */


/*! @cond Doxygen ignores this part */
/*
 * Support both AT&T and Intel dialects
 *
 * GCC doesn't convert AT&T syntax to Intel syntax, and will error out if
 * compiled with -masm=intel. Instead, it supports dialect switching with
 * curly braces: { AT&T syntax | Intel syntax }
 *
 * Clang's integrated assembler automatically converts AT&T syntax to Intel if
 * needed, making the dialect switching useless (it isn't even supported).
 *
 * Note: Comments are written in the inline assembly itself.
 */
#ifdef __clang__
#  define XXH_I_ATT(intel, att) att "\n\t"
#else
#  define XXH_I_ATT(intel, att) "{" att "|" intel "}\n\t"
#endif
/*! @endcond */

/*!
 * @private
 * @brief Runs CPUID.
 *
 * @param eax , ecx The parameters to pass to CPUID, %eax and %ecx respectively.
 * @param abcd The array to store the result in, `{ eax, ebx, ecx, edx }`
 */
static void XXH_cpuid(xxh_u32 eax, xxh_u32 ecx, xxh_u32* abcd)
{
#if defined(_MSC_VER)
    __cpuidex((int*)abcd, eax, ecx);
#else
    xxh_u32 ebx, edx;
# if defined(__i386__) && defined(__PIC__)
    __asm__(
        "# Call CPUID\n\t"
        "#\n\t"
        "# On 32-bit x86 with PIC enabled, we are not allowed to overwrite\n\t"
        "# EBX, so we use EDI instead.\n\t"
        XXH_I_ATT("mov     edi, ebx",   "movl    %%ebx, %%edi")
        XXH_I_ATT("cpuid",              "cpuid"               )
        XXH_I_ATT("xchg    edi, ebx",   "xchgl   %%ebx, %%edi")
        : "=D" (ebx),
# else
    __asm__(
        "# Call CPUID\n\t"
        XXH_I_ATT("cpuid",              "cpuid")
        : "=b" (ebx),
# endif
              "+a" (eax), "+c" (ecx), "=d" (edx));
    abcd[0] = eax;
    abcd[1] = ebx;
    abcd[2] = ecx;
    abcd[3] = edx;
#endif
}

/*
 * Modified version of Intel's guide
 * https://software.intel.com/en-us/articles/how-to-detect-new-instruction-support-in-the-4th-generation-intel-core-processor-family
 */

#if XXH_DISPATCH_AVX2 || XXH_DISPATCH_AVX512
/*!
 * @private
 * @brief Runs `XGETBV`.
 *
 * While the CPU may support AVX2, the operating system might not properly save
 * the full YMM/ZMM registers.
 *
 * xgetbv is used for detecting this: Any compliant operating system will define
 * a set of flags in the xcr0 register indicating how it saves the AVX registers.
 *
 * You can manually disable this flag on Windows by running, as admin:
 *
 *   bcdedit.exe /set xsavedisable 1
 *
 * and rebooting. Run the same command with 0 to re-enable it.
 */
static xxh_u64 XXH_xgetbv(void)
{
#if defined(_MSC_VER)
    return _xgetbv(0);  /* min VS2010 SP1 compiler is required */
#else
    xxh_u32 xcr0_lo, xcr0_hi;
    __asm__(
        "# Call XGETBV\n\t"
        "#\n\t"
        "# Older assemblers (e.g. macOS's ancient GAS version) don't support\n\t"
        "# the XGETBV opcode, so we encode it by hand instead.\n\t"
        "# See <https://github.com/asmjit/asmjit/issues/78> for details.\n\t"
        ".byte   0x0f, 0x01, 0xd0\n\t"
       : "=a" (xcr0_lo), "=d" (xcr0_hi) : "c" (0));
    return xcr0_lo | ((xxh_u64)xcr0_hi << 32);
#endif
}
#endif

/*! @cond Doxygen ignores this part */
#define XXH_SSE2_CPUID_MASK (1 << 26)
#define XXH_OSXSAVE_CPUID_MASK ((1 << 26) | (1 << 27))
#define XXH_AVX2_CPUID_MASK (1 << 5)
#define XXH_AVX2_XGETBV_MASK ((1 << 2) | (1 << 1))
#define XXH_AVX512F_CPUID_MASK (1 << 16)
#define XXH_AVX512F_XGETBV_MASK ((7 << 5) | (1 << 2) | (1 << 1))
/*! @endcond */

/*!
 * @private
 * @brief Returns the best XXH3 implementation.
 *
 * Runs various CPUID/XGETBV tests to try and determine the best implementation.
 *
 * @return The best @ref XXH_VECTOR implementation.
 * @see XXH_VECTOR_TYPES
 */
int XXH_featureTest(void)
{
    xxh_u32 abcd[4];
    xxh_u32 max_leaves;
    int best = XXH_SCALAR;
#if XXH_DISPATCH_AVX2 || XXH_DISPATCH_AVX512
    xxh_u64 xgetbv_val;
#endif
#if defined(__GNUC__) && defined(__i386__)
    xxh_u32 cpuid_supported;
    __asm__(
        "# For the sake of ruthless backwards compatibility, check if CPUID\n\t"
        "# is supported in the EFLAGS on i386.\n\t"
        "# This is not necessary on x86_64 - CPUID is mandatory.\n\t"
        "#   The ID flag (bit 21) in the EFLAGS register indicates support\n\t"
        "#   for the CPUID instruction. If a software procedure can set and\n\t"
        "#   clear this flag, the processor executing the procedure supports\n\t"
        "#   the CPUID instruction.\n\t"
        "#   <https://c9x.me/x86/html/file_module_x86_id_45.html>\n\t"
        "#\n\t"
        "# Routine is from <https://wiki.osdev.org/CPUID>.\n\t"

        "# Save EFLAGS\n\t"
        XXH_I_ATT("pushfd",                           "pushfl"                    )
        "# Store EFLAGS\n\t"
        XXH_I_ATT("pushfd",                           "pushfl"                    )
        "# Invert the ID bit in stored EFLAGS\n\t"
        XXH_I_ATT("xor     dword ptr[esp], 0x200000", "xorl    $0x200000, (%%esp)")
        "# Load stored EFLAGS (with ID bit inverted)\n\t"
        XXH_I_ATT("popfd",                            "popfl"                     )
        "# Store EFLAGS again (ID bit may or not be inverted)\n\t"
        XXH_I_ATT("pushfd",                           "pushfl"                    )
        "# eax = modified EFLAGS (ID bit may or may not be inverted)\n\t"
        XXH_I_ATT("pop     eax",                      "popl    %%eax"             )
        "# eax = whichever bits were changed\n\t"
        XXH_I_ATT("xor     eax, dword ptr[esp]",      "xorl    (%%esp), %%eax"    )
        "# Restore original EFLAGS\n\t"
        XXH_I_ATT("popfd",                            "popfl"                     )
        "# eax = zero if ID bit can't be changed, else non-zero\n\t"
        XXH_I_ATT("and     eax, 0x200000",            "andl    $0x200000, %%eax"  )
        : "=a" (cpuid_supported) :: "cc");

    if (XXH_unlikely(!cpuid_supported)) {
        XXH_debugPrint("CPUID support is not detected!");
        return best;
    }

#endif
    /* Check how many CPUID pages we have */
    XXH_cpuid(0, 0, abcd);
    max_leaves = abcd[0];

    /* Shouldn't happen on hardware, but happens on some QEMU configs. */
    if (XXH_unlikely(max_leaves == 0)) {
        XXH_debugPrint("Max CPUID leaves == 0!");
        return best;
    }

    /* Check for SSE2, OSXSAVE and xgetbv */
    XXH_cpuid(1, 0, abcd);

    /*
     * Test for SSE2. The check is redundant on x86_64, but it doesn't hurt.
     */
    if (XXH_unlikely((abcd[3] & XXH_SSE2_CPUID_MASK) != XXH_SSE2_CPUID_MASK))
        return best;

    XXH_debugPrint("SSE2 support detected.");

    best = XXH_SSE2;
#if XXH_DISPATCH_AVX2 || XXH_DISPATCH_AVX512
    /* Make sure we have enough leaves */
    if (XXH_unlikely(max_leaves < 7))
        return best;

    /* Test for OSXSAVE and XGETBV */
    if ((abcd[2] & XXH_OSXSAVE_CPUID_MASK) != XXH_OSXSAVE_CPUID_MASK)
        return best;

    /* CPUID check for AVX features */
    XXH_cpuid(7, 0, abcd);

    xgetbv_val = XXH_xgetbv();
#if XXH_DISPATCH_AVX2
    /* Validate that AVX2 is supported by the CPU */
    if ((abcd[1] & XXH_AVX2_CPUID_MASK) != XXH_AVX2_CPUID_MASK)
        return best;

    /* Validate that the OS supports YMM registers */
    if ((xgetbv_val & XXH_AVX2_XGETBV_MASK) != XXH_AVX2_XGETBV_MASK) {
        XXH_debugPrint("AVX2 supported by the CPU, but not the OS.");
        return best;
    }

    /* AVX2 supported */
    XXH_debugPrint("AVX2 support detected.");
    best = XXH_AVX2;
#endif
#if XXH_DISPATCH_AVX512
    /* Check if AVX512F is supported by the CPU */
    if ((abcd[1] & XXH_AVX512F_CPUID_MASK) != XXH_AVX512F_CPUID_MASK) {
        XXH_debugPrint("AVX512F not supported by CPU");
        return best;
    }

    /* Validate that the OS supports ZMM registers */
    if ((xgetbv_val & XXH_AVX512F_XGETBV_MASK) != XXH_AVX512F_XGETBV_MASK) {
        XXH_debugPrint("AVX512F supported by the CPU, but not the OS.");
        return best;
    }

    /* AVX512F supported */
    XXH_debugPrint("AVX512F support detected.");
    best = XXH_AVX512;
#endif
#endif
    return best;
}


/* ===   Vector implementations   === */

/*! @cond PRIVATE */
/*!
 * @private
 * @brief Defines the various dispatch functions.
 *
 * TODO: Consolidate?
 *
 * @param suffix The suffix for the functions, e.g. sse2 or scalar
 * @param target XXH_TARGET_* or empty.
 */

#define XXH_DEFINE_DISPATCH_FUNCS(suffix, target)                             \
                                                                              \
/* ===   XXH3, default variants   === */                                      \
                                                                              \
XXH_NO_INLINE target XXH64_hash_t                                             \
XXHL64_default_##suffix(XXH_NOESCAPE const void* XXH_RESTRICT input,          \
                        size_t len)                                           \
{                                                                             \
    return XXH3_hashLong_64b_internal(                                        \
               input, len, XXH3_kSecret, sizeof(XXH3_kSecret),                \
               XXH3_accumulate_##suffix, XXH3_scrambleAcc_##suffix            \
    );                                                                        \
}                                                                             \
                                                                              \
/* ===   XXH3, Seeded variants   === */                                       \
                                                                              \
XXH_NO_INLINE target XXH64_hash_t                                             \
XXHL64_seed_##suffix(XXH_NOESCAPE const void* XXH_RESTRICT input, size_t len, \
                     XXH64_hash_t seed)                                       \
{                                                                             \
    return XXH3_hashLong_64b_withSeed_internal(                               \
                    input, len, seed, XXH3_accumulate_##suffix,               \
                    XXH3_scrambleAcc_##suffix, XXH3_initCustomSecret_##suffix \
    );                                                                        \
}                                                                             \
                                                                              \
/* ===   XXH3, Secret variants   === */                                       \
                                                                              \
XXH_NO_INLINE target XXH64_hash_t                                             \
XXHL64_secret_##suffix(XXH_NOESCAPE const void* XXH_RESTRICT input,           \
                       size_t len, XXH_NOESCAPE const void* secret,           \
                       size_t secretLen)                                      \
{                                                                             \
    return XXH3_hashLong_64b_internal(                                        \
                    input, len, secret, secretLen,                            \
                    XXH3_accumulate_##suffix, XXH3_scrambleAcc_##suffix       \
    );                                                                        \
}                                                                             \
                                                                              \
/* ===   XXH3 update variants   === */                                        \
                                                                              \
XXH_NO_INLINE target XXH_errorcode                                            \
XXH3_update_##suffix(XXH_NOESCAPE XXH3_state_t* state,                        \
                     XXH_NOESCAPE const void* input, size_t len)              \
{                                                                             \
    return XXH3_update(state, (const xxh_u8*)input, len,                      \
                    XXH3_accumulate_##suffix, XXH3_scrambleAcc_##suffix);     \
}                                                                             \
                                                                              \
/* ===   XXH128 default variants   === */                                     \
                                                                              \
XXH_NO_INLINE target XXH128_hash_t                                            \
XXHL128_default_##suffix(XXH_NOESCAPE  const void* XXH_RESTRICT input,        \
                         size_t len)                                          \
{                                                                             \
    return XXH3_hashLong_128b_internal(                                       \
                    input, len, XXH3_kSecret, sizeof(XXH3_kSecret),           \
                    XXH3_accumulate_##suffix, XXH3_scrambleAcc_##suffix       \
    );                                                                        \
}                                                                             \
                                                                              \
/* ===   XXH128 Secret variants   === */                                      \
                                                                              \
XXH_NO_INLINE target XXH128_hash_t                                            \
XXHL128_secret_##suffix(XXH_NOESCAPE const void* XXH_RESTRICT input,          \
                        size_t len,                                           \
                        XXH_NOESCAPE const void* XXH_RESTRICT secret,         \
                        size_t secretLen)                                     \
{                                                                             \
    return XXH3_hashLong_128b_internal(                                       \
                    input, len, (const xxh_u8*)secret, secretLen,             \
                    XXH3_accumulate_##suffix, XXH3_scrambleAcc_##suffix);     \
}                                                                             \
                                                                              \
/* ===   XXH128 Seeded variants   === */                                      \
                                                                              \
XXH_NO_INLINE target XXH128_hash_t                                            \
XXHL128_seed_##suffix(XXH_NOESCAPE const void* XXH_RESTRICT input, size_t len,\
                      XXH64_hash_t seed)                                      \
{                                                                             \
    return XXH3_hashLong_128b_withSeed_internal(input, len, seed,             \
                    XXH3_accumulate_##suffix, XXH3_scrambleAcc_##suffix,      \
                    XXH3_initCustomSecret_##suffix);                          \
}

/*! @endcond */
/* End XXH_DEFINE_DISPATCH_FUNCS */

/*! @cond Doxygen ignores this part */
#if XXH_DISPATCH_SCALAR
XXH_DEFINE_DISPATCH_FUNCS(scalar, /* nothing */)
#endif
XXH_DEFINE_DISPATCH_FUNCS(sse2, XXH_TARGET_SSE2)
#if XXH_DISPATCH_AVX2
XXH_DEFINE_DISPATCH_FUNCS(avx2, XXH_TARGET_AVX2)
#endif
#if XXH_DISPATCH_AVX512
XXH_DEFINE_DISPATCH_FUNCS(avx512, XXH_TARGET_AVX512)
#endif
#undef XXH_DEFINE_DISPATCH_FUNCS
/*! @endcond */

/* ====    Dispatchers    ==== */

/*! @cond Doxygen ignores this part */
typedef XXH64_hash_t (*XXH3_dispatchx86_hashLong64_default)(XXH_NOESCAPE const void* XXH_RESTRICT, size_t);

typedef XXH64_hash_t (*XXH3_dispatchx86_hashLong64_withSeed)(XXH_NOESCAPE const void* XXH_RESTRICT, size_t, XXH64_hash_t);

typedef XXH64_hash_t (*XXH3_dispatchx86_hashLong64_withSecret)(XXH_NOESCAPE const void* XXH_RESTRICT, size_t, XXH_NOESCAPE const void* XXH_RESTRICT, size_t);

typedef XXH_errorcode (*XXH3_dispatchx86_update)(XXH_NOESCAPE XXH3_state_t*, XXH_NOESCAPE const void*, size_t);

typedef struct {
    XXH3_dispatchx86_hashLong64_default    hashLong64_default;
    XXH3_dispatchx86_hashLong64_withSeed   hashLong64_seed;
    XXH3_dispatchx86_hashLong64_withSecret hashLong64_secret;
    XXH3_dispatchx86_update                update;
} XXH_dispatchFunctions_s;

#define XXH_NB_DISPATCHES 4
/*! @endcond */

/*!
 * @private
 * @brief Table of dispatchers for @ref XXH3_64bits().
 *
 * @pre The indices must match @ref XXH_VECTOR_TYPE.
 */
static const XXH_dispatchFunctions_s XXH_kDispatch[XXH_NB_DISPATCHES] = {
#if XXH_DISPATCH_SCALAR
    /* Scalar */ { XXHL64_default_scalar, XXHL64_seed_scalar, XXHL64_secret_scalar, XXH3_update_scalar },
#else
    /* Scalar */ { NULL, NULL, NULL, NULL },
#endif
    /* SSE2   */ { XXHL64_default_sse2,   XXHL64_seed_sse2,   XXHL64_secret_sse2,   XXH3_update_sse2 },
#if XXH_DISPATCH_AVX2
    /* AVX2   */ { XXHL64_default_avx2,   XXHL64_seed_avx2,   XXHL64_secret_avx2,   XXH3_update_avx2 },
#else
    /* AVX2   */ { NULL, NULL, NULL, NULL },
#endif
#if XXH_DISPATCH_AVX512
    /* AVX512 */ { XXHL64_default_avx512, XXHL64_seed_avx512, XXHL64_secret_avx512, XXH3_update_avx512 }
#else
    /* AVX512 */ { NULL, NULL, NULL, NULL }
#endif
};
/*!
 * @private
 * @brief The selected dispatch table for @ref XXH3_64bits().
 */
static XXH_dispatchFunctions_s XXH_g_dispatch = { NULL, NULL, NULL, NULL };


/*! @cond Doxygen ignores this part */
typedef XXH128_hash_t (*XXH3_dispatchx86_hashLong128_default)(XXH_NOESCAPE const void* XXH_RESTRICT, size_t);

typedef XXH128_hash_t (*XXH3_dispatchx86_hashLong128_withSeed)(XXH_NOESCAPE const void* XXH_RESTRICT, size_t, XXH64_hash_t);

typedef XXH128_hash_t (*XXH3_dispatchx86_hashLong128_withSecret)(XXH_NOESCAPE const void* XXH_RESTRICT, size_t, XXH_NOESCAPE const void* XXH_RESTRICT, size_t);

typedef struct {
    XXH3_dispatchx86_hashLong128_default    hashLong128_default;
    XXH3_dispatchx86_hashLong128_withSeed   hashLong128_seed;
    XXH3_dispatchx86_hashLong128_withSecret hashLong128_secret;
    XXH3_dispatchx86_update                 update;
} XXH_dispatch128Functions_s;
/*! @endcond */


/*!
 * @private
 * @brief Table of dispatchers for @ref XXH3_128bits().
 *
 * @pre The indices must match @ref XXH_VECTOR_TYPE.
 */
static const XXH_dispatch128Functions_s XXH_kDispatch128[XXH_NB_DISPATCHES] = {
#if XXH_DISPATCH_SCALAR
    /* Scalar */ { XXHL128_default_scalar, XXHL128_seed_scalar, XXHL128_secret_scalar, XXH3_update_scalar },
#else
    /* Scalar */ { NULL, NULL, NULL, NULL },
#endif
    /* SSE2   */ { XXHL128_default_sse2,   XXHL128_seed_sse2,   XXHL128_secret_sse2,   XXH3_update_sse2 },
#if XXH_DISPATCH_AVX2
    /* AVX2   */ { XXHL128_default_avx2,   XXHL128_seed_avx2,   XXHL128_secret_avx2,   XXH3_update_avx2 },
#else
    /* AVX2   */ { NULL, NULL, NULL, NULL },
#endif
#if XXH_DISPATCH_AVX512
    /* AVX512 */ { XXHL128_default_avx512, XXHL128_seed_avx512, XXHL128_secret_avx512, XXH3_update_avx512 }
#else
    /* AVX512 */ { NULL, NULL, NULL, NULL }
#endif
};

/*!
 * @private
 * @brief The selected dispatch table for @ref XXH3_64bits().
 */
static XXH_dispatch128Functions_s XXH_g_dispatch128 = { NULL, NULL, NULL, NULL };

/*!
 * @private
 * @brief Runs a CPUID check and sets the correct dispatch tables.
 */
static XXH_CONSTRUCTOR void XXH_setDispatch(void)
{
    int vecID = XXH_featureTest();
    XXH_STATIC_ASSERT(XXH_AVX512 == XXH_NB_DISPATCHES-1);
    assert(XXH_SCALAR <= vecID && vecID <= XXH_AVX512);
#if !XXH_DISPATCH_SCALAR
    assert(vecID != XXH_SCALAR);
#endif
#if !XXH_DISPATCH_AVX512
    assert(vecID != XXH_AVX512);
#endif
#if !XXH_DISPATCH_AVX2
    assert(vecID != XXH_AVX2);
#endif
    XXH_g_dispatch = XXH_kDispatch[vecID];
    XXH_g_dispatch128 = XXH_kDispatch128[vecID];
}


/* ====    XXH3 public functions    ==== */
/*! @cond Doxygen ignores this part */

static XXH64_hash_t
XXH3_hashLong_64b_defaultSecret_selection(const void* XXH_RESTRICT input, size_t len,
                                          XXH64_hash_t seed64, const xxh_u8* XXH_RESTRICT secret, size_t secretLen)
{
    (void)seed64; (void)secret; (void)secretLen;
    if (XXH_DISPATCH_MAYBE_NULL && XXH_g_dispatch.hashLong64_default == NULL)
        XXH_setDispatch();
    return XXH_g_dispatch.hashLong64_default(input, len);
}

XXH64_hash_t XXH3_64bits_dispatch(XXH_NOESCAPE const void* input, size_t len)
{
    return XXH3_64bits_internal(input, len, 0, XXH3_kSecret, sizeof(XXH3_kSecret), XXH3_hashLong_64b_defaultSecret_selection);
}

static XXH64_hash_t
XXH3_hashLong_64b_withSeed_selection(const void* XXH_RESTRICT input, size_t len,
                                     XXH64_hash_t seed64, const xxh_u8* XXH_RESTRICT secret, size_t secretLen)
{
    (void)secret; (void)secretLen;
    if (XXH_DISPATCH_MAYBE_NULL && XXH_g_dispatch.hashLong64_seed == NULL)
        XXH_setDispatch();
    return XXH_g_dispatch.hashLong64_seed(input, len, seed64);
}

XXH64_hash_t XXH3_64bits_withSeed_dispatch(XXH_NOESCAPE const void* input, size_t len, XXH64_hash_t seed)
{
    return XXH3_64bits_internal(input, len, seed, XXH3_kSecret, sizeof(XXH3_kSecret), XXH3_hashLong_64b_withSeed_selection);
}

static XXH64_hash_t
XXH3_hashLong_64b_withSecret_selection(const void* XXH_RESTRICT input, size_t len,
                                       XXH64_hash_t seed64, const xxh_u8* XXH_RESTRICT secret, size_t secretLen)
{
    (void)seed64;
    if (XXH_DISPATCH_MAYBE_NULL && XXH_g_dispatch.hashLong64_secret == NULL)
        XXH_setDispatch();
    return XXH_g_dispatch.hashLong64_secret(input, len, secret, secretLen);
}

XXH64_hash_t XXH3_64bits_withSecret_dispatch(XXH_NOESCAPE const void* input, size_t len, XXH_NOESCAPE const void* secret, size_t secretLen)
{
    return XXH3_64bits_internal(input, len, 0, secret, secretLen, XXH3_hashLong_64b_withSecret_selection);
}

XXH_errorcode
XXH3_64bits_update_dispatch(XXH_NOESCAPE XXH3_state_t* state, XXH_NOESCAPE const void* input, size_t len)
{
    if (XXH_DISPATCH_MAYBE_NULL && XXH_g_dispatch.update == NULL)
        XXH_setDispatch();

    return XXH_g_dispatch.update(state, (const xxh_u8*)input, len);
}

/*! @endcond */


/* ====    XXH128 public functions    ==== */
/*! @cond Doxygen ignores this part */

static XXH128_hash_t
XXH3_hashLong_128b_defaultSecret_selection(const void* input, size_t len,
                                           XXH64_hash_t seed64, const void* secret, size_t secretLen)
{
    (void)seed64; (void)secret; (void)secretLen;
    if (XXH_DISPATCH_MAYBE_NULL && XXH_g_dispatch128.hashLong128_default == NULL)
        XXH_setDispatch();
    return XXH_g_dispatch128.hashLong128_default(input, len);
}

XXH128_hash_t XXH3_128bits_dispatch(XXH_NOESCAPE const void* input, size_t len)
{
    return XXH3_128bits_internal(input, len, 0, XXH3_kSecret, sizeof(XXH3_kSecret), XXH3_hashLong_128b_defaultSecret_selection);
}

static XXH128_hash_t
XXH3_hashLong_128b_withSeed_selection(const void* input, size_t len,
                                      XXH64_hash_t seed64, const void* secret, size_t secretLen)
{
    (void)secret; (void)secretLen;
    if (XXH_DISPATCH_MAYBE_NULL && XXH_g_dispatch128.hashLong128_seed == NULL)
        XXH_setDispatch();
    return XXH_g_dispatch128.hashLong128_seed(input, len, seed64);
}

XXH128_hash_t XXH3_128bits_withSeed_dispatch(XXH_NOESCAPE const void* input, size_t len, XXH64_hash_t seed)
{
    return XXH3_128bits_internal(input, len, seed, XXH3_kSecret, sizeof(XXH3_kSecret), XXH3_hashLong_128b_withSeed_selection);
}

static XXH128_hash_t
XXH3_hashLong_128b_withSecret_selection(const void* input, size_t len,
                                        XXH64_hash_t seed64, const void* secret, size_t secretLen)
{
    (void)seed64;
    if (XXH_DISPATCH_MAYBE_NULL && XXH_g_dispatch128.hashLong128_secret == NULL)
        XXH_setDispatch();
    return XXH_g_dispatch128.hashLong128_secret(input, len, secret, secretLen);
}

XXH128_hash_t XXH3_128bits_withSecret_dispatch(XXH_NOESCAPE const void* input, size_t len, XXH_NOESCAPE const void* secret, size_t secretLen)
{
    return XXH3_128bits_internal(input, len, 0, secret, secretLen, XXH3_hashLong_128b_withSecret_selection);
}

XXH_errorcode
XXH3_128bits_update_dispatch(XXH_NOESCAPE XXH3_state_t* state, XXH_NOESCAPE const void* input, size_t len)
{
    if (XXH_DISPATCH_MAYBE_NULL && XXH_g_dispatch128.update == NULL)
        XXH_setDispatch();
    return XXH_g_dispatch128.update(state, (const xxh_u8*)input, len);
}

/*! @endcond */

#if defined (__cplusplus)
}
#endif
/*! @} */
