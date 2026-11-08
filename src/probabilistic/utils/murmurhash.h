#ifndef REDIS_MURMURHASH_H
#define REDIS_MURMURHASH_H

#include "config.h"
#include <stddef.h>
#include <stdint.h>

/* byte_order is LITTLE_ENDIAN or BIG_ENDIAN; use BYTE_ORDER for host order. */
uint64_t MurmurHash64A(const void *key, size_t len, uint64_t seed, int byte_order);

#ifdef REDIS_TEST
int murmurhashTest(int argc, char **argv, int flags);
#endif

#endif
