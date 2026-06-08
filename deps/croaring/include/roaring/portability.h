/*
 * portability.h
 *
 * This header centralizes compiler-, platform-, and architecture-specific
 * portability definitions used throughout CRoaring. It provides feature
 * detection, calling-convention and attribute macros, intrinsic and inline
 * assembly enablement, endianness helpers, alignment annotations, atomic
 * reference-count support, and other low-level compatibility glue.
 *
 * The goal is to keep these conditional definitions in one place so the rest
 * of the codebase can rely on a more uniform interface across GCC, Clang,
 * MSVC, x86/x64, ARM/NEON, and other supported environments.
 */

/**
 * All macros should be prefixed with either CROARING or ROARING.
 * The library uses both ROARING_...
 * as well as CROAIRING_ as prefixes. The ROARING_ prefix is for
 * macros that are provided by the build system or that are closely
 * related to the format. The header macros may also use ROARING_.
 * The CROARING_ prefix is for internal macros that a user is unlikely
 * to ever interact with.
 */

#ifndef CROARING_INCLUDE_PORTABILITY_H_
#define CROARING_INCLUDE_PORTABILITY_H_

// Users who need _GNU_SOURCE should define it?
// #ifndef _GNU_SOURCE
// #define _GNU_SOURCE 1
// #endif  // _GNU_SOURCE
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS 1
#endif  // __STDC_FORMAT_MACROS

#ifdef _MSC_VER
#define CROARING_VISUAL_STUDIO 1
/**
 * We want to differentiate carefully between
 * clang under visual studio and regular visual
 * studio.
 */
#ifdef __clang__
// clang under visual studio
#define CROARING_CLANG_VISUAL_STUDIO 1
#else
// just regular visual studio (best guess)
#define CROARING_REGULAR_VISUAL_STUDIO 1
#endif  // __clang__
#endif  // _MSC_VER
#ifndef CROARING_VISUAL_STUDIO
#define CROARING_VISUAL_STUDIO 0
#endif
#ifndef CROARING_CLANG_VISUAL_STUDIO
#define CROARING_CLANG_VISUAL_STUDIO 0
#endif
#ifndef CROARING_REGULAR_VISUAL_STUDIO
#define CROARING_REGULAR_VISUAL_STUDIO 0
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>  // will provide posix_memalign with _POSIX_C_SOURCE as defined above
#ifdef __GLIBC__
#include <malloc.h>  // this should never be needed but there are some reports that it is needed.
#endif

#ifdef __cplusplus
extern "C" {  // portability definitions are in global scope, not a namespace
#endif

#if defined(__SIZEOF_LONG_LONG__) && __SIZEOF_LONG_LONG__ != 8
#error This code assumes  64-bit long longs (by use of the GCC intrinsics). Your system is not currently supported.
#endif

#if CROARING_REGULAR_VISUAL_STUDIO
#ifndef __restrict__
#define __restrict__ __restrict
#endif  // __restrict__
#endif  // CROARING_REGULAR_VISUAL_STUDIO

#if defined(__x86_64__) || defined(_M_X64)
// we have an x64 processor
#define CROARING_IS_X64 1

#if defined(_MSC_VER) && (_MSC_VER < 1910)
// Old visual studio systems won't support AVX2 well.
#undef CROARING_IS_X64
#endif

#if defined(__clang_major__) && (__clang_major__ <= 8) && !defined(__AVX2__)
// Older versions of clang have a bug affecting us
// https://stackoverflow.com/questions/57228537/how-does-one-use-pragma-clang-attribute-push-with-c-namespaces
#undef CROARING_IS_X64
#endif

#ifdef ROARING_DISABLE_X64
#undef CROARING_IS_X64
#endif
// we include the intrinsic header
#if !CROARING_REGULAR_VISUAL_STUDIO
/* Non-Microsoft C/C++-compatible compiler */
#include <x86intrin.h>  // on some recent GCC, this will declare posix_memalign

#if CROARING_CLANG_VISUAL_STUDIO

/**
 * You are not supposed, normally, to include these
 * headers directly. Instead you should either include intrin.h
 * or x86intrin.h. However, when compiling with clang
 * under Windows (i.e., when _MSC_VER is set), these headers
 * only get included *if* the corresponding features are detected
 * from macros:
 * e.g., if __AVX2__ is set... in turn,  we normally set these
 * macros by compiling against the corresponding architecture
 * (e.g., arch:AVX2, -mavx2, etc.) which compiles the whole
 * software with these advanced instructions. These headers would
 * normally guard against such usage, but we carefully included
 * <x86intrin.h>  (or <intrin.h>) before, so the headers
 * are fooled.
 */
// To avoid reordering imports:
// clang-format off
#include <bmiintrin.h>   // for _blsr_u64
#include <lzcntintrin.h> // for  __lzcnt64
#include <immintrin.h>   // for most things (AVX2, AVX512, _popcnt64)
#include <smmintrin.h>
#include <tmmintrin.h>
#include <avxintrin.h>
#include <avx2intrin.h>
#include <wmmintrin.h>
#if _MSC_VER >= 1920
// Important: we need the AVX-512 headers:
#include <avx512fintrin.h>
#include <avx512dqintrin.h>
#include <avx512cdintrin.h>
#include <avx512bwintrin.h>
#include <avx512vlintrin.h>
#include <avx512vbmiintrin.h>
#include <avx512vbmi2intrin.h>
#include <avx512vpopcntdqintrin.h>
// clang-format on
#endif  // _MSC_VER >= 1920
// unfortunately, we may not get _blsr_u64, but, thankfully, clang
// has it as a macro.
#ifndef _blsr_u64
// we roll our own
#define _blsr_u64(n) ((n - 1) & n)
#endif  //  _blsr_u64
#endif  // SIMDJSON_CLANG_VISUAL_STUDIO

#endif  // CROARING_REGULAR_VISUAL_STUDIO
#endif  // defined(__x86_64__) || defined(_M_X64)

#if !defined(CROARING_USENEON) && !defined(DISABLENEON) && defined(__ARM_NEON)
#define CROARING_USENEON
#endif
#if defined(CROARING_USENEON)
#include <arm_neon.h>
#endif

#if defined(__e2k__)
// we have an e2k (Elbrus-2000) processor
#define CROARING_IS_E2K 1
#endif

#if !CROARING_REGULAR_VISUAL_STUDIO && !defined(CROARING_IS_E2K) && !__FILC__
/* Non-Microsoft C/C++-compatible compiler, assumes that it supports inline
 * assembly */
#define CROARING_INLINE_ASM 1
#endif  // _MSC_VER

#if CROARING_REGULAR_VISUAL_STUDIO
/* Microsoft C/C++-compatible compiler */
#include <intrin.h>

#ifndef __clang__  // if one compiles with MSVC *with* clang, then these
                   // intrinsics are defined!!!
#define CROARING_INTRINSICS 1
// sadly there is no way to check whether we are missing these intrinsics
// specifically.

/* wrappers for Visual Studio built-ins that look like gcc built-ins
 * __builtin_ctzll */
/** result might be undefined when input_num is zero */
inline int roaring_trailing_zeroes(unsigned long long input_num) {
    unsigned long index;
#ifdef _WIN64  // highly recommended!!!
    _BitScanForward64(&index, input_num);
#else   // if we must support 32-bit Windows
    if ((uint32_t)input_num != 0) {
        _BitScanForward(&index, (uint32_t)input_num);
    } else {
        _BitScanForward(&index, (uint32_t)(input_num >> 32));
        index += 32;
    }
#endif  // _WIN64
    return index;
}

/* wrappers for Visual Studio built-ins that look like gcc built-ins
 * __builtin_clzll */
/** result might be undefined when input_num is zero */
inline int roaring_leading_zeroes(unsigned long long input_num) {
    unsigned long index;
#ifdef _WIN64  // highly recommended!!!
    _BitScanReverse64(&index, input_num);
#else   // if we must support 32-bit Windows
    if (input_num > 0xFFFFFFFF) {
        _BitScanReverse(&index, (uint32_t)(input_num >> 32));
        index += 32;
    } else {
        _BitScanReverse(&index, (uint32_t)(input_num));
    }
#endif  // _WIN64
    return 63 - index;
}

/* Use #define so this is effective even under /Ob0 (no inline) */
#define roaring_unreachable __assume(0)
#endif  // __clang__

#endif  // CROARING_REGULAR_VISUAL_STUDIO

#ifndef CROARING_INTRINSICS
#define CROARING_INTRINSICS 1
#define roaring_unreachable __builtin_unreachable()
/** result might be undefined when input_num is zero */
inline int roaring_trailing_zeroes(unsigned long long input_num) {
    return __builtin_ctzll(input_num);
}
/** result might be undefined when input_num is zero */
inline int roaring_leading_zeroes(unsigned long long input_num) {
    return __builtin_clzll(input_num);
}
#endif

#if CROARING_REGULAR_VISUAL_STUDIO
#define ALIGNED(x) __declspec(align(x))
#elif defined(__GNUC__) || defined(__clang__)
#define ALIGNED(x) __attribute__((aligned(x)))
#else
#warning "Warning. Unrecognized compiler."
#define ALIGNED(x)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CROARING_WARN_UNUSED __attribute__((warn_unused_result))
#else
#define CROARING_WARN_UNUSED
#endif

#define IS_BIG_ENDIAN (*(uint16_t *)"\0\xff" < 0x100)

#ifdef CROARING_USENEON
// we can always compute the popcount fast.
#elif (defined(_M_ARM) || defined(_M_ARM64)) && \
    ((defined(_WIN64) || defined(_WIN32)) &&    \
     defined(CROARING_REGULAR_VISUAL_STUDIO) && \
     CROARING_REGULAR_VISUAL_STUDIO)
// we will need this function:
static inline int roaring_hamming_backup(uint64_t x) {
    uint64_t c1 = UINT64_C(0x5555555555555555);
    uint64_t c2 = UINT64_C(0x3333333333333333);
    uint64_t c4 = UINT64_C(0x0F0F0F0F0F0F0F0F);
    x -= (x >> 1) & c1;
    x = ((x >> 2) & c2) + (x & c2);
    x = (x + (x >> 4)) & c4;
    x *= UINT64_C(0x0101010101010101);
    return x >> 56;
}
#endif

static inline int roaring_hamming(uint64_t x) {
#if defined(_WIN64) && defined(CROARING_REGULAR_VISUAL_STUDIO) && \
    CROARING_REGULAR_VISUAL_STUDIO
#ifdef CROARING_USENEON
    return vaddv_u8(vcnt_u8(vcreate_u8(input_num)));
#elif defined(_M_ARM64)
    return roaring_hamming_backup(x);
    // (int) _CountOneBits64(x); is unavailable
#else   // _M_ARM64
    return (int)__popcnt64(x);
#endif  // _M_ARM64
#elif defined(_WIN32) && defined(CROARING_REGULAR_VISUAL_STUDIO) && \
    CROARING_REGULAR_VISUAL_STUDIO
#ifdef _M_ARM
    return roaring_hamming_backup(x);
    // _CountOneBits is unavailable
#else   // _M_ARM
    return (int)__popcnt((unsigned int)x) +
           (int)__popcnt((unsigned int)(x >> 32));
#endif  // _M_ARM
#else
    return __builtin_popcountll(x);
#endif
}

#ifndef UINT64_C
#define UINT64_C(c) (c##ULL)
#endif  // UINT64_C

#ifndef UINT32_C
#define UINT32_C(c) (c##UL)
#endif  // UINT32_C

#ifdef __cplusplus
}  // extern "C" {
#endif  // __cplusplus

// this is almost standard?
#undef STRINGIFY_IMPLEMENTATION_
#undef STRINGIFY
#define STRINGIFY_IMPLEMENTATION_(a) #a
#define STRINGIFY(a) STRINGIFY_IMPLEMENTATION_(a)

// Our fast kernels require 64-bit systems.
//
// On 32-bit x86, we lack 64-bit popcnt, lzcnt, blsr instructions.
// Furthermore, the number of SIMD registers is reduced.
//
// On 32-bit ARM, we would have smaller registers.
//
// The library should still have the fallback kernel. It is
// slower, but it should run everywhere.

//
// Enable valid runtime implementations, and select
// CROARING_BUILTIN_IMPLEMENTATION
//

// We are going to use runtime dispatch.
#if CROARING_IS_X64
#ifdef __clang__
// clang does not have GCC push pop
// warning: clang attribute push can't be used within a namespace in clang up
// til 8.0 so CROARING_TARGET_REGION and CROARING_UNTARGET_REGION must be
// *outside* of a namespace.
#define CROARING_TARGET_REGION(T)                                      \
    _Pragma(STRINGIFY(clang attribute push(__attribute__((target(T))), \
                                           apply_to = function)))
#define CROARING_UNTARGET_REGION _Pragma("clang attribute pop")
#elif defined(__GNUC__)
// GCC is easier
#define CROARING_TARGET_REGION(T) \
    _Pragma("GCC push_options") _Pragma(STRINGIFY(GCC target(T)))
#define CROARING_UNTARGET_REGION _Pragma("GCC pop_options")
#endif  // clang then gcc

#endif  // CROARING_IS_X64

// Default target region macros don't do anything.
#ifndef CROARING_TARGET_REGION
#define CROARING_TARGET_REGION(T)
#define CROARING_UNTARGET_REGION
#endif

#define CROARING_TARGET_AVX2 \
    CROARING_TARGET_REGION("avx2,bmi,pclmul,lzcnt,popcnt")
#define CROARING_TARGET_AVX512                                         \
    CROARING_TARGET_REGION(                                            \
        "avx2,bmi,bmi2,pclmul,lzcnt,popcnt,avx512f,avx512dq,avx512bw," \
        "avx512vbmi2,avx512bitalg,avx512vpopcntdq")
#define CROARING_UNTARGET_AVX2 CROARING_UNTARGET_REGION
#define CROARING_UNTARGET_AVX512 CROARING_UNTARGET_REGION

#ifdef __AVX2__
// No need for runtime dispatching.
// It is unnecessary and harmful to old clang to tag regions.
#undef CROARING_TARGET_AVX2
#define CROARING_TARGET_AVX2
#undef CROARING_UNTARGET_AVX2
#define CROARING_UNTARGET_AVX2
#endif

#if defined(__AVX512F__) && defined(__AVX512DQ__) && defined(__AVX512BW__) && \
    defined(__AVX512VBMI2__) && defined(__AVX512BITALG__) &&                  \
    defined(__AVX512VPOPCNTDQ__)
// No need for runtime dispatching.
// It is unnecessary and harmful to old clang to tag regions.
#undef CROARING_TARGET_AVX512
#define CROARING_TARGET_AVX512
#undef CROARING_UNTARGET_AVX512
#define CROARING_UNTARGET_AVX512
#endif

// Allow unaligned memory access
#if defined(__GNUC__) || defined(__clang__)
#define CROARING_ALLOW_UNALIGNED __attribute__((no_sanitize("alignment")))
#else
#define CROARING_ALLOW_UNALIGNED
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#define CROARING_IS_BIG_ENDIAN (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#elif defined(_WIN32)
#define CROARING_IS_BIG_ENDIAN 0
#else
#if defined(__APPLE__) || \
    defined(__FreeBSD__)  // defined __BYTE_ORDER__ && defined
                          // __ORDER_BIG_ENDIAN__
#include <machine/endian.h>
#elif defined(sun) || \
    defined(__sun)  // defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/byteorder.h>
#else  // defined(__APPLE__) || defined(__FreeBSD__)

#ifdef __has_include
#if __has_include(<endian.h>)
#include <endian.h>
#endif  //__has_include(<endian.h>)
#endif  //__has_include

#endif  // defined(__APPLE__) || defined(__FreeBSD__)

#ifndef !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__)
#define CROARING_IS_BIG_ENDIAN 0
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define CROARING_IS_BIG_ENDIAN 0
#else  // __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define CROARING_IS_BIG_ENDIAN 1
#endif  // __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#endif

// Host <-> big endian conversion.
#if CROARING_IS_BIG_ENDIAN
#define croaring_htobe64(x) (x)

#elif defined(_WIN32) || defined(_WIN64)  // CROARING_IS_BIG_ENDIAN
#include <stdlib.h>
#define croaring_htobe64(x) _byteswap_uint64(x)

#elif defined(__APPLE__)  // CROARING_IS_BIG_ENDIAN
#include <libkern/OSByteOrder.h>
#define croaring_htobe64(x) OSSwapInt64(x)

#elif defined(__has_include) && \
    __has_include(              \
        <byteswap.h>)  && (defined(__linux__) || defined(__FreeBSD__))  // CROARING_IS_BIG_ENDIAN
#include <byteswap.h>
#if defined(__linux__)
#define croaring_htobe64(x) bswap_64(x)
#elif defined(__FreeBSD__)
#define croaring_htobe64(x) bswap64(x)
#else
#warning "Unknown platform, report as an error"
#endif

#else  // CROARING_IS_BIG_ENDIAN
// Gets compiled to bswap or equivalent on most compilers.
#define croaring_htobe64(x)                                                    \
    (((x & 0x00000000000000FFULL) << 56) |                                     \
     ((x & 0x000000000000FF00ULL) << 40) |                                     \
     ((x & 0x0000000000FF0000ULL) << 24) |                                     \
     ((x & 0x00000000FF000000ULL) << 8) | ((x & 0x000000FF00000000ULL) >> 8) | \
     ((x & 0x0000FF0000000000ULL) >> 24) |                                     \
     ((x & 0x00FF000000000000ULL) >> 40) |                                     \
     ((x & 0xFF00000000000000ULL) >> 56))
#endif  // CROARING_IS_BIG_ENDIAN
#define croaring_be64toh(x) croaring_htobe64(x)
// End of host <-> big endian conversion.

// Defines for the possible CROARING atomic implementations
#define CROARING_ATOMIC_IMPL_NONE 1
#define CROARING_ATOMIC_IMPL_CPP 2
#define CROARING_ATOMIC_IMPL_C 3
#define CROARING_ATOMIC_IMPL_C_WINDOWS 4

// If the use has forced a specific implementation, use that, otherwise,
// figure out the best implementation we can use.
#if !defined(CROARING_ATOMIC_IMPL)
#if defined(__cplusplus) && __cplusplus >= 201103L
#ifdef __has_include
#if __has_include(<atomic>)
#define CROARING_ATOMIC_IMPL CROARING_ATOMIC_IMPL_CPP
#endif  //__has_include(<atomic>)
#else
   // We lack __has_include to check:
#define CROARING_ATOMIC_IMPL CROARING_ATOMIC_IMPL_CPP
#endif  //__has_include
#elif __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#define CROARING_ATOMIC_IMPL CROARING_ATOMIC_IMPL_C
#elif CROARING_REGULAR_VISUAL_STUDIO
   // https://www.technetworkhub.com/c11-atomics-in-visual-studio-2022-version-17/
#define CROARING_ATOMIC_IMPL CROARING_ATOMIC_IMPL_C_WINDOWS
#endif
#endif  // !defined(CROARING_ATOMIC_IMPL)

#if CROARING_ATOMIC_IMPL == CROARING_ATOMIC_IMPL_C
#include <stdatomic.h>
typedef _Atomic(uint32_t) croaring_refcount_t;

static inline void croaring_refcount_inc(croaring_refcount_t *val) {
    // Increasing the reference counter can always be done with
    // memory_order_relaxed: New references to an object can only be formed from
    // an existing reference, and passing an existing reference from one thread
    // to another must already provide any required synchronization.
    atomic_fetch_add_explicit(val, 1, memory_order_relaxed);
}

static inline bool croaring_refcount_dec(croaring_refcount_t *val) {
    // It is important to enforce any possible access to the object in one
    // thread (through an existing reference) to happen before deleting the
    // object in a different thread. This is achieved by a "release" operation
    // after dropping a reference (any access to the object through this
    // reference must obviously happened before), and an "acquire" operation
    // before deleting the object.
    bool is_zero = atomic_fetch_sub_explicit(val, 1, memory_order_release) == 1;
    if (is_zero) {
        atomic_thread_fence(memory_order_acquire);
    }
    return is_zero;
}

static inline uint32_t croaring_refcount_get(const croaring_refcount_t *val) {
    return atomic_load_explicit(val, memory_order_relaxed);
}
#elif CROARING_ATOMIC_IMPL == CROARING_ATOMIC_IMPL_CPP
#include <atomic>
typedef std::atomic<uint32_t> croaring_refcount_t;

static inline void croaring_refcount_inc(croaring_refcount_t *val) {
    val->fetch_add(1, std::memory_order_relaxed);
}

static inline bool croaring_refcount_dec(croaring_refcount_t *val) {
    // See above comments on the c11 atomic implementation for memory ordering
    bool is_zero = val->fetch_sub(1, std::memory_order_release) == 1;
    if (is_zero) {
        std::atomic_thread_fence(std::memory_order_acquire);
    }
    return is_zero;
}

static inline uint32_t croaring_refcount_get(const croaring_refcount_t *val) {
    return val->load(std::memory_order_relaxed);
}
#elif CROARING_ATOMIC_IMPL == CROARING_ATOMIC_IMPL_C_WINDOWS
#include <intrin.h>
#pragma intrinsic(_InterlockedIncrement)
#pragma intrinsic(_InterlockedDecrement)

// _InterlockedIncrement and _InterlockedDecrement take a (signed) long, and
// overflow is defined to wrap, so we can pretend it is a uint32_t for our case
typedef volatile long croaring_refcount_t;

static inline void croaring_refcount_inc(croaring_refcount_t *val) {
    _InterlockedIncrement(val);
}

static inline bool croaring_refcount_dec(croaring_refcount_t *val) {
    return _InterlockedDecrement(val) == 0;
}

static inline uint32_t croaring_refcount_get(const croaring_refcount_t *val) {
    // Per
    // https://learn.microsoft.com/en-us/windows/win32/sync/interlocked-variable-access
    // > Simple reads and writes to properly-aligned 32-bit variables are atomic
    // > operations. In other words, you will not end up with only one portion
    // > of the variable updated; all bits are updated in an atomic fashion.
    return *val;
}
#elif CROARING_ATOMIC_IMPL == CROARING_ATOMIC_IMPL_NONE
#include <assert.h>
typedef uint32_t croaring_refcount_t;

static inline void croaring_refcount_inc(croaring_refcount_t *val) {
    *val += 1;
}

static inline bool croaring_refcount_dec(croaring_refcount_t *val) {
    assert(*val > 0);
    *val -= 1;
    return val == 0;
}

static inline uint32_t croaring_refcount_get(const croaring_refcount_t *val) {
    return *val;
}
#else
#error "Unknown atomic implementation"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CROARING_DEPRECATED __attribute__((deprecated))
#elif defined(_MSC_VER)
#define CROARING_DEPRECATED __declspec(deprecated)
#else
#define CROARING_DEPRECATED
#endif  // defined(__GNUC__) || defined(__clang__)

// We want to initialize structs to zero portably (C and C++), without
// warnings. We can do mystruct s = CROARING_ZERO_INITIALIZER;
#if __cplusplus
#define CROARING_ZERO_INITIALIZER \
    {}
#else
#define CROARING_ZERO_INITIALIZER \
    { 0 }
#endif

#if defined(__cplusplus)
#define CROARING_STATIC_ASSERT(x, y) static_assert(x, y)
#else
#define CROARING_STATIC_ASSERT(x, y) _Static_assert(x, y)
#endif

// We need portability.h to be included first,
// but we also always want isadetection.h to be
// included (right after).
// See https://github.com/RoaringBitmap/CRoaring/issues/394
// There is no scenario where we want portability.h to
// be included, but not isadetection.h: the latter is a
// strict requirement.
#include <roaring/isadetection.h>  // include it last!
#endif                             /* INCLUDE_PORTABILITY_H_ */
