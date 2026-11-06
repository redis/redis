/*
   SipHash reference C implementation

   Copyright (c) 2012-2016 Jean-Philippe Aumasson
   <jeanphilippe.aumasson@gmail.com>
   Copyright (c) 2012-2014 Daniel J. Bernstein <djb@cr.yp.to>
   Copyright (c) 2017-current Redis Ltd.

   To the extent possible under law, the author(s) have dedicated all copyright
   and related and neighboring rights to this software to the public domain
   worldwide. This software is distributed without any warranty.

   You should have received a copy of the CC0 Public Domain Dedication along
   with this software. If not, see
   <http://creativecommons.org/publicdomain/zero/1.0/>.

   ----------------------------------------------------------------------------

   This version was modified by Salvatore Sanfilippo <antirez@gmail.com>
   in the following ways:

   1. We use SipHash 1-2. This is not believed to be as strong as the
      suggested 2-4 variant, but AFAIK there are not trivial attacks
      against this reduced-rounds version, and it runs at the same speed
      as Murmurhash2 that we used previously, while the 2-4 variant slowed
      down Redis by a 4% figure more or less.
   2. Hard-code rounds in the hope the compiler can optimize it more
      in this raw form. Anyway we always want the standard 2-4 variant.
   3. Modify the prototype and implementation so that the function directly
      returns an uint64_t value, the hash itself, instead of receiving an
      output buffer. This also means that the output size is set to 8 bytes
      and the 16 bytes output code handling was removed.
   4. Provide a case insensitive variant to be used when hashing strings that
      must be considered identical by the hash table regardless of the case.
      If we don't have directly a case insensitive hash function, we need to
      perform a text transformation in some temporary buffer, which is costly.
   5. Remove debugging code.
   6. Modified the original test.c file to be a stand-alone function testing
      the function in the new form (returning an uint64_t) using just the
      relevant test vector.
 */
#define _DEFAULT_SOURCE 1  /* expose syscall()/riscv_hwprobe under strict -std */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* RISC-V siphash: by default the core follows the toolchain -march
 * (a zbb-capable march emits roli directly; otherwise fully portable, zero
 * runtime tax). SIPHASH_RISCV_DISPATCH is the only build define here: the
 * opt-in universal single-binary mode, which renames the portable core and
 * selects between it and the Zbb core at startup (see block below).
 * SIPHASH_RISCV_ZBB is used only by the separate siphash_zbb.c TU. */
#if defined(SIPHASH_RISCV_ZBB)
#define siphash siphash_zbb
#define siphash_nocase siphash_nocase_zbb
#elif defined(SIPHASH_RISCV_DISPATCH)
#define siphash siphash_portable
#define siphash_nocase siphash_nocase_portable
#endif

/* Fast tolower() alike function that does not care about locale
 * but just returns a-z instead of A-Z. */
static int siptlw(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c+('a'-'A');
    } else {
        return c;
    }
}

#if defined(__has_attribute)
#if __has_attribute(no_sanitize)
#define NO_SANITIZE(sanitizer) __attribute__((no_sanitize(sanitizer)))
#endif
#endif

#if !defined(NO_SANITIZE)
#define NO_SANITIZE(sanitizer)
#endif

/* Test of the CPU is Little Endian and supports not aligned accesses.
 * Two interesting conditions to speedup the function that happen to be
 * in most of x86 servers. */
#if defined(__X86_64__) || defined(__x86_64__) || defined (__i386__) \
	|| defined (__aarch64__) || defined (__arm64__) \
    || (defined(__riscv) && defined(__riscv_zicclsm))
#define UNALIGNED_LE_CPU
#endif

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define U32TO8_LE(p, v)                                                        \
    (p)[0] = (uint8_t)((v));                                                   \
    (p)[1] = (uint8_t)((v) >> 8);                                              \
    (p)[2] = (uint8_t)((v) >> 16);                                             \
    (p)[3] = (uint8_t)((v) >> 24);

#define U64TO8_LE(p, v)                                                        \
    U32TO8_LE((p), (uint32_t)((v)));                                           \
    U32TO8_LE((p) + 4, (uint32_t)((v) >> 32));

#ifdef UNALIGNED_LE_CPU
#define U8TO64_LE(p) (*((uint64_t*)(p)))
#elif defined(__riscv)
/* RISC-V without Zicclsm: unaligned 64-bit loads may trap; use one aligned
 * load when 8-byte aligned, byte-wise otherwise (LE, identical reads). */
#define U8TO64_LE(p)                                                           \
    ((((uintptr_t)(p) & 7) == 0)                                              \
         ? (*((const uint64_t *)(p)))                                          \
         : (((uint64_t)((p)[0])) | ((uint64_t)((p)[1]) << 8) |                 \
            ((uint64_t)((p)[2]) << 16) | ((uint64_t)((p)[3]) << 24) |          \
            ((uint64_t)((p)[4]) << 32) | ((uint64_t)((p)[5]) << 40) |          \
            ((uint64_t)((p)[6]) << 48) | ((uint64_t)((p)[7]) << 56)))
#else
#define U8TO64_LE(p)                                                           \
    (((uint64_t)((p)[0])) | ((uint64_t)((p)[1]) << 8) |                        \
     ((uint64_t)((p)[2]) << 16) | ((uint64_t)((p)[3]) << 24) |                 \
     ((uint64_t)((p)[4]) << 32) | ((uint64_t)((p)[5]) << 40) |                 \
     ((uint64_t)((p)[6]) << 48) | ((uint64_t)((p)[7]) << 56))
#endif

#define U8TO64_LE_NOCASE(p)                                                    \
    (((uint64_t)(siptlw((p)[0]))) |                                           \
     ((uint64_t)(siptlw((p)[1])) << 8) |                                      \
     ((uint64_t)(siptlw((p)[2])) << 16) |                                     \
     ((uint64_t)(siptlw((p)[3])) << 24) |                                     \
     ((uint64_t)(siptlw((p)[4])) << 32) |                                              \
     ((uint64_t)(siptlw((p)[5])) << 40) |                                              \
     ((uint64_t)(siptlw((p)[6])) << 48) |                                              \
     ((uint64_t)(siptlw((p)[7])) << 56))

#define SIPROUND                                                               \
    do {                                                                       \
        v0 += v1;                                                              \
        v1 = ROTL(v1, 13);                                                     \
        v1 ^= v0;                                                              \
        v0 = ROTL(v0, 32);                                                     \
        v2 += v3;                                                              \
        v3 = ROTL(v3, 16);                                                     \
        v3 ^= v2;                                                              \
        v0 += v3;                                                              \
        v3 = ROTL(v3, 21);                                                     \
        v3 ^= v0;                                                              \
        v2 += v1;                                                              \
        v1 = ROTL(v1, 17);                                                     \
        v1 ^= v2;                                                              \
        v2 = ROTL(v2, 32);                                                     \
    } while (0)

NO_SANITIZE("alignment")
uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k) {
#ifndef UNALIGNED_LE_CPU
    uint64_t hash;
    uint8_t *out = (uint8_t*) &hash;
#endif
    uint64_t v0 = 0x736f6d6570736575ULL;
    uint64_t v1 = 0x646f72616e646f6dULL;
    uint64_t v2 = 0x6c7967656e657261ULL;
    uint64_t v3 = 0x7465646279746573ULL;
    uint64_t k0 = U8TO64_LE(k);
    uint64_t k1 = U8TO64_LE(k + 8);
    uint64_t m;
    const uint8_t *end = in + inlen - (inlen % sizeof(uint64_t));
    const int left = inlen & 7;
    uint64_t b = ((uint64_t)inlen) << 56;
    v3 ^= k1;
    v2 ^= k0;
    v1 ^= k1;
    v0 ^= k0;

    for (; in != end; in += 8) {
        m = U8TO64_LE(in);
        v3 ^= m;

        SIPROUND;

        v0 ^= m;
    }

    switch (left) {
    case 7: b |= ((uint64_t)in[6]) << 48; /* fall-thru */
    case 6: b |= ((uint64_t)in[5]) << 40; /* fall-thru */
    case 5: b |= ((uint64_t)in[4]) << 32; /* fall-thru */
    case 4: b |= ((uint64_t)in[3]) << 24; /* fall-thru */
    case 3: b |= ((uint64_t)in[2]) << 16; /* fall-thru */
    case 2: b |= ((uint64_t)in[1]) << 8; /* fall-thru */
    case 1: b |= ((uint64_t)in[0]); break;
    case 0: break;
    }

    v3 ^= b;

    SIPROUND;

    v0 ^= b;
    v2 ^= 0xff;

    SIPROUND;
    SIPROUND;

    b = v0 ^ v1 ^ v2 ^ v3;
#ifndef UNALIGNED_LE_CPU
    U64TO8_LE(out, b);
    return hash;
#else
    return b;
#endif
}

NO_SANITIZE("alignment")
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k)
{
#ifndef UNALIGNED_LE_CPU
    uint64_t hash;
    uint8_t *out = (uint8_t*) &hash;
#endif
    uint64_t v0 = 0x736f6d6570736575ULL;
    uint64_t v1 = 0x646f72616e646f6dULL;
    uint64_t v2 = 0x6c7967656e657261ULL;
    uint64_t v3 = 0x7465646279746573ULL;
    uint64_t k0 = U8TO64_LE(k);
    uint64_t k1 = U8TO64_LE(k + 8);
    uint64_t m;
    const uint8_t *end = in + inlen - (inlen % sizeof(uint64_t));
    const int left = inlen & 7;
    uint64_t b = ((uint64_t)inlen) << 56;
    v3 ^= k1;
    v2 ^= k0;
    v1 ^= k1;
    v0 ^= k0;

    for (; in != end; in += 8) {
        m = U8TO64_LE_NOCASE(in);
        v3 ^= m;

        SIPROUND;

        v0 ^= m;
    }

    switch (left) {
    case 7: b |= ((uint64_t)siptlw(in[6])) << 48; /* fall-thru */
    case 6: b |= ((uint64_t)siptlw(in[5])) << 40; /* fall-thru */
    case 5: b |= ((uint64_t)siptlw(in[4])) << 32; /* fall-thru */
    case 4: b |= ((uint64_t)siptlw(in[3])) << 24; /* fall-thru */
    case 3: b |= ((uint64_t)siptlw(in[2])) << 16; /* fall-thru */
    case 2: b |= ((uint64_t)siptlw(in[1])) << 8; /* fall-thru */
    case 1: b |= ((uint64_t)siptlw(in[0])); break;
    case 0: break;
    }

    v3 ^= b;

    SIPROUND;

    v0 ^= b;
    v2 ^= 0xff;

    SIPROUND;
    SIPROUND;

    b = v0 ^ v1 ^ v2 ^ v3;
#ifndef UNALIGNED_LE_CPU
    U64TO8_LE(out, b);
    return hash;
#else
    return b;
#endif
}


/* --------------------------------- TEST ------------------------------------ */

#if defined(__riscv) && defined(SIPHASH_RISCV_DISPATCH)
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__has_include)
#  if __has_include(<asm/hwprobe.h>)
#    include <asm/hwprobe.h>
#  endif
#endif
#ifndef __NR_riscv_hwprobe
#define __NR_riscv_hwprobe 258  /* Linux >= 6.5 */
#endif
#ifndef RISCV_HWPROBE_KEY_IMA_EXT_0
#define RISCV_HWPROBE_KEY_IMA_EXT_0 4
struct riscv_hwprobe { long key; unsigned long value; };
#endif
#ifndef RISCV_HWPROBE_EXT_ZBB
#define RISCV_HWPROBE_EXT_ZBB (1ULL << 4)
#endif
#undef siphash
#undef siphash_nocase

extern uint64_t siphash_zbb(const uint8_t *in, size_t inlen, const uint8_t *k);
extern uint64_t siphash_nocase_zbb(const uint8_t *in, size_t inlen, const uint8_t *k);

static uint64_t (*siphash_impl)(const uint8_t *, size_t, const uint8_t *) = siphash_portable;
static uint64_t (*siphash_nocase_impl)(const uint8_t *, size_t, const uint8_t *) = siphash_nocase_portable;

static int siphash_riscv_zbb_probe(void) {
    /* riscv_hwprobe via raw syscall first (kernel >= 6.5; avoids depending
     * on <sys/hwprobe.h> from glibc), /proc/cpuinfo as fallback. */
#if defined(__linux__)
    struct riscv_hwprobe pair = { RISCV_HWPROBE_KEY_IMA_EXT_0, 0 };
    if (syscall(__NR_riscv_hwprobe, &pair, 1, 0, NULL, 0) == 0)
        return (pair.value & RISCV_HWPROBE_EXT_ZBB) ? 1 : 0;
#endif
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[512];
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "isa", 3) == 0 && strstr(line, "zbb")) { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

void siphash_init_riscv(void) {
    int zbb = 1;
    if (getenv("SIPHASH_DISABLE_ZBB") != NULL) zbb = 0;
    else zbb = siphash_riscv_zbb_probe();
    if (zbb) { siphash_impl = siphash_zbb; siphash_nocase_impl = siphash_nocase_zbb; }
    else     { siphash_impl = siphash_portable; siphash_nocase_impl = siphash_nocase_portable; }
}

uint64_t siphash(const uint8_t *in, size_t inlen, const uint8_t *k) {
    return siphash_impl(in, inlen, k);
}

uint64_t siphash_nocase(const uint8_t *in, size_t inlen, const uint8_t *k) {
    return siphash_nocase_impl(in, inlen, k);
}
#endif

#ifdef SIPHASH_TEST
/* Self-test for the 1-2 round variant this build uses (the upstream 2-4
 * round vectors do not apply). Golden values are identical for the portable
 * and Zbb cores, so this also validates the runtime-selected path. */
int siphash_test(void) {
    uint8_t k[16];
    for (int i = 0; i < 16; i++) k[i] = (uint8_t)i;
    const char *in[] = {"", "HELLO world", "key:12345",
                        "0123456789abcdef", "a.longer.key.forcing.two.blocks"};
    uint64_t exp[] = { 0xcea28b51565c12e2ULL, 0xf52472e910a1c769ULL,
                       0xaad89bb60a425b72ULL, 0x840afe4bca75c333ULL,
                       0xb3714b2e39b5760dULL };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
        if (siphash((const uint8_t *)in[i], strlen(in[i]), k) != exp[i]) fails++;
    }
    if (siphash_nocase((const uint8_t *)"HELLO world", 11, k) != 0xb91b8e97a1658e27ULL) fails++;
    if (fails == 0) { printf("SipHash test: OK\n"); return 0; }
    printf("SipHash test: FAILED\n");
    return 1;
}

int main(void) {
    return siphash_test();
}
#endif
