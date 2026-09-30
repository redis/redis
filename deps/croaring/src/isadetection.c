
/* From
https://github.com/endorno/pytorch/blob/master/torch/lib/TH/generic/simd/simd.h
Highly modified.

Copyright (c) 2016-     Facebook, Inc            (Adam Paszke)
Copyright (c) 2014-     Facebook, Inc            (Soumith Chintala)
Copyright (c) 2011-2014 Idiap Research Institute (Ronan Collobert)
Copyright (c) 2012-2014 Deepmind Technologies    (Koray Kavukcuoglu)
Copyright (c) 2011-2012 NEC Laboratories America (Koray Kavukcuoglu)
Copyright (c) 2011-2013 NYU                      (Clement Farabet)
Copyright (c) 2006-2010 NEC Laboratories America (Ronan Collobert, Leon Bottou,
Iain Melvin, Jason Weston) Copyright (c) 2006      Idiap Research Institute
(Samy Bengio) Copyright (c) 2001-2004 Idiap Research Institute (Ronan Collobert,
Samy Bengio, Johnny Mariethoz)

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the names of Facebook, Deepmind Technologies, NYU, NEC Laboratories
America and IDIAP Research Institute nor the names of its contributors may be
   used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// Binaries produced by Visual Studio 19.38 with solely AVX2 routines
// can compile to AVX-512 thus causing crashes on non-AVX-512 systems.
// This appears to affect VS 17.8 and 17.9. We disable AVX-512 and AVX2
// on these systems. It seems that ClangCL is not affected.
// https://github.com/RoaringBitmap/CRoaring/pull/603
#ifndef __clang__
#if _MSC_VER == 1938
#define ROARING_DISABLE_AVX 1
#endif  // _MSC_VER == 1938
#endif  // __clang__

#ifdef __FILC__
#include <stdfil.h>
#endif

// We need portability.h to be included first, see
// https://github.com/RoaringBitmap/CRoaring/issues/394
#include <roaring/portability.h>
#if CROARING_REGULAR_VISUAL_STUDIO
#include <intrin.h>
#elif (defined(HAVE_GCC_GET_CPUID) && defined(USE_GCC_GET_CPUID)) || \
    defined(__FILC__)
#include <cpuid.h>
#endif  // CROARING_REGULAR_VISUAL_STUDIO
#include <roaring/isadetection.h>

#if CROARING_IS_X64
#ifndef CROARING_COMPILER_SUPPORTS_AVX512
#error "CROARING_COMPILER_SUPPORTS_AVX512 needs to be defined."
#endif  // CROARING_COMPILER_SUPPORTS_AVX512
#endif

#ifdef __cplusplus
extern "C" {
namespace roaring {
namespace internal {
#endif
enum croaring_instruction_set {
    CROARING_DEFAULT = 0x0,
    CROARING_NEON = 0x1,
    CROARING_AVX2 = 0x4,
    CROARING_SSE42 = 0x8,
    CROARING_PCLMULQDQ = 0x10,
    CROARING_BMI1 = 0x20,
    CROARING_BMI2 = 0x40,
    CROARING_ALTIVEC = 0x80,
    CROARING_AVX512F = 0x100,
    CROARING_AVX512DQ = 0x200,
    CROARING_AVX512BW = 0x400,
    CROARING_AVX512VBMI2 = 0x800,
    CROARING_AVX512BITALG = 0x1000,
    CROARING_AVX512VPOPCNTDQ = 0x2000,
    CROARING_UNINITIALIZED = 0x8000
};

#if CROARING_COMPILER_SUPPORTS_AVX512
unsigned int CROARING_AVX512_REQUIRED =
    (CROARING_AVX512F | CROARING_AVX512DQ | CROARING_AVX512BW |
     CROARING_AVX512VBMI2 | CROARING_AVX512BITALG | CROARING_AVX512VPOPCNTDQ);
#endif

#if defined(__x86_64__) || defined(_M_AMD64)  // x64

static inline void cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx,
                         uint32_t *edx) {
#if CROARING_REGULAR_VISUAL_STUDIO
    int cpu_info[4];
    __cpuidex(cpu_info, *eax, *ecx);
    *eax = cpu_info[0];
    *ebx = cpu_info[1];
    *ecx = cpu_info[2];
    *edx = cpu_info[3];
#elif (defined(HAVE_GCC_GET_CPUID) && defined(USE_GCC_GET_CPUID)) || \
    defined(__FILC__)
    uint32_t level = *eax;
    __get_cpuid(level, eax, ebx, ecx, edx);
#else
    uint32_t a = *eax, b, c = *ecx, d;
    __asm__("cpuid\n\t" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    *eax = a;
    *ebx = b;
    *ecx = c;
    *edx = d;
#endif
}

static inline uint64_t xgetbv(void) {
#if defined(_MSC_VER)
    return _xgetbv(0);
#elif defined(__FILC__)
    return zxgetbv();
#else
    uint32_t xcr0_lo, xcr0_hi;
    __asm__("xgetbv\n\t" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    return xcr0_lo | ((uint64_t)xcr0_hi << 32);
#endif
}

/**
 * This is a relatively expensive function but it will get called at most
 * *once* per compilation units. Normally, the CRoaring library is built
 * as one compilation unit.
 */
static inline uint32_t dynamic_croaring_detect_supported_architectures(void) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t host_isa = 0x0;
    // Can be found on Intel ISA Reference for CPUID
    static uint32_t cpuid_avx2_bit =
        1 << 5;  ///< @private Bit 5 of EBX for EAX=0x7
    static uint32_t cpuid_bmi1_bit =
        1 << 3;  ///< @private bit 3 of EBX for EAX=0x7
    static uint32_t cpuid_bmi2_bit =
        1 << 8;  ///< @private bit 8 of EBX for EAX=0x7
    static uint32_t cpuid_avx512f_bit =
        1 << 16;  ///< @private bit 16 of EBX for EAX=0x7
    static uint32_t cpuid_avx512dq_bit =
        1 << 17;  ///< @private bit 17 of EBX for EAX=0x7
    static uint32_t cpuid_avx512bw_bit =
        1 << 30;  ///< @private bit 30 of EBX for EAX=0x7
    static uint32_t cpuid_avx512vbmi2_bit =
        1 << 6;  ///< @private bit 6 of ECX for EAX=0x7
    static uint32_t cpuid_avx512bitalg_bit =
        1 << 12;  ///< @private bit 12 of ECX for EAX=0x7
    static uint32_t cpuid_avx512vpopcntdq_bit =
        1 << 14;  ///< @private bit 14 of ECX for EAX=0x7
    static uint64_t cpuid_avx256_saved = 1 << 2;  ///< @private bit 2 = AVX
    static uint64_t cpuid_avx512_saved =
        7 << 5;  ///< @private bits 5,6,7 = opmask, ZMM_hi256, hi16_ZMM
    static uint32_t cpuid_sse42_bit =
        1 << 20;  ///< @private bit 20 of ECX for EAX=0x1
    static uint32_t cpuid_osxsave =
        (1 << 26) | (1 << 27);  ///< @private bits 26+27 of ECX for EAX=0x1
    static uint32_t cpuid_pclmulqdq_bit =
        1 << 1;  ///< @private bit  1 of ECX for EAX=0x1

    // EBX for EAX=0x1
    eax = 0x1;
    ecx = 0x0;
    cpuid(&eax, &ebx, &ecx, &edx);

    if (ecx & cpuid_sse42_bit) {
        host_isa |= CROARING_SSE42;
    } else {
        return host_isa;  // everything after is redundant
    }

    if (ecx & cpuid_pclmulqdq_bit) {
        host_isa |= CROARING_PCLMULQDQ;
    }

    if ((ecx & cpuid_osxsave) != cpuid_osxsave) {
        return host_isa;
    }

    // xgetbv for checking if the OS saves registers
    uint64_t xcr0 = xgetbv();

    if ((xcr0 & cpuid_avx256_saved) == 0) {
        return host_isa;
    }

    // ECX for EAX=0x7
    eax = 0x7;
    ecx = 0x0;
    cpuid(&eax, &ebx, &ecx, &edx);
    if (ebx & cpuid_avx2_bit) {
        host_isa |= CROARING_AVX2;
    }
    if (ebx & cpuid_bmi1_bit) {
        host_isa |= CROARING_BMI1;
    }

    if (ebx & cpuid_bmi2_bit) {
        host_isa |= CROARING_BMI2;
    }

    if (!((xcr0 & cpuid_avx512_saved) == cpuid_avx512_saved)) {
        return host_isa;
    }

    if (ebx & cpuid_avx512f_bit) {
        host_isa |= CROARING_AVX512F;
    }

    if (ebx & cpuid_avx512bw_bit) {
        host_isa |= CROARING_AVX512BW;
    }

    if (ebx & cpuid_avx512dq_bit) {
        host_isa |= CROARING_AVX512DQ;
    }

    if (ecx & cpuid_avx512vbmi2_bit) {
        host_isa |= CROARING_AVX512VBMI2;
    }

    if (ecx & cpuid_avx512bitalg_bit) {
        host_isa |= CROARING_AVX512BITALG;
    }

    if (ecx & cpuid_avx512vpopcntdq_bit) {
        host_isa |= CROARING_AVX512VPOPCNTDQ;
    }

    return host_isa;
}

#endif  // end SIMD extension detection code

#if defined(__x86_64__) || defined(_M_AMD64)  // x64

#if CROARING_ATOMIC_IMPL == CROARING_ATOMIC_IMPL_CPP
static inline uint32_t croaring_detect_supported_architectures(void) {
    // thread-safe as per the C++11 standard.
    static uint32_t buffer = dynamic_croaring_detect_supported_architectures();
    return buffer;
}
#elif CROARING_ATOMIC_IMPL == CROARING_ATOMIC_IMPL_C
static uint32_t croaring_detect_supported_architectures(void) {
    // we use an atomic for thread safety
    static _Atomic uint32_t buffer = CROARING_UNINITIALIZED;
    if (buffer == CROARING_UNINITIALIZED) {
        // atomicity is sufficient
        buffer = dynamic_croaring_detect_supported_architectures();
    }
    return buffer;
}
#else
// If we do not have atomics, we do the best we can.
static inline uint32_t croaring_detect_supported_architectures(void) {
    static uint32_t buffer = CROARING_UNINITIALIZED;
    if (buffer == CROARING_UNINITIALIZED) {
        buffer = dynamic_croaring_detect_supported_architectures();
    }
    return buffer;
}
#endif  // CROARING_C_ATOMIC

#ifdef ROARING_DISABLE_AVX

int croaring_hardware_support(void) { return 0; }

#elif defined(__AVX512F__) && defined(__AVX512DQ__) &&   \
    defined(__AVX512BW__) && defined(__AVX512VBMI2__) && \
    defined(__AVX512BITALG__) && defined(__AVX512VPOPCNTDQ__)
int croaring_hardware_support(void) {
    return ROARING_SUPPORTS_AVX2 | ROARING_SUPPORTS_AVX512;
}
#elif defined(__AVX2__)

int croaring_hardware_support(void) {
    static
#if CROARING_ATOMIC_IMPL == CROARING_ATOMIC_IMPL_C
        _Atomic
#endif
        int support = 0xFFFFFFF;
    if (support == 0xFFFFFFF) {
        bool avx512_support = false;
#if CROARING_COMPILER_SUPPORTS_AVX512
        avx512_support =
            ((croaring_detect_supported_architectures() &
              CROARING_AVX512_REQUIRED) == CROARING_AVX512_REQUIRED);
#endif
        support = ROARING_SUPPORTS_AVX2 |
                  (avx512_support ? ROARING_SUPPORTS_AVX512 : 0);
    }
    return support;
}
#else

int croaring_hardware_support(void) {
    static
#if CROARING_ATOMIC_IMPL == CROARING_ATOMIC_IMPL_C
        _Atomic
#endif
        int support = 0xFFFFFFF;
    if (support == 0xFFFFFFF) {
        bool has_avx2 = (croaring_detect_supported_architectures() &
                         CROARING_AVX2) == CROARING_AVX2;
        bool has_avx512 = false;
#if CROARING_COMPILER_SUPPORTS_AVX512
        has_avx512 = (croaring_detect_supported_architectures() &
                      CROARING_AVX512_REQUIRED) == CROARING_AVX512_REQUIRED;
#endif  // CROARING_COMPILER_SUPPORTS_AVX512
        support = (has_avx2 ? ROARING_SUPPORTS_AVX2 : 0) |
                  (has_avx512 ? ROARING_SUPPORTS_AVX512 : 0);
    }
    return support;
}
#endif

#endif  // defined(__x86_64__) || defined(_M_AMD64) // x64
#ifdef __cplusplus
}
}
}  // extern "C" { namespace roaring { namespace internal {
#endif
