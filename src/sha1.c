
/* from valgrind tests */

/* ================ sha1.c ================ */
/*
SHA-1 in C
By Steve Reid <steve@edmweb.com>
100% Public Domain

Test Vectors (from FIPS PUB 180-1)
"abc"
  A9993E36 4706816A BA3E2571 7850C26C 9CD0D89D
"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
  84983E44 1C3BD26E BAAE4AA1 F95129E5 E54670F1
A million repetitions of "a"
  34AA973C D4C4DAA4 F61EEB2B DBAD2731 6534016F
*/

/* #define LITTLE_ENDIAN * This should be #define'd already, if true. */
/* #define SHA1HANDSOFF * Copies data before messing with it. */

#define SHA1HANDSOFF

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "solarisfixes.h"
#include "sha1.h"
#include "config.h"

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/* blk0() and blk() perform the initial expand. */
/* I got the idea of expanding during the round function from SSLeay */
#if BYTE_ORDER == LITTLE_ENDIAN
#define blk0(i) (block->l[i] = (rol(block->l[i],24)&0xFF00FF00) \
    |(rol(block->l[i],8)&0x00FF00FF))
#elif BYTE_ORDER == BIG_ENDIAN
#define blk0(i) block->l[i]
#else
#error "Endianness not defined!"
#endif
#define blk(i) (block->l[i&15] = rol(block->l[(i+13)&15]^block->l[(i+8)&15] \
    ^block->l[(i+2)&15]^block->l[i&15],1))

/* (R0+R1), R2, R3, R4 are the different operations used in SHA1 */
#define R0(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk0(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R1(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R2(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0x6ED9EBA1+rol(v,5);w=rol(w,30);
#define R3(v,w,x,y,z,i) z+=(((w|x)&y)|(w&x))+blk(i)+0x8F1BBCDC+rol(v,5);w=rol(w,30);
#define R4(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0xCA62C1D6+rol(v,5);w=rol(w,30);


/* Hash a single 512-bit block. This is the core of the algorithm. */

void SHA1Transform_software(uint32_t state[5], const unsigned char buffer[64])
{
    uint32_t a, b, c, d, e;
    typedef union {
        unsigned char c[64];
        uint32_t l[16];
    } CHAR64LONG16;
#ifdef SHA1HANDSOFF
    CHAR64LONG16 block[1];  /* use array to appear as a pointer */
    memcpy(block, buffer, 64);
#else
    /* The following had better never be used because it causes the
     * pointer-to-const buffer to be cast into a pointer to non-const.
     * And the result is written through.  I threw a "const" in, hoping
     * this will cause a diagnostic.
     */
    CHAR64LONG16* block = (const CHAR64LONG16*)buffer;
#endif
    /* Copy context->state[] to working vars */
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    /* 4 rounds of 20 operations each. Loop unrolled. */
    R0(a,b,c,d,e, 0); R0(e,a,b,c,d, 1); R0(d,e,a,b,c, 2); R0(c,d,e,a,b, 3);
    R0(b,c,d,e,a, 4); R0(a,b,c,d,e, 5); R0(e,a,b,c,d, 6); R0(d,e,a,b,c, 7);
    R0(c,d,e,a,b, 8); R0(b,c,d,e,a, 9); R0(a,b,c,d,e,10); R0(e,a,b,c,d,11);
    R0(d,e,a,b,c,12); R0(c,d,e,a,b,13); R0(b,c,d,e,a,14); R0(a,b,c,d,e,15);
    R1(e,a,b,c,d,16); R1(d,e,a,b,c,17); R1(c,d,e,a,b,18); R1(b,c,d,e,a,19);
    R2(a,b,c,d,e,20); R2(e,a,b,c,d,21); R2(d,e,a,b,c,22); R2(c,d,e,a,b,23);
    R2(b,c,d,e,a,24); R2(a,b,c,d,e,25); R2(e,a,b,c,d,26); R2(d,e,a,b,c,27);
    R2(c,d,e,a,b,28); R2(b,c,d,e,a,29); R2(a,b,c,d,e,30); R2(e,a,b,c,d,31);
    R2(d,e,a,b,c,32); R2(c,d,e,a,b,33); R2(b,c,d,e,a,34); R2(a,b,c,d,e,35);
    R2(e,a,b,c,d,36); R2(d,e,a,b,c,37); R2(c,d,e,a,b,38); R2(b,c,d,e,a,39);
    R3(a,b,c,d,e,40); R3(e,a,b,c,d,41); R3(d,e,a,b,c,42); R3(c,d,e,a,b,43);
    R3(b,c,d,e,a,44); R3(a,b,c,d,e,45); R3(e,a,b,c,d,46); R3(d,e,a,b,c,47);
    R3(c,d,e,a,b,48); R3(b,c,d,e,a,49); R3(a,b,c,d,e,50); R3(e,a,b,c,d,51);
    R3(d,e,a,b,c,52); R3(c,d,e,a,b,53); R3(b,c,d,e,a,54); R3(a,b,c,d,e,55);
    R3(e,a,b,c,d,56); R3(d,e,a,b,c,57); R3(c,d,e,a,b,58); R3(b,c,d,e,a,59);
    R4(a,b,c,d,e,60); R4(e,a,b,c,d,61); R4(d,e,a,b,c,62); R4(c,d,e,a,b,63);
    R4(b,c,d,e,a,64); R4(a,b,c,d,e,65); R4(e,a,b,c,d,66); R4(d,e,a,b,c,67);
    R4(c,d,e,a,b,68); R4(b,c,d,e,a,69); R4(a,b,c,d,e,70); R4(e,a,b,c,d,71);
    R4(d,e,a,b,c,72); R4(c,d,e,a,b,73); R4(b,c,d,e,a,74); R4(a,b,c,d,e,75);
    R4(e,a,b,c,d,76); R4(d,e,a,b,c,77); R4(c,d,e,a,b,78); R4(b,c,d,e,a,79);
    /* Add the working vars back into context.state[] */
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    /* Wipe variables */
    a = b = c = d = e = 0;
#ifdef SHA1HANDSOFF
    memset(block, '\0', sizeof(block));
#endif
}

#ifdef ARMV8_CE_SHA
void SHA1Transform_arm64(uint32_t state[5], const unsigned char buffer[64])
{
    /* declare variables */
    uint32x4_t k0, k1, k2, k3;
    uint32x4_t abcd, abcd0;
    uint32x4_t w0, w1, w2, w3;
    uint32_t   a, e, e0, e1;
    uint32x4_t wk0, wk1;

    /* set K0..K3 constants */
    k0 = vdupq_n_u32( 0x5A827999 );
    k1 = vdupq_n_u32( 0x6ED9EBA1 );
    k2 = vdupq_n_u32( 0x8F1BBCDC );
    k3 = vdupq_n_u32( 0xCA62C1D6 );

    /* load state */
    abcd = vld1q_u32( state );
    abcd0 = abcd;
    e = state[4];

    /* load message */
    w0 = vld1q_u32( (uint32_t const *)(buffer) );
    w1 = vld1q_u32( (uint32_t const *)(buffer + 16) );
    w2 = vld1q_u32( (uint32_t const *)(buffer + 32) );
    w3 = vld1q_u32( (uint32_t const *)(buffer + 48) );

    #ifdef LITTLE_ENDIAN_ORDER
    w0 = vreinterpretq_u32_u8( vrev32q_u8( vreinterpretq_u8_u32( w0 ) ) );
    w1 = vreinterpretq_u32_u8( vrev32q_u8( vreinterpretq_u8_u32( w1 ) ) );
    w2 = vreinterpretq_u32_u8( vrev32q_u8( vreinterpretq_u8_u32( w2 ) ) );
    w3 = vreinterpretq_u32_u8( vrev32q_u8( vreinterpretq_u8_u32( w3 ) ) );
    #endif

    /* initialize wk0 wk1 */
    wk0 = vaddq_u32( w0, k0 );
    wk1 = vaddq_u32( w1, k0 );

    /* perform rounds */
    a = vgetq_lane_u32( abcd, 0 );
    e1 = vsha1h_u32( a );
    abcd = vsha1cq_u32( abcd, e, wk0 ); /* 0 */
    wk0 = vaddq_u32( w2, k0 );
    w0 = vsha1su0q_u32( w0, w1, w2 );

    a = vgetq_lane_u32( abcd, 0 );
    e0 = vsha1h_u32( a );
    abcd = vsha1cq_u32( abcd, e1, wk1 ); /* 1 */
    wk1 = vaddq_u32( w3, k0 );
    w0 = vsha1su1q_u32( w0, w3 );
    w1 = vsha1su0q_u32( w1, w2, w3 );

    a = vgetq_lane_u32( abcd, 0 );
    e1 = vsha1h_u32( a );
    abcd = vsha1cq_u32( abcd, e0, wk0 ); /* 2 */
    wk0 = vaddq_u32( w0, k0 );
    w1 = vsha1su1q_u32( w1, w0 );
    w2 = vsha1su0q_u32( w2, w3, w0 );

    a = vgetq_lane_u32( abcd, 0 );
    e0 = vsha1h_u32( a );
    abcd = vsha1cq_u32( abcd, e1, wk1 ); /* 3 */
    wk1 = vaddq_u32( w1, k1 );
    w2 = vsha1su1q_u32( w2, w1 );
    w3 = vsha1su0q_u32( w3, w0, w1 );

    a = vgetq_lane_u32( abcd, 0 );
    e1 = vsha1h_u32( a );
    abcd = vsha1cq_u32( abcd, e0, wk0 ); /* 4 */
    wk0 = vaddq_u32( w2, k1 );
    w3 = vsha1su1q_u32( w3, w2 );
    w0 = vsha1su0q_u32( w0, w1, w2 );

    a = vgetq_lane_u32( abcd, 0 );
    e0 = vsha1h_u32( a );
    abcd = vsha1pq_u32( abcd, e1, wk1 ); /* 5 */
    wk1 = vaddq_u32( w3, k1 );
    w0 = vsha1su1q_u32( w0, w3 );
    w1 = vsha1su0q_u32( w1, w2, w3 );

    a = vgetq_lane_u32( abcd, 0 );
    e1 = vsha1h_u32( a );
    abcd = vsha1pq_u32( abcd, e0, wk0 ); /* 6 */
    wk0 = vaddq_u32( w0, k1 );
    w1 = vsha1su1q_u32( w1, w0 );
    w2 = vsha1su0q_u32( w2, w3, w0 );

    a = vgetq_lane_u32( abcd, 0 );
    e0 = vsha1h_u32( a );
    abcd = vsha1pq_u32( abcd, e1, wk1 ); /* 7 */
    wk1 = vaddq_u32( w1, k1 );
    w2 = vsha1su1q_u32( w2, w1 );
    w3 = vsha1su0q_u32( w3, w0, w1 );

    a = vgetq_lane_u32( abcd, 0 );
    e1 = vsha1h_u32( a );
    abcd = vsha1pq_u32( abcd, e0, wk0 ); /* 8 */
    wk0 = vaddq_u32( w2, k2 );
    w3 = vsha1su1q_u32( w3, w2 );
    w0 = vsha1su0q_u32( w0, w1, w2 );

    a = vgetq_lane_u32( abcd, 0 );
    e0 = vsha1h_u32( a );
    abcd = vsha1pq_u32( abcd, e1, wk1 ); /* 9 */
    wk1 = vaddq_u32( w3, k2 );
    w0 = vsha1su1q_u32( w0, w3 );
    w1 = vsha1su0q_u32( w1, w2, w3 );

    a = vgetq_lane_u32( abcd, 0 );
    e1 = vsha1h_u32( a );
    abcd = vsha1mq_u32( abcd, e0, wk0 ); /* 10 */
    wk0 = vaddq_u32( w0, k2 );
    w1 = vsha1su1q_u32( w1, w0 );
    w2 = vsha1su0q_u32( w2, w3, w0 );

    a = vgetq_lane_u32( abcd, 0 );
    e0 = vsha1h_u32( a );
    abcd = vsha1mq_u32( abcd, e1, wk1 ); /* 11 */
    wk1 = vaddq_u32( w1, k2 );
    w2 = vsha1su1q_u32( w2, w1 );
    w3 = vsha1su0q_u32( w3, w0, w1 );

    a = vgetq_lane_u32( abcd, 0 );
    e1 = vsha1h_u32( a );
    abcd = vsha1mq_u32( abcd, e0, wk0 ); /* 12 */
    wk0 = vaddq_u32( w2, k2 );
    w3 = vsha1su1q_u32( w3, w2 );
    w0 = vsha1su0q_u32( w0, w1, w2 );

    a = vgetq_lane_u32( abcd, 0 );
    e0 = vsha1h_u32( a );
    abcd = vsha1mq_u32( abcd, e1, wk1 ); /* 13 */
    wk1 = vaddq_u32( w3, k3 );
    w0 = vsha1su1q_u32( w0, w3 );
    w1 = vsha1su0q_u32( w1, w2, w3 );

    a = vgetq_lane_u32( abcd, 0 );
    e1 = vsha1h_u32( a );
    abcd = vsha1mq_u32( abcd, e0, wk0 ); /* 14 */
    wk0 = vaddq_u32( w0, k3 );
    w1 = vsha1su1q_u32( w1, w0 );
    w2 = vsha1su0q_u32( w2, w3, w0 );

    a = vgetq_lane_u32( abcd, 0 );
    e0 = vsha1h_u32( a );
    abcd = vsha1pq_u32( abcd, e1, wk1 ); /* 15 */
    wk1 = vaddq_u32( w1, k3 );
    w2 = vsha1su1q_u32( w2, w1 );
    w3 = vsha1su0q_u32( w3, w0, w1 );

    a = vgetq_lane_u32( abcd, 0 );
    e1 = vsha1h_u32( a );
    abcd = vsha1pq_u32( abcd, e0, wk0 ); /* 16 */
    wk0 = vaddq_u32( w2, k3 );
    w3 = vsha1su1q_u32( w3, w2 );
    w0 = vsha1su0q_u32( w0, w1, w2 );

    a = vgetq_lane_u32( abcd, 0 );
    e0 = vsha1h_u32( a );
    abcd = vsha1pq_u32( abcd, e1, wk1 ); /* 17 */
    wk1 = vaddq_u32( w3, k3 );
    w0 = vsha1su1q_u32( w0, w3 );

    a = vgetq_lane_u32( abcd, 0 );
    e1 = vsha1h_u32( a );
    abcd = vsha1pq_u32( abcd, e0, wk0 ); /* 18 */

    a = vgetq_lane_u32( abcd, 0 );
    e0 = vsha1h_u32( a );
    abcd = vsha1pq_u32( abcd, e1, wk1 ); /* 19 */

    e = e + e0;
    abcd = vaddq_u32( abcd0, abcd );

    /* save state */
    vst1q_u32(state, abcd );
    state[4] = e;
}

static int sha1_armv8_available(void)
{
    unsigned long auxv = getauxval(AT_HWCAP);
    return (auxv & HWCAP_CRC32) != 0;
}

#endif

void SHA1Transform(uint32_t state[5], const unsigned char buffer[64])
{
    #if defined(ARMV8_CE_SHA)
        if (sha1_armv8_available())
            SHA1Transform_arm64(state, buffer);
        else
            SHA1Transform_software(state, buffer);
    #else
        SHA1Transform_software(state, buffer);
    #endif
}

/* SHA1Init - Initialize new context */

void SHA1Init(SHA1_CTX* context)
{
    /* SHA1 initialization constants */
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = context->count[1] = 0;
}


/* Run your data through this. */

void SHA1Update(SHA1_CTX* context, const unsigned char* data, uint32_t len)
{
    uint32_t i, j;

    j = context->count[0];
    if ((context->count[0] += len << 3) < j)
        context->count[1]++;
    context->count[1] += (len>>29);
    j = (j >> 3) & 63;
    if ((j + len) > 63) {
        memcpy(&context->buffer[j], data, (i = 64-j));
        SHA1Transform(context->state, context->buffer);
        for ( ; i + 63 < len; i += 64) {
            SHA1Transform(context->state, &data[i]);
        }
        j = 0;
    }
    else i = 0;
    memcpy(&context->buffer[j], &data[i], len - i);
}


/* Add padding and return the message digest. */

void SHA1Final(unsigned char digest[20], SHA1_CTX* context)
{
    unsigned i;
    unsigned char finalcount[8];
    unsigned char c;

#if 0	/* untested "improvement" by DHR */
    /* Convert context->count to a sequence of bytes
     * in finalcount.  Second element first, but
     * big-endian order within element.
     * But we do it all backwards.
     */
    unsigned char *fcp = &finalcount[8];

    for (i = 0; i < 2; i++)
       {
        uint32_t t = context->count[i];
        int j;

        for (j = 0; j < 4; t >>= 8, j++)
	          *--fcp = (unsigned char) t;
    }
#else
    for (i = 0; i < 8; i++) {
        finalcount[i] = (unsigned char)((context->count[(i >= 4 ? 0 : 1)]
         >> ((3-(i & 3)) * 8) ) & 255);  /* Endian independent */
    }
#endif
    c = 0200;
    SHA1Update(context, &c, 1);
    while ((context->count[0] & 504) != 448) {
	c = 0000;
        SHA1Update(context, &c, 1);
    }
    SHA1Update(context, finalcount, 8);  /* Should cause a SHA1Transform() */
    for (i = 0; i < 20; i++) {
        digest[i] = (unsigned char)
         ((context->state[i>>2] >> ((3-(i & 3)) * 8) ) & 255);
    }
    /* Wipe variables */
    memset(context, '\0', sizeof(*context));
    memset(&finalcount, '\0', sizeof(finalcount));
}
/* ================ end of sha1.c ================ */

#ifdef REDIS_TEST
#define BUFSIZE 4096

#define UNUSED(x) (void)(x)
int sha1Test(int argc, char **argv)
{
    SHA1_CTX ctx;
    unsigned char hash[20], buf[BUFSIZE];
    int i;

    UNUSED(argc);
    UNUSED(argv);

    for(i=0;i<BUFSIZE;i++)
        buf[i] = i;

    SHA1Init(&ctx);
    for(i=0;i<1000;i++)
        SHA1Update(&ctx, buf, BUFSIZE);
    SHA1Final(hash, &ctx);

    printf("SHA1=");
    for(i=0;i<20;i++)
        printf("%02x", hash[i]);
    printf("\n");
    return 0;
}
#endif
