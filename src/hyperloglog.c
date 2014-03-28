/* hyperloglog.c - Redis HyperLogLog probabilistic cardinality approximation.
 * This file implements the algorithm and the exported Redis commands.
 *
 * Copyright (c) 2014, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"

/* The Redis HyperLogLog implementation is based on the following ideas:
 *
 * * The use of a 64 bit hash function as proposed in [1], in order to don't
 *   limited to cardinalities up to 10^9, at the cost of just 1 additional
 *   bit per register.
 * * The use of 16384 6-bit registers for a great level of accuracy, using
 *   a total of 12k per key.
 * * The use of the Redis string data type. No new type is introduced.
 * * No attempt is made to compress the data structure as in [1]. Also the
 *   algorithm used is the original HyperLogLog Algorithm as in [2], with
 *   the only difference that a 64 bit hash function is used, so no correction
 *   is performed for values near 2^32 as in [1].
 *
 * [1] Heule, Nunkesser, Hall: HyperLogLog in Practice: Algorithmic
 *     Engineering of a State of The Art Cardinality Estimation Algorithm.
 *
 * [2] P. Flajolet, Éric Fusy, O. Gandouet, and F. Meunier. Hyperloglog: The
 *     analysis of a near-optimal cardinality estimation algorithm.
 */

#define REDIS_HLL_REGISTERS 16384
#define REDIS_HLL_BITS 6
#define REDIS_HLL_REGISTER_MAX ((1<<REDIS_HLL_BITS)-1)
#define REDIS_HLL_SIZE ((REDIS_HLL_REGISTERS*REDIS_HLL_BITS+7)/8)

/* =========================== Low level bit macros ========================= */

/* We need to get and set 6 bit counters in an array of 8 bit bytes.
 * We use macros to make sure the code is inlined since speed is critical
 * especially in order to compute the approximated cardinality in
 * HLLCOUNT where we need to access all the registers at once.
 *
 * +--------+--------+--------+------//
 * |00000011|11112222|22333333|444444
 * +--------+--------+--------+------//
 *
 * Assuming we want to access counter at zero based index 'pos' = 2.
 * (In the example it is "222222")
 *
 * The first byte "b" containing our data is:
 *   b = 6 * pos / 8 -> 1
 *
 *   +--------+
 *   |11112222|  <- Our byte at "b"
 *   +--------+
 *
 * The amount of left shifting "ls" in the first byte is:
 *   ls = 6 * pos & 7 -> 4
 *
 *   +--------+
 *   |2222    |  <- Left shift 4 pos.
 *   +--------+
 *
 * To add the bits in the next byte b+1, we need to right shift them right of
 * "rs" bits positions before xoring it to our current value in the first byte
 * (after the left shift):
 *   rs = 8 - ls -> 4
 *
 *   +--------+
 *   |    2233|  <- Byte "b+1" right shifted 4 pos.
 *   +--------+
 *
 * Now we can just bitwise-OR the two bytes and mask for 2^6-1 in order to
 * clear bits 6 and 7 if they are set, that are not part of our 6 bit unsigned
 * integer.
 *
 * -------------------------------------------------------------------------
 *
 * Setting the register is a bit more complex, let's assume that 'val'
 * is the value we want to set, already in the right range.
 *
 * We need two steps, in one we need to clear the bits, and in the other
 * we need to bitwise-OR the new bits.
 *
 * This time let's try with 'pos' = 1, so our first byte at 'b' is 0,
 * "ls" is 6, and "rs" is 2.
 *
 *   +--------+
 *   |00000011|  <- Our initial byte at "b"
 *   +--------+
 *
 * We store at "mask" the value 255, consisting of a byte with all bits
 * set to 1. We left-shift it of "rs" bits to the left.
 *
 *   +--------+
 *   |11111100|  <- "mask" after the left-shift of 'rs' bits.
 *   +--------+
 *
 * Now we can bitwise-AND the byte at "b" with the mask, and bitwise-OR
 * it with "val" right-shifted of "ls" to set the new bits.
 *
 * Now let's focus on the next byte b+1:
 *
 *   +--------+
 *   |11112222| <- byte at b+1
 *   +--------+
 *
 * To build the AND mask for the next byte b+1 we left shift it by "rs"
 * amount of bits a byte with value 2^6-1. Later we negate (bitwise-not) it.
 *
 *   +--------+
 *   |11111100|  <- "filter" set at 2&6-1
 *   |11110000|  <- "filter" after the left shift of "rs" bits.
 *   |00001111|  <- "filter" after bitwise not.
 *   +--------+
 *
 * Now we can mask it with b+1 to clear the old bits, and bitwise-OR
 * with "val" left-shifted by "rs" bits to set the new value.
 */

/* Note: if we access the last counter, we will also access the b+1 byte
 * that is out of the array, but sds strings always have an implicit null
 * term, so the byte exists, and we can skip the conditional (or the need
 * to allocate 1 byte more explicitly). */

/* Store the value of the register at position 'regnum' into variable 'target'.
 * 'p' is an array of unsigned bytes. */
#define HLL_GET_REGISTER(target,p,regnum) do { \
    uint8_t *_p = (uint8_t*) p; \
    int _byte = regnum*REDIS_HLL_BITS/8; \
    int _leftshift = regnum*REDIS_HLL_BITS&7; \
    int _rightshift = 8 - _leftshift; \
    target = ((_p[_byte] << _leftshift) | \
             (_p[_byte+1] >> _rightshift)) & \
             ((1<<REDIS_HLL_BITS)-1); \
} while(0)

/* Set the value of the register at position 'regnum' to 'val'.
 * 'p' is an array of unsigned bytes. */
#define HLL_SET_REGISTER(p,regnum,val) do { \
    uint8_t *_p = (uint8_t*) p; \
    int _byte = regnum*REDIS_HLL_BITS/8; \
    int _leftshift = regnum*REDIS_HLL_BITS&7; \
    int _rightshift = 8 - _leftshift; \
    uint8_t m1 = 255, m2 = REDIS_HLL_REGISTER_MAX; \
    _p[_byte] &= m1 << _rightshift; \
    _p[_byte] |= val >> _leftshift; \
    _p[_byte+1] &= ~(m2 << _rightshift); \
    _p[_byte+1] |= val << _rightshift; \
} while(0)

/* ========================= HyperLogLog algorithm  ========================= */

/* ========================== HyperLogLog commands ========================== */

/* This command performs a self-test of the HLL registers implementation.
 * Something that is not easy to test from within the outside.
 *
 * The test is conceived to test that the different counters of our data
 * structure are accessible and that setting their values both result in
 * the correct value to be retained and not affect adjacent values. */

#define REDIS_HLL_TEST_CYCLES 1000
void hllSelftestCommand(redisClient *c) {
    int j, i;
    sds bitcounters = sdsnewlen(NULL,REDIS_HLL_SIZE);
    uint8_t bytecounters[REDIS_HLL_REGISTERS];

    for (j = 0; j < REDIS_HLL_TEST_CYCLES; j++) {
        /* Set the HLL counters and an array of unsigned byes of the
         * same size to the same set of random values. */
        for (i = 0; i < REDIS_HLL_REGISTERS; i++) {
            unsigned int r = rand() & REDIS_HLL_REGISTER_MAX;

            bytecounters[i] = r;
            HLL_SET_REGISTER(bitcounters,i,r);
        }
        /* Check that we are able to retrieve the same values. */
        for (i = 0; i < REDIS_HLL_REGISTERS; i++) {
            unsigned int val;

            HLL_GET_REGISTER(val,bitcounters,i);
            if (val != bytecounters[i]) {
                addReplyErrorFormat(c,
                    "TESTFAILED Register %d should be %d but is %d",
                    i, (int) bytecounters[i], (int) val);
                goto cleanup;
            }
        }
    }

    /* Success! */
    addReply(c,shared.ok);

cleanup:
    sdsfree(bitcounters);
}
