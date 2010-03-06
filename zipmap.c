/* String -> String Map data structure optimized for size.
 * This file implements a data structure mapping strings to other strings
 * implementing an O(n) lookup data structure designed to be very memory
 * efficient.
 *
 * The Redis Hash type uses this data structure for hashes composed of a small
 * number of elements, to switch to an hash table once a given number of
 * elements is reached.
 *
 * Given that many times Redis Hashes are used to represent objects composed
 * of few fields, this is a very big win in terms of used memory.
 *
 * --------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* Memory layout of a zipmap, for the map "foo" => "bar", "hello" => "world":
 *
 * <status><len>"foo"<len><free>"bar"<len>"hello"<len><free>"world"
 *
 * <status> is 1 byte status. Currently only 1 bit is used: if the least
 * significant bit is set, it means the zipmap needs to be defragmented.
 *
 * <len> is the length of the following string (key or value).
 * <len> lengths are encoded in a single value or in a 5 bytes value.
 * If the first byte value (as an unsigned 8 bit value) is between 0 and
 * 252, it's a single-byte length. If it is 253 then a four bytes unsigned
 * integer follows (in the host byte ordering). A value fo 255 is used to
 * signal the end of the hash. The special value 254 is used to mark
 * empty space that can be used to add new key/value pairs.
 *
 * <free> is the number of free unused bytes
 * after the string, resulting from modification of values associated to a
 * key (for instance if "foo" is set to "bar', and later "foo" will be se to
 * "hi", I'll have a free byte to use if the value will enlarge again later,
 * or even in order to add a key/value pair if it fits.
 *
 * <free> is always an unsigned 8 bit number, because if after an
 * update operation there are more than a few free bytes, they'll be converted
 * into empty space prefixed by the special value 254.
 *
 * The most compact representation of the above two elements hash is actually:
 *
 * "\x00\x03foo\x03\x00bar\x05hello\x05\x00world\xff"
 *
 * Empty space is marked using a 254 bytes + a <len> (coded as already
 * specified). The length includes the 254 bytes in the count and the
 * space taken by the <len> field. So for instance removing the "foo" key
 * from the zipmap above will lead to the following representation:
 *
 * "\x00\xfd\x10........\x05hello\x05\x00world\xff"
 *
 * Note that because empty space, keys, values, are all prefixed length
 * "objects", the lookup will take O(N) where N is the numeber of elements
 * in the zipmap and *not* the number of bytes needed to represent the zipmap.
 * This lowers the constant times considerably.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "zmalloc.h"

#define ZIPMAP_BIGLEN 253
#define ZIPMAP_EMPTY 254
#define ZIPMAP_END 255

#define ZIPMAP_STATUS_FRAGMENTED 1

/* The following defines the max value for the <free> field described in the
 * comments above, that is, the max number of trailing bytes in a value. */
#define ZIPMAP_VALUE_MAX_FREE 5

/* The following macro returns the number of bytes needed to encode the length
 * for the integer value _l, that is, 1 byte for lengths < ZIPMAP_BIGLEN and
 * 5 bytes for all the other lengths. */
#define ZIPMAP_LEN_BYTES(_l) (((_l) < ZIPMAP_BIGLEN) ? 1 : sizeof(unsigned int)+1)

/* Create a new empty zipmap. */
unsigned char *zipmapNew(void) {
    unsigned char *zm = zmalloc(2);

    zm[0] = 0; /* Status */
    zm[1] = ZIPMAP_END;
    return zm;
}

/* Decode the encoded length pointed by 'p' */
static unsigned int zipmapDecodeLength(unsigned char *p) {
    unsigned int len = *p;

    if (len < ZIPMAP_BIGLEN) return len;
    memcpy(&len,p,sizeof(unsigned int));
    return len;
}

/* Encode the length 'l' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. */
static unsigned int zipmapEncodeLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        return ZIPMAP_LEN_BYTES(len);
    } else {
        if (len < ZIPMAP_BIGLEN) {
            p[0] = len;
            return 1;
        } else {
            p[0] = ZIPMAP_BIGLEN;
            memcpy(p+1,&len,sizeof(len));
            return 1+sizeof(len);
        }
    }
}

/* Search for a matching key, returning a pointer to the entry inside the
 * zipmap. Returns NULL if the key is not found.
 *
 * If NULL is returned, and totlen is not NULL, it is set to the entire
 * size of the zimap, so that the calling function will be able to
 * reallocate the original zipmap to make room for more entries.
 *
 * If NULL is returned, and freeoff and freelen are not NULL, they are set
 * to the offset of the first empty space that can hold '*freelen' bytes
 * (freelen is an integer pointer used both to signal the required length
 * and to get the reply from the function). If there is not a suitable
 * free space block to hold the requested bytes, *freelen is set to 0. */
static unsigned char *zipmapLookupRaw(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned int *totlen, unsigned int *freeoff, unsigned int *freelen) {
    unsigned char *p = zm+1;
    unsigned int l;
    unsigned int reqfreelen = 0; /* initialized just to prevent warning */

    if (freelen) {
        reqfreelen = *freelen;
        *freelen = 0;
        assert(reqfreelen != 0);
    }
    while(*p != ZIPMAP_END) {
        if (*p == ZIPMAP_EMPTY) {
            l = zipmapDecodeLength(p+1);
            /* if the user want a free space report, and this space is
             * enough, and we did't already found a suitable space... */
            if (freelen && l >= reqfreelen && *freelen == 0) {
                *freelen = l;
                *freeoff = p-zm;
            }
            p += l;
            zm[0] |= ZIPMAP_STATUS_FRAGMENTED;
        } else {
            unsigned char free;

            /* Match or skip the key */
            l = zipmapDecodeLength(p);
            if (l == klen && !memcmp(p+1,key,l)) return p;
            p += zipmapEncodeLength(NULL,l) + l;
            /* Skip the value as well */
            l = zipmapDecodeLength(p);
            p += zipmapEncodeLength(NULL,l);
            free = p[0];
            p += l+1+free; /* +1 to skip the free byte */
        }
    }
    if (totlen != NULL) *totlen = (unsigned int)(p-zm)+1;
    return NULL;
}

static unsigned long zipmapRequiredLength(unsigned int klen, unsigned int vlen) {
    unsigned int l;

    l = klen+vlen+3;
    if (klen >= ZIPMAP_BIGLEN) l += 4;
    if (vlen >= ZIPMAP_BIGLEN) l += 4;
    return l;
}

/* Return the total amount used by a key (encoded length + payload) */
static unsigned int zipmapRawKeyLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);
    
    return zipmapEncodeLength(NULL,l) + l;
}

/* Return the total amount used by a value
 * (encoded length + single byte free count + payload) */
static unsigned int zipmapRawValueLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);
    unsigned int used;
    
    used = zipmapEncodeLength(NULL,l);
    used += p[used] + 1 + l;
    return used;
}

/* If 'p' points to a key, this function returns the total amount of
 * bytes used to store this entry (entry = key + associated value + trailing
 * free space if any). */
static unsigned int zipmapRawEntryLength(unsigned char *p) {
    unsigned int l = zipmapRawKeyLength(p);

    return l + zipmapRawValueLength(p+l);
}

/* Set key to value, creating the key if it does not already exist.
 * If 'update' is not NULL, *update is set to 1 if the key was
 * already preset, otherwise to 0. */
unsigned char *zipmapSet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char *val, unsigned int vlen, int *update) {
    unsigned int oldlen = 0, freeoff = 0, freelen;
    unsigned int reqlen = zipmapRequiredLength(klen,vlen);
    unsigned int empty, vempty;
    unsigned char *p;
   
    freelen = reqlen;
    if (update) *update = 0;
    p = zipmapLookupRaw(zm,key,klen,&oldlen,&freeoff,&freelen);
    if (p == NULL && freelen == 0) {
        /* Key not found, and not space for the new key. Enlarge */
        zm = zrealloc(zm,oldlen+reqlen);
        p = zm+oldlen-1;
        zm[oldlen+reqlen-1] = ZIPMAP_END;
        freelen = reqlen;
    } else if (p == NULL) {
        /* Key not found, but there is enough free space. */
        p = zm+freeoff;
        /* note: freelen is already set in this case */
    } else {
        unsigned char *b = p;

        /* Key found. Is there enough space for the new value? */
        /* Compute the total length: */
        if (update) *update = 1;
        freelen = zipmapRawKeyLength(b);
        b += freelen;
        freelen += zipmapRawValueLength(b);
        if (freelen < reqlen) {
            /* Mark this entry as free and recurse */
            p[0] = ZIPMAP_EMPTY;
            zipmapEncodeLength(p+1,freelen);
            zm[0] |= ZIPMAP_STATUS_FRAGMENTED;
            return zipmapSet(zm,key,klen,val,vlen,NULL);
        }
    }

    /* Ok we have a suitable block where to write the new key/value
     * entry. */
    empty = freelen-reqlen;
    /* If there is too much free space mark it as a free block instead
     * of adding it as trailing empty space for the value, as we want
     * zipmaps to be very space efficient. */
    if (empty > ZIPMAP_VALUE_MAX_FREE) {
        unsigned char *e;

        e = p+reqlen;
        e[0] = ZIPMAP_EMPTY;
        zipmapEncodeLength(e+1,empty);
        vempty = 0;
        zm[0] |= ZIPMAP_STATUS_FRAGMENTED;
    } else {
        vempty = empty;
    }

    /* Just write the key + value and we are done. */
    /* Key: */
    p += zipmapEncodeLength(p,klen);
    memcpy(p,key,klen);
    p += klen;
    /* Value: */
    p += zipmapEncodeLength(p,vlen);
    *p++ = vempty;
    memcpy(p,val,vlen);
    return zm;
}

/* Remove the specified key. If 'deleted' is not NULL the pointed integer is
 * set to 0 if the key was not found, to 1 if it was found and deleted. */
unsigned char *zipmapDel(unsigned char *zm, unsigned char *key, unsigned int klen, int *deleted) {
    unsigned char *p = zipmapLookupRaw(zm,key,klen,NULL,NULL,NULL);
    if (p) {
        unsigned int freelen = zipmapRawEntryLength(p);

        p[0] = ZIPMAP_EMPTY;
        zipmapEncodeLength(p+1,freelen);
        zm[0] |= ZIPMAP_STATUS_FRAGMENTED;
        if (deleted) *deleted = 1;
    } else {
        if (deleted) *deleted = 0;
    }
    return zm;
}

/* Call it before to iterate trought elements via zipmapNext() */
unsigned char *zipmapRewind(unsigned char *zm) {
    return zm+1;
}

/* This function is used to iterate through all the zipmap elements.
 * In the first call the first argument is the pointer to the zipmap + 1.
 * In the next calls what zipmapNext returns is used as first argument.
 * Example:
 *
 * unsigned char *i = zipmapRewind(my_zipmap);
 * while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
 *     printf("%d bytes key at $p\n", klen, key);
 *     printf("%d bytes value at $p\n", vlen, value);
 * }
 */
unsigned char *zipmapNext(unsigned char *zm, unsigned char **key, unsigned int *klen, unsigned char **value, unsigned int *vlen) {
    while(zm[0] == ZIPMAP_EMPTY)
        zm += zipmapDecodeLength(zm+1);
    if (zm[0] == ZIPMAP_END) return NULL;
    if (key) {
        *key = zm;
        *klen = zipmapDecodeLength(zm);
        *key += ZIPMAP_LEN_BYTES(*klen);
    }
    zm += zipmapRawKeyLength(zm);
    if (value) {
        *value = zm+1;
        *vlen = zipmapDecodeLength(zm);
        *value += ZIPMAP_LEN_BYTES(*vlen);
    }
    zm += zipmapRawValueLength(zm);
    return zm;
}

/* Search a key and retrieve the pointer and len of the associated value.
 * If the key is found the function returns 1, otherwise 0. */
int zipmapGet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char **value, unsigned int *vlen) {
    unsigned char *p;

    if ((p = zipmapLookupRaw(zm,key,klen,NULL,NULL,NULL)) == NULL) return 0;
    p += zipmapRawKeyLength(p);
    *vlen = zipmapDecodeLength(p);
    *value = p + ZIPMAP_LEN_BYTES(*vlen) + 1;
    return 1;
}

/* Return 1 if the key exists, otherwise 0 is returned. */
int zipmapExists(unsigned char *zm, unsigned char *key, unsigned int klen) {
    return zipmapLookupRaw(zm,key,klen,NULL,NULL,NULL) != NULL;
}

void zipmapRepr(unsigned char *p) {
    unsigned int l;

    printf("{status %u}",*p++);
    while(1) {
        if (p[0] == ZIPMAP_END) {
            printf("{end}");
            break;
        } else if (p[0] == ZIPMAP_EMPTY) {
            l = zipmapDecodeLength(p+1);
            printf("{%u empty block}", l);
            p += l;
        } else {
            unsigned char e;

            l = zipmapDecodeLength(p);
            printf("{key %u}",l);
            p += zipmapEncodeLength(NULL,l);
            fwrite(p,l,1,stdout);
            p += l;

            l = zipmapDecodeLength(p);
            printf("{value %u}",l);
            p += zipmapEncodeLength(NULL,l);
            e = *p++;
            fwrite(p,l,1,stdout);
            p += l+e;
            if (e) {
                printf("[");
                while(e--) printf(".");
                printf("]");
            }
        }
    }
    printf("\n");
}

#ifdef ZIPMAP_TEST_MAIN
int main(void) {
    unsigned char *zm;

    zm = zipmapNew();
    zm = zipmapSet(zm,(unsigned char*) "hello",5, (unsigned char*) "world!",6,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "bar",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "!",1,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "12345",5,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "new",3, (unsigned char*) "xx",2,NULL);
    zm = zipmapSet(zm,(unsigned char*) "noval",5, (unsigned char*) "",0,NULL);
    zipmapRepr(zm);
    zm = zipmapDel(zm,(unsigned char*) "new",3,NULL);
    zipmapRepr(zm);
    printf("\nPerform a direct lookup:\n");
    {
        unsigned char *value;
        unsigned int vlen;

        if (zipmapGet(zm,(unsigned char*) "foo",3,&value,&vlen)) {
            printf("  foo is associated to the %d bytes value: %.*s\n",
                vlen, vlen, value);
        }
    }
    printf("\nIterate trought elements:\n");
    {
        unsigned char *i = zipmapRewind(zm);
        unsigned char *key, *value;
        unsigned int klen, vlen;

        while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
            printf("  %d:%.*s => %d:%.*s\n", klen, klen, key, vlen, vlen, value);
        }
    }
    return 0;
}
#endif
