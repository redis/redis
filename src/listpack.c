/* Listpack -- A lists of strings serialization format
 *
 * This file implements the specification you can find at:
 *
 *  https://github.com/antirez/listpack
 *
 * Copyright (c) 2017, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2020, Redis Labs, Inc
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

#include <stdint.h>
#include <limits.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "listpack.h"
#include "listpack_malloc.h"
#include "redisassert.h"

#define LP_HDR_SIZE 6       /* 32 bit total len + 16 bit number of elements. */
#define LP_HDR_NUMELE_UNKNOWN UINT16_MAX
#define LP_MAX_INT_ENCODING_LEN 9
#define LP_MAX_BACKLEN_SIZE 5
#define LP_MAX_ENTRY_BACKLEN 34359738367ULL
#define LP_ENCODING_INT 0
#define LP_ENCODING_STRING 1

#define LP_ENCODING_7BIT_UINT 0
#define LP_ENCODING_7BIT_UINT_MASK 0x80
#define LP_ENCODING_IS_7BIT_UINT(byte) (((byte)&LP_ENCODING_7BIT_UINT_MASK)==LP_ENCODING_7BIT_UINT)
#define LP_ENCODING_7BIT_UINT_ENTRY_SIZE 2

#define LP_ENCODING_6BIT_STR 0x80
#define LP_ENCODING_6BIT_STR_MASK 0xC0
#define LP_ENCODING_IS_6BIT_STR(byte) (((byte)&LP_ENCODING_6BIT_STR_MASK)==LP_ENCODING_6BIT_STR)

#define LP_ENCODING_13BIT_INT 0xC0
#define LP_ENCODING_13BIT_INT_MASK 0xE0
#define LP_ENCODING_IS_13BIT_INT(byte) (((byte)&LP_ENCODING_13BIT_INT_MASK)==LP_ENCODING_13BIT_INT)
#define LP_ENCODING_13BIT_INT_ENTRY_SIZE 3

#define LP_ENCODING_12BIT_STR 0xE0
#define LP_ENCODING_12BIT_STR_MASK 0xF0
#define LP_ENCODING_IS_12BIT_STR(byte) (((byte)&LP_ENCODING_12BIT_STR_MASK)==LP_ENCODING_12BIT_STR)

#define LP_ENCODING_16BIT_INT 0xF1
#define LP_ENCODING_16BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_16BIT_INT(byte) (((byte)&LP_ENCODING_16BIT_INT_MASK)==LP_ENCODING_16BIT_INT)
#define LP_ENCODING_16BIT_INT_ENTRY_SIZE 4

#define LP_ENCODING_24BIT_INT 0xF2
#define LP_ENCODING_24BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_24BIT_INT(byte) (((byte)&LP_ENCODING_24BIT_INT_MASK)==LP_ENCODING_24BIT_INT)
#define LP_ENCODING_24BIT_INT_ENTRY_SIZE 5

#define LP_ENCODING_32BIT_INT 0xF3
#define LP_ENCODING_32BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_32BIT_INT(byte) (((byte)&LP_ENCODING_32BIT_INT_MASK)==LP_ENCODING_32BIT_INT)
#define LP_ENCODING_32BIT_INT_ENTRY_SIZE 6

#define LP_ENCODING_64BIT_INT 0xF4
#define LP_ENCODING_64BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_64BIT_INT(byte) (((byte)&LP_ENCODING_64BIT_INT_MASK)==LP_ENCODING_64BIT_INT)
#define LP_ENCODING_64BIT_INT_ENTRY_SIZE 10

#define LP_ENCODING_32BIT_STR 0xF0
#define LP_ENCODING_32BIT_STR_MASK 0xFF
#define LP_ENCODING_IS_32BIT_STR(byte) (((byte)&LP_ENCODING_32BIT_STR_MASK)==LP_ENCODING_32BIT_STR)

#define LP_EOF 0xFF

#define LP_ENCODING_6BIT_STR_LEN(p) ((p)[0] & 0x3F)
#define LP_ENCODING_12BIT_STR_LEN(p) ((((p)[0] & 0xF) << 8) | (p)[1])
#define LP_ENCODING_32BIT_STR_LEN(p) (((uint32_t)(p)[1]<<0) | \
                                      ((uint32_t)(p)[2]<<8) | \
                                      ((uint32_t)(p)[3]<<16) | \
                                      ((uint32_t)(p)[4]<<24))

#define lpGetTotalBytes(p)           (((uint32_t)(p)[0]<<0) | \
                                      ((uint32_t)(p)[1]<<8) | \
                                      ((uint32_t)(p)[2]<<16) | \
                                      ((uint32_t)(p)[3]<<24))

#define lpGetNumElements(p)          (((uint32_t)(p)[4]<<0) | \
                                      ((uint32_t)(p)[5]<<8))
#define lpSetTotalBytes(p,v) do { \
    (p)[0] = (v)&0xff; \
    (p)[1] = ((v)>>8)&0xff; \
    (p)[2] = ((v)>>16)&0xff; \
    (p)[3] = ((v)>>24)&0xff; \
} while(0)

#define lpSetNumElements(p,v) do { \
    (p)[4] = (v)&0xff; \
    (p)[5] = ((v)>>8)&0xff; \
} while(0)

/* Validates that 'p' is not outside the listpack.
 * All function that return a pointer to an element in the listpack will assert
 * that this element is valid, so it can be freely used.
 * Generally functions such lpNext and lpDelete assume the input pointer is
 * already validated (since it's the return value of another function). */
#define ASSERT_INTEGRITY(lp, p) do { \
    assert((p) >= (lp)+LP_HDR_SIZE && (p) < (lp)+lpGetTotalBytes((lp))); \
} while (0)

/* Similar to the above, but validates the entire element length rather than just
 * it's pointer. */
#define ASSERT_INTEGRITY_LEN(lp, p, len) do { \
    assert((p) >= (lp)+LP_HDR_SIZE && (p)+(len) < (lp)+lpGetTotalBytes((lp))); \
} while (0)

/* Convert a string into a signed 64 bit integer.
 * The function returns 1 if the string could be parsed into a (non-overflowing)
 * signed 64 bit int, 0 otherwise. The 'value' will be set to the parsed value
 * when the function returns success.
 *
 * Note that this function demands that the string strictly represents
 * a int64 value: no spaces or other characters before or after the string
 * representing the number are accepted, nor zeroes at the start if not
 * for the string "0" representing the zero number.
 *
 * Because of its strictness, it is safe to use this function to check if
 * you can convert a string into a long long, and obtain back the string
 * from the number without any loss in the string representation. *
 *
 * -----------------------------------------------------------------------------
 *
 * Credits: this function was adapted from the Redis source code, file
 * "utils.c", function string2ll(), and is copyright:
 *
 * Copyright(C) 2011, Pieter Noordhuis
 * Copyright(C) 2011, Salvatore Sanfilippo
 *
 * The function is released under the BSD 3-clause license.
 */
int lpStringToInt64(const char *s, unsigned long slen, int64_t *value) {
    const char *p = s;
    unsigned long plen = 0;
    int negative = 0;
    uint64_t v;

    if (plen == slen)
        return 0;

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0') {
        if (value != NULL) *value = 0;
        return 1;
    }

    if (p[0] == '-') {
        negative = 1;
        p++; plen++;

        /* Abort on only a negative sign. */
        if (plen == slen)
            return 0;
    }

    /* First digit should be 1-9, otherwise the string should just be 0. */
    if (p[0] >= '1' && p[0] <= '9') {
        v = p[0]-'0';
        p++; plen++;
    } else {
        return 0;
    }

    while (plen < slen && p[0] >= '0' && p[0] <= '9') {
        if (v > (UINT64_MAX / 10)) /* Overflow. */
            return 0;
        v *= 10;

        if (v > (UINT64_MAX - (p[0]-'0'))) /* Overflow. */
            return 0;
        v += p[0]-'0';

        p++; plen++;
    }

    /* Return if not all bytes were used. */
    if (plen < slen)
        return 0;

    if (negative) {
        if (v > ((uint64_t)(-(INT64_MIN+1))+1)) /* Overflow. */
            return 0;
        if (value != NULL) *value = -v;
    } else {
        if (v > INT64_MAX) /* Overflow. */
            return 0;
        if (value != NULL) *value = v;
    }
    return 1;
}

/* Create a new, empty listpack.
 * On success the new listpack is returned, otherwise an error is returned.
 * Pre-allocate at least `capacity` bytes of memory,
 * over-allocated memory can be shrunk by `lpShrinkToFit`.
 * */
unsigned char *lpNew(size_t capacity) {
    unsigned char *lp = lp_malloc(capacity > LP_HDR_SIZE+1 ? capacity : LP_HDR_SIZE+1);
    if (lp == NULL) return NULL;
    lpSetTotalBytes(lp,LP_HDR_SIZE+1);
    lpSetNumElements(lp,0);
    lp[LP_HDR_SIZE] = LP_EOF;
    return lp;
}

/* Free the specified listpack. */
void lpFree(unsigned char *lp) {
    lp_free(lp);
}

/* Shrink the memory to fit. */
unsigned char* lpShrinkToFit(unsigned char *lp) {
    size_t size = lpGetTotalBytes(lp);
    if (size < lp_malloc_size(lp)) {
        return lp_realloc(lp, size);
    } else {
        return lp;
    }
}

/* Given an element 'ele' of size 'size', determine if the element can be
 * represented inside the listpack encoded as integer, and returns
 * LP_ENCODING_INT if so. Otherwise returns LP_ENCODING_STR if no integer
 * encoding is possible.
 *
 * If the LP_ENCODING_INT is returned, the function stores the integer encoded
 * representation of the element in the 'intenc' buffer.
 *
 * Regardless of the returned encoding, 'enclen' is populated by reference to
 * the number of bytes that the string or integer encoded element will require
 * in order to be represented. */
static inline int lpEncodeGetType(unsigned char *ele, uint32_t size, unsigned char *intenc, uint64_t *enclen) {
    int64_t v;
    if (lpStringToInt64((const char*)ele, size, &v)) {
        if (v >= 0 && v <= 127) {
            /* Single byte 0-127 integer. */
            intenc[0] = v;
            *enclen = 1;
        } else if (v >= -4096 && v <= 4095) {
            /* 13 bit integer. */
            if (v < 0) v = ((int64_t)1<<13)+v;
            intenc[0] = (v>>8)|LP_ENCODING_13BIT_INT;
            intenc[1] = v&0xff;
            *enclen = 2;
        } else if (v >= -32768 && v <= 32767) {
            /* 16 bit integer. */
            if (v < 0) v = ((int64_t)1<<16)+v;
            intenc[0] = LP_ENCODING_16BIT_INT;
            intenc[1] = v&0xff;
            intenc[2] = v>>8;
            *enclen = 3;
        } else if (v >= -8388608 && v <= 8388607) {
            /* 24 bit integer. */
            if (v < 0) v = ((int64_t)1<<24)+v;
            intenc[0] = LP_ENCODING_24BIT_INT;
            intenc[1] = v&0xff;
            intenc[2] = (v>>8)&0xff;
            intenc[3] = v>>16;
            *enclen = 4;
        } else if (v >= -2147483648 && v <= 2147483647) {
            /* 32 bit integer. */
            if (v < 0) v = ((int64_t)1<<32)+v;
            intenc[0] = LP_ENCODING_32BIT_INT;
            intenc[1] = v&0xff;
            intenc[2] = (v>>8)&0xff;
            intenc[3] = (v>>16)&0xff;
            intenc[4] = v>>24;
            *enclen = 5;
        } else {
            /* 64 bit integer. */
            uint64_t uv = v;
            intenc[0] = LP_ENCODING_64BIT_INT;
            intenc[1] = uv&0xff;
            intenc[2] = (uv>>8)&0xff;
            intenc[3] = (uv>>16)&0xff;
            intenc[4] = (uv>>24)&0xff;
            intenc[5] = (uv>>32)&0xff;
            intenc[6] = (uv>>40)&0xff;
            intenc[7] = (uv>>48)&0xff;
            intenc[8] = uv>>56;
            *enclen = 9;
        }
        return LP_ENCODING_INT;
    } else {
        if (size < 64) *enclen = 1+size;
        else if (size < 4096) *enclen = 2+size;
        else *enclen = 5+size;
        return LP_ENCODING_STRING;
    }
}

/* Store a reverse-encoded variable length field, representing the length
 * of the previous element of size 'l', in the target buffer 'buf'.
 * The function returns the number of bytes used to encode it, from
 * 1 to 5. If 'buf' is NULL the function just returns the number of bytes
 * needed in order to encode the backlen. */
static inline unsigned long lpEncodeBacklen(unsigned char *buf, uint64_t l) {
    if (l <= 127) {
        if (buf) buf[0] = l;
        return 1;
    } else if (l < 16383) {
        if (buf) {
            buf[0] = l>>7;
            buf[1] = (l&127)|128;
        }
        return 2;
    } else if (l < 2097151) {
        if (buf) {
            buf[0] = l>>14;
            buf[1] = ((l>>7)&127)|128;
            buf[2] = (l&127)|128;
        }
        return 3;
    } else if (l < 268435455) {
        if (buf) {
            buf[0] = l>>21;
            buf[1] = ((l>>14)&127)|128;
            buf[2] = ((l>>7)&127)|128;
            buf[3] = (l&127)|128;
        }
        return 4;
    } else {
        if (buf) {
            buf[0] = l>>28;
            buf[1] = ((l>>21)&127)|128;
            buf[2] = ((l>>14)&127)|128;
            buf[3] = ((l>>7)&127)|128;
            buf[4] = (l&127)|128;
        }
        return 5;
    }
}

/* Decode the backlen and returns it. If the encoding looks invalid (more than
 * 5 bytes are used), UINT64_MAX is returned to report the problem. */
static inline uint64_t lpDecodeBacklen(unsigned char *p) {
    uint64_t val = 0;
    uint64_t shift = 0;
    do {
        val |= (uint64_t)(p[0] & 127) << shift;
        if (!(p[0] & 128)) break;
        shift += 7;
        p--;
        if (shift > 28) return UINT64_MAX;
    } while(1);
    return val;
}

/* Encode the string element pointed by 's' of size 'len' in the target
 * buffer 's'. The function should be called with 'buf' having always enough
 * space for encoding the string. This is done by calling lpEncodeGetType()
 * before calling this function. */
static inline void lpEncodeString(unsigned char *buf, unsigned char *s, uint32_t len) {
    if (len < 64) {
        buf[0] = len | LP_ENCODING_6BIT_STR;
        memcpy(buf+1,s,len);
    } else if (len < 4096) {
        buf[0] = (len >> 8) | LP_ENCODING_12BIT_STR;
        buf[1] = len & 0xff;
        memcpy(buf+2,s,len);
    } else {
        buf[0] = LP_ENCODING_32BIT_STR;
        buf[1] = len & 0xff;
        buf[2] = (len >> 8) & 0xff;
        buf[3] = (len >> 16) & 0xff;
        buf[4] = (len >> 24) & 0xff;
        memcpy(buf+5,s,len);
    }
}

/* Return the encoded length of the listpack element pointed by 'p'.
 * This includes the encoding byte, length bytes, and the element data itself.
 * If the element encoding is wrong then 0 is returned.
 * Note that this method may access additional bytes (in case of 12 and 32 bit
 * str), so should only be called when we know 'p' was already validated by
 * lpCurrentEncodedSizeBytes or ASSERT_INTEGRITY_LEN (possibly since 'p' is
 * a return value of another function that validated its return. */
static inline uint32_t lpCurrentEncodedSizeUnsafe(unsigned char *p) {
    if (LP_ENCODING_IS_7BIT_UINT(p[0])) return 1;
    if (LP_ENCODING_IS_6BIT_STR(p[0])) return 1+LP_ENCODING_6BIT_STR_LEN(p);
    if (LP_ENCODING_IS_13BIT_INT(p[0])) return 2;
    if (LP_ENCODING_IS_16BIT_INT(p[0])) return 3;
    if (LP_ENCODING_IS_24BIT_INT(p[0])) return 4;
    if (LP_ENCODING_IS_32BIT_INT(p[0])) return 5;
    if (LP_ENCODING_IS_64BIT_INT(p[0])) return 9;
    if (LP_ENCODING_IS_12BIT_STR(p[0])) return 2+LP_ENCODING_12BIT_STR_LEN(p);
    if (LP_ENCODING_IS_32BIT_STR(p[0])) return 5+LP_ENCODING_32BIT_STR_LEN(p);
    if (p[0] == LP_EOF) return 1;
    return 0;
}

/* Return bytes needed to encode the length of the listpack element pointed by 'p'.
 * This includes just the encoding byte, and the bytes needed to encode the length
 * of the element (excluding the element data itself)
 * If the element encoding is wrong then 0 is returned. */
static inline uint32_t lpCurrentEncodedSizeBytes(unsigned char *p) {
    if (LP_ENCODING_IS_7BIT_UINT(p[0])) return 1;
    if (LP_ENCODING_IS_6BIT_STR(p[0])) return 1;
    if (LP_ENCODING_IS_13BIT_INT(p[0])) return 1;
    if (LP_ENCODING_IS_16BIT_INT(p[0])) return 1;
    if (LP_ENCODING_IS_24BIT_INT(p[0])) return 1;
    if (LP_ENCODING_IS_32BIT_INT(p[0])) return 1;
    if (LP_ENCODING_IS_64BIT_INT(p[0])) return 1;
    if (LP_ENCODING_IS_12BIT_STR(p[0])) return 2;
    if (LP_ENCODING_IS_32BIT_STR(p[0])) return 5;
    if (p[0] == LP_EOF) return 1;
    return 0;
}

/* Skip the current entry returning the next. It is invalid to call this
 * function if the current element is the EOF element at the end of the
 * listpack, however, while this function is used to implement lpNext(),
 * it does not return NULL when the EOF element is encountered. */
unsigned char *lpSkip(unsigned char *p) {
    unsigned long entrylen = lpCurrentEncodedSizeUnsafe(p);
    entrylen += lpEncodeBacklen(NULL,entrylen);
    p += entrylen;
    return p;
}

/* If 'p' points to an element of the listpack, calling lpNext() will return
 * the pointer to the next element (the one on the right), or NULL if 'p'
 * already pointed to the last element of the listpack. */
unsigned char *lpNext(unsigned char *lp, unsigned char *p) {
    assert(p);
    p = lpSkip(p);
    ASSERT_INTEGRITY(lp, p);
    if (p[0] == LP_EOF) return NULL;
    return p;
}

/* If 'p' points to an element of the listpack, calling lpPrev() will return
 * the pointer to the previous element (the one on the left), or NULL if 'p'
 * already pointed to the first element of the listpack. */
unsigned char *lpPrev(unsigned char *lp, unsigned char *p) {
    assert(p);
    if (p-lp == LP_HDR_SIZE) return NULL;
    p--; /* Seek the first backlen byte of the last element. */
    uint64_t prevlen = lpDecodeBacklen(p);
    prevlen += lpEncodeBacklen(NULL,prevlen);
    p -= prevlen-1; /* Seek the first byte of the previous entry. */
    ASSERT_INTEGRITY(lp, p);
    return p;
}

/* Return a pointer to the first element of the listpack, or NULL if the
 * listpack has no elements. */
unsigned char *lpFirst(unsigned char *lp) {
    lp += LP_HDR_SIZE; /* Skip the header. */
    if (lp[0] == LP_EOF) return NULL;
    return lp;
}

/* Return a pointer to the last element of the listpack, or NULL if the
 * listpack has no elements. */
unsigned char *lpLast(unsigned char *lp) {
    unsigned char *p = lp+lpGetTotalBytes(lp)-1; /* Seek EOF element. */
    return lpPrev(lp,p); /* Will return NULL if EOF is the only element. */
}

/* Return the number of elements inside the listpack. This function attempts
 * to use the cached value when within range, otherwise a full scan is
 * needed. As a side effect of calling this function, the listpack header
 * could be modified, because if the count is found to be already within
 * the 'numele' header field range, the new value is set. */
unsigned long lpLength(unsigned char *lp) {
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN) return numele;

    /* Too many elements inside the listpack. We need to scan in order
     * to get the total number. */
    uint32_t count = 0;
    unsigned char *p = lpFirst(lp);
    while(p) {
        count++;
        p = lpNext(lp,p);
    }

    /* If the count is again within range of the header numele field,
     * set it. */
    if (count < LP_HDR_NUMELE_UNKNOWN) lpSetNumElements(lp,count);
    return count;
}

/* Return the listpack element pointed by 'p'.
 *
 * The function changes behavior depending on the passed 'intbuf' value.
 * Specifically, if 'intbuf' is NULL:
 *
 * If the element is internally encoded as an integer, the function returns
 * NULL and populates the integer value by reference in 'count'. Otherwise if
 * the element is encoded as a string a pointer to the string (pointing inside
 * the listpack itself) is returned, and 'count' is set to the length of the
 * string.
 *
 * If instead 'intbuf' points to a buffer passed by the caller, that must be
 * at least LP_INTBUF_SIZE bytes, the function always returns the element as
 * it was a string (returning the pointer to the string and setting the
 * 'count' argument to the string length by reference). However if the element
 * is encoded as an integer, the 'intbuf' buffer is used in order to store
 * the string representation.
 *
 * The user should use one or the other form depending on what the value will
 * be used for. If there is immediate usage for an integer value returned
 * by the function, than to pass a buffer (and convert it back to a number)
 * is of course useless.
 *
 * If the function is called against a badly encoded ziplist, so that there
 * is no valid way to parse it, the function returns like if there was an
 * integer encoded with value 12345678900000000 + <unrecognized byte>, this may
 * be an hint to understand that something is wrong. To crash in this case is
 * not sensible because of the different requirements of the application using
 * this lib.
 *
 * Similarly, there is no error returned since the listpack normally can be
 * assumed to be valid, so that would be a very high API cost. However a function
 * in order to check the integrity of the listpack at load time is provided,
 * check lpIsValid(). */
static inline unsigned char *lpGetWithSize(unsigned char *p, int64_t *count, unsigned char *intbuf, uint64_t *size) {
    int64_t val;
    uint64_t uval, negstart, negmax;

    assert(p); /* assertion for valgrind (avoid NPD) */
    if (LP_ENCODING_IS_7BIT_UINT(p[0])) {
        negstart = UINT64_MAX; /* 7 bit ints are always positive. */
        negmax = 0;
        uval = p[0] & 0x7f;
        if (size) *size = LP_ENCODING_7BIT_UINT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_6BIT_STR(p[0])) {
        *count = LP_ENCODING_6BIT_STR_LEN(p);
        if (size) *size = 2 + *count;
        return p+1;
    } else if (LP_ENCODING_IS_13BIT_INT(p[0])) {
        uval = ((p[0]&0x1f)<<8) | p[1];
        negstart = (uint64_t)1<<12;
        negmax = 8191;
        if (size) *size = LP_ENCODING_13BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_16BIT_INT(p[0])) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8;
        negstart = (uint64_t)1<<15;
        negmax = UINT16_MAX;
        if (size) *size = LP_ENCODING_16BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_24BIT_INT(p[0])) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8 |
               (uint64_t)p[3]<<16;
        negstart = (uint64_t)1<<23;
        negmax = UINT32_MAX>>8;
        if (size) *size = LP_ENCODING_24BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_32BIT_INT(p[0])) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8 |
               (uint64_t)p[3]<<16 |
               (uint64_t)p[4]<<24;
        negstart = (uint64_t)1<<31;
        negmax = UINT32_MAX;
        if (size) *size = LP_ENCODING_32BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_64BIT_INT(p[0])) {
        uval = (uint64_t)p[1] |
               (uint64_t)p[2]<<8 |
               (uint64_t)p[3]<<16 |
               (uint64_t)p[4]<<24 |
               (uint64_t)p[5]<<32 |
               (uint64_t)p[6]<<40 |
               (uint64_t)p[7]<<48 |
               (uint64_t)p[8]<<56;
        negstart = (uint64_t)1<<63;
        negmax = UINT64_MAX;
        if (size) *size = LP_ENCODING_64BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_12BIT_STR(p[0])) {
        *count = LP_ENCODING_12BIT_STR_LEN(p);
        if (size) *size = 2 + *count + lpEncodeBacklen(NULL, *count + 2);
        return p+2;
    } else if (LP_ENCODING_IS_32BIT_STR(p[0])) {
        *count = LP_ENCODING_32BIT_STR_LEN(p);
        if (size) *size = 5 + *count + lpEncodeBacklen(NULL, *count + 5);
        return p+5;
    } else {
        uval = 12345678900000000ULL + p[0];
        negstart = UINT64_MAX;
        negmax = 0;
    }

    /* We reach this code path only for integer encodings.
     * Convert the unsigned value to the signed one using two's complement
     * rule. */
    if (uval >= negstart) {
        /* This three steps conversion should avoid undefined behaviors
         * in the unsigned -> signed conversion. */
        uval = negmax-uval;
        val = uval;
        val = -val-1;
    } else {
        val = uval;
    }

    /* Return the string representation of the integer or the value itself
     * depending on intbuf being NULL or not. */
    if (intbuf) {
        *count = snprintf((char*)intbuf,LP_INTBUF_SIZE,"%lld",(long long)val);
        return intbuf;
    } else {
        *count = val;
        return NULL;
    }
}

unsigned char *lpGet(unsigned char *p, int64_t *count, unsigned char *intbuf) {
    return lpGetWithSize(p, count, intbuf, NULL);
}

/* Find pointer to the entry equal to the specified entry. Skip 'skip' entries
 * between every comparison. Returns NULL when the field could not be found. */
unsigned char *lpFind(unsigned char *lp, unsigned char *p, unsigned char *s, 
                      uint32_t slen, unsigned int skip) {
    int skipcnt = 0;
    unsigned char vencoding = 0;
    unsigned char *value;
    int64_t ll, vll;
    uint64_t entry_size = 123456789; /* initialized to avoid warning. */
    uint32_t lp_bytes = lpBytes(lp);

    assert(p);
    while (p) {
        if (skipcnt == 0) {
            value = lpGetWithSize(p, &ll, NULL, &entry_size);
            if (value) {
                if (slen == ll && memcmp(value, s, slen) == 0) {
                    return p;
                }
            } else {
                /* Find out if the searched field can be encoded. Note that
                 * we do it only the first time, once done vencoding is set
                 * to non-zero and vll is set to the integer value. */
                if (vencoding == 0) {
                    /* If the entry can be encoded as integer we set it to
                     * 1, else set it to UCHAR_MAX, so that we don't retry
                     * again the next time. */
                    if (slen >= 32 || slen == 0 || !lpStringToInt64((const char*)s, slen, &vll)) {
                        vencoding = UCHAR_MAX;
                    } else {
                        vencoding = 1;
                    }
                }

                /* Compare current entry with specified entry, do it only
                 * if vencoding != UCHAR_MAX because if there is no encoding
                 * possible for the field it can't be a valid integer. */
                if (vencoding != UCHAR_MAX && ll == vll) {
                    return p;
                }
            }

            /* Reset skip count */
            skipcnt = skip;
            p += entry_size;
        } else {
            /* Skip entry */
            skipcnt--;

            /* Move to next entry, avoid use `lpNext` due to `ASSERT_INTEGRITY` in
            * `lpNext` will call `lpBytes`, will cause performance degradation */
            p = lpSkip(p);
        }

        assert(p >= lp + LP_HDR_SIZE && p < lp + lp_bytes);
        if (p[0] == LP_EOF) break;
    }

    return NULL;
}

/* Insert, delete or replace the specified element 'ele' of length 'len' at
 * the specified position 'p', with 'p' being a listpack element pointer
 * obtained with lpFirst(), lpLast(), lpNext(), lpPrev() or lpSeek().
 *
 * The element is inserted before, after, or replaces the element pointed
 * by 'p' depending on the 'where' argument, that can be LP_BEFORE, LP_AFTER
 * or LP_REPLACE.
 *
 * If 'ele' is set to NULL, the function removes the element pointed by 'p'
 * instead of inserting one.
 *
 * Returns NULL on out of memory or when the listpack total length would exceed
 * the max allowed size of 2^32-1, otherwise the new pointer to the listpack
 * holding the new element is returned (and the old pointer passed is no longer
 * considered valid)
 *
 * If 'newp' is not NULL, at the end of a successful call '*newp' will be set
 * to the address of the element just added, so that it will be possible to
 * continue an interaction with lpNext() and lpPrev().
 *
 * For deletion operations ('ele' set to NULL) 'newp' is set to the next
 * element, on the right of the deleted one, or to NULL if the deleted element
 * was the last one. */
unsigned char *lpInsert(unsigned char *lp, unsigned char *ele, uint32_t size, unsigned char *p, int where, unsigned char **newp) {
    unsigned char intenc[LP_MAX_INT_ENCODING_LEN];
    unsigned char backlen[LP_MAX_BACKLEN_SIZE];

    uint64_t enclen; /* The length of the encoded element. */

    /* An element pointer set to NULL means deletion, which is conceptually
     * replacing the element with a zero-length element. So whatever we
     * get passed as 'where', set it to LP_REPLACE. */
    if (ele == NULL) where = LP_REPLACE;

    /* If we need to insert after the current element, we just jump to the
     * next element (that could be the EOF one) and handle the case of
     * inserting before. So the function will actually deal with just two
     * cases: LP_BEFORE and LP_REPLACE. */
    if (where == LP_AFTER) {
        p = lpSkip(p);
        where = LP_BEFORE;
        ASSERT_INTEGRITY(lp, p);
    }

    /* Store the offset of the element 'p', so that we can obtain its
     * address again after a reallocation. */
    unsigned long poff = p-lp;

    /* Calling lpEncodeGetType() results into the encoded version of the
     * element to be stored into 'intenc' in case it is representable as
     * an integer: in that case, the function returns LP_ENCODING_INT.
     * Otherwise if LP_ENCODING_STR is returned, we'll have to call
     * lpEncodeString() to actually write the encoded string on place later.
     *
     * Whatever the returned encoding is, 'enclen' is populated with the
     * length of the encoded element. */
    int enctype;
    if (ele) {
        enctype = lpEncodeGetType(ele,size,intenc,&enclen);
    } else {
        enctype = -1;
        enclen = 0;
    }

    /* We need to also encode the backward-parsable length of the element
     * and append it to the end: this allows to traverse the listpack from
     * the end to the start. */
    unsigned long backlen_size = ele ? lpEncodeBacklen(backlen,enclen) : 0;
    uint64_t old_listpack_bytes = lpGetTotalBytes(lp);
    uint32_t replaced_len  = 0;
    if (where == LP_REPLACE) {
        replaced_len = lpCurrentEncodedSizeUnsafe(p);
        replaced_len += lpEncodeBacklen(NULL,replaced_len);
        ASSERT_INTEGRITY_LEN(lp, p, replaced_len);
    }

    uint64_t new_listpack_bytes = old_listpack_bytes + enclen + backlen_size
                                  - replaced_len;
    if (new_listpack_bytes > UINT32_MAX) return NULL;

    /* We now need to reallocate in order to make space or shrink the
     * allocation (in case 'when' value is LP_REPLACE and the new element is
     * smaller). However we do that before memmoving the memory to
     * make room for the new element if the final allocation will get
     * larger, or we do it after if the final allocation will get smaller. */

    unsigned char *dst = lp + poff; /* May be updated after reallocation. */

    /* Realloc before: we need more room. */
    if (new_listpack_bytes > old_listpack_bytes &&
        new_listpack_bytes > lp_malloc_size(lp)) {
        if ((lp = lp_realloc(lp,new_listpack_bytes)) == NULL) return NULL;
        dst = lp + poff;
    }

    /* Setup the listpack relocating the elements to make the exact room
     * we need to store the new one. */
    if (where == LP_BEFORE) {
        memmove(dst+enclen+backlen_size,dst,old_listpack_bytes-poff);
    } else { /* LP_REPLACE. */
        long lendiff = (enclen+backlen_size)-replaced_len;
        memmove(dst+replaced_len+lendiff,
                dst+replaced_len,
                old_listpack_bytes-poff-replaced_len);
    }

    /* Realloc after: we need to free space. */
    if (new_listpack_bytes < old_listpack_bytes) {
        if ((lp = lp_realloc(lp,new_listpack_bytes)) == NULL) return NULL;
        dst = lp + poff;
    }

    /* Store the entry. */
    if (newp) {
        *newp = dst;
        /* In case of deletion, set 'newp' to NULL if the next element is
         * the EOF element. */
        if (!ele && dst[0] == LP_EOF) *newp = NULL;
    }
    if (ele) {
        if (enctype == LP_ENCODING_INT) {
            memcpy(dst,intenc,enclen);
        } else {
            lpEncodeString(dst,ele,size);
        }
        dst += enclen;
        memcpy(dst,backlen,backlen_size);
        dst += backlen_size;
    }

    /* Update header. */
    if (where != LP_REPLACE || ele == NULL) {
        uint32_t num_elements = lpGetNumElements(lp);
        if (num_elements != LP_HDR_NUMELE_UNKNOWN) {
            if (ele)
                lpSetNumElements(lp,num_elements+1);
            else
                lpSetNumElements(lp,num_elements-1);
        }
    }
    lpSetTotalBytes(lp,new_listpack_bytes);

#if 0
    /* This code path is normally disabled: what it does is to force listpack
     * to return *always* a new pointer after performing some modification to
     * the listpack, even if the previous allocation was enough. This is useful
     * in order to spot bugs in code using listpacks: by doing so we can find
     * if the caller forgets to set the new pointer where the listpack reference
     * is stored, after an update. */
    unsigned char *oldlp = lp;
    lp = lp_malloc(new_listpack_bytes);
    memcpy(lp,oldlp,new_listpack_bytes);
    if (newp) {
        unsigned long offset = (*newp)-oldlp;
        *newp = lp + offset;
    }
    /* Make sure the old allocation contains garbage. */
    memset(oldlp,'A',new_listpack_bytes);
    lp_free(oldlp);
#endif

    return lp;
}

/* Append the specified element 's' of length 'slen' at the head of the listpack. */
unsigned char *lpPrepend(unsigned char *lp, unsigned char *s, uint32_t slen) {
    unsigned char *p = lpFirst(lp);
    if (!p) return lpAppend(lp, s, slen);
    return lpInsert(lp, s, slen, p, LP_BEFORE, NULL);
}

/* Append the specified element 'ele' of length 'len' at the end of the
 * listpack. It is implemented in terms of lpInsert(), so the return value is
 * the same as lpInsert(). */
unsigned char *lpAppend(unsigned char *lp, unsigned char *ele, uint32_t size) {
    uint64_t listpack_bytes = lpGetTotalBytes(lp);
    unsigned char *eofptr = lp + listpack_bytes - 1;
    return lpInsert(lp,ele,size,eofptr,LP_BEFORE,NULL);
}

/* Remove the element pointed by 'p'. */
unsigned char *lpReplace(unsigned char *lp, unsigned char *p, unsigned char *s, uint32_t slen) {
    return lpInsert(lp, s, slen, p, LP_REPLACE, NULL);
}

/* Remove the element pointed by 'p', and return the resulting listpack.
 * If 'newp' is not NULL, the next element pointer (to the right of the
 * deleted one) is returned by reference. If the deleted element was the
 * last one, '*newp' is set to NULL. */
unsigned char *lpDelete(unsigned char *lp, unsigned char *p, unsigned char **newp) {
    return lpInsert(lp,NULL,0,p,LP_REPLACE,newp);
}

/* Return the total number of bytes the listpack is composed of. */
size_t lpBytes(unsigned char *lp) {
    return lpGetTotalBytes(lp);
}

/* Seek the specified element and returns the pointer to the seeked element.
 * Positive indexes specify the zero-based element to seek from the head to
 * the tail, negative indexes specify elements starting from the tail, where
 * -1 means the last element, -2 the penultimate and so forth. If the index
 * is out of range, NULL is returned. */
unsigned char *lpSeek(unsigned char *lp, long index) {
    int forward = 1; /* Seek forward by default. */

    /* We want to seek from left to right or the other way around
     * depending on the listpack length and the element position.
     * However if the listpack length cannot be obtained in constant time,
     * we always seek from left to right. */
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN) {
        if (index < 0) index = (long)numele+index;
        if (index < 0) return NULL; /* Index still < 0 means out of range. */
        if (index >= (long)numele) return NULL; /* Out of range the other side. */
        /* We want to scan right-to-left if the element we are looking for
         * is past the half of the listpack. */
        if (index > (long)numele/2) {
            forward = 0;
            /* Right to left scanning always expects a negative index. Convert
             * our index to negative form. */
            index -= numele;
        }
    } else {
        /* If the listpack length is unspecified, for negative indexes we
         * want to always scan right-to-left. */
        if (index < 0) forward = 0;
    }

    /* Forward and backward scanning is trivially based on lpNext()/lpPrev(). */
    if (forward) {
        unsigned char *ele = lpFirst(lp);
        while (index > 0 && ele) {
            ele = lpNext(lp,ele);
            index--;
        }
        return ele;
    } else {
        unsigned char *ele = lpLast(lp);
        while (index < -1 && ele) {
            ele = lpPrev(lp,ele);
            index++;
        }
        return ele;
    }
}

/* Validate the integrity of a single listpack entry and move to the next one.
 * The input argument 'pp' is a reference to the current record and is advanced on exit.
 * Returns 1 if valid, 0 if invalid. */
int lpValidateNext(unsigned char *lp, unsigned char **pp, size_t lpbytes) {
#define OUT_OF_RANGE(p) ( \
        (p) < lp + LP_HDR_SIZE || \
        (p) > lp + lpbytes - 1)
    unsigned char *p = *pp;
    if (!p)
        return 0;

    if (*p == LP_EOF) {
        *pp = NULL;
        return 1;
    }

    /* check that we can read the encoded size */
    uint32_t lenbytes = lpCurrentEncodedSizeBytes(p);
    if (!lenbytes)
        return 0;

    /* make sure the encoded entry length doesn't reach outside the edge of the listpack */
    if (OUT_OF_RANGE(p + lenbytes))
        return 0;

    /* get the entry length and encoded backlen. */
    unsigned long entrylen = lpCurrentEncodedSizeUnsafe(p);
    unsigned long encodedBacklen = lpEncodeBacklen(NULL,entrylen);
    entrylen += encodedBacklen;

    /* make sure the entry doesn't reach outside the edge of the listpack */
    if (OUT_OF_RANGE(p + entrylen))
        return 0;

    /* move to the next entry */
    p += entrylen;

    /* make sure the encoded length at the end patches the one at the beginning. */
    uint64_t prevlen = lpDecodeBacklen(p-1);
    if (prevlen + encodedBacklen != entrylen)
        return 0;

    *pp = p;
    return 1;
#undef OUT_OF_RANGE
}

/* Validate the integrity of the data structure.
 * when `deep` is 0, only the integrity of the header is validated.
 * when `deep` is 1, we scan all the entries one by one. */
int lpValidateIntegrity(unsigned char *lp, size_t size, int deep, 
                        listpackValidateEntryCB entry_cb, void *cb_userdata) {
    /* Check that we can actually read the header. (and EOF) */
    if (size < LP_HDR_SIZE + 1)
        return 0;

    /* Check that the encoded size in the header must match the allocated size. */
    size_t bytes = lpGetTotalBytes(lp);
    if (bytes != size)
        return 0;

    /* The last byte must be the terminator. */
    if (lp[size-1] != LP_EOF)
        return 0;

    if (!deep)
        return 1;

    /* Validate the individual entries. */
    uint32_t count = 0;
    unsigned char *p = lpFirst(lp);
    while(p && p[0] != LP_EOF) {
        /* Optionally let the caller validate the entry too. */
        if (entry_cb && !entry_cb(p, cb_userdata))
            return 0;

        /* Move to the next entry */
        if (!lpValidateNext(lp, &p, bytes))
            return 0;
        count++;
    }

    /* Check that the count in the header is correct */
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN && numele != count)
        return 0;

    return 1;
}

unsigned int lpCompare(unsigned char *p, unsigned char *s, uint32_t slen) {
    unsigned char buf[LP_INTBUF_SIZE];
    unsigned char *value;
    int64_t sz;

    if (p[0] == LP_EOF) return 0;
    value = lpGet(p, &sz, buf);
    return (slen == sz) && memcmp(value,s,slen) == 0;
}

/* uint compare for qsort */
static int uintCompare(const void *a, const void *b) {
    return (*(unsigned int *) a - *(unsigned int *) b);
}

/* Helper method to store a string into from val or lval into dest */
static inline void lpSaveValue(unsigned char *val, unsigned int len, int64_t lval, listpackEntry *dest) {
    dest->sval = val;
    dest->slen = len;
    dest->lval = lval;
}

void lpRandomPair(unsigned char *lp, unsigned long total_count, listpackEntry *key, listpackEntry *val) {
    unsigned char *p;
    int64_t vlen;

    /* Avoid div by zero on corrupt listpack */
    assert(total_count);

    /* Generate even numbers, because listpack saved K-V pair */
    int r = (rand() % total_count) * 2;
    p = lpSeek(lp, r);
    assert(p);
    key->sval = lpGet(p, &vlen, NULL);
    if (key->sval) {
        key->slen = vlen;
    } else {
        key->lval = vlen;
    }

    if (!val)
        return;
    p = lpNext(lp, p);
    assert(p);
    val->sval = lpGet(p, &vlen, NULL);
    if (val->sval) {
        val->slen = vlen;
    } else {
        val->lval = vlen;
    }
}

void lpRandomPairs(unsigned char *lp, unsigned int count, listpackEntry *keys, listpackEntry *vals) {
    unsigned char *p, *key, *value;
    unsigned int klen = 0, vlen = 0;
    int64_t klval = 0, vlval = 0, ele_len = 0;

    /* Notice: the index member must be first due to the use in uintCompare */
    typedef struct {
        unsigned int index;
        unsigned int order;
    } rand_pick;
    rand_pick *picks = zmalloc(sizeof(rand_pick)*count);
    unsigned int total_size = lpLength(lp)/2;

    /* Avoid div by zero on corrupt listpack */
    assert(total_size);

    /* create a pool of random indexes (some may be duplicate). */
    for (unsigned int i = 0; i < count; i++) {
        picks[i].index = (rand() % total_size) * 2; /* Generate even indexes */
        /* keep track of the order we picked them */
        picks[i].order = i;
    }

    /* sort by indexes. */
    qsort(picks, count, sizeof(rand_pick), uintCompare);

    /* fetch the elements form the listpack into a output array respecting the original order. */
    unsigned int lpindex = picks[0].index, pickindex = 0;
    p = lpSeek(lp, lpindex);
    while (p && pickindex < count) {
        key = lpGet(p, &ele_len, NULL);
        if (key) {
            klen = ele_len;
        } else {
            klval = ele_len;
        }
        p = lpNext(lp, p);
        assert(p);
        value = lpGet(p, &ele_len, NULL);
        if (value) {
            vlen = ele_len;
        } else {
            vlval = ele_len;
        }
        while (pickindex < count && lpindex == picks[pickindex].index) {
            int storeorder = picks[pickindex].order;
            lpSaveValue(key, klen, klval, &keys[storeorder]);
            if (vals)
                lpSaveValue(value, vlen, vlval, &vals[storeorder]);
             pickindex++;
        }
        lpindex += 2;
        p = lpNext(lp, p);
    }

    zfree(picks);
}

unsigned int lpRandomPairsUnique(unsigned char *lp, unsigned int count, listpackEntry *keys, listpackEntry *vals) {
    unsigned char *p, *key;
    unsigned int klen = 0;
    int64_t klval = 0, ele_len = 0;
    unsigned int total_size = lpLength(lp)/2;
    unsigned int index = 0;
    if (count > total_size)
        count = total_size;

    /* To only iterate once, every time we try to pick a member, the probability
     * we pick it is the quotient of the count left we want to pick and the
     * count still we haven't visited in the dict, this way, we could make every
     * member be equally picked.*/
    p = lpFirst(lp);
    unsigned int picked = 0, remaining = count;
    while (picked < count && p) {
        double randomDouble = ((double)rand()) / RAND_MAX;
        double threshold = ((double)remaining) / (total_size - index);
        if (randomDouble <= threshold) {
            key = lpGet(p, &ele_len, NULL);
            if (key) {
                klen = ele_len;
            } else {
                klval = ele_len;
            }
            lpSaveValue(key, klen, klval, &keys[picked]);
            p = lpNext(lp, p);
            assert(p);
            if (vals) {
                key = lpGet(p, &ele_len, NULL);
                if (key) {
                    klen = ele_len;
                } else {
                    klval = ele_len;
                }
                lpSaveValue(key, klen, klval, &vals[picked]);
            }
            remaining--;
            picked++;
        } else {
            p = lpNext(lp, p);
            assert(p);
        }
        p = lpNext(lp, p);
        index++;
    }
    return picked;
}

#ifdef REDIS_TEST

#include <sys/time.h>
#include "adlist.h"
#include "sds.h"

#define UNUSED(x) (void)(x)
#define TEST(name) printf("test — %s\n", name);

char *mixlist[] = {"hello", "foo", "quux", "1024"};
char *intlist[] = {"4294967296", "-100", "100", "128000", 
                   "non integer", "much much longer non integer"};

static unsigned char *createList() {
    unsigned char *lp = lpNew(0);
    lp = lpAppend(lp, (unsigned char*)mixlist[1], strlen(mixlist[1]));
    lp = lpAppend(lp, (unsigned char*)mixlist[2], strlen(mixlist[2]));
    lp = lpPrepend(lp, (unsigned char*)mixlist[0], strlen(mixlist[0]));
    lp = lpAppend(lp, (unsigned char*)mixlist[3], strlen(mixlist[3]));
    return lp;
}

static unsigned char *createIntList() {
    unsigned char *lp = lpNew(0);
    lp = lpAppend(lp, (unsigned char*)intlist[2], strlen(intlist[2]));
    lp = lpAppend(lp, (unsigned char*)intlist[3], strlen(intlist[3]));
    lp = lpPrepend(lp, (unsigned char*)intlist[1], strlen(intlist[1]));
    lp = lpPrepend(lp, (unsigned char*)intlist[0], strlen(intlist[0]));
    lp = lpAppend(lp, (unsigned char*)intlist[4], strlen(intlist[4]));
    lp = lpAppend(lp, (unsigned char*)intlist[5], strlen(intlist[5]));
    return lp;
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

static void stress(int pos, int num, int maxsize, int dnum) {
    int i, j, k;
    unsigned char *lp;
    char posstr[2][5] = { "HEAD", "TAIL" };
    long long start;
    for (i = 0; i < maxsize; i+=dnum) {
        lp = lpNew(0);
        for (j = 0; j < i; j++) {
            lp = lpAppend(lp, (unsigned char*)"quux", 4);
        }

        /* Do num times a push+pop from pos */
        start = usec();
        for (k = 0; k < num; k++) {
            if (pos == 0) {
                lp = lpPrepend(lp, (unsigned char*)"quux", 4);
            } else {
                lp = lpAppend(lp, (unsigned char*)"quux", 4);

            }
            lp = lpDelete(lp, lpFirst(lp), NULL);
        }
        printf("List size: %8d, bytes: %8zu, %dx push+pop (%s): %6lld usec\n",
               i, lpBytes(lp), num, posstr[pos], usec()-start);
        lpFree(lp);
    }
}

static unsigned char *pop(unsigned char *lp, int where) {
    unsigned char *p, *vstr;
    int64_t vlen;

    p = lpSeek(lp, where == 0 ? 0 : -1);
    vstr = lpGet(p, &vlen, NULL);
    if (where == 0)
        printf("Pop head: ");
    else
        printf("Pop tail: ");

    if (vstr) {
        if (vlen && fwrite(vstr, vlen, 1, stdout) == 0) perror("fwrite");
    } else {
        printf("%lld", (long long)vlen);
    }

    printf("\n");
    return lpDelete(lp, p, &p);
}

static int randstring(char *target, unsigned int min, unsigned int max) {
    int p = 0;
    int len = min+rand()%(max-min+1);
    int minval, maxval;
    switch(rand() % 3) {
    case 0:
        minval = 0;
        maxval = 255;
    break;
    case 1:
        minval = 48;
        maxval = 122;
    break;
    case 2:
        minval = 48;
        maxval = 52;
    break;
    default:
        assert(NULL);
    }

    while(p < len)
        target[p++] = minval+rand()%(maxval-minval+1);
    return len;
}

static void verifyEntry(unsigned char *p, unsigned char *s, size_t slen) {
    assert(lpCompare(p, s, slen));
}

static int lpValidation(unsigned char *p, void *userdata) {
    UNUSED(p);

    int ret;
    long *count = userdata;
    ret = lpCompare(p, (unsigned char *)mixlist[*count], strlen(mixlist[*count]));
    (*count)++;
    return ret;
}

int listpackTest(int argc, char *argv[], int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    int i;
    unsigned char *lp, *p, *vstr;
    int64_t vlen;
    unsigned char intbuf[LP_INTBUF_SIZE];

    TEST("Create int list") {
        lp = createIntList();
        assert(lpLength(lp) == 6);
        lpFree(lp);
    }

    TEST("Create list") {
        lp = createList();
        assert(lpLength(lp) == 4);
        lpFree(lp);
    }

    TEST("Get element at index") {
        lp = createList();
        verifyEntry(lpSeek(lp, 0), (unsigned char*)"hello", 5);
        verifyEntry(lpSeek(lp, 3), (unsigned char*)"1024", 4);
        verifyEntry(lpSeek(lp, -1), (unsigned char*)"1024", 4);
        verifyEntry(lpSeek(lp, -4), (unsigned char*)"hello", 5);
        assert(lpSeek(lp, 4) == NULL);
        assert(lpSeek(lp, -5) == NULL);
        lpFree(lp);
    }
    
    TEST("Pop list") {
        lp = createList();
        lp = pop(lp, 1);
        lp = pop(lp, 0);
        lp = pop(lp, 1);
        lp = pop(lp, 1);
        lpFree(lp);
    }

    TEST("Get element at index") {
        lp = createList();
        verifyEntry(lpSeek(lp, 0), (unsigned char*)"hello", 5);
        verifyEntry(lpSeek(lp, 3), (unsigned char*)"1024", 4);
        verifyEntry(lpSeek(lp, -1), (unsigned char*)"1024", 4);
        verifyEntry(lpSeek(lp, -4), (unsigned char*)"hello", 5);
        assert(lpSeek(lp, 4) == NULL);
        assert(lpSeek(lp, -5) == NULL);
        lpFree(lp);
    }

    TEST("Iterate list from 0 to end") {
        lp = createList();
        p = lpFirst(lp);
        i = 0;
        while (p) {
            verifyEntry(p, (unsigned char*)mixlist[i], strlen(mixlist[i]));
            p = lpNext(lp, p);
            i++;
        }
        lpFree(lp);
    }
    
    TEST("Iterate list from 1 to end") {
        lp = createList();
        i = 1;
        p = lpSeek(lp, i);
        while (p) {
            verifyEntry(p, (unsigned char*)mixlist[i], strlen(mixlist[i]));
            p = lpNext(lp, p);
            i++;
        }
        lpFree(lp);
    }
    
    TEST("Iterate list from 2 to end") {
        lp = createList();
        i = 2;
        p = lpSeek(lp, i);
        while (p) {
            verifyEntry(p, (unsigned char*)mixlist[i], strlen(mixlist[i]));
            p = lpNext(lp, p);
            i++;
        }
        lpFree(lp);
    }
    
    TEST("Iterate from back to front") {
        lp = createList();
        p = lpLast(lp);
        i = 3;
        while (p) {
            verifyEntry(p, (unsigned char*)mixlist[i], strlen(mixlist[i]));
            p = lpPrev(lp, p);
            i--;
        }
        lpFree(lp);
    }
    
    TEST("Iterate from back to front, deleting all items") {
        lp = createList();
        p = lpLast(lp);
        i = 3;
        while ((p = lpLast(lp))) {
            verifyEntry(p, (unsigned char*)mixlist[i], strlen(mixlist[i]));
            lp = lpDelete(lp, p, &p);
            assert(p == NULL);
            i--;
        }
        lpFree(lp);
    }

    TEST("Delete foo while iterating") {
        lp = createList();
        p = lpFirst(lp);
        while (p) {
            if (lpCompare(p, (unsigned char*)"foo", 3)) {
                lp = lpDelete(lp, p, &p);
            } else {
                p = lpNext(lp, p);
            }
        }
        lpFree(lp);
    }

    TEST("Replace with same size") {
        lp = createList(); /* "hello", "foo", "quux", "1024" */
        unsigned char *orig_lp = lp;
        p = lpSeek(lp, 0);
        lp = lpReplace(lp, p, (unsigned char*)"zoink", 5);
        p = lpSeek(lp, 3);
        lp = lpReplace(lp, p, (unsigned char*)"y", 1);
        p = lpSeek(lp, 1);
        lp = lpReplace(lp, p, (unsigned char*)"65536", 5);
        p = lpSeek(lp, 0);
        assert(!memcmp((char*)p,
                       "\x85zoink\x06"
                       "\xf2\x00\x00\x01\x04" /* 65536 as int24 */
                       "\x84quux\05" "\x81y\x02" "\xff",
                       22));
        assert(lp == orig_lp); /* no reallocations have happened */
        lpFree(lp);
    }

    TEST("Replace with different size") {
        lp = createList(); /* "hello", "foo", "quux", "1024" */
        p = lpSeek(lp, 1);
        lp = lpReplace(lp, p, (unsigned char*)"squirrel", 8);
        p = lpSeek(lp, 0);
        assert(!strncmp((char*)p,
                        "\x85hello\x06" "\x88squirrel\x09" "\x84quux\x05"
                        "\xc4\x00\x02" "\xff",
                        27));
        lpFree(lp);
    }

    TEST("Regression test for >255 byte strings") {
        char v1[257] = {0}, v2[257] = {0};
        memset(v1,'x',256);
        memset(v2,'y',256);
        lp = lpNew(0);
        lp = lpAppend(lp, (unsigned char*)v1 ,strlen(v1));
        lp = lpAppend(lp, (unsigned char*)v2 ,strlen(v2));

        /* Pop values again and compare their value. */
        p = lpFirst(lp);
        vstr = lpGet(p, &vlen, NULL);
        assert(strncmp(v1, (char*)vstr, vlen) == 0);
        p = lpSeek(lp, 1);
        vstr = lpGet(p, &vlen, NULL);
        assert(strncmp(v2, (char*)vstr, vlen) == 0);
        lpFree(lp);
    }

    TEST("Create long list and check indices") {
        lp = lpNew(0);
        char buf[32];
        int i,len;
        for (i = 0; i < 1000; i++) {
            len = sprintf(buf, "%d", i);
            lp = lpAppend(lp, (unsigned char*)buf, len);
        }
        for (i = 0; i < 1000; i++) {
            p = lpSeek(lp, i);
            vstr = lpGet(p, &vlen, NULL);
            assert(i == vlen);

            p = lpSeek(lp, -i-1);
            vstr = lpGet(p, &vlen, NULL);
            assert(999-i == vlen);
        }
        lpFree(lp);
    }

    TEST("Compare strings with listpack entries") {
        lp = createList();
        p = lpSeek(lp,0);
        assert(lpCompare(p,(unsigned char*)"hello",5));
        assert(!lpCompare(p,(unsigned char*)"hella",5));

        p = lpSeek(lp,3);
        assert(lpCompare(p,(unsigned char*)"1024",4));
        assert(!lpCompare(p,(unsigned char*)"1025",4));
        lpFree(lp);
    }

    TEST("Random pair with one element") {
        listpackEntry key, val;
        unsigned char *lp = lpNew(0);
        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lpRandomPair(lp, 1, &key, &val);
        assert(memcmp(key.sval, "abc", key.slen) == 0);
        assert(val.lval == 123);
        lpFree(lp);
    }

    TEST("Random pair with many elements") {
        listpackEntry key, val;
        unsigned char *lp = lpNew(0);
        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lp = lpAppend(lp, (unsigned char*)"456", 3);
        lp = lpAppend(lp, (unsigned char*)"def", 3);
        lpRandomPair(lp, 2, &key, &val);
        if (key.sval) {
            assert(!memcmp(key.sval, "abc", key.slen));
            assert(key.slen == 3);
            assert(val.lval == 123);
        }
        if (!key.sval) {
            assert(key.lval == 456);
            assert(!memcmp(val.sval, "def", val.slen));
        }
        lpFree(lp);
    }

    TEST("Random pairs with one element") {
        int count = 5;
        unsigned char *lp = lpNew(0);
        listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
        listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lpRandomPairs(lp, count, keys, vals);
        assert(memcmp(keys[4].sval, "abc", keys[4].slen) == 0);
        assert(vals[4].lval == 123);
        zfree(keys);
        zfree(vals);
        lpFree(lp);
    }

    TEST("Random pairs with many elements") {
        int count = 5;
        lp = lpNew(0);
        listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
        listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lp = lpAppend(lp, (unsigned char*)"456", 3);
        lp = lpAppend(lp, (unsigned char*)"def", 3);
        lpRandomPairs(lp, count, keys, vals);
        for (int i = 0; i < count; i++) {
            if (keys[i].sval) {
                assert(!memcmp(keys[i].sval, "abc", keys[i].slen));
                assert(keys[i].slen == 3);
                assert(vals[i].lval == 123);
            }
            if (!keys[i].sval) {
                assert(keys[i].lval == 456);
                assert(!memcmp(vals[i].sval, "def", vals[i].slen));
            }
        }
        zfree(keys);
        zfree(vals);
        lpFree(lp);
    }

    TEST("Random pairs unique with one element") {
        unsigned picked;
        int count = 5;
        lp = lpNew(0);
        listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
        listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        picked = lpRandomPairsUnique(lp, count, keys, vals);
        assert(picked == 1);
        assert(memcmp(keys[0].sval, "abc", keys[0].slen) == 0);
        assert(vals[0].lval == 123);
        zfree(keys);
        zfree(vals);
        lpFree(lp);
    }

    TEST("Random pairs unique with many elements") {
        unsigned picked;
        int count = 5;
        lp = lpNew(0);
        listpackEntry *keys = zmalloc(sizeof(listpackEntry) * count);
        listpackEntry *vals = zmalloc(sizeof(listpackEntry) * count);

        lp = lpAppend(lp, (unsigned char*)"abc", 3);
        lp = lpAppend(lp, (unsigned char*)"123", 3);
        lp = lpAppend(lp, (unsigned char*)"456", 3);
        lp = lpAppend(lp, (unsigned char*)"def", 3);
        picked = lpRandomPairsUnique(lp, count, keys, vals);
        assert(picked == 2);
        for (int i = 0; i < 2; i++) {
            if (keys[i].sval) {
                assert(!memcmp(keys[i].sval, "abc", keys[i].slen));
                assert(keys[i].slen == 3);
                assert(vals[i].lval == 123);
            }
            if (!keys[i].sval) {
                assert(keys[i].lval == 456);
                assert(!memcmp(vals[i].sval, "def", vals[i].slen));
            }
        }
        zfree(keys);
        zfree(vals);
        lpFree(lp);
    }

    TEST("push various encodings") {
        lp = lpNew(0);

        /* integer encode */
        lp = lpAppend(lp, (unsigned char*)"127", 3);
        assert(LP_ENCODING_IS_7BIT_UINT(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)"4095", 4);
        assert(LP_ENCODING_IS_13BIT_INT(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)"32767", 5);
        assert(LP_ENCODING_IS_16BIT_INT(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)"8388607", 7);
        assert(LP_ENCODING_IS_24BIT_INT(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)"2147483647", 10);
        assert(LP_ENCODING_IS_32BIT_INT(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)"9223372036854775807", 19);
        assert(LP_ENCODING_IS_64BIT_INT(lpLast(lp)[0]));

        /* string encode */
        unsigned char *str = zmalloc(65535);
        memset(str, 0, 65535);
        lp = lpAppend(lp, (unsigned char*)str, 63);
        assert(LP_ENCODING_IS_6BIT_STR(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)str, 4095);
        assert(LP_ENCODING_IS_12BIT_STR(lpLast(lp)[0]));
        lp = lpAppend(lp, (unsigned char*)str, 65535);
        assert(LP_ENCODING_IS_32BIT_STR(lpLast(lp)[0]));
        zfree(str);
        lpFree(lp);
    }

    TEST("Test lpFind") {
        lp = createList();
        assert(lpFind(lp, lpFirst(lp), (unsigned char*)"abc", 3, 0) == NULL);
        verifyEntry(lpFind(lp, lpFirst(lp), (unsigned char*)"hello", 5, 0), (unsigned char*)"hello", 5);
        verifyEntry(lpFind(lp, lpFirst(lp), (unsigned char*)"1024", 4, 0), (unsigned char*)"1024", 4);
        lpFree(lp);
    }

    TEST("Test lpValidateIntegrity") {
        lp = createList();
        long count = 0;
        assert(lpValidateIntegrity(lp, lpBytes(lp), 1, lpValidation, &count) == 1);
        lpFree(lp);
    }

    TEST("Stress with random payloads of different encoding") {
        unsigned long long start = usec();
        int i,j,len,where;
        unsigned char *p;
        char buf[1024];
        int buflen;
        list *ref;
        listNode *refnode;

        int iteration = accurate ? 20000 : 20;
        for (i = 0; i < iteration; i++) {
            lp = lpNew(0);
            ref = listCreate();
            listSetFreeMethod(ref,(void (*)(void*))sdsfree);
            len = rand() % 256;

            /* Create lists */
            for (j = 0; j < len; j++) {
                where = (rand() & 1) ? 0 : 1;
                if (rand() % 2) {
                    buflen = randstring(buf,1,sizeof(buf)-1);
                } else {
                    switch(rand() % 3) {
                    case 0:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) >> 20);
                        break;
                    case 1:
                        buflen = sprintf(buf,"%lld",(0LL + rand()));
                        break;
                    case 2:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) << 20);
                        break;
                    default:
                        assert(NULL);
                    }
                }

                /* Add to listpack */
                if (where == 0) {
                    lp = lpPrepend(lp, (unsigned char*)buf, buflen);
                } else {
                    lp = lpAppend(lp, (unsigned char*)buf, buflen);
                }

                /* Add to reference list */
                if (where == 0) {
                    listAddNodeHead(ref,sdsnewlen(buf, buflen));
                } else if (where == 1) {
                    listAddNodeTail(ref,sdsnewlen(buf, buflen));
                } else {
                    assert(NULL);
                }
            }

            assert(listLength(ref) == lpLength(lp));
            for (j = 0; j < len; j++) {
                /* Naive way to get elements, but similar to the stresser
                 * executed from the Tcl test suite. */
                p = lpSeek(lp,j);
                refnode = listIndex(ref,j);

                vstr = lpGet(p, &vlen, intbuf);
                assert(memcmp(vstr,listNodeValue(refnode),vlen) == 0);
            }
            lpFree(lp);
            listRelease(ref);
        }
        printf("Done. usec=%lld\n\n", usec()-start);
    }

    TEST("Stress with variable listpack size") {
        unsigned long long start = usec();
        int maxsize = accurate ? 16384 : 16;
        stress(0,100000,maxsize,256);
        stress(1,100000,maxsize,256);
        printf("Done. usec=%lld\n\n", usec()-start);
    }

    /* Benchmarks */
    {
        int iteration = accurate ? 100000 : 100;
        lp = lpNew(0);
        TEST("Benchmark lpAppend") {
            unsigned long long start = usec();
            for (int i=0; i<iteration; i++) {
                char buf[4096] = "asdf";
                lp = lpAppend(lp, (unsigned char*)buf, 4);
                lp = lpAppend(lp, (unsigned char*)buf, 40);
                lp = lpAppend(lp, (unsigned char*)buf, 400);
                lp = lpAppend(lp, (unsigned char*)buf, 4000);
                lp = lpAppend(lp, (unsigned char*)"1", 1);
                lp = lpAppend(lp, (unsigned char*)"10", 2);
                lp = lpAppend(lp, (unsigned char*)"100", 3);
                lp = lpAppend(lp, (unsigned char*)"1000", 4);
                lp = lpAppend(lp, (unsigned char*)"10000", 5);
                lp = lpAppend(lp, (unsigned char*)"100000", 6);
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        TEST("Benchmark lpFind string") {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                unsigned char *fptr = lpFirst(lp);
                fptr = lpFind(lp, fptr, (unsigned char*)"nothing", 7, 1);
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        TEST("Benchmark lpFind number") {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                unsigned char *fptr = lpFirst(lp);
                fptr = lpFind(lp, fptr, (unsigned char*)"99999", 5, 1);
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        TEST("Benchmark lpSeek") {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                lpSeek(lp, 99999);
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        TEST("Benchmark lpValidateIntegrity") {
            unsigned long long start = usec();
            for (int i = 0; i < 2000; i++) {
                lpValidateIntegrity(lp, lpBytes(lp), 1, NULL, NULL);
            }
            printf("Done. usec=%lld\n", usec()-start);
        }

        lpFree(lp);
    }

    return 0;
}

#endif
