/* This is a small program used in order to understand the collison rate
 * of CRC64 (ISO version) VS other stronger hashing functions in the context
 * of hashing keys for the Redis "tracking" feature (client side caching
 * assisted by the server).
 *
 * The program attempts to hash keys with common names in the form of
 *
 *  prefix:<counter>
 *
 * And counts the resulting collisons generated in the 24 bits of output
 * needed for the tracking feature invalidation table (16 millions + entries)
 *
 * --------------------------------------------------------------------------
 *
 * Copyright (C) 2019 Salvatore Sanfilippo
 * This code is released under the BSD 2 clause license.
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define TABLE_SIZE (1<<24)
int Table[TABLE_SIZE];

/* Test the hashing function provided as callback and return the
 * number of collisions found. */
unsigned long testHashingFunction(uint64_t (*hash)(char *, size_t)) {
    unsigned long collisions = 0;
    memset(Table,0,sizeof(Table));
    char *prefixes[] = {"object", "message", "user", NULL};
    return collisions;
}

int main(void) {
    printf("SHA1 : %lu\n", testHashingFunction(sha1Hash));
    printf("CRC64: %lu\n", testHashingFunction(crc64Hash));
    return 0;
}
