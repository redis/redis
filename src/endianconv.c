/* endinconv.c -- Endian conversions utilities.
 *
 * This functions are never called directly, but always using the macros
 * defined into endianconv.h, this way we define everything is a non-operation
 * if the arch is already little endian.
 *
 * Redis tries to encode everything as little endian (but a few things that need
 * to be backward compatible are still in big endian) because most of the
 * production environments are little endian, and we have a lot of conversions
 * in a few places because ziplists, intsets, zipmaps, need to be endian-neutral
 * even in memory, since they are serialized on RDB files directly with a single
 * write(2) without other additional steps.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2011-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */


#include <stdint.h>

/* Toggle the 16 bit unsigned integer pointed by *p from little endian to
 * big endian */
void memrev16(void *p) {
    unsigned char *x = p, t;

    t = x[0];
    x[0] = x[1];
    x[1] = t;
}

/* Toggle the 32 bit unsigned integer pointed by *p from little endian to
 * big endian */
void memrev32(void *p) {
    unsigned char *x = p, t;

    t = x[0];
    x[0] = x[3];
    x[3] = t;
    t = x[1];
    x[1] = x[2];
    x[2] = t;
}

/* Toggle the 64 bit unsigned integer pointed by *p from little endian to
 * big endian */
void memrev64(void *p) {
    unsigned char *x = p, t;

    t = x[0];
    x[0] = x[7];
    x[7] = t;
    t = x[1];
    x[1] = x[6];
    x[6] = t;
    t = x[2];
    x[2] = x[5];
    x[5] = t;
    t = x[3];
    x[3] = x[4];
    x[4] = t;
}

uint16_t intrev16(uint16_t v) {
    memrev16(&v);
    return v;
}

uint32_t intrev32(uint32_t v) {
    memrev32(&v);
    return v;
}

uint64_t intrev64(uint64_t v) {
    memrev64(&v);
    return v;
}

#ifdef REDIS_TEST
#include <stdio.h>

#define UNUSED(x) (void)(x)
int endianconvTest(int argc, char *argv[], int flags) {
    char buf[32];

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    snprintf(buf,sizeof(buf),"ciaoroma");
    memrev16(buf);
    printf("%s\n", buf);

    snprintf(buf,sizeof(buf),"ciaoroma");
    memrev32(buf);
    printf("%s\n", buf);

    snprintf(buf,sizeof(buf),"ciaoroma");
    memrev64(buf);
    printf("%s\n", buf);

    return 0;
}
#endif
