/* Shared MurmurHash2 64-bit implementation, extracted from hyperloglog.c.
 *
 * Copyright (c) 2014-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

#include "murmurhash.h"
#include "config.h"
#include <string.h>

/* Our hash function is MurmurHash2, 64 bit version.
 * It was modified for Redis in order to provide the same result in
 * big and little endian archs (endian neutral). */
REDIS_NO_SANITIZE("alignment")
uint64_t MurmurHash64A (const void * key, size_t len, uint64_t seed) {
    const uint64_t m = 0xc6a4a7935bd1e995;
    const int r = 47;
    uint64_t h = seed ^ (len * m);
    const uint8_t *data = (const uint8_t *)key;
    const uint8_t *end = data + (len-(len&7));

    while(data != end) {
        uint64_t k;

#if (BYTE_ORDER == LITTLE_ENDIAN)
    #ifdef USE_ALIGNED_ACCESS
        memcpy(&k,data,sizeof(uint64_t));
    #else
        k = *((uint64_t*)data);
    #endif
#else
        k = (uint64_t) data[0];
        k |= (uint64_t) data[1] << 8;
        k |= (uint64_t) data[2] << 16;
        k |= (uint64_t) data[3] << 24;
        k |= (uint64_t) data[4] << 32;
        k |= (uint64_t) data[5] << 40;
        k |= (uint64_t) data[6] << 48;
        k |= (uint64_t) data[7] << 56;
#endif

        k *= m;
        k ^= k >> r;
        k *= m;
        h ^= k;
        h *= m;
        data += 8;
    }

    switch(len & 7) {
    case 7: h ^= (uint64_t)data[6] << 48; /* fall-thru */
    case 6: h ^= (uint64_t)data[5] << 40; /* fall-thru */
    case 5: h ^= (uint64_t)data[4] << 32; /* fall-thru */
    case 4: h ^= (uint64_t)data[3] << 24; /* fall-thru */
    case 3: h ^= (uint64_t)data[2] << 16; /* fall-thru */
    case 2: h ^= (uint64_t)data[1] << 8; /* fall-thru */
    case 1: h ^= (uint64_t)data[0];
            h *= m; /* fall-thru */
    };

    h ^= h >> r;
    h *= m;
    h ^= h >> r;
    return h;
}

#ifdef REDIS_TEST
#include <stdio.h>
#include "testhelp.h"

int murmurhashTest(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;
    /* Original HLL and little-endian RedisBloom outputs for bytes i * 37,
     * lengths 0..16: HLL seed, Bloom seed, and Bloom's second hash. */
    static const uint64_t expected[][3] = {
        {UINT64_C(0xd8dfea6585bc9732), UINT64_C(0x1ab11ea5a7b2c56e), UINT64_C(0xbbddcb5ab56dd547)},
        {UINT64_C(0xa55b92ce23afa288), UINT64_C(0x0000000000000000), UINT64_C(0x5825f5f3bd962979)},
        {UINT64_C(0x59a12bdc1f53564a), UINT64_C(0xdf8e711878217767), UINT64_C(0xedbfd4d10c6114f3)},
        {UINT64_C(0xc1e619be04492918), UINT64_C(0x29fa17f86bf4efc1), UINT64_C(0x75cd46d1e1fc1493)},
        {UINT64_C(0x936456d78485ec91), UINT64_C(0xdaf2f6806d9982e9), UINT64_C(0xd9f335c148baa231)},
        {UINT64_C(0x8be139d8f7ed2827), UINT64_C(0x0157c623c33826bc), UINT64_C(0x3e9245ec737438c0)},
        {UINT64_C(0x1ebf69a66474d60a), UINT64_C(0xa1a92e6598c8d3d3), UINT64_C(0x30ec92efc97f5ba9)},
        {UINT64_C(0x3af205d7e1ffaee2), UINT64_C(0x22c0ab7ea4a1a3d3), UINT64_C(0x58150974015c2bc8)},
        {UINT64_C(0x904680e50821c0c2), UINT64_C(0x01fc7ca3fd7fc2eb), UINT64_C(0xf80ab7e60dcedba5)},
        {UINT64_C(0xc855a34c90b18880), UINT64_C(0xd22b9320e9d4f488), UINT64_C(0x0fb9e9901c793a39)},
        {UINT64_C(0xdc001dfa5e743b4b), UINT64_C(0xbbb1c9f141f03e16), UINT64_C(0x5c938d3e1048465f)},
        {UINT64_C(0xdc5ac8114ac86977), UINT64_C(0xef7649cdb9500412), UINT64_C(0x065849c79d348360)},
        {UINT64_C(0x7e0541c1d8ef5b74), UINT64_C(0x08b0cd475db467c5), UINT64_C(0xd0db750c2625426e)},
        {UINT64_C(0x3985253e5023a3b9), UINT64_C(0x90139d28120d353b), UINT64_C(0x99bffad620343913)},
        {UINT64_C(0x513aac49c3eee936), UINT64_C(0x50d60a3c7cbcbbea), UINT64_C(0x21ee947f9f7c11af)},
        {UINT64_C(0xddc49c60334e3814), UINT64_C(0x3f05997c3da1b13f), UINT64_C(0x034d50458c2c0406)},
        {UINT64_C(0xb20ce6200cda81cc), UINT64_C(0x8c055f659bfabb85), UINT64_C(0xc0db03a760c548fb)},
    };
    unsigned char input[16], storage[24];
    for (size_t i = 0; i < sizeof(input); i++) input[i] = (unsigned char)(i * 37);
    int hll_ok = 1, bloom_ok = 1;
    for (size_t offset = 0; offset < 8; offset++) {
        memcpy(storage + offset, input, sizeof(input));
        for (size_t len = 0; len <= sizeof(input); len++) {
            const void *data = storage + offset;
            uint64_t a = MurmurHash64A(data, len, UINT64_C(0xc6a4a7935bd1e995));
            hll_ok &= MurmurHash64A(data, len, 0xadc83b19) == expected[len][0];
            bloom_ok &= a == expected[len][1];
            bloom_ok &= MurmurHash64A(data, len, a) == expected[len][2];
        }
    }
    test_cond("MurmurHash64A preserves HLL hashes for all tails and alignments", hll_ok);
    test_cond("MurmurHash64A preserves Bloom 64-bit seeds and double hashing", bloom_ok);
    return !(hll_ok && bloom_ok);
}
#endif
