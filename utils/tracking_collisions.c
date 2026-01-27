/* This is a small program used in order to understand the collision rate
 * of CRC64 (ISO version) VS other stronger hashing functions in the context
 * of hashing keys for the Redis "tracking" feature (client side caching
 * assisted by the server).
 *
 * The program attempts to hash keys with common names in the form of
 *
 *  prefix:<counter>
 *
 * And counts the resulting collisions generated in the 24 bits of output
 * needed for the tracking feature invalidation table (16 millions + entries)
 *
 * Compile with:
 *
 *  cc -O2 ./tracking_collisions.c ../src/crc64.c ../src/sha1.c
 *  ./a.out
 *
 * --------------------------------------------------------------------------
 *
 * Copyright (C) 2019-Present Redis Ltd. All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../src/crc64.h"
#include "../src/sha1.h"

#define TABLE_SIZE (1<<24)
int Table[TABLE_SIZE];

uint64_t crc64Hash(char *key, size_t len) {
    return crc64(0,(unsigned char*)key,len);
}

uint64_t sha1Hash(char *key, size_t len) {
    SHA1_CTX ctx;
    unsigned char hash[20];

    SHA1Init(&ctx);
    SHA1Update(&ctx,(unsigned char*)key,len);
    SHA1Final(hash,&ctx);
    uint64_t hash64;
    memcpy(&hash64,hash,sizeof(hash64));
    return hash64;
}

/* Test the hashing function provided as callback and return the
 * number of collisions found. */
unsigned long testHashingFunction(uint64_t (*hash)(char *, size_t)) {
    unsigned long collisions = 0;
    memset(Table,0,sizeof(Table));
    char *prefixes[] = {"object", "message", "user", NULL};
    for (int i = 0; prefixes[i] != NULL; i++) {
        for (int j = 0; j < TABLE_SIZE/2; j++) {
            char keyname[128];
            size_t keylen = snprintf(keyname,sizeof(keyname),"%s:%d",
                                     prefixes[i],j);
            uint64_t bucket = hash(keyname,keylen) % TABLE_SIZE;
            if (Table[bucket]) {
                collisions++;
            } else {
                Table[bucket] = 1;
            }
        }
    }
    return collisions;
}

int main(void) {
    printf("SHA1 : %lu\n", testHashingFunction(sha1Hash));
    printf("CRC64: %lu\n", testHashingFunction(crc64Hash));
    return 0;
}
