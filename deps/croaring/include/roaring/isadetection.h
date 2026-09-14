/*
 * isadetection.h
 *
 * This header declares the small interface used to detect instruction-set
 * capabilities relevant to CRoaring's optimized kernels on x64 platforms. It
 * also defines compile-time feature macros that indicate whether the compiler
 * toolchain is capable of building AVX-512 code paths.
 *
 * The resulting flags are used to decide whether accelerated implementations,
 * such as AVX2 or AVX-512 variants, can be selected safely at runtime.
 */
#ifndef ROARING_ISADETECTION_H
#define ROARING_ISADETECTION_H
#if defined(__x86_64__) || defined(_M_AMD64)  // x64

#ifndef CROARING_COMPILER_SUPPORTS_AVX512
#ifdef __has_include
// We want to make sure that the AVX-512 functions are only built on compilers
// fully supporting AVX-512.
#if __has_include(<avx512vbmi2intrin.h>)
#define CROARING_COMPILER_SUPPORTS_AVX512 1
#endif  // #if __has_include(<avx512vbmi2intrin.h>)
#endif  // #ifdef __has_include

// Visual Studio 2019 and up support AVX-512
#ifdef _MSC_VER
#if _MSC_VER >= 1920
#define CROARING_COMPILER_SUPPORTS_AVX512 1
#endif  // #if _MSC_VER >= 1920
#endif  // #ifdef _MSC_VER

#ifndef CROARING_COMPILER_SUPPORTS_AVX512
#define CROARING_COMPILER_SUPPORTS_AVX512 0
#endif  // #ifndef CROARING_COMPILER_SUPPORTS_AVX512
#endif  // #ifndef CROARING_COMPILER_SUPPORTS_AVX512

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif
enum {
    ROARING_SUPPORTS_AVX2 = 1,
    ROARING_SUPPORTS_AVX512 = 2,
};
int croaring_hardware_support(void);
#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif
#endif  // x64
#endif  // ROARING_ISADETECTION_H
