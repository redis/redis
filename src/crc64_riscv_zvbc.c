/*
 * RISC-V Zvbc accelerated CRC64 for redis (RDB/AOF checksum path).
 *
 * This implementation is kept separate from the baseline feature probe and
 * the scalar Zbc implementation so unsupported CPUs never execute Zvbc code.
 */
#include "crc64.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern uint64_t crc64_riscv_zbc(uint64_t, const unsigned char *, uint64_t);

#if defined(__riscv_xlen) && (__riscv_xlen == 64) && defined(__riscv_zbc) && \
    defined(__riscv_zvbc) && (__riscv_zvbc > 0)
#include <riscv_vector.h>

#define REV UINT64_C(0x95ac9329ac4bc9b5)

static uint64_t crc64_riscv_tab[256];
static uint64_t foldk1, foldk2;
static uint64_t MC1[4], MC2[4];
static int crc64_riscv_zvbc_init_done = 0;

static inline uint64_t rv_clmul(uint64_t a, uint64_t b) {
    uint64_t r;
    __asm__ __volatile__("clmul %0,%1,%2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static inline uint64_t rv_clmulh(uint64_t a, uint64_t b) {
    uint64_t r;
    __asm__ __volatile__("clmulh %0,%1,%2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static uint64_t rv_rev64(uint64_t x) {
    x = ((x & UINT64_C(0x5555555555555555)) << 1) | ((x >> 1) & UINT64_C(0x5555555555555555));
    x = ((x & UINT64_C(0x3333333333333333)) << 2) | ((x >> 2) & UINT64_C(0x3333333333333333));
    x = ((x & UINT64_C(0x0f0f0f0f0f0f0f0f)) << 4) | ((x >> 4) & UINT64_C(0x0f0f0f0f0f0f0f0f));
    x = ((x & UINT64_C(0x00ff00ff00ff00ff)) << 8) | ((x >> 8) & UINT64_C(0x00ff00ff00ff00ff));
    x = ((x & UINT64_C(0x0000ffff0000ffff)) << 16) | ((x >> 16) & UINT64_C(0x0000ffff0000ffff));
    return (x << 32) | (x >> 32);
}

static uint64_t rv_xpow(long e) {
    uint64_t r = 1;
    for (long i = 0; i < e; i++) {
        int t = r >> 63;
        r <<= 1;
        if (t) r ^= UINT64_C(0xad93d23594c935a9);
    }
    return r;
}

static uint64_t crc64_rv_tail(uint64_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        crc = crc64_riscv_tab[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

static void crc64_riscv_zvbc_init(void) {
    for (unsigned i = 0; i < 256; i++) {
        uint64_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (REV & (uint64_t)(-(int64_t)(c & 1)));
        crc64_riscv_tab[i] = c;
    }
    foldk1 = rv_rev64(rv_xpow(63 + 512));
    foldk2 = rv_rev64(rv_xpow(511));
    for (unsigned j = 0; j < 4; j++) {
        long p = 128 * (long)(3 - j);
        MC1[j] = p ? rv_rev64(rv_xpow(63 + p)) : 0;
        MC2[j] = p ? rv_rev64(rv_xpow(p - 1)) : 0;
    }
    crc64_riscv_zvbc_init_done = 1;
}

static uint64_t crc64_riscv_zvbc_impl(uint64_t crc, const uint8_t *buf, size_t len) {
    const size_t vl = 4;
    vuint64m1x2_t sg = __riscv_vlseg2e64_v_u64m1x2((const uint64_t *)buf, vl);
    vuint64m1_t lo_v = __riscv_vget_v_u64m1x2_u64m1(sg, 0);
    vuint64m1_t hi_v = __riscv_vget_v_u64m1x2_u64m1(sg, 1);
    uint64_t tmp[4];
    __riscv_vse64_v_u64m1(tmp, lo_v, vl);
    tmp[0] ^= crc;
    lo_v = __riscv_vle64_v_u64m1(tmp, vl);
    buf += 64;
    len -= 64;
    while (len >= 64) {
        vuint64m1x2_t s2 = __riscv_vlseg2e64_v_u64m1x2((const uint64_t *)buf, vl);
        vuint64m1_t dl = __riscv_vget_v_u64m1x2_u64m1(s2, 0);
        vuint64m1_t dh = __riscv_vget_v_u64m1x2_u64m1(s2, 1);
        vuint64m1_t olo = lo_v, ohi = hi_v;
        lo_v = __riscv_vxor_vv_u64m1(__riscv_vxor_vv_u64m1(
            __riscv_vclmul_vx_u64m1(olo, foldk1, vl), __riscv_vclmul_vx_u64m1(ohi, foldk2, vl), vl), dl, vl);
        hi_v = __riscv_vxor_vv_u64m1(__riscv_vxor_vv_u64m1(
            __riscv_vclmulh_vx_u64m1(olo, foldk1, vl), __riscv_vclmulh_vx_u64m1(ohi, foldk2, vl), vl), dh, vl);
        buf += 64;
        len -= 64;
    }
    uint64_t loa[4], hia[4];
    __riscv_vse64_v_u64m1(loa, lo_v, vl);
    __riscv_vse64_v_u64m1(hia, hi_v, vl);
    uint64_t rlo = 0, rhi = 0;
    for (unsigned j = 0; j < 4; j++) {
        if (MC1[j] == 0 && MC2[j] == 0) {
            rlo ^= loa[j];
            rhi ^= hia[j];
        } else {
            rlo ^= rv_clmul(loa[j], MC1[j]) ^ rv_clmul(hia[j], MC2[j]);
            rhi ^= rv_clmulh(loa[j], MC1[j]) ^ rv_clmulh(hia[j], MC2[j]);
        }
    }
    uint8_t t[16];
    memcpy(t, &rlo, 8);
    memcpy(t + 8, &rhi, 8);
    uint64_t c = crc64_rv_tail(0, t, 16);
    if (len) c = crc64_rv_tail(c, buf, len);
    return c;
}
#endif

uint64_t crc64_riscv_zvbc(uint64_t crc, const unsigned char *buf, uint64_t len) {
#if defined(__riscv_xlen) && (__riscv_xlen == 64) && defined(__riscv_zbc) && \
    defined(__riscv_zvbc) && (__riscv_zvbc > 0)
    if (len < 128 || __riscv_vsetvlmax_e64m1() < 4)
        return crc64_riscv_zbc(crc, buf, len);

    if (!crc64_riscv_zvbc_init_done) crc64_riscv_zvbc_init();
    size_t prefix = (-(uintptr_t)buf) & (sizeof(uint64_t) - 1);
    if (prefix) {
        crc = crc64_rv_tail(crc, buf, prefix);
        buf += prefix;
        len -= prefix;
        if (len < 128) return crc64_riscv_zbc(crc, buf, len);
    }
    return crc64_riscv_zvbc_impl(crc, buf, len);
#else
    return crc64_riscv_zbc(crc, buf, len);
#endif
}
