#ifndef REDIS_MURMURHASH_H
#define REDIS_MURMURHASH_H

#include <stddef.h>
#include <stdint.h>

/* Endian-neutral MurmurHash2 with a full 64-bit seed. */
uint64_t MurmurHash64A(const void *key, size_t len, uint64_t seed);

#ifdef REDIS_TEST
int murmurhashTest(int argc, char **argv, int flags);
#endif

#endif
