#ifndef SHA1_H
#define SHA1_H
/* ================ sha1.h ================ */
/*
SHA-1 in C
By Steve Reid <steve@edmweb.com>
100% Public Domain
*/

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    unsigned char buffer[64];
} SHA1_CTX;

void SHA1Transform(uint32_t state[5], const unsigned char buffer[64]);
void SHA1Init(SHA1_CTX* context);
/* 'noinline' attribute is intended to prevent the `-Wstringop-overread` warning
 * when using gcc-12 later with LTO enabled. It may be removed once the
 * bug[https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80922] is fixed. */
__attribute__((noinline)) void SHA1Update(SHA1_CTX* context, const unsigned char* data, uint32_t len);
void SHA1Final(unsigned char digest[20], SHA1_CTX* context);

#ifdef REDIS_TEST
int sha1Test(int argc, char **argv, int flags);
#endif
#endif
