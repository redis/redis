/* Bit operations.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "ctype.h"

#ifdef HAVE_AVX2
/* Define __MM_MALLOC_H to prevent importing the memory aligned
 * allocation functions, which we don't use. */
#define __MM_MALLOC_H
#include <immintrin.h>
#endif

#ifdef HAVE_AVX2
#define BITOP_USE_AVX2 (__builtin_cpu_supports("avx2"))
#else
#define BITOP_USE_AVX2 0
#endif

/* -----------------------------------------------------------------------------
 * Helpers and low level bit functions.
 * -------------------------------------------------------------------------- */

/* Count number of bits set in the binary array pointed by 's' and long
 * 'count' bytes. The implementation of this function is required to
 * work with an input string length up to 512 MB or more (server.proto_max_bulk_len) */
ATTRIBUTE_TARGET_POPCNT
long long redisPopcount(void *s, long count) {
    long long bits = 0;
    unsigned char *p = s;
    uint32_t *p4;
#if defined(HAVE_POPCNT)
    int use_popcnt = __builtin_cpu_supports("popcnt"); /* Check if CPU supports POPCNT instruction. */
#else
    int use_popcnt = 0; /* Assume CPU does not support POPCNT if
                         * __builtin_cpu_supports() is not available. */
#endif
    static const unsigned char bitsinbyte[256] = {0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,4,5,5,6,5,6,6,7,5,6,6,7,6,7,7,8};
    
    /* Count initial bytes not aligned to 64-bit when using the POPCNT instruction,
     * otherwise align to 32-bit. */
    int align = use_popcnt ? 7 : 3;
    while ((unsigned long)p & align && count) {
        bits += bitsinbyte[*p++];
        count--;
    }

    if (likely(use_popcnt)) {
        /* Use separate counters to make the CPU think there are no
         * dependencies between these popcnt operations. */
        uint64_t cnt[4];
        memset(cnt, 0, sizeof(cnt));

        /* Count bits 32 bytes at a time by using popcnt.
         * Unroll the loop to avoid the overhead of a single popcnt per iteration,
         * allowing the CPU to extract more instruction-level parallelism.
         * Reference: https://danluu.com/assembly-intrinsics/ */
        while (count >= 32) {
            cnt[0] += __builtin_popcountll(*(uint64_t*)(p));
            cnt[1] += __builtin_popcountll(*(uint64_t*)(p + 8));
            cnt[2] += __builtin_popcountll(*(uint64_t*)(p + 16));
            cnt[3] += __builtin_popcountll(*(uint64_t*)(p + 24));
            count -= 32;
            p += 32;
            /* Prefetch with 2K stride is just enough to overlap L3 miss latency effectively
             * without causing pressure on lower memory hierarchy or polluting L1/L2 */
            redis_prefetch_read(p + 2048);
        }
        bits += cnt[0] + cnt[1] + cnt[2] + cnt[3];
        goto remain;
    }

    /* Count bits 28 bytes at a time */
    p4 = (uint32_t*)p;
    while(count>=28) {
        uint32_t aux1, aux2, aux3, aux4, aux5, aux6, aux7;

        aux1 = *p4++;
        aux2 = *p4++;
        aux3 = *p4++;
        aux4 = *p4++;
        aux5 = *p4++;
        aux6 = *p4++;
        aux7 = *p4++;
        count -= 28;

        aux1 = aux1 - ((aux1 >> 1) & 0x55555555);
        aux1 = (aux1 & 0x33333333) + ((aux1 >> 2) & 0x33333333);
        aux2 = aux2 - ((aux2 >> 1) & 0x55555555);
        aux2 = (aux2 & 0x33333333) + ((aux2 >> 2) & 0x33333333);
        aux3 = aux3 - ((aux3 >> 1) & 0x55555555);
        aux3 = (aux3 & 0x33333333) + ((aux3 >> 2) & 0x33333333);
        aux4 = aux4 - ((aux4 >> 1) & 0x55555555);
        aux4 = (aux4 & 0x33333333) + ((aux4 >> 2) & 0x33333333);
        aux5 = aux5 - ((aux5 >> 1) & 0x55555555);
        aux5 = (aux5 & 0x33333333) + ((aux5 >> 2) & 0x33333333);
        aux6 = aux6 - ((aux6 >> 1) & 0x55555555);
        aux6 = (aux6 & 0x33333333) + ((aux6 >> 2) & 0x33333333);
        aux7 = aux7 - ((aux7 >> 1) & 0x55555555);
        aux7 = (aux7 & 0x33333333) + ((aux7 >> 2) & 0x33333333);
        bits += ((((aux1 + (aux1 >> 4)) & 0x0F0F0F0F) +
                    ((aux2 + (aux2 >> 4)) & 0x0F0F0F0F) +
                    ((aux3 + (aux3 >> 4)) & 0x0F0F0F0F) +
                    ((aux4 + (aux4 >> 4)) & 0x0F0F0F0F) +
                    ((aux5 + (aux5 >> 4)) & 0x0F0F0F0F) +
                    ((aux6 + (aux6 >> 4)) & 0x0F0F0F0F) +
                    ((aux7 + (aux7 >> 4)) & 0x0F0F0F0F))* 0x01010101) >> 24;
    }
    p = (unsigned char*)p4;

remain:
    /* Count the remaining bytes. */
    while(count--) bits += bitsinbyte[*p++];
    return bits;
}

/* Return the position of the first bit set to one (if 'bit' is 1) or
 * zero (if 'bit' is 0) in the bitmap starting at 's' and long 'count' bytes.
 *
 * The function is guaranteed to return a value >= 0 if 'bit' is 0 since if
 * no zero bit is found, it returns count*8 assuming the string is zero
 * padded on the right. However if 'bit' is 1 it is possible that there is
 * not a single set bit in the bitmap. In this special case -1 is returned. */
long long redisBitpos(void *s, unsigned long count, int bit) {
    unsigned long *l;
    unsigned char *c;
    unsigned long skipval, word = 0, one;
    long long pos = 0; /* Position of bit, to return to the caller. */
    unsigned long j;
    int found;

    /* Process whole words first, seeking for first word that is not
     * all ones or all zeros respectively if we are looking for zeros
     * or ones. This is much faster with large strings having contiguous
     * blocks of 1 or 0 bits compared to the vanilla bit per bit processing.
     *
     * Note that if we start from an address that is not aligned
     * to sizeof(unsigned long) we consume it byte by byte until it is
     * aligned. */

    /* Skip initial bits not aligned to sizeof(unsigned long) byte by byte. */
    skipval = bit ? 0 : UCHAR_MAX;
    c = (unsigned char*) s;
    found = 0;
    while((unsigned long)c & (sizeof(*l)-1) && count) {
        if (*c != skipval) {
            found = 1;
            break;
        }
        c++;
        count--;
        pos += 8;
    }

    /* Skip bits with full word step. */
    l = (unsigned long*) c;
    if (!found) {
        skipval = bit ? 0 : ULONG_MAX;
        while (count >= sizeof(*l)) {
            if (*l != skipval) break;
            l++;
            count -= sizeof(*l);
            pos += sizeof(*l)*8;
        }
    }

    /* Load bytes into "word" considering the first byte as the most significant
     * (we basically consider it as written in big endian, since we consider the
     * string as a set of bits from left to right, with the first bit at position
     * zero.
     *
     * Note that the loading is designed to work even when the bytes left
     * (count) are less than a full word. We pad it with zero on the right. */
    c = (unsigned char*)l;
    for (j = 0; j < sizeof(*l); j++) {
        word <<= 8;
        if (count) {
            word |= *c;
            c++;
            count--;
        }
    }

    /* Special case:
     * If bits in the string are all zero and we are looking for one,
     * return -1 to signal that there is not a single "1" in the whole
     * string. This can't happen when we are looking for "0" as we assume
     * that the right of the string is zero padded. */
    if (bit == 1 && word == 0) return -1;

    /* Last word left, scan bit by bit. The first thing we need is to
     * have a single "1" set in the most significant position in an
     * unsigned long. We don't know the size of the long so we use a
     * simple trick. */
    one = ULONG_MAX; /* All bits set to 1.*/
    one >>= 1;       /* All bits set to 1 but the MSB. */
    one = ~one;      /* All bits set to 0 but the MSB. */

    while(one) {
        if (((one & word) != 0) == bit) return pos;
        pos++;
        one >>= 1;
    }

    /* If we reached this point, there is a bug in the algorithm, since
     * the case of no match is handled as a special case before. */
    serverPanic("End of redisBitpos() reached.");
    return 0; /* Just to avoid warnings. */
}

/* The following set.*Bitfield and get.*Bitfield functions implement setting
 * and getting arbitrary size (up to 64 bits) signed and unsigned integers
 * at arbitrary positions into a bitmap.
 *
 * The representation considers the bitmap as having the bit number 0 to be
 * the most significant bit of the first byte, and so forth, so for example
 * setting a 5 bits unsigned integer to value 23 at offset 7 into a bitmap
 * previously set to all zeroes, will produce the following representation:
 *
 * +--------+--------+
 * |00000001|01110000|
 * +--------+--------+
 *
 * When offsets and integer sizes are aligned to bytes boundaries, this is the
 * same as big endian, however when such alignment does not exist, its important
 * to also understand how the bits inside a byte are ordered.
 *
 * Note that this format follows the same convention as SETBIT and related
 * commands.
 */

void setUnsignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits, uint64_t value) {
    uint64_t byte, bit, byteval, bitval, j;

    for (j = 0; j < bits; j++) {
        bitval = (value & ((uint64_t)1<<(bits-1-j))) != 0;
        byte = offset >> 3;
        bit = 7 - (offset & 0x7);
        byteval = p[byte];
        byteval &= ~(1 << bit);
        byteval |= bitval << bit;
        p[byte] = byteval & 0xff;
        offset++;
    }
}

void setSignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits, int64_t value) {
    uint64_t uv = value; /* Casting will add UINT64_MAX + 1 if v is negative. */
    setUnsignedBitfield(p,offset,bits,uv);
}

uint64_t getUnsignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits) {
    uint64_t byte, bit, byteval, bitval, j, value = 0;

    for (j = 0; j < bits; j++) {
        byte = offset >> 3;
        bit = 7 - (offset & 0x7);
        byteval = p[byte];
        bitval = (byteval >> bit) & 1;
        value = (value<<1) | bitval;
        offset++;
    }
    return value;
}

int64_t getSignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits) {
    int64_t value;
    union {uint64_t u; int64_t i;} conv;

    /* Converting from unsigned to signed is undefined when the value does
     * not fit, however here we assume two's complement and the original value
     * was obtained from signed -> unsigned conversion, so we'll find the
     * most significant bit set if the original value was negative.
     *
     * Note that two's complement is mandatory for exact-width types
     * according to the C99 standard. */
    conv.u = getUnsignedBitfield(p,offset,bits);
    value = conv.i;

    /* If the top significant bit is 1, propagate it to all the
     * higher bits for two's complement representation of signed
     * integers. */
    if (bits < 64 && (value & ((uint64_t)1 << (bits-1))))
        value |= ((uint64_t)-1) << bits;
    return value;
}

/* The following two functions detect overflow of a value in the context
 * of storing it as an unsigned or signed integer with the specified
 * number of bits. The functions both take the value and a possible increment.
 * If no overflow could happen and the value+increment fit inside the limits,
 * then zero is returned, otherwise in case of overflow, 1 is returned,
 * otherwise in case of underflow, -1 is returned.
 *
 * When non-zero is returned (overflow or underflow), if not NULL, *limit is
 * set to the value the operation should result when an overflow happens,
 * depending on the specified overflow semantics:
 *
 * For BFOVERFLOW_SAT if 1 is returned, *limit it is set maximum value that
 * you can store in that integer. when -1 is returned, *limit is set to the
 * minimum value that an integer of that size can represent.
 *
 * For BFOVERFLOW_WRAP *limit is set by performing the operation in order to
 * "wrap" around towards zero for unsigned integers, or towards the most
 * negative number that is possible to represent for signed integers. */

#define BFOVERFLOW_WRAP 0
#define BFOVERFLOW_SAT 1
#define BFOVERFLOW_FAIL 2 /* Used by the BITFIELD command implementation. */

int checkUnsignedBitfieldOverflow(uint64_t value, int64_t incr, uint64_t bits, int owtype, uint64_t *limit) {
    uint64_t max = (bits == 64) ? UINT64_MAX : (((uint64_t)1<<bits)-1);
    int64_t maxincr = max-value;
    int64_t minincr = -value;

    if (value > max || (incr > 0 && incr > maxincr)) {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = max;
            }
        }
        return 1;
    } else if (incr < 0 && incr < minincr) {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = 0;
            }
        }
        return -1;
    }
    return 0;

handle_wrap:
    {
        uint64_t mask = ((uint64_t)-1) << bits;
        uint64_t res = value+incr;

        res &= ~mask;
        *limit = res;
    }
    return 1;
}

int checkSignedBitfieldOverflow(int64_t value, int64_t incr, uint64_t bits, int owtype, int64_t *limit) {
    int64_t max = (bits == 64) ? INT64_MAX : (((int64_t)1<<(bits-1))-1);
    int64_t min = (-max)-1;

    /* Note that maxincr and minincr could overflow, but we use the values
     * only after checking 'value' range, so when we use it no overflow
     * happens. 'uint64_t' cast is there just to prevent undefined behavior on
     * overflow */
    int64_t maxincr = (uint64_t)max-value;
    int64_t minincr = min-value;

    if (value > max || (bits != 64 && incr > maxincr) || (value >= 0 && incr > 0 && incr > maxincr))
    {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = max;
            }
        }
        return 1;
    } else if (value < min || (bits != 64 && incr < minincr) || (value < 0 && incr < 0 && incr < minincr)) {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = min;
            }
        }
        return -1;
    }
    return 0;

handle_wrap:
    {
        uint64_t msb = (uint64_t)1 << (bits-1);
        uint64_t a = value, b = incr, c;
        c = a+b; /* Perform addition as unsigned so that's defined. */

        /* If the sign bit is set, propagate to all the higher order
         * bits, to cap the negative value. If it's clear, mask to
         * the positive integer limit. */
        if (bits < 64) {
            uint64_t mask = ((uint64_t)-1) << bits;
            if (c & msb) {
                c |= mask;
            } else {
                c &= ~mask;
            }
        }
        *limit = c;
    }
    return 1;
}

/* Debugging function. Just show bits in the specified bitmap. Not used
 * but here for not having to rewrite it when debugging is needed. */
void printBits(unsigned char *p, unsigned long count) {
    unsigned long j, i, byte;

    for (j = 0; j < count; j++) {
        byte = p[j];
        for (i = 0x80; i > 0; i /= 2)
            printf("%c", (byte & i) ? '1' : '0');
        printf("|");
    }
    printf("\n");
}

/* -----------------------------------------------------------------------------
 * Bits related string commands: GETBIT, SETBIT, BITCOUNT, BITOP.
 * -------------------------------------------------------------------------- */

#define BITOP_AND   0
#define BITOP_OR    1
#define BITOP_XOR   2
#define BITOP_NOT   3
#define BITOP_DIFF  4 /* DIFF(X, A1, A2, ..., An) = X & !(A1 | A2 | ... | An) */
#define BITOP_DIFF1 5 /* DIFF1(X, A1, A2, ..., An) = !X & (A1 | A2 | ... | An) */
#define BITOP_ANDOR 6 /* ANDOR(X, A1, A2, ..., An) = X & (A1 | A2 | ... | An) */

/* ONE(A1, A2, ..., An) = X.
 * If X[i] is the i-th bit of X then:
 * X[i] == 1 if and only if there is m such that:
 * Am[i] == 1 and Al[i] == 0 for all l != m. */
#define BITOP_ONE   7

#define BITFIELDOP_GET 0
#define BITFIELDOP_SET 1
#define BITFIELDOP_INCRBY 2

/* This helper function used by GETBIT / SETBIT parses the bit offset argument
 * making sure an error is returned if it is negative or if it overflows
 * Redis 512 MB limit for the string value or more (server.proto_max_bulk_len).
 *
 * If the 'hash' argument is true, and 'bits is positive, then the command
 * will also parse bit offsets prefixed by "#". In such a case the offset
 * is multiplied by 'bits'. This is useful for the BITFIELD command. */
int getBitOffsetFromArgument(client *c, robj *o, uint64_t *offset, int hash, int bits) {
    long long loffset;
    char *err = "bit offset is not an integer or out of range";
    char *p = o->ptr;
    size_t plen = sdslen(p);
    int usehash = 0;

    /* Handle #<offset> form. */
    if (p[0] == '#' && hash && bits > 0) usehash = 1;

    if (string2ll(p+usehash,plen-usehash,&loffset) == 0) {
        addReplyError(c,err);
        return C_ERR;
    }

    /* Adjust the offset by 'bits' for #<offset> form. */
    if (usehash) loffset *= bits;

    /* Limit offset to server.proto_max_bulk_len (512MB in bytes by default) */
    if (loffset < 0 || (!mustObeyClient(c) && (loffset >> 3) >= server.proto_max_bulk_len))
    {
        addReplyError(c,err);
        return C_ERR;
    }

    *offset = loffset;
    return C_OK;
}

/* This helper function for BITFIELD parses a bitfield type in the form
 * <sign><bits> where sign is 'u' or 'i' for unsigned and signed, and
 * the bits is a value between 1 and 64. However 64 bits unsigned integers
 * are reported as an error because of current limitations of Redis protocol
 * to return unsigned integer values greater than INT64_MAX.
 *
 * On error C_ERR is returned and an error is sent to the client. */
int getBitfieldTypeFromArgument(client *c, robj *o, int *sign, int *bits) {
    char *p = o->ptr;
    char *err = "Invalid bitfield type. Use something like i16 u8. Note that u64 is not supported but i64 is.";
    long long llbits;

    if (p[0] == 'i') {
        *sign = 1;
    } else if (p[0] == 'u') {
        *sign = 0;
    } else {
        addReplyError(c,err);
        return C_ERR;
    }

    if ((string2ll(p+1,strlen(p+1),&llbits)) == 0 ||
        llbits < 1 ||
        (*sign == 1 && llbits > 64) ||
        (*sign == 0 && llbits > 63))
    {
        addReplyError(c,err);
        return C_ERR;
    }
    *bits = llbits;
    return C_OK;
}

/* This is a helper function for commands implementations that need to write
 * bits to a string object. The command creates or pad with zeroes the string
 * so that the 'maxbit' bit can be addressed. The object is finally
 * returned. Otherwise if the key holds a wrong type NULL is returned and
 * an error is sent to the client.
 * 
 * (Must provide all the arguments to the function)
 */
static kvobj *lookupStringForBitCommand(client *c, uint64_t maxbit, 
                                       size_t *strOldSize, size_t *strGrowSize) 
{
    dictEntryLink link;
    size_t byte = maxbit >> 3;
    kvobj *o = lookupKeyWriteWithLink(c->db,c->argv[1],&link);
    if (checkType(c,o,OBJ_STRING)) return NULL;

    if (o == NULL) {
        o = createObject(OBJ_STRING,sdsnewlen(NULL, byte+1));
        dbAddByLink(c->db,c->argv[1],&o,&link);
        *strGrowSize = byte + 1;
        *strOldSize = 0;
    } else {
        o = dbUnshareStringValue(c->db,c->argv[1],o);
        *strOldSize  = sdslen(o->ptr);
        o->ptr = sdsgrowzero(o->ptr,byte+1);
        *strGrowSize = sdslen(o->ptr) - *strOldSize;
    }
    return o;
}

/* Return a pointer to the string object content, and stores its length
 * in 'len'. The user is required to pass (likely stack allocated) buffer
 * 'llbuf' of at least LONG_STR_SIZE bytes. Such a buffer is used in the case
 * the object is integer encoded in order to provide the representation
 * without using heap allocation.
 *
 * The function returns the pointer to the object array of bytes representing
 * the string it contains, that may be a pointer to 'llbuf' or to the
 * internal object representation. As a side effect 'len' is filled with
 * the length of such buffer.
 *
 * If the source object is NULL the function is guaranteed to return NULL
 * and set 'len' to 0. */
unsigned char *getObjectReadOnlyString(robj *o, long *len, char *llbuf) {
    serverAssert(!o || o->type == OBJ_STRING);
    unsigned char *p = NULL;

    /* Set the 'p' pointer to the string, that can be just a stack allocated
     * array if our string was integer encoded. */
    if (o && o->encoding == OBJ_ENCODING_INT) {
        p = (unsigned char*) llbuf;
        if (len) *len = ll2string(llbuf,LONG_STR_SIZE,(long)o->ptr);
    } else if (o) {
        p = (unsigned char*) o->ptr;
        if (len) *len = sdslen(o->ptr);
    } else {
        if (len) *len = 0;
    }
    return p;
}

/* SETBIT key offset bitvalue */
void setbitCommand(client *c) {
    char *err = "bit is not an integer or out of range";
    uint64_t bitoffset;
    ssize_t byte, bit;
    int byteval, bitval;
    long on;

    if (getBitOffsetFromArgument(c,c->argv[2],&bitoffset,0,0) != C_OK)
        return;

    if (getLongFromObjectOrReply(c,c->argv[3],&on,err) != C_OK)
        return;

    /* Bits can only be set or cleared... */
    if (on & ~1) {
        addReplyError(c,err);
        return;
    }

    size_t strOldSize, strGrowSize;
    kvobj *o = lookupStringForBitCommand(c, bitoffset, &strOldSize, &strGrowSize);
    if (o == NULL) return;

    /* Get current values */
    byte = bitoffset >> 3;
    byteval = ((uint8_t*)o->ptr)[byte];
    bit = 7 - (bitoffset & 0x7);
    bitval = byteval & (1 << bit);

    /* Either it is newly created, changed length, or the bit changes before and after.
     * Note that the bitval here is actually a decimal number.
     * So we need to use `!!` to convert it to 0 or 1 for comparison. */
    if (strGrowSize || (!!bitval != on)) {
        /* Update byte with new bit value. */
        byteval &= ~(1 << bit);
        byteval |= ((on & 0x1) << bit);
        ((uint8_t*)o->ptr)[byte] = byteval;
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING,"setbit",c->argv[1],c->db->id);
        server.dirty++;

        /* If this is not a new key (old size not 0) and size changed, then 
         * update the keysizes histogram. Otherwise, the histogram already 
         * updated in lookupStringForBitCommand() by calling dbAdd(). */
        if ((strOldSize > 0) && (strGrowSize != 0))
            updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_STRING, 
                               strOldSize, strOldSize + strGrowSize);
    }

    /* Return original value. */
    addReply(c, bitval ? shared.cone : shared.czero);
}

/* GETBIT key offset */
void getbitCommand(client *c) {
    char llbuf[32];
    uint64_t bitoffset;
    size_t byte, bit;
    size_t bitval = 0;

    if (getBitOffsetFromArgument(c,c->argv[2],&bitoffset,0,0) != C_OK)
        return;

    kvobj *kv = lookupKeyReadOrReply(c, c->argv[1], shared.czero);
    if (kv == NULL || checkType(c,kv,OBJ_STRING)) return;

    byte = bitoffset >> 3;
    bit = 7 - (bitoffset & 0x7);
    if (sdsEncodedObject(kv)) {
        if (byte < sdslen(kv->ptr))
            bitval = ((uint8_t*)kv->ptr)[byte] & (1 << bit);
    } else {
        if (byte < (size_t)ll2string(llbuf,sizeof(llbuf),(long)kv->ptr))
            bitval = llbuf[byte] & (1 << bit);
    }

    addReply(c, bitval ? shared.cone : shared.czero);
}

#ifdef HAVE_AVX2
/* Compute the given bitop operation using AVX2 intrinsics.
 * Return how many bytes were successfully processed, as AVX2 operates on
 * 256-bit registers so if `minlen` is not a multiple of 32 some of the bytes
 * will be skipped. They will be taken care for in the unoptimized loop in the
 * main bitopCommand function. */
ATTRIBUTE_TARGET_AVX2
unsigned long bitopCommandAVX(unsigned char **keys, unsigned char *res, 
                              unsigned long op, unsigned long numkeys,
                              unsigned long minlen)
{
    const unsigned long step = sizeof(__m256i);

    unsigned long i;
    unsigned long processed = 0;
    unsigned char *res_start = res;
    unsigned char *fst_key = keys[0];

    if (minlen < step) {
        return 0;
    }

    /* Unlike other operations that do the same with all source keys
     * DIFF, DIFF1 and ANDOR all compute the disjunction of all the source keys
     * but the first one. We first store that disjunction in `lres` and later 
     * compute the final operation using the first source key. */
    if (op != BITOP_DIFF && op != BITOP_DIFF1 && op != BITOP_ANDOR) {
        memcpy(res, keys[0], minlen);
    }

    const __m256i max256 = _mm256_set1_epi64x(-1);
    const __m256i zero256 = _mm256_set1_epi64x(0);

    switch (op) {
    case BITOP_AND:
        while (minlen >= step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                lres = _mm256_and_si256(lres, lkey);
            }
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_DIFF:
    case BITOP_DIFF1:
    case BITOP_ANDOR:
    case BITOP_OR:
        while (minlen >= step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                lres = _mm256_or_si256(lres, lkey);
            }
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_XOR:
        while (minlen >= step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                lres = _mm256_xor_si256(lres, lkey);
            }
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_NOT:
        while (minlen >= step) {
             __m256i lres = _mm256_lddqu_si256((__m256i*)res);
            lres = _mm256_xor_si256(lres, max256);
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_ONE:
        while (minlen >= step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);
            __m256i common_bits = zero256;

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                __m256i common = _mm256_and_si256(lres, lkey);
                common_bits = _mm256_or_si256(common_bits, common);

                lres = _mm256_xor_si256(lres, lkey);
            }
            lres = _mm256_andnot_si256(common_bits, lres);
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    default:
        break;
    }

    res = res_start;
    switch (op) {
    case BITOP_DIFF:
        for (i = 0; i < processed; i += step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);
            __m256i fkey = _mm256_lddqu_si256((__m256i*)fst_key);

            lres = _mm256_andnot_si256(lres, fkey);
            _mm256_storeu_si256((__m256i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    case BITOP_DIFF1:
        for (i = 0; i < processed; i += step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);
            __m256i fkey = _mm256_lddqu_si256((__m256i*)fst_key);

            lres = _mm256_andnot_si256(fkey, lres);
            _mm256_storeu_si256((__m256i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    case BITOP_ANDOR:
        for (i = 0; i < processed; i += step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);
            __m256i fkey = _mm256_lddqu_si256((__m256i*)fst_key);

            lres = _mm256_and_si256(fkey, lres);
            _mm256_storeu_si256((__m256i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    default:
        break;
    }

    return processed;
}
#endif /* HAVE_AVX2 */

/* BITOP op_name target_key src_key1 src_key2 src_key3 ... src_keyN */
REDIS_NO_SANITIZE("alignment")
void bitopCommand(client *c) {
    char *opname = c->argv[1]->ptr;
    robj *targetkey = c->argv[2];
    unsigned long op, j, numkeys;
    robj **objects;      /* Array of source objects. */
    unsigned char **src; /* Array of source strings pointers. */
    unsigned long *len, maxlen = 0; /* Array of length of src strings,
                                       and max len. */
    unsigned long minlen = 0;    /* Min len among the input keys. */
    unsigned char *res = NULL; /* Resulting string. */

    /* Parse the operation name. */
    if ((opname[0] == 'a' || opname[0] == 'A') && !strcasecmp(opname,"and"))
        op = BITOP_AND;
    else if((opname[0] == 'o' || opname[0] == 'O') && !strcasecmp(opname,"or"))
        op = BITOP_OR;
    else if((opname[0] == 'x' || opname[0] == 'X') && !strcasecmp(opname,"xor"))
        op = BITOP_XOR;
    else if((opname[0] == 'n' || opname[0] == 'N') && !strcasecmp(opname,"not"))
        op = BITOP_NOT;
    else if ((opname[0] == 'd' || opname[0] == 'D') && !strcasecmp(opname,"diff"))
        op = BITOP_DIFF;
    else if ((opname[0] == 'd' || opname[0] == 'D') && !strcasecmp(opname,"diff1"))
        op = BITOP_DIFF1;
    else if ((opname[0] == 'a' || opname[0] == 'A') && !strcasecmp(opname,"andor"))
        op = BITOP_ANDOR;
    else if ((opname[0] == 'o' || opname[0] == 'O') && !strcasecmp(opname,"one"))
        op = BITOP_ONE;
    else {
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Sanity check: NOT accepts only a single key argument. */
    if (op == BITOP_NOT && c->argc != 4) {
        addReplyError(c,"BITOP NOT must be called with a single source key.");
        return;
    }

    if ((op == BITOP_DIFF || op == BITOP_DIFF1 || op == BITOP_ANDOR) && c->argc < 5) {
        sds opname_upper = sdsnew(opname);
        sdstoupper(opname_upper);
        addReplyErrorFormat(c,"BITOP %s must be called with at least two source keys.", opname_upper);
        sdsfree(opname_upper);
        return;
    }

    /* Lookup keys, and store pointers to the string objects into an array. */
    numkeys = c->argc - 3;
    src = zmalloc(sizeof(unsigned char*) * numkeys);
    len = zmalloc(sizeof(long) * numkeys);
    objects = zmalloc(sizeof(robj*) * numkeys);
    for (j = 0; j < numkeys; j++) {
        kvobj *kv = lookupKeyRead(c->db, c->argv[j + 3]);
        /* Handle non-existing keys as empty strings. */
        if (kv == NULL) {
            objects[j] = NULL;
            src[j] = NULL;
            len[j] = 0;
            minlen = 0;
            continue;
        }
        /* Return an error if one of the keys is not a string. */
        if (checkType(c, kv, OBJ_STRING)) {
            unsigned long i;
            for (i = 0; i < j; i++) {
                if (objects[i])
                    decrRefCount(objects[i]);
            }
            zfree(src);
            zfree(len);
            zfree(objects);
            return;
        }
        objects[j] = getDecodedObject(kv);
        src[j] = objects[j]->ptr;
        len[j] = sdslen(objects[j]->ptr);
        if (len[j] > maxlen) maxlen = len[j];
        if (j == 0 || len[j] < minlen) minlen = len[j];
    }

    /* Compute the bit operation, if at least one string is not empty. */
    if (maxlen) {
        res = (unsigned char*) sdsnewlen(NULL,maxlen);
        unsigned char output, byte, disjunction, common_bits;
        unsigned long i;
        int useAVX2 = 0;

        /* Number of bytes processed from each source key */
        j = 0;

#if defined(HAVE_AVX2)
        if (BITOP_USE_AVX2) {
            j = bitopCommandAVX(src, res, op, numkeys, minlen);

            serverAssert(minlen >= j);
            minlen -= j;

            useAVX2 = 1;
        }
#endif

#if !defined(USE_ALIGNED_ACCESS)
        /* We don't have AVX2 but we still have fast path:
         * as far as we have data for all the input bitmaps we
         * can take a fast path that performs much better than the
         * vanilla algorithm. On ARM we skip the fast path since it will
         * result in GCC compiling the code using multiple-words load/store
         * operations that are not supported even in ARM >= v6. */
        if (minlen >= sizeof(unsigned long)*4) {
            /* We can't have entered the AVX2 path since minlen >= sizeof(unsigned long)*4
             * AVX2 path operates on steps of sizeof(__m256i) which for 64-bit
             * machines (the only ones supporting AVX2) is equal to
             * sizeof(unsigned long)*4. That means after the AVX2
             * path minlen will necessarily be < sizeof(unsigned long)*4. */
            serverAssert(!useAVX2);

            unsigned long **lp = (unsigned long**)src;
            unsigned long *lres = (unsigned long*) res;

            /* Index over the unsigned long version of the source keys */
            size_t k = 0;

            /* Unlike other operations that do the same with all source keys
             * DIFF, DIFF1 and ANDOR all compute the disjunction of all the
             * source keys but the first one. We first store that disjunction
             * in `lres` and later compute the final operation using the first
             * source key. */
            if (op != BITOP_DIFF && op != BITOP_DIFF1 && op != BITOP_ANDOR)
                memcpy(lres,src[0],minlen);

            /* Different branches per different operations for speed (sorry). */
            if (op == BITOP_AND) {
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] &= lp[i][k+0];
                        lres[1] &= lp[i][k+1];
                        lres[2] &= lp[i][k+2];
                        lres[3] &= lp[i][k+3];
                    }
                    k+=4;
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_OR) {
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] |= lp[i][k+0];
                        lres[1] |= lp[i][k+1];
                        lres[2] |= lp[i][k+2];
                        lres[3] |= lp[i][k+3];
                    }
                    k+=4;
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_XOR) {
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] ^= lp[i][k+0];
                        lres[1] ^= lp[i][k+1];
                        lres[2] ^= lp[i][k+2];
                        lres[3] ^= lp[i][k+3];
                    }
                    k+=4;
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_NOT) {
                while(minlen >= sizeof(unsigned long)*4) {
                    lres[0] = ~lres[0];
                    lres[1] = ~lres[1];
                    lres[2] = ~lres[2];
                    lres[3] = ~lres[3];
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_DIFF || op == BITOP_DIFF1 || op == BITOP_ANDOR) {
                size_t processed = 0;
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] |= lp[i][k+0];
                        lres[1] |= lp[i][k+1];
                        lres[2] |= lp[i][k+2];
                        lres[3] |= lp[i][k+3];
                    }
                    k+=4;
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                    processed += sizeof(unsigned long)*4;
                }

                lres = (unsigned long*) res;
                unsigned long *first_key = (unsigned long*)src[0];
                switch (op) {
                case BITOP_DIFF:
                    for (i = 0; i < processed; i += sizeof(unsigned long)*4) {
                        lres[0] = (first_key[0] & ~lres[0]);
                        lres[1] = (first_key[1] & ~lres[1]);
                        lres[2] = (first_key[2] & ~lres[2]);
                        lres[3] = (first_key[3] & ~lres[3]);
                        lres+=4;
                        first_key += 4;
                    }
                    break;
                case BITOP_DIFF1:
                    for (i = 0; i < processed; i += sizeof(unsigned long)*4) {
                        lres[0] = (~first_key[0] & lres[0]);
                        lres[1] = (~first_key[1] & lres[1]);
                        lres[2] = (~first_key[2] & lres[2]);
                        lres[3] = (~first_key[3] & lres[3]);
                        lres+=4;
                        first_key += 4;
                    }
                    break;
                case BITOP_ANDOR:
                    for (i = 0; i < processed; i += sizeof(unsigned long)*4) {
                        lres[0] = (first_key[0] & lres[0]);
                        lres[1] = (first_key[1] & lres[1]);
                        lres[2] = (first_key[2] & lres[2]);
                        lres[3] = (first_key[3] & lres[3]);
                        lres+=4;
                        first_key += 4;
                    }
                    break;
                }
            } else if (op == BITOP_ONE) {
                unsigned long lcommon_bits[4];

                while(minlen >= sizeof(unsigned long)*4) {
                    memset(lcommon_bits, 0, sizeof(lcommon_bits));

                    for (i = 1; i < numkeys; i++) {
                        lcommon_bits[0] |= (lres[0] & lp[i][k+0]);
                        lcommon_bits[1] |= (lres[1] & lp[i][k+1]);
                        lcommon_bits[2] |= (lres[2] & lp[i][k+2]);
                        lcommon_bits[3] |= (lres[3] & lp[i][k+3]);

                        lres[0] ^= lp[i][k+0];
                        lres[1] ^= lp[i][k+1];
                        lres[2] ^= lp[i][k+2];
                        lres[3] ^= lp[i][k+3];
                    }

                    lres[0] &= ~lcommon_bits[0];
                    lres[1] &= ~lcommon_bits[1];
                    lres[2] &= ~lcommon_bits[2];
                    lres[3] &= ~lcommon_bits[3];

                    k+=4;
                    lres+=4;
                    j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            }
        }
#endif /* !defined(USE_ALIGNED_ACCESS) */

        /* j is set to the next byte to process by the previous loop. */
        for (; j < maxlen; j++) {
            output = (len[0] <= j) ? 0 : src[0][j];
            if (op == BITOP_NOT) output = ~output;
            disjunction = 0;
            common_bits = 0;

            for (i = 1; i < numkeys; i++) {
                int skip = 0;
                byte = (len[i] <= j) ? 0 : src[i][j];
                switch(op) {
                case BITOP_AND:
                    output &= byte;
                    skip = (output == 0);
                    break;
                case BITOP_OR:
                    output |= byte;
                    skip = (output == 0xff);
                    break;
                case BITOP_XOR: output ^= byte; break;

                /* For DIFF, DIFF1 and ANDOR we compute the disjunction of all
                 * key arguments except the first one. After that we do their
                 * respective bit op on said first arg and that disjunction.
                 * */
                case BITOP_DIFF:
                case BITOP_DIFF1:
                case BITOP_ANDOR:
                    disjunction |= byte;
                    skip = (disjunction == 0xff);
                    break;

                /* BITOP ONE dest key_1 [key_2...]
                 * If dest[i] is the i-th bit of dest then:
                 * dest[i] == 1 if and only if there is j such that key_j[i] == 1
                 * and key_n[i] == 0 for all n != j.
                 *
                 * In order to compute that on each step we track which bits
                 * were seen in more than one key and store that in a helper
                 * variable. Then the operation is just XOR but on each step we
                 * nullify the bits that are set in the helper.
                 * Logically, this operation is the same as nullifying the
                 * helper bits only once at the end, but performance-wise it had
                 * no significant benefit and makes the code only more unclear.
                 *
                 * e.g:
                 * 0001 0111 # key1
                 * 0010 0110 # key2
                 *
                 * 0011 0001 # intermediate1
                 * 0000 0110 # helper
                 * 0011 0001 # intermediate1 & ~helper
                 *
                 * 0100 1101 # key3
                 *
                 * 0111 1100 # intermediate2
                 * 0000 0111 # helper
                 * 0111 1000 # intermediate2 & ~helper
                 * ---------
                 * 0111 1000 # result
                 * */
                case BITOP_ONE:
                    common_bits |= (output & byte);
                    output ^= byte;
                    output &= ~common_bits;
                    skip = (common_bits == 0xff);
                    break;
                default:
                    break;
                }

                if (skip) {
                    break;
                }
            }

            switch(op) {
            case BITOP_DIFF:
                res[j] = (output & ~disjunction);
                break;
            case BITOP_DIFF1:
                res[j] = (~output & disjunction);
                break;
            case BITOP_ANDOR:
                res[j] = (output & disjunction);
                break;
            default:
                res[j] = output;
                break;
            }
        }
    }
    for (j = 0; j < numkeys; j++) {
        if (objects[j])
            decrRefCount(objects[j]);
    }
    zfree(src);
    zfree(len);
    zfree(objects);

    /* Store the computed value into the target key */
    if (maxlen) {
        robj *o = createObject(OBJ_STRING, res);
        setKey(c, c->db, targetkey, &o, 0);
        notifyKeyspaceEvent(NOTIFY_STRING,"set",targetkey,c->db->id);
        server.dirty++;
    } else if (dbDelete(c->db,targetkey)) {
        signalModifiedKey(c,c->db,targetkey);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",targetkey,c->db->id);
        server.dirty++;
    }
    addReplyLongLong(c,maxlen); /* Return the output string length in bytes. */
}

/* BITCOUNT key [start end [BIT|BYTE]] */
void bitcountCommand(client *c) {
    kvobj *o;
    long long start, end;
    long strlen;
    unsigned char *p;
    char llbuf[LONG_STR_SIZE];
    int isbit = 0;
    unsigned char first_byte_neg_mask = 0, last_byte_neg_mask = 0;

    /* Parse start/end range if any. */
    if (c->argc == 4 || c->argc == 5) {
        if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK)
            return;
        if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK)
            return;
        if (c->argc == 5) {
            if (!strcasecmp(c->argv[4]->ptr,"bit")) isbit = 1;
            else if (!strcasecmp(c->argv[4]->ptr,"byte")) isbit = 0;
            else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
        }
        /* Lookup, check for type. */
        o = lookupKeyRead(c->db, c->argv[1]);
        if (checkType(c, o, OBJ_STRING)) return;
        p = getObjectReadOnlyString(o,&strlen,llbuf);
        long long totlen = strlen;

        /* Make sure we will not overflow */
        serverAssert(totlen <= LLONG_MAX >> 3);

        /* Convert negative indexes */
        if (start < 0 && end < 0 && start > end) {
            addReply(c,shared.czero);
            return;
        }
        if (isbit) totlen <<= 3;
        if (start < 0) start = totlen+start;
        if (end < 0) end = totlen+end;
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        if (end >= totlen) end = totlen-1;
        if (isbit && start <= end) {
            /* Before converting bit offset to byte offset, create negative masks
             * for the edges. */
            first_byte_neg_mask = ~((1<<(8-(start&7)))-1) & 0xFF;
            last_byte_neg_mask = (1<<(7-(end&7)))-1;
            start >>= 3;
            end >>= 3;
        }
    } else if (c->argc == 2) {
        /* Lookup, check for type. */
        o = lookupKeyRead(c->db, c->argv[1]);
        if (checkType(c, o, OBJ_STRING)) return;
        p = getObjectReadOnlyString(o,&strlen,llbuf);
        /* The whole string. */
        start = 0;
        end = strlen-1;
    } else {
        /* Syntax error. */
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* Return 0 for non existing keys. */
    if (o == NULL) {
        addReply(c, shared.czero);
        return;
    }

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * zero can be returned is: start > end. */
    if (start > end) {
        addReply(c,shared.czero);
    } else {
        long bytes = (long)(end-start+1);
        long long count = redisPopcount(p+start,bytes);
        if (first_byte_neg_mask != 0 || last_byte_neg_mask != 0) {
            unsigned char firstlast[2] = {0, 0};
            /* We may count bits of first byte and last byte which are out of
            * range. So we need to subtract them. Here we use a trick. We set
            * bits in the range to zero. So these bit will not be excluded. */
            if (first_byte_neg_mask != 0) firstlast[0] = p[start] & first_byte_neg_mask;
            if (last_byte_neg_mask != 0) firstlast[1] = p[end] & last_byte_neg_mask;
            count -= redisPopcount(firstlast,2);
        }
        addReplyLongLong(c,count);
    }
}

/* BITPOS key bit [start [end [BIT|BYTE]]] */
void bitposCommand(client *c) {
    kvobj *o;
    long long start, end;
    long bit, strlen;
    unsigned char *p;
    char llbuf[LONG_STR_SIZE];
    int isbit = 0, end_given = 0;
    unsigned char first_byte_neg_mask = 0, last_byte_neg_mask = 0;

    /* Parse the bit argument to understand what we are looking for, set
     * or clear bits. */
    if (getLongFromObjectOrReply(c,c->argv[2],&bit,NULL) != C_OK)
        return;
    if (bit != 0 && bit != 1) {
        addReplyError(c, "The bit argument must be 1 or 0.");
        return;
    }

    /* Parse start/end range if any. */
    if (c->argc == 4 || c->argc == 5 || c->argc == 6) {
        if (getLongLongFromObjectOrReply(c,c->argv[3],&start,NULL) != C_OK)
            return;
        if (c->argc == 6) {
            if (!strcasecmp(c->argv[5]->ptr,"bit")) isbit = 1;
            else if (!strcasecmp(c->argv[5]->ptr,"byte")) isbit = 0;
            else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
        }
        if (c->argc >= 5) {
            if (getLongLongFromObjectOrReply(c,c->argv[4],&end,NULL) != C_OK)
                return;
            end_given = 1;
        }

        /* Lookup, check for type. */
        o = lookupKeyRead(c->db, c->argv[1]);
        if (checkType(c, o, OBJ_STRING)) return;
        p = getObjectReadOnlyString(o, &strlen, llbuf);

        /* Make sure we will not overflow */
        long long totlen = strlen;
        serverAssert(totlen <= LLONG_MAX >> 3);

        if (c->argc < 5) {
            if (isbit) end = (totlen<<3) + 7;
            else end = totlen-1;
        }

        if (isbit) totlen <<= 3;
        /* Convert negative indexes */
        if (start < 0) start = totlen+start;
        if (end < 0) end = totlen+end;
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        if (end >= totlen) end = totlen-1;
        if (isbit && start <= end) {
            /* Before converting bit offset to byte offset, create negative masks
             * for the edges. */
            first_byte_neg_mask = ~((1<<(8-(start&7)))-1) & 0xFF;
            last_byte_neg_mask = (1<<(7-(end&7)))-1;
            start >>= 3;
            end >>= 3;
        }
    } else if (c->argc == 3) {
        /* Lookup, check for type. */
        o = lookupKeyRead(c->db, c->argv[1]);
        if (checkType(c,o,OBJ_STRING)) return;
        p = getObjectReadOnlyString(o,&strlen,llbuf);

        /* The whole string. */
        start = 0;
        end = strlen-1;
    } else {
        /* Syntax error. */
        addReplyErrorObject(c,shared.syntaxerr);
        return;
    }

    /* If the key does not exist, from our point of view it is an infinite
     * array of 0 bits. If the user is looking for the first clear bit return 0,
     * If the user is looking for the first set bit, return -1. */
    if (o == NULL) {
        addReplyLongLong(c, bit ? -1 : 0);
        return;
    }

    /* For empty ranges (start > end) we return -1 as an empty range does
     * not contain a 0 nor a 1. */
    if (start > end) {
        addReplyLongLong(c, -1);
    } else {
        long bytes = end-start+1;
        long long pos;
        unsigned char tmpchar;
        if (first_byte_neg_mask) {
            if (bit) tmpchar = p[start] & ~first_byte_neg_mask;
            else tmpchar = p[start] | first_byte_neg_mask;
            /* Special case, there is only one byte */
            if (last_byte_neg_mask && bytes == 1) {
                if (bit) tmpchar = tmpchar & ~last_byte_neg_mask;
                else tmpchar = tmpchar | last_byte_neg_mask;
            }
            pos = redisBitpos(&tmpchar,1,bit);
            /* If there are no more bytes or we get valid pos, we can exit early */
            if (bytes == 1 || (pos != -1 && pos != 8)) goto result;
            start++;
            bytes--;
        }
        /* If the last byte has not bits in the range, we should exclude it */
        long curbytes = bytes - (last_byte_neg_mask ? 1 : 0);
        if (curbytes > 0) {
            pos = redisBitpos(p+start,curbytes,bit);
            /* If there is no more bytes or we get valid pos, we can exit early */
            if (bytes == curbytes || (pos != -1 && pos != (long long)curbytes<<3)) goto result;
            start += curbytes;
            bytes -= curbytes;
        }
        if (bit) tmpchar = p[end] & ~last_byte_neg_mask;
        else tmpchar = p[end] | last_byte_neg_mask;
        pos = redisBitpos(&tmpchar,1,bit);

    result:
        /* If we are looking for clear bits, and the user specified an exact
         * range with start-end, we can't consider the right of the range as
         * zero padded (as we do when no explicit end is given).
         *
         * So if redisBitpos() returns the first bit outside the range,
         * we return -1 to the caller, to mean, in the specified range there
         * is not a single "0" bit. */
        if (end_given && bit == 0 && pos == (long long)bytes<<3) {
            addReplyLongLong(c,-1);
            return;
        }
        if (pos != -1) pos += (long long)start<<3; /* Adjust for the bytes we skipped. */
        addReplyLongLong(c,pos);
    }
}

/* BITFIELD key subcommand-1 arg ... subcommand-2 arg ... subcommand-N ...
 *
 * Supported subcommands:
 *
 * GET <type> <offset>
 * SET <type> <offset> <value>
 * INCRBY <type> <offset> <increment>
 * OVERFLOW [WRAP|SAT|FAIL]
 */

#define BITFIELD_FLAG_NONE      0
#define BITFIELD_FLAG_READONLY  (1<<0)

struct bitfieldOp {
    uint64_t offset;    /* Bitfield offset. */
    int64_t i64;        /* Increment amount (INCRBY) or SET value */
    int opcode;         /* Operation id. */
    int owtype;         /* Overflow type to use. */
    int bits;           /* Integer bitfield bits width. */
    int sign;           /* True if signed, otherwise unsigned op. */
};

/* This implements both the BITFIELD command and the BITFIELD_RO command
 * when flags is set to BITFIELD_FLAG_READONLY: in this case only the
 * GET subcommand is allowed, other subcommands will return an error. */
void bitfieldGeneric(client *c, int flags) {
    kvobj *o;
    uint64_t bitoffset;
    int j, numops = 0, changes = 0;
    size_t strOldSize, strGrowSize = 0;
    struct bitfieldOp *ops = NULL; /* Array of ops to execute at end. */
    int owtype = BFOVERFLOW_WRAP; /* Overflow type. */
    int readonly = 1;
    uint64_t highest_write_offset = 0;

    for (j = 2; j < c->argc; j++) {
        int remargs = c->argc-j-1; /* Remaining args other than current. */
        char *subcmd = c->argv[j]->ptr; /* Current command name. */
        int opcode; /* Current operation code. */
        long long i64 = 0;  /* Signed SET value. */
        int sign = 0; /* Signed or unsigned type? */
        int bits = 0; /* Bitfield width in bits. */

        if (!strcasecmp(subcmd,"get") && remargs >= 2)
            opcode = BITFIELDOP_GET;
        else if (!strcasecmp(subcmd,"set") && remargs >= 3)
            opcode = BITFIELDOP_SET;
        else if (!strcasecmp(subcmd,"incrby") && remargs >= 3)
            opcode = BITFIELDOP_INCRBY;
        else if (!strcasecmp(subcmd,"overflow") && remargs >= 1) {
            char *owtypename = c->argv[j+1]->ptr;
            j++;
            if (!strcasecmp(owtypename,"wrap"))
                owtype = BFOVERFLOW_WRAP;
            else if (!strcasecmp(owtypename,"sat"))
                owtype = BFOVERFLOW_SAT;
            else if (!strcasecmp(owtypename,"fail"))
                owtype = BFOVERFLOW_FAIL;
            else {
                addReplyError(c,"Invalid OVERFLOW type specified");
                zfree(ops);
                return;
            }
            continue;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            zfree(ops);
            return;
        }

        /* Get the type and offset arguments, common to all the ops. */
        if (getBitfieldTypeFromArgument(c,c->argv[j+1],&sign,&bits) != C_OK) {
            zfree(ops);
            return;
        }

        if (getBitOffsetFromArgument(c,c->argv[j+2],&bitoffset,1,bits) != C_OK){
            zfree(ops);
            return;
        }

        if (opcode != BITFIELDOP_GET) {
            readonly = 0;
            if (highest_write_offset < bitoffset + bits - 1)
                highest_write_offset = bitoffset + bits - 1;
            /* INCRBY and SET require another argument. */
            if (getLongLongFromObjectOrReply(c,c->argv[j+3],&i64,NULL) != C_OK){
                zfree(ops);
                return;
            }
        }

        /* Populate the array of operations we'll process. */
        ops = zrealloc(ops,sizeof(*ops)*(numops+1));
        ops[numops].offset = bitoffset;
        ops[numops].i64 = i64;
        ops[numops].opcode = opcode;
        ops[numops].owtype = owtype;
        ops[numops].bits = bits;
        ops[numops].sign = sign;
        numops++;

        j += 3 - (opcode == BITFIELDOP_GET);
    }

    if (readonly) {
        /* Lookup for read is ok if key doesn't exit, but errors
         * if it's not a string. */
        o = lookupKeyRead(c->db,c->argv[1]);
        if (o != NULL && checkType(c,o,OBJ_STRING)) {
            zfree(ops);
            return;
        }
    } else {
        if (flags & BITFIELD_FLAG_READONLY) {
            zfree(ops);
            addReplyError(c, "BITFIELD_RO only supports the GET subcommand");
            return;
        }

        /* Lookup by making room up to the farthest bit reached by
         * this operation. */
        if ((o = lookupStringForBitCommand(c,
            highest_write_offset,&strOldSize,&strGrowSize)) == NULL) {
            zfree(ops);
            return;
        }
    }

    addReplyArrayLen(c,numops);

    /* Actually process the operations. */
    for (j = 0; j < numops; j++) {
        struct bitfieldOp *thisop = ops+j;

        /* Execute the operation. */
        if (thisop->opcode == BITFIELDOP_SET ||
            thisop->opcode == BITFIELDOP_INCRBY)
        {
            /* SET and INCRBY: We handle both with the same code path
             * for simplicity. SET return value is the previous value so
             * we need fetch & store as well. */

            /* We need two different but very similar code paths for signed
             * and unsigned operations, since the set of functions to get/set
             * the integers and the used variables types are different. */
            if (thisop->sign) {
                int64_t oldval, newval, wrapped, retval;
                int overflow;

                oldval = getSignedBitfield(o->ptr,thisop->offset,
                        thisop->bits);

                if (thisop->opcode == BITFIELDOP_INCRBY) {
                    overflow = checkSignedBitfieldOverflow(oldval,
                            thisop->i64,thisop->bits,thisop->owtype,&wrapped);
                    newval = overflow ? wrapped : oldval + thisop->i64;
                    retval = newval;
                } else {
                    newval = thisop->i64;
                    overflow = checkSignedBitfieldOverflow(newval,
                            0,thisop->bits,thisop->owtype,&wrapped);
                    if (overflow) newval = wrapped;
                    retval = oldval;
                }

                /* On overflow of type is "FAIL", don't write and return
                 * NULL to signal the condition. */
                if (!(overflow && thisop->owtype == BFOVERFLOW_FAIL)) {
                    addReplyLongLong(c,retval);
                    setSignedBitfield(o->ptr,thisop->offset,
                                      thisop->bits,newval);

                    if (strGrowSize || (oldval != newval))
                        changes++;
                } else {
                    addReplyNull(c);
                }
            } else {
                /* Initialization of 'wrapped' is required to avoid
                * false-positive warning "-Wmaybe-uninitialized" */
                uint64_t oldval, newval, retval, wrapped = 0;
                int overflow;

                oldval = getUnsignedBitfield(o->ptr,thisop->offset,
                        thisop->bits);

                if (thisop->opcode == BITFIELDOP_INCRBY) {
                    newval = oldval + thisop->i64;
                    overflow = checkUnsignedBitfieldOverflow(oldval,
                            thisop->i64,thisop->bits,thisop->owtype,&wrapped);
                    if (overflow) newval = wrapped;
                    retval = newval;
                } else {
                    newval = thisop->i64;
                    overflow = checkUnsignedBitfieldOverflow(newval,
                            0,thisop->bits,thisop->owtype,&wrapped);
                    if (overflow) newval = wrapped;
                    retval = oldval;
                }
                /* On overflow of type is "FAIL", don't write and return
                 * NULL to signal the condition. */
                if (!(overflow && thisop->owtype == BFOVERFLOW_FAIL)) {
                    addReplyLongLong(c,retval);
                    setUnsignedBitfield(o->ptr,thisop->offset,
                                        thisop->bits,newval);

                    if (strGrowSize || (oldval != newval))
                        changes++;
                } else {
                    addReplyNull(c);
                }
            }
        } else {
            /* GET */
            unsigned char buf[9];
            long strlen = 0;
            unsigned char *src = NULL;
            char llbuf[LONG_STR_SIZE];

            if (o != NULL)
                src = getObjectReadOnlyString(o,&strlen,llbuf);

            /* For GET we use a trick: before executing the operation
             * copy up to 9 bytes to a local buffer, so that we can easily
             * execute up to 64 bit operations that are at actual string
             * object boundaries. */
            memset(buf,0,9);
            int i;
            uint64_t byte = thisop->offset >> 3;
            for (i = 0; i < 9; i++) {
                if (src == NULL || i+byte >= (uint64_t)strlen) break;
                buf[i] = src[i+byte];
            }

            /* Now operate on the copied buffer which is guaranteed
             * to be zero-padded. */
            if (thisop->sign) {
                int64_t val = getSignedBitfield(buf,thisop->offset-(byte*8),
                                            thisop->bits);
                addReplyLongLong(c,val);
            } else {
                uint64_t val = getUnsignedBitfield(buf,thisop->offset-(byte*8),
                                            thisop->bits);
                addReplyLongLong(c,val);
            }
        }
    }

    if (changes) {

        /* If this is not a new key (old size not 0) and size changed, then 
         * update the keysizes histogram. Otherwise, the histogram already 
         * updated in lookupStringForBitCommand() by calling dbAdd(). */
        if ((strOldSize > 0) && (strGrowSize != 0))
            updateKeysizesHist(c->db, getKeySlot(c->argv[1]->ptr), OBJ_STRING,
                               strOldSize, strOldSize + strGrowSize);
        
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING,"setbit",c->argv[1],c->db->id);
        server.dirty += changes;
    }
    zfree(ops);
}

void bitfieldCommand(client *c) {
    bitfieldGeneric(c, BITFIELD_FLAG_NONE);
}

void bitfieldroCommand(client *c) {
    bitfieldGeneric(c, BITFIELD_FLAG_READONLY);
}
