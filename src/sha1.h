#ifndef SHA1_H
#define SHA1_H
/* ================ sha1.h ================ */
/*
SHA-1 in C
By Steve Reid <steve@edmweb.com>
100% Public Domain
*/

#if defined(__ARM_ARCH) && (__ARM_ARCH > 7)
    #define ARMV8_CE_SHA
#endif

#ifdef ARMV8_CE_SHA
    #include <arm_neon.h>
    #include <sys/auxv.h>
    #include <asm/hwcap.h>

    #if defined(__BYTE_ORDER__)
        #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            #define LITTLE_ENDIAN_ORDER
        #else
            #define BIG_ENDIAN_ORDER
        #endif
    #else
        #error macro __BYTE_ORDER__ is not defined in host compiler
    #endif

    void SHA1Transform_arm64(uint32_t state[5], const unsigned char buffer[64]);
#endif

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    unsigned char buffer[64];
} SHA1_CTX;

void SHA1Transform(uint32_t state[5], const unsigned char buffer[64]);
void SHA1Transform_software(uint32_t state[5], const unsigned char buffer[64]);

void SHA1Init(SHA1_CTX* context);
void SHA1Update(SHA1_CTX* context, const unsigned char* data, uint32_t len);
void SHA1Final(unsigned char digest[20], SHA1_CTX* context);

#ifdef REDIS_TEST
int sha1Test(int argc, char **argv);
#endif
#endif
