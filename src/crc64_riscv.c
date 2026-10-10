/*
 * RISC-V accelerated CRC64 for redis (RDB/AOF checksum path).
 *
 * Two implementations on top of the scalar crcspeed slice-by-8 baseline
 * (non-inverted external CRC, reflected domain, polynomial 0xad93d23594c935a9):
 *
 *   - Zbc scalar: 128-bit 4-way parallel folding + per-lane merge using
 *     clmul/clmulh (rv64gc_zbc). Single-digit speedup on spacemit X100.
 *
 * Called only after baseline code has checked for Zbc support. Falls back to
 * the crcspeed slice-by-8 path on any other RISC-V or non-RISC-V build.
 *
 * Correctness: bit-exact with _crc64 for "123456789" (e9c6d914c4b8d9ca),
 * the official Lorem ipsum vector (c7794709e69683b3), plus randomized,
 * chained and boundary-length inputs.
 */
#include "crc64.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

extern uint64_t _crc64(uint_fast64_t crc, const void *in_data, uint64_t len);

static uint64_t crc64_riscv_reflect(uint64_t data) {
    data=((data>>1)&0x5555555555555555ULL)|((data&0x5555555555555555ULL)<<1);
    data=((data>>2)&0x3333333333333333ULL)|((data&0x3333333333333333ULL)<<2);
    data=((data>>4)&0x0F0F0F0F0F0F0F0FULL)|((data&0x0F0F0F0F0F0F0F0FULL)<<4);
    return __builtin_bswap64(data);
}

#if defined(__riscv_xlen) && (__riscv_xlen == 64) && defined(__riscv_zbc) && (__riscv_zbc > 0)
#define HAVE_RISCV_ZBC 1
#else
#define HAVE_RISCV_ZBC 0
#endif

const int crc64_riscv_compiled = HAVE_RISCV_ZBC;

#if HAVE_RISCV_ZBC

/* ---- slice-by-8 fallback for short inputs (crcspeed64little math) ---- */
static uint64_t crc64_slice8_tab[8][256];
static uint64_t crc64_sbitwise(uint64_t crc, const void *in_data, uint64_t len){
    const uint8_t *data=(const uint8_t*)in_data;
    for(uint64_t off=0;off<len;off++){
        uint8_t c=data[off];
        for(uint8_t i=0x01;i&0xff;i<<=1){
            uint64_t bit=crc&0x8000000000000000ULL;
            if(c&i) bit=!bit;
            crc<<=1; if(bit) crc^=UINT64_C(0xad93d23594c935a9);
        }
        crc&=0xffffffffffffffffULL;
    }
    return crc64_riscv_reflect(crc&0xffffffffffffffffULL);
}
static void crc64_slice8_init(void){
    for(int n=0;n<256;n++){ unsigned char v=n; crc64_slice8_tab[0][n]=crc64_sbitwise(0,&v,1); }
    for(int n=0;n<256;n++){
        uint64_t crc=crc64_slice8_tab[0][n];
        for(int k=1;k<8;k++){ crc=crc64_slice8_tab[0][crc&0xff]^(crc>>8); crc64_slice8_tab[k][n]=crc; }
    }
}
static uint64_t crc64_slice8(uint64_t crc, const void *buf, size_t len){
    const unsigned char *next=(const unsigned char*)buf;
    while(len&&((uintptr_t)next&7)!=0){ crc=crc64_slice8_tab[0][(crc^*next++)&0xff]^(crc>>8); len--; }
    while(len>=8){
        crc^=*(uint64_t*)next;
        crc=crc64_slice8_tab[7][(uint8_t)crc]^crc64_slice8_tab[6][(uint8_t)(crc>>8)]^
            crc64_slice8_tab[5][(uint8_t)(crc>>16)]^crc64_slice8_tab[4][(uint8_t)(crc>>24)]^
            crc64_slice8_tab[3][(uint8_t)(crc>>32)]^crc64_slice8_tab[2][(uint8_t)(crc>>40)]^
            crc64_slice8_tab[1][(uint8_t)(crc>>48)]^crc64_slice8_tab[0][crc>>56];
        next+=8; len-=8;
    }
    while(len){ crc=crc64_slice8_tab[0][(crc^*next++)&0xff]^(crc>>8); len--; }
    return crc;
}

/* ---- local slice-by-8 tables (redis crc64.c's crc64_table is file-local) ---- */
static uint64_t crc64_riscv_tab[256];
static uint64_t foldk1, foldk2;      /* main-loop fold constants (512-bit) */
static uint64_t MC1[8], MC2[8];      /* per-lane merge constants */

#define REV UINT64_C(0x95ac9329ac4bc9b5)

static inline uint64_t rv_clmul(uint64_t a, uint64_t b) {
    uint64_t r; __asm__ __volatile__("clmul %0,%1,%2":"=r"(r):"r"(a),"r"(b)); return r;
}
static inline uint64_t rv_clmulh(uint64_t a, uint64_t b) {
    uint64_t r; __asm__ __volatile__("clmulh %0,%1,%2":"=r"(r):"r"(a),"r"(b)); return r;
}
static inline uint64_t rv_rev64(uint64_t x){
    x=((x&0x5555555555555555ULL)<<1)|((x>>1)&0x5555555555555555ULL);
    x=((x&0x3333333333333333ULL)<<2)|((x>>2)&0x3333333333333333ULL);
    x=((x&0x0f0f0f0f0f0f0f0fULL)<<4)|((x>>4)&0x0f0f0f0f0f0f0f0fULL);
    x=((x&0x00ff00ff00ff00ffULL)<<8)|((x>>8)&0x00ff00ff00ff00ffULL);
    x=((x&0x0000ffff0000ffffULL)<<16)|((x>>16)&0x0000ffff0000ffffULL);
    return (x<<32)|(x>>32);
}
static uint64_t rv_xpow(long e){
    uint64_t r=1;
    for(long i=0;i<e;i++){ int t=r>>63; r<<=1; if(t) r^=UINT64_C(0xad93d23594c935a9); }
    return r;
}
static uint64_t crc64_rv_tail(uint64_t crc, const uint8_t *p, size_t n){
    for(size_t i=0;i<n;i++) crc = crc64_riscv_tab[(crc^p[i])&0xff] ^ (crc>>8);
    return crc;
}
static int crc64_riscv_init_done = 0;
static void crc64_riscv_init_tables(void){
    crc64_slice8_init();
    for(unsigned i=0;i<256;i++){ uint64_t c=i; for(int k=0;k<8;k++) c=(c>>1)^(REV&(uint64_t)(-(int64_t)(c&1))); crc64_riscv_tab[i]=c; }
    foldk1=rv_rev64(rv_xpow(63+512)); foldk2=rv_rev64(rv_xpow(511));
    for(unsigned j=0;j<4;j++){ long p=128*(long)(3-j); MC1[j]=p?rv_rev64(rv_xpow(63+p)):0; MC2[j]=p?rv_rev64(rv_xpow(p-1)):0; }
    crc64_riscv_init_done = 1;
}

/* Zbc scalar: 4 independent 128-bit lanes, fold-512 per 64-byte iter */
static uint64_t crc64_riscv_zbc(uint64_t crc, const uint8_t *buf, size_t len){
    uint64_t x0,x1,y0,y1,z0,z1,w0,w1;
    memcpy(&x0,buf+0,8); memcpy(&x1,buf+8,8);
    memcpy(&y0,buf+16,8); memcpy(&y1,buf+24,8);
    memcpy(&z0,buf+32,8); memcpy(&z1,buf+40,8);
    memcpy(&w0,buf+48,8); memcpy(&w1,buf+56,8);
    x0 ^= crc;
    buf += 64; len -= 64;
    while(len>=64){
        uint64_t d0,d1;
        memcpy(&d0,buf+0,8); memcpy(&d1,buf+8,8);
        { uint64_t l=rv_clmul(x0,foldk1)^rv_clmul(x1,foldk2), h=rv_clmulh(x0,foldk1)^rv_clmulh(x1,foldk2); x0=l^d0; x1=h^d1; }
        memcpy(&d0,buf+16,8); memcpy(&d1,buf+24,8);
        { uint64_t l=rv_clmul(y0,foldk1)^rv_clmul(y1,foldk2), h=rv_clmulh(y0,foldk1)^rv_clmulh(y1,foldk2); y0=l^d0; y1=h^d1; }
        memcpy(&d0,buf+32,8); memcpy(&d1,buf+40,8);
        { uint64_t l=rv_clmul(z0,foldk1)^rv_clmul(z1,foldk2), h=rv_clmulh(z0,foldk1)^rv_clmulh(z1,foldk2); z0=l^d0; z1=h^d1; }
        memcpy(&d0,buf+48,8); memcpy(&d1,buf+56,8);
        { uint64_t l=rv_clmul(w0,foldk1)^rv_clmul(w1,foldk2), h=rv_clmulh(w0,foldk1)^rv_clmulh(w1,foldk2); w0=l^d0; w1=h^d1; }
        buf += 64; len -= 64;
    }
    uint64_t Lo[4]={x0,y0,z0,w0}, Hi[4]={x1,y1,z1,w1};
    uint64_t rlo=0, rhi=0;
    for(unsigned j=0;j<4;j++){
        if(MC1[j]==0 && MC2[j]==0){ rlo^=Lo[j]; rhi^=Hi[j]; }
        else { rlo^=rv_clmul(Lo[j],MC1[j])^rv_clmul(Hi[j],MC2[j]);
               rhi^=rv_clmulh(Lo[j],MC1[j])^rv_clmulh(Hi[j],MC2[j]); }
    }
    uint8_t t[16]; memcpy(t,&rlo,8); memcpy(t+8,&rhi,8);
    uint64_t c = crc64_rv_tail(0,t,16);
    if(len) c = crc64_rv_tail(c,buf,len);
    return c;
}

#endif /* riscv64 + zbc */

/* ---- public API (same contract as crcspeed64native) ---- */
extern uint64_t crc64_riscv(uint64_t crc, const unsigned char *buf, uint64_t len) {
#if HAVE_RISCV_ZBC
    if (!crc64_riscv_init_done) crc64_riscv_init_tables();
    if (len < 128)
        return crc64_slice8(crc, buf, len);
    return crc64_riscv_zbc(crc, buf, len);
#else
    /* _crc64 returns a reflected CRC, but consumes an unreflected state. */
    return _crc64(crc64_riscv_reflect(crc), buf, len);
#endif
}
