/* hyperloglog.c - Redis HyperLogLog probabilistic cardinality approximation.
 * This file implements the algorithm and the exported Redis commands.
 *
 * Copyright (c) 2014-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#include "server.h"

#include <stdint.h>
#include <math.h>

/* The Redis HyperLogLog implementation is based on the following ideas:
 *
 * * The use of a 64 bit hash function as proposed in [1], in order to estimate
 *   cardinalities larger than 10^9, at the cost of just 1 additional bit per
 *   register.
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
 *
 * Redis uses two representations:
 *
 * 1) A "dense" representation where every entry is represented by
 *    a 6-bit integer.
 * 2) A "sparse" representation using run length compression suitable
 *    for representing HyperLogLogs with many registers set to 0 in
 *    a memory efficient way.
 *
 *
 * HLL header
 * ===
 *
 * Both the dense and sparse representation have a 16 byte header as follows:
 *
 * +------+---+-----+----------+
 * | HYLL | E | N/U | Cardin.  |
 * +------+---+-----+----------+
 *
 * The first 4 bytes are a magic string set to the bytes "HYLL".
 * "E" is one byte encoding, currently set to HLL_DENSE or
 * HLL_SPARSE. N/U are three not used bytes.
 *
 * The "Cardin." field is a 64 bit integer stored in little endian format
 * with the latest cardinality computed that can be reused if the data
 * structure was not modified since the last computation (this is useful
 * because there are high probabilities that HLLADD operations don't
 * modify the actual data structure and hence the approximated cardinality).
 *
 * When the most significant bit in the most significant byte of the cached
 * cardinality is set, it means that the data structure was modified and
 * we can't reuse the cached value that must be recomputed.
 *
 * Dense representation
 * ===
 *
 * The dense representation used by Redis is the following:
 *
 * +--------+--------+--------+------//      //--+
 * |11000000|22221111|33333322|55444444 ....     |
 * +--------+--------+--------+------//      //--+
 *
 * The 6 bits counters are encoded one after the other starting from the
 * LSB to the MSB, and using the next bytes as needed.
 *
 * Sparse representation
 * ===
 *
 * The sparse representation encodes registers using a run length
 * encoding composed of three opcodes, two using one byte, and one using
 * of two bytes. The opcodes are called ZERO, XZERO and VAL.
 *
 * ZERO opcode is represented as 00xxxxxx. The 6-bit integer represented
 * by the six bits 'xxxxxx', plus 1, means that there are N registers set
 * to 0. This opcode can represent from 1 to 64 contiguous registers set
 * to the value of 0.
 *
 * XZERO opcode is represented by two bytes 01xxxxxx yyyyyyyy. The 14-bit
 * integer represented by the bits 'xxxxxx' as most significant bits and
 * 'yyyyyyyy' as least significant bits, plus 1, means that there are N
 * registers set to 0. This opcode can represent from 0 to 16384 contiguous
 * registers set to the value of 0.
 *
 * VAL opcode is represented as 1vvvvvxx. It contains a 5-bit integer
 * representing the value of a register, and a 2-bit integer representing
 * the number of contiguous registers set to that value 'vvvvv'.
 * To obtain the value and run length, the integers vvvvv and xx must be
 * incremented by one. This opcode can represent values from 1 to 32,
 * repeated from 1 to 4 times.
 *
 * The sparse representation can't represent registers with a value greater
 * than 32, however it is very unlikely that we find such a register in an
 * HLL with a cardinality where the sparse representation is still more
 * memory efficient than the dense representation. When this happens the
 * HLL is converted to the dense representation.
 *
 * The sparse representation is purely positional. For example a sparse
 * representation of an empty HLL is just: XZERO:16384.
 *
 * An HLL having only 3 non-zero registers at position 1000, 1020, 1021
 * respectively set to 2, 3, 3, is represented by the following three
 * opcodes:
 *
 * XZERO:1000 (Registers 0-999 are set to 0)
 * VAL:2,1    (1 register set to value 2, that is register 1000)
 * ZERO:19    (Registers 1001-1019 set to 0)
 * VAL:3,2    (2 registers set to value 3, that is registers 1020,1021)
 * XZERO:15362 (Registers 1022-16383 set to 0)
 *
 * In the example the sparse representation used just 7 bytes instead
 * of 12k in order to represent the HLL registers. In general for low
 * cardinality there is a big win in terms of space efficiency, traded
 * with CPU time since the sparse representation is slower to access.
 *
 * The following table shows average cardinality vs bytes used, 100
 * samples per cardinality (when the set was not representable because
 * of registers with too big value, the dense representation size was used
 * as a sample).
 *
 * 100 267
 * 200 485
 * 300 678
 * 400 859
 * 500 1033
 * 600 1205
 * 700 1375
 * 800 1544
 * 900 1713
 * 1000 1882
 * 2000 3480
 * 3000 4879
 * 4000 6089
 * 5000 7138
 * 6000 8042
 * 7000 8823
 * 8000 9500
 * 9000 10088
 * 10000 10591
 *
 * The dense representation uses 12288 bytes, so there is a big win up to
 * a cardinality of ~2000-3000. For bigger cardinalities the constant times
 * involved in updating the sparse representation is not justified by the
 * memory savings. The exact maximum length of the sparse representation
 * when this implementation switches to the dense representation is
 * configured via the define server.hll_sparse_max_bytes.
 */

struct hllhdr {
    char magic[4];      /* "HYLL" */
    uint8_t encoding;   /* HLL_DENSE or HLL_SPARSE. */
    uint8_t notused[3]; /* Reserved for future use, must be zero. */
    uint8_t card[8];    /* Cached cardinality, little endian. */
    uint8_t registers[]; /* Data bytes. */
};

/* The cached cardinality MSB is used to signal validity of the cached value. */
#define HLL_INVALIDATE_CACHE(hdr) (hdr)->card[7] |= (1<<7)
#define HLL_VALID_CACHE(hdr) (((hdr)->card[7] & (1<<7)) == 0)

#define HLL_P 14 /* The greater is P, the smaller the error. */
#define HLL_Q (64-HLL_P) /* The number of bits of the hash value used for
                            determining the number of leading zeros. */
#define HLL_REGISTERS (1<<HLL_P) /* With P=14, 16384 registers. */
#define HLL_P_MASK (HLL_REGISTERS-1) /* Mask to index register. */
#define HLL_BITS 6 /* Enough to count up to 63 leading zeroes. */
#define HLL_REGISTER_MAX ((1<<HLL_BITS)-1)
#define HLL_HDR_SIZE sizeof(struct hllhdr)
#define HLL_DENSE_SIZE (HLL_HDR_SIZE+((HLL_REGISTERS*HLL_BITS+7)/8))
#define HLL_DENSE 0 /* Dense encoding. */
#define HLL_SPARSE 1 /* Sparse encoding. */
#define HLL_RAW 255 /* Only used internally, never exposed. */
#define HLL_MAX_ENCODING 1

static char *invalid_hll_err = "-INVALIDOBJ Corrupted HLL object detected";

/* =========================== Low level bit macros ========================= */

/* Macros to access the dense representation.
 *
 * We need to get and set 6 bit counters in an array of 8 bit bytes.
 * We use macros to make sure the code is inlined since speed is critical
 * especially in order to compute the approximated cardinality in
 * HLLCOUNT where we need to access all the registers at once.
 * For the same reason we also want to avoid conditionals in this code path.
 *
 * +--------+--------+--------+------//
 * |11000000|22221111|33333322|55444444
 * +--------+--------+--------+------//
 *
 * Note: in the above representation the most significant bit (MSB)
 * of every byte is on the left. We start using bits from the LSB to MSB,
 * and so forth passing to the next byte.
 *
 * Example, we want to access to counter at pos = 1 ("111111" in the
 * illustration above).
 *
 * The index of the first byte b0 containing our data is:
 *
 *  b0 = 6 * pos / 8 = 0
 *
 *   +--------+
 *   |11000000|  <- Our byte at b0
 *   +--------+
 *
 * The position of the first bit (counting from the LSB = 0) in the byte
 * is given by:
 *
 *  fb = 6 * pos % 8 -> 6
 *
 * Right shift b0 of 'fb' bits.
 *
 *   +--------+
 *   |11000000|  <- Initial value of b0
 *   |00000011|  <- After right shift of 6 pos.
 *   +--------+
 *
 * Left shift b1 of bits 8-fb bits (2 bits)
 *
 *   +--------+
 *   |22221111|  <- Initial value of b1
 *   |22111100|  <- After left shift of 2 bits.
 *   +--------+
 *
 * OR the two bits, and finally AND with 111111 (63 in decimal) to
 * clean the higher order bits we are not interested in:
 *
 *   +--------+
 *   |00000011|  <- b0 right shifted
 *   |22111100|  <- b1 left shifted
 *   |22111111|  <- b0 OR b1
 *   |  111111|  <- (b0 OR b1) AND 63, our value.
 *   +--------+
 *
 * We can try with a different example, like pos = 0. In this case
 * the 6-bit counter is actually contained in a single byte.
 *
 *  b0 = 6 * pos / 8 = 0
 *
 *   +--------+
 *   |11000000|  <- Our byte at b0
 *   +--------+
 *
 *  fb = 6 * pos % 8 = 0
 *
 *  So we right shift of 0 bits (no shift in practice) and
 *  left shift the next byte of 8 bits, even if we don't use it,
 *  but this has the effect of clearing the bits so the result
 *  will not be affected after the OR.
 *
 * -------------------------------------------------------------------------
 *
 * Setting the register is a bit more complex, let's assume that 'val'
 * is the value we want to set, already in the right range.
 *
 * We need two steps, in one we need to clear the bits, and in the other
 * we need to bitwise-OR the new bits.
 *
 * Let's try with 'pos' = 1, so our first byte at 'b' is 0,
 *
 * "fb" is 6 in this case.
 *
 *   +--------+
 *   |11000000|  <- Our byte at b0
 *   +--------+
 *
 * To create an AND-mask to clear the bits about this position, we just
 * initialize the mask with the value 63, left shift it of "fs" bits,
 * and finally invert the result.
 *
 *   +--------+
 *   |00111111|  <- "mask" starts at 63
 *   |11000000|  <- "mask" after left shift of "ls" bits.
 *   |00111111|  <- "mask" after invert.
 *   +--------+
 *
 * Now we can bitwise-AND the byte at "b" with the mask, and bitwise-OR
 * it with "val" left-shifted of "ls" bits to set the new bits.
 *
 * Now let's focus on the next byte b1:
 *
 *   +--------+
 *   |22221111|  <- Initial value of b1
 *   +--------+
 *
 * To build the AND mask we start again with the 63 value, right shift
 * it by 8-fb bits, and invert it.
 *
 *   +--------+
 *   |00111111|  <- "mask" set at 2&6-1
 *   |00001111|  <- "mask" after the right shift by 8-fb = 2 bits
 *   |11110000|  <- "mask" after bitwise not.
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
#define HLL_DENSE_GET_REGISTER(target,p,regnum) do { \
    uint8_t *_p = (uint8_t*) p; \
    unsigned long _byte = regnum*HLL_BITS/8; \
    unsigned long _fb = regnum*HLL_BITS&7; \
    unsigned long _fb8 = 8 - _fb; \
    unsigned long b0 = _p[_byte]; \
    unsigned long b1 = _p[_byte+1]; \
    target = ((b0 >> _fb) | (b1 << _fb8)) & HLL_REGISTER_MAX; \
} while(0)

/* Set the value of the register at position 'regnum' to 'val'.
 * 'p' is an array of unsigned bytes. */
#define HLL_DENSE_SET_REGISTER(p,regnum,val) do { \
    uint8_t *_p = (uint8_t*) p; \
    unsigned long _byte = (regnum)*HLL_BITS/8; \
    unsigned long _fb = (regnum)*HLL_BITS&7; \
    unsigned long _fb8 = 8 - _fb; \
    unsigned long _v = (val); \
    _p[_byte] &= ~(HLL_REGISTER_MAX << _fb); \
    _p[_byte] |= _v << _fb; \
    _p[_byte+1] &= ~(HLL_REGISTER_MAX >> _fb8); \
    _p[_byte+1] |= _v >> _fb8; \
} while(0)

/* Macros to access the sparse representation.
 * The macros parameter is expected to be an uint8_t pointer. */
#define HLL_SPARSE_XZERO_BIT 0x40 /* 01xxxxxx */
#define HLL_SPARSE_VAL_BIT 0x80 /* 1vvvvvxx */
#define HLL_SPARSE_IS_ZERO(p) (((*(p)) & 0xc0) == 0) /* 00xxxxxx */
#define HLL_SPARSE_IS_XZERO(p) (((*(p)) & 0xc0) == HLL_SPARSE_XZERO_BIT)
#define HLL_SPARSE_IS_VAL(p) ((*(p)) & HLL_SPARSE_VAL_BIT)
#define HLL_SPARSE_ZERO_LEN(p) (((*(p)) & 0x3f)+1)
#define HLL_SPARSE_XZERO_LEN(p) (((((*(p)) & 0x3f) << 8) | (*((p)+1)))+1)
#define HLL_SPARSE_VAL_VALUE(p) ((((*(p)) >> 2) & 0x1f)+1)
#define HLL_SPARSE_VAL_LEN(p) (((*(p)) & 0x3)+1)
#define HLL_SPARSE_VAL_MAX_VALUE 32
#define HLL_SPARSE_VAL_MAX_LEN 4
#define HLL_SPARSE_ZERO_MAX_LEN 64
#define HLL_SPARSE_XZERO_MAX_LEN 16384
#define HLL_SPARSE_VAL_SET(p,val,len) do { \
    *(p) = (((val)-1)<<2|((len)-1))|HLL_SPARSE_VAL_BIT; \
} while(0)
#define HLL_SPARSE_ZERO_SET(p,len) do { \
    *(p) = (len)-1; \
} while(0)
#define HLL_SPARSE_XZERO_SET(p,len) do { \
    int _l = (len)-1; \
    *(p) = (_l>>8) | HLL_SPARSE_XZERO_BIT; \
    *((p)+1) = (_l&0xff); \
} while(0)
#define HLL_ALPHA_INF 0.721347520444481703680 /* constant for 0.5/ln(2) */

/* ========================= HyperLogLog algorithm  ========================= */

/* Our hash function is MurmurHash2, 64 bit version.
 * It was modified for Redis in order to provide the same result in
 * big and little endian archs (endian neutral). */
REDIS_NO_SANITIZE("alignment")
uint64_t MurmurHash64A (const void * key, int len, unsigned int seed) {
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

/* Given a string element to add to the HyperLogLog, returns the length
 * of the pattern 000..1 of the element hash. As a side effect 'regp' is
 * set to the register index this element hashes to. */
int hllPatLen(unsigned char *ele, size_t elesize, long *regp) {
    uint64_t hash, bit, index;
    int count;

    /* Count the number of zeroes starting from bit HLL_REGISTERS
     * (that is a power of two corresponding to the first bit we don't use
     * as index). The max run can be 64-P+1 = Q+1 bits.
     *
     * Note that the final "1" ending the sequence of zeroes must be
     * included in the count, so if we find "001" the count is 3, and
     * the smallest count possible is no zeroes at all, just a 1 bit
     * at the first position, that is a count of 1.
     *
     * This may sound like inefficient, but actually in the average case
     * there are high probabilities to find a 1 after a few iterations. */
    hash = MurmurHash64A(ele,elesize,0xadc83b19ULL);
    index = hash & HLL_P_MASK; /* Register index. */
    hash >>= HLL_P; /* Remove bits used to address the register. */
    hash |= ((uint64_t)1<<HLL_Q); /* Make sure the loop terminates
                                     and count will be <= Q+1. */
    bit = 1;
    count = 1; /* Initialized to 1 since we count the "00000...1" pattern. */
    while((hash & bit) == 0) {
        count++;
        bit <<= 1;
    }
    *regp = (int) index;
    return count;
}

/* ================== Dense representation implementation  ================== */

/* Low level function to set the dense HLL register at 'index' to the
 * specified value if the current value is smaller than 'count'.
 *
 * 'registers' is expected to have room for HLL_REGISTERS plus an
 * additional byte on the right. This requirement is met by sds strings
 * automatically since they are implicitly null terminated.
 *
 * The function always succeed, however if as a result of the operation
 * the approximated cardinality changed, 1 is returned. Otherwise 0
 * is returned. */
int hllDenseSet(uint8_t *registers, long index, uint8_t count) {
    uint8_t oldcount;

    HLL_DENSE_GET_REGISTER(oldcount,registers,index);
    if (count > oldcount) {
        HLL_DENSE_SET_REGISTER(registers,index,count);
        return 1;
    } else {
        return 0;
    }
}

/* "Add" the element in the dense hyperloglog data structure.
 * Actually nothing is added, but the max 0 pattern counter of the subset
 * the element belongs to is incremented if needed.
 *
 * This is just a wrapper to hllDenseSet(), performing the hashing of the
 * element in order to retrieve the index and zero-run count. */
int hllDenseAdd(uint8_t *registers, unsigned char *ele, size_t elesize) {
    long index;
    uint8_t count = hllPatLen(ele,elesize,&index);
    /* Update the register if this element produced a longer run of zeroes. */
    return hllDenseSet(registers,index,count);
}

/* Compute the register histogram in the dense representation. */
void hllDenseRegHisto(uint8_t *registers, int* reghisto) {
    int j;

    /* Redis default is to use 16384 registers 6 bits each. The code works
     * with other values by modifying the defines, but for our target value
     * we take a faster path with unrolled loops. */
    if (HLL_REGISTERS == 16384 && HLL_BITS == 6) {
        uint8_t *r = registers;
        unsigned long r0, r1, r2, r3, r4, r5, r6, r7, r8, r9,
                      r10, r11, r12, r13, r14, r15;
        for (j = 0; j < 1024; j++) {
            /* Handle 16 registers per iteration. */
            r0 = r[0] & 63;
            r1 = (r[0] >> 6 | r[1] << 2) & 63;
            r2 = (r[1] >> 4 | r[2] << 4) & 63;
            r3 = (r[2] >> 2) & 63;
            r4 = r[3] & 63;
            r5 = (r[3] >> 6 | r[4] << 2) & 63;
            r6 = (r[4] >> 4 | r[5] << 4) & 63;
            r7 = (r[5] >> 2) & 63;
            r8 = r[6] & 63;
            r9 = (r[6] >> 6 | r[7] << 2) & 63;
            r10 = (r[7] >> 4 | r[8] << 4) & 63;
            r11 = (r[8] >> 2) & 63;
            r12 = r[9] & 63;
            r13 = (r[9] >> 6 | r[10] << 2) & 63;
            r14 = (r[10] >> 4 | r[11] << 4) & 63;
            r15 = (r[11] >> 2) & 63;

            reghisto[r0]++;
            reghisto[r1]++;
            reghisto[r2]++;
            reghisto[r3]++;
            reghisto[r4]++;
            reghisto[r5]++;
            reghisto[r6]++;
            reghisto[r7]++;
            reghisto[r8]++;
            reghisto[r9]++;
            reghisto[r10]++;
            reghisto[r11]++;
            reghisto[r12]++;
            reghisto[r13]++;
            reghisto[r14]++;
            reghisto[r15]++;

            r += 12;
        }
    } else {
        for(j = 0; j < HLL_REGISTERS; j++) {
            unsigned long reg;
            HLL_DENSE_GET_REGISTER(reg,registers,j);
            reghisto[reg]++;
        }
    }
}

/* ================== Sparse representation implementation  ================= */

/* Convert the HLL with sparse representation given as input in its dense
 * representation. Both representations are represented by SDS strings, and
 * the input representation is freed as a side effect.
 *
 * The function returns C_OK if the sparse representation was valid,
 * otherwise C_ERR is returned if the representation was corrupted. */
int hllSparseToDense(robj *o) {
    sds sparse = o->ptr, dense;
    struct hllhdr *hdr, *oldhdr = (struct hllhdr*)sparse;
    int idx = 0, runlen, regval;
    uint8_t *p = (uint8_t*)sparse, *end = p+sdslen(sparse);

    /* If the representation is already the right one return ASAP. */
    hdr = (struct hllhdr*) sparse;
    if (hdr->encoding == HLL_DENSE) return C_OK;

    /* Create a string of the right size filled with zero bytes.
     * Note that the cached cardinality is set to 0 as a side effect
     * that is exactly the cardinality of an empty HLL. */
    dense = sdsnewlen(NULL,HLL_DENSE_SIZE);
    hdr = (struct hllhdr*) dense;
    *hdr = *oldhdr; /* This will copy the magic and cached cardinality. */
    hdr->encoding = HLL_DENSE;

    /* Now read the sparse representation and set non-zero registers
     * accordingly. */
    p += HLL_HDR_SIZE;
    while(p < end) {
        if (HLL_SPARSE_IS_ZERO(p)) {
            runlen = HLL_SPARSE_ZERO_LEN(p);
            idx += runlen;
            p++;
        } else if (HLL_SPARSE_IS_XZERO(p)) {
            runlen = HLL_SPARSE_XZERO_LEN(p);
            idx += runlen;
            p += 2;
        } else {
            runlen = HLL_SPARSE_VAL_LEN(p);
            regval = HLL_SPARSE_VAL_VALUE(p);
            if ((runlen + idx) > HLL_REGISTERS) break; /* Overflow. */
            while(runlen--) {
                HLL_DENSE_SET_REGISTER(hdr->registers,idx,regval);
                idx++;
            }
            p++;
        }
    }

    /* If the sparse representation was valid, we expect to find idx
     * set to HLL_REGISTERS. */
    if (idx != HLL_REGISTERS) {
        sdsfree(dense);
        return C_ERR;
    }

    /* Free the old representation and set the new one. */
    sdsfree(o->ptr);
    o->ptr = dense;
    return C_OK;
}

/* Low level function to set the sparse HLL register at 'index' to the
 * specified value if the current value is smaller than 'count'.
 *
 * The object 'o' is the String object holding the HLL. The function requires
 * a reference to the object in order to be able to enlarge the string if
 * needed.
 *
 * On success, the function returns 1 if the cardinality changed, or 0
 * if the register for this element was not updated.
 * On error (if the representation is invalid) -1 is returned.
 *
 * As a side effect the function may promote the HLL representation from
 * sparse to dense: this happens when a register requires to be set to a value
 * not representable with the sparse representation, or when the resulting
 * size would be greater than server.hll_sparse_max_bytes. */
int hllSparseSet(robj *o, long index, uint8_t count) {
    struct hllhdr *hdr;
    uint8_t oldcount, *sparse, *end, *p, *prev, *next;
    long first, span;
    long is_zero = 0, is_xzero = 0, is_val = 0, runlen = 0;

    /* If the count is too big to be representable by the sparse representation
     * switch to dense representation. */
    if (count > HLL_SPARSE_VAL_MAX_VALUE) goto promote;

    /* When updating a sparse representation, sometimes we may need to enlarge the
     * buffer for up to 3 bytes in the worst case (XZERO split into XZERO-VAL-XZERO),
     * and the following code does the enlarge job.
     * Actually, we use a greedy strategy, enlarge more than 3 bytes to avoid the need
     * for future reallocates on incremental growth. But we do not allocate more than
     * 'server.hll_sparse_max_bytes' bytes for the sparse representation.
     * If the available size of hyperloglog sds string is not enough for the increment
     * we need, we promote the hypreloglog to dense representation in 'step 3'.
     */
    if (sdsalloc(o->ptr) < server.hll_sparse_max_bytes && sdsavail(o->ptr) < 3) {
        size_t newlen = sdslen(o->ptr) + 3;
        newlen += min(newlen, 300); /* Greediness: double 'newlen' if it is smaller than 300, or add 300 to it when it exceeds 300 */
        if (newlen > server.hll_sparse_max_bytes)
            newlen = server.hll_sparse_max_bytes;
        o->ptr = sdsResize(o->ptr, newlen, 1);
    }

    /* Step 1: we need to locate the opcode we need to modify to check
     * if a value update is actually needed. */
    sparse = p = ((uint8_t*)o->ptr) + HLL_HDR_SIZE;
    end = p + sdslen(o->ptr) - HLL_HDR_SIZE;

    first = 0;
    prev = NULL; /* Points to previous opcode at the end of the loop. */
    next = NULL; /* Points to the next opcode at the end of the loop. */
    span = 0;
    while(p < end) {
        long oplen;

        /* Set span to the number of registers covered by this opcode.
         *
         * This is the most performance critical loop of the sparse
         * representation. Sorting the conditionals from the most to the
         * least frequent opcode in many-bytes sparse HLLs is faster. */
        oplen = 1;
        if (HLL_SPARSE_IS_ZERO(p)) {
            span = HLL_SPARSE_ZERO_LEN(p);
        } else if (HLL_SPARSE_IS_VAL(p)) {
            span = HLL_SPARSE_VAL_LEN(p);
        } else { /* XZERO. */
            span = HLL_SPARSE_XZERO_LEN(p);
            oplen = 2;
        }
        /* Break if this opcode covers the register as 'index'. */
        if (index <= first+span-1) break;
        prev = p;
        p += oplen;
        first += span;
    }
    if (span == 0 || p >= end) return -1; /* Invalid format. */

    next = HLL_SPARSE_IS_XZERO(p) ? p+2 : p+1;
    if (next >= end) next = NULL;

    /* Cache current opcode type to avoid using the macro again and
     * again for something that will not change.
     * Also cache the run-length of the opcode. */
    if (HLL_SPARSE_IS_ZERO(p)) {
        is_zero = 1;
        runlen = HLL_SPARSE_ZERO_LEN(p);
    } else if (HLL_SPARSE_IS_XZERO(p)) {
        is_xzero = 1;
        runlen = HLL_SPARSE_XZERO_LEN(p);
    } else {
        is_val = 1;
        runlen = HLL_SPARSE_VAL_LEN(p);
    }

    /* Step 2: After the loop:
     *
     * 'first' stores to the index of the first register covered
     *  by the current opcode, which is pointed by 'p'.
     *
     * 'next' ad 'prev' store respectively the next and previous opcode,
     *  or NULL if the opcode at 'p' is respectively the last or first.
     *
     * 'span' is set to the number of registers covered by the current
     *  opcode.
     *
     * There are different cases in order to update the data structure
     * in place without generating it from scratch:
     *
     * A) If it is a VAL opcode already set to a value >= our 'count'
     *    no update is needed, regardless of the VAL run-length field.
     *    In this case PFADD returns 0 since no changes are performed.
     *
     * B) If it is a VAL opcode with len = 1 (representing only our
     *    register) and the value is less than 'count', we just update it
     *    since this is a trivial case. */
    if (is_val) {
        oldcount = HLL_SPARSE_VAL_VALUE(p);
        /* Case A. */
        if (oldcount >= count) return 0;

        /* Case B. */
        if (runlen == 1) {
            HLL_SPARSE_VAL_SET(p,count,1);
            goto updated;
        }
    }

    /* C) Another trivial to handle case is a ZERO opcode with a len of 1.
     * We can just replace it with a VAL opcode with our value and len of 1. */
    if (is_zero && runlen == 1) {
        HLL_SPARSE_VAL_SET(p,count,1);
        goto updated;
    }

    /* D) General case.
     *
     * The other cases are more complex: our register requires to be updated
     * and is either currently represented by a VAL opcode with len > 1,
     * by a ZERO opcode with len > 1, or by an XZERO opcode.
     *
     * In those cases the original opcode must be split into multiple
     * opcodes. The worst case is an XZERO split in the middle resulting into
     * XZERO - VAL - XZERO, so the resulting sequence max length is
     * 5 bytes.
     *
     * We perform the split writing the new sequence into the 'new' buffer
     * with 'newlen' as length. Later the new sequence is inserted in place
     * of the old one, possibly moving what is on the right a few bytes
     * if the new sequence is longer than the older one. */
    uint8_t seq[5], *n = seq;
    int last = first+span-1; /* Last register covered by the sequence. */
    int len;

    if (is_zero || is_xzero) {
        /* Handle splitting of ZERO / XZERO. */
        if (index != first) {
            len = index-first;
            if (len > HLL_SPARSE_ZERO_MAX_LEN) {
                HLL_SPARSE_XZERO_SET(n,len);
                n += 2;
            } else {
                HLL_SPARSE_ZERO_SET(n,len);
                n++;
            }
        }
        HLL_SPARSE_VAL_SET(n,count,1);
        n++;
        if (index != last) {
            len = last-index;
            if (len > HLL_SPARSE_ZERO_MAX_LEN) {
                HLL_SPARSE_XZERO_SET(n,len);
                n += 2;
            } else {
                HLL_SPARSE_ZERO_SET(n,len);
                n++;
            }
        }
    } else {
        /* Handle splitting of VAL. */
        int curval = HLL_SPARSE_VAL_VALUE(p);

        if (index != first) {
            len = index-first;
            HLL_SPARSE_VAL_SET(n,curval,len);
            n++;
        }
        HLL_SPARSE_VAL_SET(n,count,1);
        n++;
        if (index != last) {
            len = last-index;
            HLL_SPARSE_VAL_SET(n,curval,len);
            n++;
        }
    }

    /* Step 3: substitute the new sequence with the old one.
     *
     * Note that we already allocated space on the sds string
     * calling sdsResize(). */
    int seqlen = n-seq;
    int oldlen = is_xzero ? 2 : 1;
    int deltalen = seqlen-oldlen;

    if (deltalen > 0 &&
        sdslen(o->ptr) + deltalen > server.hll_sparse_max_bytes) goto promote;
    serverAssert(sdslen(o->ptr) + deltalen <= sdsalloc(o->ptr));
    if (deltalen && next) memmove(next+deltalen,next,end-next);
    sdsIncrLen(o->ptr,deltalen);
    memcpy(p,seq,seqlen);
    end += deltalen;

updated:
    /* Step 4: Merge adjacent values if possible.
     *
     * The representation was updated, however the resulting representation
     * may not be optimal: adjacent VAL opcodes can sometimes be merged into
     * a single one. */
    p = prev ? prev : sparse;
    int scanlen = 5; /* Scan up to 5 upcodes starting from prev. */
    while (p < end && scanlen--) {
        if (HLL_SPARSE_IS_XZERO(p)) {
            p += 2;
            continue;
        } else if (HLL_SPARSE_IS_ZERO(p)) {
            p++;
            continue;
        }
        /* We need two adjacent VAL opcodes to try a merge, having
         * the same value, and a len that fits the VAL opcode max len. */
        if (p+1 < end && HLL_SPARSE_IS_VAL(p+1)) {
            int v1 = HLL_SPARSE_VAL_VALUE(p);
            int v2 = HLL_SPARSE_VAL_VALUE(p+1);
            if (v1 == v2) {
                int len = HLL_SPARSE_VAL_LEN(p)+HLL_SPARSE_VAL_LEN(p+1);
                if (len <= HLL_SPARSE_VAL_MAX_LEN) {
                    HLL_SPARSE_VAL_SET(p+1,v1,len);
                    memmove(p,p+1,end-p);
                    sdsIncrLen(o->ptr,-1);
                    end--;
                    /* After a merge we reiterate without incrementing 'p'
                     * in order to try to merge the just merged value with
                     * a value on its right. */
                    continue;
                }
            }
        }
        p++;
    }

    /* Invalidate the cached cardinality. */
    hdr = o->ptr;
    HLL_INVALIDATE_CACHE(hdr);
    return 1;

promote: /* Promote to dense representation. */
    if (hllSparseToDense(o) == C_ERR) return -1; /* Corrupted HLL. */
    hdr = o->ptr;

    /* We need to call hllDenseAdd() to perform the operation after the
     * conversion. However the result must be 1, since if we need to
     * convert from sparse to dense a register requires to be updated.
     *
     * Note that this in turn means that PFADD will make sure the command
     * is propagated to slaves / AOF, so if there is a sparse -> dense
     * conversion, it will be performed in all the slaves as well. */
    int dense_retval = hllDenseSet(hdr->registers,index,count);
    serverAssert(dense_retval == 1);
    return dense_retval;
}

/* "Add" the element in the sparse hyperloglog data structure.
 * Actually nothing is added, but the max 0 pattern counter of the subset
 * the element belongs to is incremented if needed.
 *
 * This function is actually a wrapper for hllSparseSet(), it only performs
 * the hashing of the element to obtain the index and zeros run length. */
int hllSparseAdd(robj *o, unsigned char *ele, size_t elesize) {
    long index;
    uint8_t count = hllPatLen(ele,elesize,&index);
    /* Update the register if this element produced a longer run of zeroes. */
    return hllSparseSet(o,index,count);
}

/* Compute the register histogram in the sparse representation. */
void hllSparseRegHisto(uint8_t *sparse, int sparselen, int *invalid, int* reghisto) {
    int idx = 0, runlen, regval;
    uint8_t *end = sparse+sparselen, *p = sparse;

    while(p < end) {
        if (HLL_SPARSE_IS_ZERO(p)) {
            runlen = HLL_SPARSE_ZERO_LEN(p);
            idx += runlen;
            reghisto[0] += runlen;
            p++;
        } else if (HLL_SPARSE_IS_XZERO(p)) {
            runlen = HLL_SPARSE_XZERO_LEN(p);
            idx += runlen;
            reghisto[0] += runlen;
            p += 2;
        } else {
            runlen = HLL_SPARSE_VAL_LEN(p);
            regval = HLL_SPARSE_VAL_VALUE(p);
            idx += runlen;
            reghisto[regval] += runlen;
            p++;
        }
    }
    if (idx != HLL_REGISTERS && invalid) *invalid = 1;
}

/* ========================= HyperLogLog Count ==============================
 * This is the core of the algorithm where the approximated count is computed.
 * The function uses the lower level hllDenseRegHisto() and hllSparseRegHisto()
 * functions as helpers to compute histogram of register values part of the
 * computation, which is representation-specific, while all the rest is common. */

/* Implements the register histogram calculation for uint8_t data type
 * which is only used internally as speedup for PFCOUNT with multiple keys. */
void hllRawRegHisto(uint8_t *registers, int* reghisto) {
    uint64_t *word = (uint64_t*) registers;
    uint8_t *bytes;
    int j;

    for (j = 0; j < HLL_REGISTERS/8; j++) {
        if (*word == 0) {
            reghisto[0] += 8;
        } else {
            bytes = (uint8_t*) word;
            reghisto[bytes[0]]++;
            reghisto[bytes[1]]++;
            reghisto[bytes[2]]++;
            reghisto[bytes[3]]++;
            reghisto[bytes[4]]++;
            reghisto[bytes[5]]++;
            reghisto[bytes[6]]++;
            reghisto[bytes[7]]++;
        }
        word++;
    }
}

/* Helper function sigma as defined in
 * "New cardinality estimation algorithms for HyperLogLog sketches"
 * Otmar Ertl, arXiv:1702.01284 */
double hllSigma(double x) {
    if (x == 1.) return INFINITY;
    double zPrime;
    double y = 1;
    double z = x;
    do {
        x *= x;
        zPrime = z;
        z += x * y;
        y += y;
    } while(zPrime != z);
    return z;
}

/* Helper function tau as defined in
 * "New cardinality estimation algorithms for HyperLogLog sketches"
 * Otmar Ertl, arXiv:1702.01284 */
double hllTau(double x) {
    if (x == 0. || x == 1.) return 0.;
    double zPrime;
    double y = 1.0;
    double z = 1 - x;
    do {
        x = sqrt(x);
        zPrime = z;
        y *= 0.5;
        z -= pow(1 - x, 2)*y;
    } while(zPrime != z);
    return z / 3;
}

/* Return the approximated cardinality of the set based on the harmonic
 * mean of the registers values. 'hdr' points to the start of the SDS
 * representing the String object holding the HLL representation.
 *
 * If the sparse representation of the HLL object is not valid, the integer
 * pointed by 'invalid' is set to non-zero, otherwise it is left untouched.
 *
 * hllCount() supports a special internal-only encoding of HLL_RAW, that
 * is, hdr->registers will point to an uint8_t array of HLL_REGISTERS element.
 * This is useful in order to speedup PFCOUNT when called against multiple
 * keys (no need to work with 6-bit integers encoding). */
uint64_t hllCount(struct hllhdr *hdr, int *invalid) {
    double m = HLL_REGISTERS;
    double E;
    int j;
    /* Note that reghisto size could be just HLL_Q+2, because HLL_Q+1 is
     * the maximum frequency of the "000...1" sequence the hash function is
     * able to return. However it is slow to check for sanity of the
     * input: instead we history array at a safe size: overflows will
     * just write data to wrong, but correctly allocated, places. */
    int reghisto[64] = {0};

    /* Compute register histogram */
    if (hdr->encoding == HLL_DENSE) {
        hllDenseRegHisto(hdr->registers,reghisto);
    } else if (hdr->encoding == HLL_SPARSE) {
        hllSparseRegHisto(hdr->registers,
                         sdslen((sds)hdr)-HLL_HDR_SIZE,invalid,reghisto);
    } else if (hdr->encoding == HLL_RAW) {
        hllRawRegHisto(hdr->registers,reghisto);
    } else {
        serverPanic("Unknown HyperLogLog encoding in hllCount()");
    }

    /* Estimate cardinality from register histogram. See:
     * "New cardinality estimation algorithms for HyperLogLog sketches"
     * Otmar Ertl, arXiv:1702.01284 */
    double z = m * hllTau((m-reghisto[HLL_Q+1])/(double)m);
    for (j = HLL_Q; j >= 1; --j) {
        z += reghisto[j];
        z *= 0.5;
    }
    z += m * hllSigma(reghisto[0]/(double)m);
    E = llroundl(HLL_ALPHA_INF*m*m/z);

    return (uint64_t) E;
}

/* Call hllDenseAdd() or hllSparseAdd() according to the HLL encoding. */
int hllAdd(robj *o, unsigned char *ele, size_t elesize) {
    struct hllhdr *hdr = o->ptr;
    switch(hdr->encoding) {
    case HLL_DENSE: return hllDenseAdd(hdr->registers,ele,elesize);
    case HLL_SPARSE: return hllSparseAdd(o,ele,elesize);
    default: return -1; /* Invalid representation. */
    }
}

/* Merge by computing MAX(registers[i],hll[i]) the HyperLogLog 'hll'
 * with an array of uint8_t HLL_REGISTERS registers pointed by 'max'.
 *
 * The hll object must be already validated via isHLLObjectOrReply()
 * or in some other way.
 *
 * If the HyperLogLog is sparse and is found to be invalid, C_ERR
 * is returned, otherwise the function always succeeds. */
int hllMerge(uint8_t *max, robj *hll) {
    struct hllhdr *hdr = hll->ptr;
    int i;

    if (hdr->encoding == HLL_DENSE) {
        uint8_t val;

        for (i = 0; i < HLL_REGISTERS; i++) {
            HLL_DENSE_GET_REGISTER(val,hdr->registers,i);
            if (val > max[i]) max[i] = val;
        }
    } else {
        uint8_t *p = hll->ptr, *end = p + sdslen(hll->ptr);
        long runlen, regval;

        p += HLL_HDR_SIZE;
        i = 0;
        while(p < end) {
            if (HLL_SPARSE_IS_ZERO(p)) {
                runlen = HLL_SPARSE_ZERO_LEN(p);
                i += runlen;
                p++;
            } else if (HLL_SPARSE_IS_XZERO(p)) {
                runlen = HLL_SPARSE_XZERO_LEN(p);
                i += runlen;
                p += 2;
            } else {
                runlen = HLL_SPARSE_VAL_LEN(p);
                regval = HLL_SPARSE_VAL_VALUE(p);
                if ((runlen + i) > HLL_REGISTERS) break; /* Overflow. */
                while(runlen--) {
                    if (regval > max[i]) max[i] = regval;
                    i++;
                }
                p++;
            }
        }
        if (i != HLL_REGISTERS) return C_ERR;
    }
    return C_OK;
}

/* ========================== HyperLogLog commands ========================== */

/* Create an HLL object. We always create the HLL using sparse encoding.
 * This will be upgraded to the dense representation as needed. */
robj *createHLLObject(void) {
    robj *o;
    struct hllhdr *hdr;
    sds s;
    uint8_t *p;
    int sparselen = HLL_HDR_SIZE +
                    (((HLL_REGISTERS+(HLL_SPARSE_XZERO_MAX_LEN-1)) /
                     HLL_SPARSE_XZERO_MAX_LEN)*2);
    int aux;

    /* Populate the sparse representation with as many XZERO opcodes as
     * needed to represent all the registers. */
    aux = HLL_REGISTERS;
    s = sdsnewlen(NULL,sparselen);
    p = (uint8_t*)s + HLL_HDR_SIZE;
    while(aux) {
        int xzero = HLL_SPARSE_XZERO_MAX_LEN;
        if (xzero > aux) xzero = aux;
        HLL_SPARSE_XZERO_SET(p,xzero);
        p += 2;
        aux -= xzero;
    }
    serverAssert((p-(uint8_t*)s) == sparselen);

    /* Create the actual object. */
    o = createObject(OBJ_STRING,s);
    hdr = o->ptr;
    memcpy(hdr->magic,"HYLL",4);
    hdr->encoding = HLL_SPARSE;
    return o;
}

/* Check if the object is a String with a valid HLL representation.
 * Return C_OK if this is true, otherwise reply to the client
 * with an error and return C_ERR. */
int isHLLObjectOrReply(client *c, robj *o) {
    struct hllhdr *hdr;

    /* Key exists, check type */
    if (checkType(c,o,OBJ_STRING))
        return C_ERR; /* Error already sent. */

    if (!sdsEncodedObject(o)) goto invalid;
    if (stringObjectLen(o) < sizeof(*hdr)) goto invalid;
    hdr = o->ptr;

    /* Magic should be "HYLL". */
    if (hdr->magic[0] != 'H' || hdr->magic[1] != 'Y' ||
        hdr->magic[2] != 'L' || hdr->magic[3] != 'L') goto invalid;

    if (hdr->encoding > HLL_MAX_ENCODING) goto invalid;

    /* Dense representation string length should match exactly. */
    if (hdr->encoding == HLL_DENSE &&
        stringObjectLen(o) != HLL_DENSE_SIZE) goto invalid;

    /* All tests passed. */
    return C_OK;

invalid:
    addReplyError(c,"-WRONGTYPE Key is not a valid "
               "HyperLogLog string value.");
    return C_ERR;
}

/* PFADD var ele ele ele ... ele => :0 or :1 */
void pfaddCommand(client *c) {
    robj *o = lookupKeyWrite(c->db,c->argv[1]);
    struct hllhdr *hdr;
    int updated = 0, j;

    if (o == NULL) {
        /* Create the key with a string value of the exact length to
         * hold our HLL data structure. sdsnewlen() when NULL is passed
         * is guaranteed to return bytes initialized to zero. */
        o = createHLLObject();
        dbAdd(c->db,c->argv[1],o);
        updated++;
    } else {
        if (isHLLObjectOrReply(c,o) != C_OK) return;
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }
    /* Perform the low level ADD operation for every element. */
    for (j = 2; j < c->argc; j++) {
        int retval = hllAdd(o, (unsigned char*)c->argv[j]->ptr,
                               sdslen(c->argv[j]->ptr));
        switch(retval) {
        case 1:
            updated++;
            break;
        case -1:
            addReplyError(c,invalid_hll_err);
            return;
        }
    }
    hdr = o->ptr;
    if (updated) {
        HLL_INVALIDATE_CACHE(hdr);
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING,"pfadd",c->argv[1],c->db->id);
        server.dirty += updated;
    }
    addReply(c, updated ? shared.cone : shared.czero);
}

/* PFCOUNT var -> approximated cardinality of set. */
void pfcountCommand(client *c) {
    robj *o;
    struct hllhdr *hdr;
    uint64_t card;

    /* Case 1: multi-key keys, cardinality of the union.
     *
     * When multiple keys are specified, PFCOUNT actually computes
     * the cardinality of the merge of the N HLLs specified. */
    if (c->argc > 2) {
        uint8_t max[HLL_HDR_SIZE+HLL_REGISTERS], *registers;
        int j;

        /* Compute an HLL with M[i] = MAX(M[i]_j). */
        memset(max,0,sizeof(max));
        hdr = (struct hllhdr*) max;
        hdr->encoding = HLL_RAW; /* Special internal-only encoding. */
        registers = max + HLL_HDR_SIZE;
        for (j = 1; j < c->argc; j++) {
            /* Check type and size. */
            robj *o = lookupKeyRead(c->db,c->argv[j]);
            if (o == NULL) continue; /* Assume empty HLL for non existing var.*/
            if (isHLLObjectOrReply(c,o) != C_OK) return;

            /* Merge with this HLL with our 'max' HLL by setting max[i]
             * to MAX(max[i],hll[i]). */
            if (hllMerge(registers,o) == C_ERR) {
                addReplyError(c,invalid_hll_err);
                return;
            }
        }

        /* Compute cardinality of the resulting set. */
        addReplyLongLong(c,hllCount(hdr,NULL));
        return;
    }

    /* Case 2: cardinality of the single HLL.
     *
     * The user specified a single key. Either return the cached value
     * or compute one and update the cache.
     *
     * Since a HLL is a regular Redis string type value, updating the cache does
     * modify the value. We do a lookupKeyRead anyway since this is flagged as a
     * read-only command. The difference is that with lookupKeyWrite, a
     * logically expired key on a replica is deleted, while with lookupKeyRead
     * it isn't, but the lookup returns NULL either way if the key is logically
     * expired, which is what matters here. */
    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        /* No key? Cardinality is zero since no element was added, otherwise
         * we would have a key as HLLADD creates it as a side effect. */
        addReply(c,shared.czero);
    } else {
        if (isHLLObjectOrReply(c,o) != C_OK) return;
        o = dbUnshareStringValue(c->db,c->argv[1],o);

        /* Check if the cached cardinality is valid. */
        hdr = o->ptr;
        if (HLL_VALID_CACHE(hdr)) {
            /* Just return the cached value. */
            card = (uint64_t)hdr->card[0];
            card |= (uint64_t)hdr->card[1] << 8;
            card |= (uint64_t)hdr->card[2] << 16;
            card |= (uint64_t)hdr->card[3] << 24;
            card |= (uint64_t)hdr->card[4] << 32;
            card |= (uint64_t)hdr->card[5] << 40;
            card |= (uint64_t)hdr->card[6] << 48;
            card |= (uint64_t)hdr->card[7] << 56;
        } else {
            int invalid = 0;
            /* Recompute it and update the cached value. */
            card = hllCount(hdr,&invalid);
            if (invalid) {
                addReplyError(c,invalid_hll_err);
                return;
            }
            hdr->card[0] = card & 0xff;
            hdr->card[1] = (card >> 8) & 0xff;
            hdr->card[2] = (card >> 16) & 0xff;
            hdr->card[3] = (card >> 24) & 0xff;
            hdr->card[4] = (card >> 32) & 0xff;
            hdr->card[5] = (card >> 40) & 0xff;
            hdr->card[6] = (card >> 48) & 0xff;
            hdr->card[7] = (card >> 56) & 0xff;
            /* This is considered a read-only command even if the cached value
             * may be modified and given that the HLL is a Redis string
             * we need to propagate the change. */
            signalModifiedKey(c,c->db,c->argv[1]);
            server.dirty++;
        }
        addReplyLongLong(c,card);
    }
}

/* PFMERGE dest src1 src2 src3 ... srcN => OK */
void pfmergeCommand(client *c) {
    uint8_t max[HLL_REGISTERS];
    struct hllhdr *hdr;
    int j;
    int use_dense = 0; /* Use dense representation as target? */

    /* Compute an HLL with M[i] = MAX(M[i]_j).
     * We store the maximum into the max array of registers. We'll write
     * it to the target variable later. */
    memset(max,0,sizeof(max));
    for (j = 1; j < c->argc; j++) {
        /* Check type and size. */
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) continue; /* Assume empty HLL for non existing var. */
        if (isHLLObjectOrReply(c,o) != C_OK) return;

        /* If at least one involved HLL is dense, use the dense representation
         * as target ASAP to save time and avoid the conversion step. */
        hdr = o->ptr;
        if (hdr->encoding == HLL_DENSE) use_dense = 1;

        /* Merge with this HLL with our 'max' HLL by setting max[i]
         * to MAX(max[i],hll[i]). */
        if (hllMerge(max,o) == C_ERR) {
            addReplyError(c,invalid_hll_err);
            return;
        }
    }

    /* Create / unshare the destination key's value if needed. */
    robj *o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* Create the key with a string value of the exact length to
         * hold our HLL data structure. sdsnewlen() when NULL is passed
         * is guaranteed to return bytes initialized to zero. */
        o = createHLLObject();
        dbAdd(c->db,c->argv[1],o);
    } else {
        /* If key exists we are sure it's of the right type/size
         * since we checked when merging the different HLLs, so we
         * don't check again. */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }

    /* Convert the destination object to dense representation if at least
     * one of the inputs was dense. */
    if (use_dense && hllSparseToDense(o) == C_ERR) {
        addReplyError(c,invalid_hll_err);
        return;
    }

    /* Write the resulting HLL to the destination HLL registers and
     * invalidate the cached value. */
    for (j = 0; j < HLL_REGISTERS; j++) {
        if (max[j] == 0) continue;
        hdr = o->ptr;
        switch(hdr->encoding) {
        case HLL_DENSE: hllDenseSet(hdr->registers,j,max[j]); break;
        case HLL_SPARSE: hllSparseSet(o,j,max[j]); break;
        }
    }
    hdr = o->ptr; /* o->ptr may be different now, as a side effect of
                     last hllSparseSet() call. */
    HLL_INVALIDATE_CACHE(hdr);

    signalModifiedKey(c,c->db,c->argv[1]);
    /* We generate a PFADD event for PFMERGE for semantical simplicity
     * since in theory this is a mass-add of elements. */
    notifyKeyspaceEvent(NOTIFY_STRING,"pfadd",c->argv[1],c->db->id);
    server.dirty++;
    addReply(c,shared.ok);
}

/* ========================== Testing / Debugging  ========================== */

/* PFSELFTEST
 * This command performs a self-test of the HLL registers implementation.
 * Something that is not easy to test from within the outside. */
#define HLL_TEST_CYCLES 1000
void pfselftestCommand(client *c) {
    unsigned int j, i;
    sds bitcounters = sdsnewlen(NULL,HLL_DENSE_SIZE);
    struct hllhdr *hdr = (struct hllhdr*) bitcounters, *hdr2;
    robj *o = NULL;
    uint8_t bytecounters[HLL_REGISTERS];

    /* Test 1: access registers.
     * The test is conceived to test that the different counters of our data
     * structure are accessible and that setting their values both result in
     * the correct value to be retained and not affect adjacent values. */
    for (j = 0; j < HLL_TEST_CYCLES; j++) {
        /* Set the HLL counters and an array of unsigned byes of the
         * same size to the same set of random values. */
        for (i = 0; i < HLL_REGISTERS; i++) {
            unsigned int r = rand() & HLL_REGISTER_MAX;

            bytecounters[i] = r;
            HLL_DENSE_SET_REGISTER(hdr->registers,i,r);
        }
        /* Check that we are able to retrieve the same values. */
        for (i = 0; i < HLL_REGISTERS; i++) {
            unsigned int val;

            HLL_DENSE_GET_REGISTER(val,hdr->registers,i);
            if (val != bytecounters[i]) {
                addReplyErrorFormat(c,
                    "TESTFAILED Register %d should be %d but is %d",
                    i, (int) bytecounters[i], (int) val);
                goto cleanup;
            }
        }
    }

    /* Test 2: approximation error.
     * The test adds unique elements and check that the estimated value
     * is always reasonable bounds.
     *
     * We check that the error is smaller than a few times than the expected
     * standard error, to make it very unlikely for the test to fail because
     * of a "bad" run.
     *
     * The test is performed with both dense and sparse HLLs at the same
     * time also verifying that the computed cardinality is the same. */
    memset(hdr->registers,0,HLL_DENSE_SIZE-HLL_HDR_SIZE);
    o = createHLLObject();
    double relerr = 1.04/sqrt(HLL_REGISTERS);
    int64_t checkpoint = 1;
    uint64_t seed = (uint64_t)rand() | (uint64_t)rand() << 32;
    uint64_t ele;
    for (j = 1; j <= 10000000; j++) {
        ele = j ^ seed;
        hllDenseAdd(hdr->registers,(unsigned char*)&ele,sizeof(ele));
        hllAdd(o,(unsigned char*)&ele,sizeof(ele));

        /* Make sure that for small cardinalities we use sparse
         * encoding. */
        if (j == checkpoint && j < server.hll_sparse_max_bytes/2) {
            hdr2 = o->ptr;
            if (hdr2->encoding != HLL_SPARSE) {
                addReplyError(c, "TESTFAILED sparse encoding not used");
                goto cleanup;
            }
        }

        /* Check that dense and sparse representations agree. */
        if (j == checkpoint && hllCount(hdr,NULL) != hllCount(o->ptr,NULL)) {
                addReplyError(c, "TESTFAILED dense/sparse disagree");
                goto cleanup;
        }

        /* Check error. */
        if (j == checkpoint) {
            int64_t abserr = checkpoint - (int64_t)hllCount(hdr,NULL);
            uint64_t maxerr = ceil(relerr*6*checkpoint);

            /* Adjust the max error we expect for cardinality 10
             * since from time to time it is statistically likely to get
             * much higher error due to collision, resulting into a false
             * positive. */
            if (j == 10) maxerr = 1;

            if (abserr < 0) abserr = -abserr;
            if (abserr > (int64_t)maxerr) {
                addReplyErrorFormat(c,
                    "TESTFAILED Too big error. card:%llu abserr:%llu",
                    (unsigned long long) checkpoint,
                    (unsigned long long) abserr);
                goto cleanup;
            }
            checkpoint *= 10;
        }
    }

    /* Success! */
    addReply(c,shared.ok);

cleanup:
    sdsfree(bitcounters);
    if (o) decrRefCount(o);
}

/* Different debugging related operations about the HLL implementation.
 *
 * PFDEBUG GETREG <key>
 * PFDEBUG DECODE <key>
 * PFDEBUG ENCODING <key>
 * PFDEBUG TODENSE <key>
 */
void pfdebugCommand(client *c) {
    char *cmd = c->argv[1]->ptr;
    struct hllhdr *hdr;
    robj *o;
    int j;

    o = lookupKeyWrite(c->db,c->argv[2]);
    if (o == NULL) {
        addReplyError(c,"The specified key does not exist");
        return;
    }
    if (isHLLObjectOrReply(c,o) != C_OK) return;
    o = dbUnshareStringValue(c->db,c->argv[2],o);
    hdr = o->ptr;

    /* PFDEBUG GETREG <key> */
    if (!strcasecmp(cmd,"getreg")) {
        if (c->argc != 3) goto arityerr;

        if (hdr->encoding == HLL_SPARSE) {
            if (hllSparseToDense(o) == C_ERR) {
                addReplyError(c,invalid_hll_err);
                return;
            }
            server.dirty++; /* Force propagation on encoding change. */
        }

        hdr = o->ptr;
        addReplyArrayLen(c,HLL_REGISTERS);
        for (j = 0; j < HLL_REGISTERS; j++) {
            uint8_t val;

            HLL_DENSE_GET_REGISTER(val,hdr->registers,j);
            addReplyLongLong(c,val);
        }
    }
    /* PFDEBUG DECODE <key> */
    else if (!strcasecmp(cmd,"decode")) {
        if (c->argc != 3) goto arityerr;

        uint8_t *p = o->ptr, *end = p+sdslen(o->ptr);
        sds decoded = sdsempty();

        if (hdr->encoding != HLL_SPARSE) {
            sdsfree(decoded);
            addReplyError(c,"HLL encoding is not sparse");
            return;
        }

        p += HLL_HDR_SIZE;
        while(p < end) {
            int runlen, regval;

            if (HLL_SPARSE_IS_ZERO(p)) {
                runlen = HLL_SPARSE_ZERO_LEN(p);
                p++;
                decoded = sdscatprintf(decoded,"z:%d ",runlen);
            } else if (HLL_SPARSE_IS_XZERO(p)) {
                runlen = HLL_SPARSE_XZERO_LEN(p);
                p += 2;
                decoded = sdscatprintf(decoded,"Z:%d ",runlen);
            } else {
                runlen = HLL_SPARSE_VAL_LEN(p);
                regval = HLL_SPARSE_VAL_VALUE(p);
                p++;
                decoded = sdscatprintf(decoded,"v:%d,%d ",regval,runlen);
            }
        }
        decoded = sdstrim(decoded," ");
        addReplyBulkCBuffer(c,decoded,sdslen(decoded));
        sdsfree(decoded);
    }
    /* PFDEBUG ENCODING <key> */
    else if (!strcasecmp(cmd,"encoding")) {
        char *encodingstr[2] = {"dense","sparse"};
        if (c->argc != 3) goto arityerr;

        addReplyStatus(c,encodingstr[hdr->encoding]);
    }
    /* PFDEBUG TODENSE <key> */
    else if (!strcasecmp(cmd,"todense")) {
        int conv = 0;
        if (c->argc != 3) goto arityerr;

        if (hdr->encoding == HLL_SPARSE) {
            if (hllSparseToDense(o) == C_ERR) {
                addReplyError(c,invalid_hll_err);
                return;
            }
            conv = 1;
            server.dirty++; /* Force propagation on encoding change. */
        }
        addReply(c,conv ? shared.cone : shared.czero);
    } else {
        addReplyErrorFormat(c,"Unknown PFDEBUG subcommand '%s'", cmd);
    }
    return;

arityerr:
    addReplyErrorFormat(c,
        "Wrong number of arguments for the '%s' subcommand",cmd);
}

