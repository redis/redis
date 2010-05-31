/* Memory layout of a ziplist, containing "foo", "bar", "quux":
 * <zlbytes><zllen><len>"foo"<len>"bar"<len>"quux"
 *
 * <zlbytes> is an unsigned integer to hold the number of bytes that
 * the ziplist occupies. This is stored to not have to traverse the ziplist
 * to know the new length when pushing.
 *
 * <zllen> is the number of items in the ziplist. When this value is
 * greater than 254, we need to traverse the entire list to know
 * how many items it holds.
 *
 * <len> is the number of bytes occupied by a single entry. When this
 * number is greater than 253, the length will occupy 5 bytes, where
 * the extra bytes contain an unsigned integer to hold the length.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include "zmalloc.h"
#include "sds.h"
#include "ziplist.h"

/* Important note: the ZIP_END value is used to depict the end of the
 * ziplist structure. When a pointer contains an entry, the first couple
 * of bytes contain the encoded length of the previous entry. This length
 * is encoded as ZIP_ENC_RAW length, so the first two bits will contain 00
 * and the byte will therefore never have a value of 255. */
#define ZIP_END 255
#define ZIP_BIGLEN 254

/* Entry encoding */
#define ZIP_ENC_RAW     0
#define ZIP_ENC_SHORT   1
#define ZIP_ENC_INT     2
#define ZIP_ENC_LLONG   3
#define ZIP_ENCODING(p) ((p)[0] >> 6)

/* Length encoding for raw entries */
#define ZIP_LEN_INLINE  0
#define ZIP_LEN_UINT16  1
#define ZIP_LEN_UINT32  2

/* Utility macros */
#define ZIPLIST_BYTES(zl) (*((unsigned int*)(zl)))
#define ZIPLIST_TAIL_OFFSET(zl) (*((unsigned int*)((zl)+sizeof(unsigned int))))
#define ZIPLIST_LENGTH(zl) (*((zl)+2*sizeof(unsigned int)))
#define ZIPLIST_HEADER_SIZE (2*sizeof(unsigned int)+1)
#define ZIPLIST_ENTRY_HEAD(zl) ((zl)+ZIPLIST_HEADER_SIZE)
#define ZIPLIST_ENTRY_TAIL(zl) ((zl)+ZIPLIST_TAIL_OFFSET(zl))
#define ZIPLIST_ENTRY_END(zl) ((zl)+ZIPLIST_BYTES(zl)-1)
#define ZIPLIST_INCR_LENGTH(zl,incr) { \
    if (ZIPLIST_LENGTH(zl) < ZIP_BIGLEN) ZIPLIST_LENGTH(zl)+=incr; }

typedef struct zlentry {
    unsigned int prevrawlensize, prevrawlen;
    unsigned int lensize, len;
    unsigned int headersize;
    unsigned char encoding;
    unsigned char *p;
} zlentry;

/* Return bytes needed to store integer encoded by 'encoding' */
static unsigned int zipEncodingSize(unsigned char encoding) {
    if (encoding == ZIP_ENC_SHORT) {
        return sizeof(short int);
    } else if (encoding == ZIP_ENC_INT) {
        return sizeof(int);
    } else if (encoding == ZIP_ENC_LLONG) {
        return sizeof(long long);
    }
    assert(NULL);
}

/* Decode the encoded length pointed by 'p'. If a pointer to 'lensize' is
 * provided, it is set to the number of bytes required to encode the length. */
static unsigned int zipDecodeLength(unsigned char *p, unsigned int *lensize) {
    unsigned char encoding = ZIP_ENCODING(p), lenenc;
    unsigned int len;

    if (encoding == ZIP_ENC_RAW) {
        lenenc = (p[0] >> 4) & 0x3;
        if (lenenc == ZIP_LEN_INLINE) {
            len = p[0] & 0xf;
            if (lensize) *lensize = 1;
        } else if (lenenc == ZIP_LEN_UINT16) {
            len = p[1] | (p[2] << 8);
            if (lensize) *lensize = 3;
        } else {
            len = p[1] | (p[2] << 8) | (p[3] << 16) | (p[4] << 24);
            if (lensize) *lensize = 5;
        }
    } else {
        len = zipEncodingSize(encoding);
        if (lensize) *lensize = 1;
    }
    return len;
}

/* Encode the length 'l' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. */
static unsigned int zipEncodeLength(unsigned char *p, char encoding, unsigned int rawlen) {
    unsigned char len = 1, lenenc, buf[5];
    if (encoding == ZIP_ENC_RAW) {
        if (rawlen <= 0xf) {
            if (!p) return len;
            lenenc = ZIP_LEN_INLINE;
            buf[0] = rawlen;
        } else if (rawlen <= 0xffff) {
            len += 2;
            if (!p) return len;
            lenenc = ZIP_LEN_UINT16;
            buf[1] = (rawlen     ) & 0xff;
            buf[2] = (rawlen >> 8) & 0xff;
        } else {
            len += 4;
            if (!p) return len;
            lenenc = ZIP_LEN_UINT32;
            buf[1] = (rawlen      ) & 0xff;
            buf[2] = (rawlen >>  8) & 0xff;
            buf[3] = (rawlen >> 16) & 0xff;
            buf[4] = (rawlen >> 24) & 0xff;
        }
        buf[0] = (lenenc << 4) | (buf[0] & 0xf);
    }
    if (!p) return len;

    /* Apparently we need to store the length in 'p' */
    buf[0] = (encoding << 6) | (buf[0] & 0x3f);
    memcpy(p,buf,len);
    return len;
}

/* Decode the length of the previous element stored at "p". */
static unsigned int zipPrevDecodeLength(unsigned char *p, unsigned int *lensize) {
    unsigned int len = *p;
    if (len < ZIP_BIGLEN) {
        if (lensize) *lensize = 1;
    } else {
        if (lensize) *lensize = 1+sizeof(len);
        memcpy(&len,p+1,sizeof(len));
    }
    return len;
}

/* Encode the length of the previous entry and write it to "p". Return the
 * number of bytes needed to encode this length if "p" is NULL. */
static unsigned int zipPrevEncodeLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        return (len < ZIP_BIGLEN) ? 1 : sizeof(len)+1;
    } else {
        if (len < ZIP_BIGLEN) {
            p[0] = len;
            return 1;
        } else {
            p[0] = ZIP_BIGLEN;
            memcpy(p+1,&len,sizeof(len));
            return 1+sizeof(len);
        }
    }
}

/* Return the difference in number of bytes needed to store the new length
 * "len" on the entry pointed to by "p". */
static int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
    unsigned int prevlensize;
    zipPrevDecodeLength(p,&prevlensize);
    return zipPrevEncodeLength(NULL,len)-prevlensize;
}

/* Check if string pointed to by 'entry' can be encoded as an integer.
 * Stores the integer value in 'v' and its encoding in 'encoding'.
 * Warning: this function requires a NULL-terminated string! */
static int zipTryEncoding(unsigned char *entry, long long *v, unsigned char *encoding) {
    long long value;
    char *eptr;

    if (entry[0] == '-' || (entry[0] >= '0' && entry[0] <= '9')) {
        value = strtoll((char*)entry,&eptr,10);
        if (eptr[0] != '\0') return 0;
        if (value >= SHRT_MIN && value <= SHRT_MAX) {
            *encoding = ZIP_ENC_SHORT;
        } else if (value >= INT_MIN && value <= INT_MAX) {
            *encoding = ZIP_ENC_INT;
        } else {
            *encoding = ZIP_ENC_LLONG;
        }
        *v = value;
        return 1;
    }
    return 0;
}

/* Store integer 'value' at 'p', encoded as 'encoding' */
static void zipSaveInteger(unsigned char *p, long long value, unsigned char encoding) {
    short int s;
    int i;
    long long l;
    if (encoding == ZIP_ENC_SHORT) {
        s = value;
        memcpy(p,&s,sizeof(s));
    } else if (encoding == ZIP_ENC_INT) {
        i = value;
        memcpy(p,&i,sizeof(i));
    } else if (encoding == ZIP_ENC_LLONG) {
        l = value;
        memcpy(p,&l,sizeof(l));
    } else {
        assert(NULL);
    }
}

/* Read integer encoded as 'encoding' from 'p' */
static long long zipLoadInteger(unsigned char *p, unsigned char encoding) {
    short int s;
    int i;
    long long l, ret;
    if (encoding == ZIP_ENC_SHORT) {
        memcpy(&s,p,sizeof(s));
        ret = s;
    } else if (encoding == ZIP_ENC_INT) {
        memcpy(&i,p,sizeof(i));
        ret = i;
    } else if (encoding == ZIP_ENC_LLONG) {
        memcpy(&l,p,sizeof(l));
        ret = l;
    } else {
        assert(NULL);
    }
    return ret;
}

/* Return a struct with all information about an entry. */
static zlentry zipEntry(unsigned char *p) {
    zlentry e;
    e.prevrawlen = zipPrevDecodeLength(p,&e.prevrawlensize);
    e.len = zipDecodeLength(p+e.prevrawlensize,&e.lensize);
    e.headersize = e.prevrawlensize+e.lensize;
    e.encoding = ZIP_ENCODING(p+e.prevrawlensize);
    e.p = p;
    return e;
}

/* Return the total number of bytes used by the entry at "p". */
static unsigned int zipRawEntryLength(unsigned char *p) {
    zlentry e = zipEntry(p);
    return e.headersize + e.len;
}

/* Create a new empty ziplist. */
unsigned char *ziplistNew(void) {
    unsigned int bytes = ZIPLIST_HEADER_SIZE+1;
    unsigned char *zl = zmalloc(bytes);
    ZIPLIST_BYTES(zl) = bytes;
    ZIPLIST_TAIL_OFFSET(zl) = ZIPLIST_HEADER_SIZE;
    ZIPLIST_LENGTH(zl) = 0;
    zl[bytes-1] = ZIP_END;
    return zl;
}

/* Resize the ziplist. */
static unsigned char *ziplistResize(unsigned char *zl, unsigned int len) {
    zl = zrealloc(zl,len);
    ZIPLIST_BYTES(zl) = len;
    zl[len-1] = ZIP_END;
    return zl;
}

/* Delete "num" entries, starting at "p". Returns pointer to the ziplist. */
static unsigned char *__ziplistDelete(unsigned char *zl, unsigned char *p, unsigned int num) {
    unsigned int i, totlen, deleted = 0;
    int nextdiff = 0;
    zlentry first = zipEntry(p);
    for (i = 0; p[0] != ZIP_END && i < num; i++) {
        p += zipRawEntryLength(p);
        deleted++;
    }

    totlen = p-first.p;
    if (totlen > 0) {
        if (p[0] != ZIP_END) {
            /* Tricky: storing the prevlen in this entry might reduce or
             * increase the number of bytes needed, compared to the current
             * prevlen. Note that we can always store this length because
             * it was previously stored by an entry that is being deleted. */
            nextdiff = zipPrevLenByteDiff(p,first.prevrawlen);
            zipPrevEncodeLength(p-nextdiff,first.prevrawlen);

            /* Update offset for tail */
            ZIPLIST_TAIL_OFFSET(zl) -= totlen+nextdiff;

            /* Move tail to the front of the ziplist */
            memmove(first.p,p-nextdiff,ZIPLIST_BYTES(zl)-(p-zl)-1+nextdiff);
        } else {
            /* The entire tail was deleted. No need to move memory. */
            ZIPLIST_TAIL_OFFSET(zl) = (first.p-zl)-first.prevrawlen;
        }

        /* Resize and update length */
        zl = ziplistResize(zl, ZIPLIST_BYTES(zl)-totlen+nextdiff);
        ZIPLIST_INCR_LENGTH(zl,-deleted);
    }
    return zl;
}

/* Insert item at "p". */
static unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    unsigned int curlen = ZIPLIST_BYTES(zl), reqlen, prevlen = 0;
    unsigned int offset, nextdiff = 0;
    unsigned char *tail;
    unsigned char encoding = ZIP_ENC_RAW;
    long long value;
    zlentry entry;

    /* Find out prevlen for the entry that is inserted. */
    if (p[0] != ZIP_END) {
        entry = zipEntry(p);
        prevlen = entry.prevrawlen;
    } else {
        tail = ZIPLIST_ENTRY_TAIL(zl);
        if (tail[0] != ZIP_END) {
            prevlen = zipRawEntryLength(tail);
        }
    }

    /* See if the entry can be encoded */
    if (zipTryEncoding(s,&value,&encoding)) {
        reqlen = zipEncodingSize(encoding);
    } else {
        reqlen = slen;
    }

    /* We need space for both the length of the previous entry and
     * the length of the payload. */
    reqlen += zipPrevEncodeLength(NULL,prevlen);
    reqlen += zipEncodeLength(NULL,encoding,slen);

    /* When the insert position is not equal to the tail, we need to
     * make sure that the next entry can hold this entry's length in
     * its prevlen field. */
    nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p,reqlen) : 0;

    /* Store offset because a realloc may change the address of zl. */
    offset = p-zl;
    zl = ziplistResize(zl,curlen+reqlen+nextdiff);
    p = zl+offset;

    /* Apply memory move when necessary and update tail offset. */
    if (p[0] != ZIP_END) {
        /* Subtract one because of the ZIP_END bytes */
        memmove(p+reqlen,p-nextdiff,curlen-offset-1+nextdiff);
        /* Encode this entry's raw length in the next entry. */
        zipPrevEncodeLength(p+reqlen,reqlen);
        /* Update offset for tail */
        ZIPLIST_TAIL_OFFSET(zl) += reqlen+nextdiff;
    } else {
        /* This element will be the new tail. */
        ZIPLIST_TAIL_OFFSET(zl) = p-zl;
    }

    /* Write the entry */
    p += zipPrevEncodeLength(p,prevlen);
    p += zipEncodeLength(p,encoding,slen);
    if (encoding != ZIP_ENC_RAW) {
        zipSaveInteger(p,value,encoding);
    } else {
        memcpy(p,s,slen);
    }
    ZIPLIST_INCR_LENGTH(zl,1);
    return zl;
}

unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {
    unsigned char *p;
    p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);
    return __ziplistInsert(zl,p,s,slen);
}

unsigned char *ziplistPop(unsigned char *zl, sds *target, int where) {
    zlentry entry;
    unsigned char *p;
    long long value;
    if (target) *target = NULL;

    /* Get pointer to element to remove */
    p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_TAIL(zl);
    if (*p == ZIP_END) return zl;

    entry = zipEntry(p);
    if (target) {
        if (entry.encoding == ZIP_ENC_RAW) {
            *target = sdsnewlen(p+entry.headersize,entry.len);
        } else {
            value = zipLoadInteger(p+entry.headersize,entry.encoding);
            *target = sdscatprintf(sdsempty(), "%lld", value);
        }
    }

    zl = __ziplistDelete(zl,p,1);
    return zl;
}

/* Returns an offset to use for iterating with ziplistNext. When the given
 * index is negative, the list is traversed back to front. When the list
 * doesn't contain an element at the provided index, NULL is returned. */
unsigned char *ziplistIndex(unsigned char *zl, int index) {
    unsigned char *p;
    zlentry entry;
    if (index < 0) {
        index = (-index)-1;
        p = ZIPLIST_ENTRY_TAIL(zl);
        if (p[0] != ZIP_END) {
            entry = zipEntry(p);
            while (entry.prevrawlen > 0 && index--) {
                p -= entry.prevrawlen;
                entry = zipEntry(p);
            }
        }
    } else {
        p = ZIPLIST_ENTRY_HEAD(zl);
        while (p[0] != ZIP_END && index--) {
            p += zipRawEntryLength(p);
        }
    }
    return (p[0] == ZIP_END || index > 0) ? NULL : p;
}

/* Return pointer to next entry in ziplist. */
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p) {
    ((void) zl);
    return (p[0] == ZIP_END) ? NULL : p+zipRawEntryLength(p);
}

/* Return pointer to previous entry in ziplist. */
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p) {
    zlentry entry;

    /* Iterating backwards from ZIP_END should return the tail. When "p" is
     * equal to the first element of the list, we're already at the head,
     * and should return NULL. */
    if (p[0] == ZIP_END) {
        p = ZIPLIST_ENTRY_TAIL(zl);
        return (p[0] == ZIP_END) ? NULL : p;
    } else if (p == ZIPLIST_ENTRY_HEAD(zl)) {
        return NULL;
    } else {
        entry = zipEntry(p);
        return p-entry.prevrawlen;
    }
}

/* Get entry pointer to by 'p' and store in either 'e' or 'v' depending
 * on the encoding of the entry. 'e' is always set to NULL to be able
 * to find out whether the string pointer or the integer value was set.
 * Return 0 if 'p' points to the end of the zipmap, 1 otherwise. */
unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *sval) {
    zlentry entry;
    if (p == NULL || p[0] == ZIP_END) return 0;
    if (sstr) *sstr = NULL;

    entry = zipEntry(p);
    if (entry.encoding == ZIP_ENC_RAW) {
        if (sstr) {
            *slen = entry.len;
            *sstr = p+entry.headersize;
        }
    } else {
        if (sval) {
            *sval = zipLoadInteger(p+entry.headersize,entry.encoding);
        }
    }
    return 1;
}

/* Insert an entry at "p". */
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    return __ziplistInsert(zl,p,s,slen);
}

/* Delete a single entry from the ziplist, pointed to by *p.
 * Also update *p in place, to be able to iterate over the
 * ziplist, while deleting entries. */
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {
    unsigned int offset = *p-zl;
    zl = __ziplistDelete(zl,*p,1);

    /* Store pointer to current element in p, because ziplistDelete will
     * do a realloc which might result in a different "zl"-pointer.
     * When the delete direction is back to front, we might delete the last
     * entry and end up with "p" pointing to ZIP_END, so check this. */
    *p = zl+offset;
    return zl;
}

/* Delete a range of entries from the ziplist. */
unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index, unsigned int num) {
    unsigned char *p = ziplistIndex(zl,index);
    return (p == NULL) ? zl : __ziplistDelete(zl,p,num);
}

/* Compare entry pointer to by 'p' with 'entry'. Return 1 if equal. */
unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen) {
    zlentry entry;
    unsigned char sencoding;
    long long zval, sval;
    if (p[0] == ZIP_END) return 0;

    entry = zipEntry(p);
    if (entry.encoding == ZIP_ENC_RAW) {
        /* Raw compare */
        if (entry.len == slen) {
            return memcmp(p+entry.headersize,sstr,slen) == 0;
        } else {
            return 0;
        }
    } else {
        /* Try to compare encoded values */
        if (zipTryEncoding(sstr,&sval,&sencoding)) {
            if (entry.encoding == sencoding) {
                zval = zipLoadInteger(p+entry.headersize,entry.encoding);
                return zval == sval;
            }
        }
    }
    return 0;
}

/* Return length of ziplist. */
unsigned int ziplistLen(unsigned char *zl) {
    unsigned int len = 0;
    if (ZIPLIST_LENGTH(zl) < ZIP_BIGLEN) {
        len = ZIPLIST_LENGTH(zl);
    } else {
        unsigned char *p = zl+ZIPLIST_HEADER_SIZE;
        while (*p != ZIP_END) {
            p += zipRawEntryLength(p);
            len++;
        }

        /* Re-store length if small enough */
        if (len < ZIP_BIGLEN) ZIPLIST_LENGTH(zl) = len;
    }
    return len;
}

/* Return size in bytes of ziplist. */
unsigned int ziplistSize(unsigned char *zl) {
    return ZIPLIST_BYTES(zl);
}

void ziplistRepr(unsigned char *zl) {
    unsigned char *p;
    zlentry entry;

    printf("{total bytes %d} {length %u}\n",ZIPLIST_BYTES(zl), ZIPLIST_LENGTH(zl));
    p = ZIPLIST_ENTRY_HEAD(zl);
    while(*p != ZIP_END) {
        entry = zipEntry(p);
        printf("{offset %ld, header %u, payload %u} ",p-zl,entry.headersize,entry.len);
        p += entry.headersize;
        if (entry.encoding == ZIP_ENC_RAW) {
            fwrite(p,entry.len,1,stdout);
        } else {
            printf("%lld", zipLoadInteger(p,entry.encoding));
        }
        printf("\n");
        p += entry.len;
    }
    printf("{end}\n\n");
}

#ifdef ZIPLIST_TEST_MAIN

unsigned char *createList() {
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, "foo", 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, "quux", 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, "hello", 5, ZIPLIST_HEAD);
    zl = ziplistPush(zl, "1024", 4, ZIPLIST_TAIL);
    return zl;
}

unsigned char *createIntList() {
    unsigned char *zl = ziplistNew();
    char buf[32];

    sprintf(buf, "100");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "128000");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "-100");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "4294967296");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "non integer");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "much much longer non integer");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_TAIL);
    return zl;
}

int main(int argc, char **argv) {
    unsigned char *zl, *p;
    char *entry;
    unsigned int elen;
    long long value;
    sds s;

    zl = createIntList();
    ziplistRepr(zl);

    zl = createList();
    ziplistRepr(zl);

    zl = ziplistPop(zl, &s, ZIPLIST_TAIL);
    printf("Pop tail: %s (length %ld)\n", s, sdslen(s));
    ziplistRepr(zl);

    zl = ziplistPop(zl, &s, ZIPLIST_HEAD);
    printf("Pop head: %s (length %ld)\n", s, sdslen(s));
    ziplistRepr(zl);

    zl = ziplistPop(zl, &s, ZIPLIST_TAIL);
    printf("Pop tail: %s (length %ld)\n", s, sdslen(s));
    ziplistRepr(zl);

    zl = ziplistPop(zl, &s, ZIPLIST_TAIL);
    printf("Pop tail: %s (length %ld)\n", s, sdslen(s));
    ziplistRepr(zl);

    printf("Get element at index 3:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 3);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index 3\n");
            return 1;
        }
        if (entry) {
            fwrite(entry,elen,1,stdout);
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index 4 (out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
    }

    printf("Get element at index -1 (last element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -1\n");
            return 1;
        }
        if (entry) {
            fwrite(entry,elen,1,stdout);
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index -4 (first element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -4\n");
            return 1;
        }
        if (entry) {
            fwrite(entry,elen,1,stdout);
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index -5 (reverse out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -5);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
    }

    printf("Iterate list from 0 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                fwrite(entry,elen,1,stdout);
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 1 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                fwrite(entry,elen,1,stdout);
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 2 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 2);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                fwrite(entry,elen,1,stdout);
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate starting out of range:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("No entry\n");
        } else {
            printf("ERROR\n");
        }
        printf("\n");
    }

    printf("Iterate from back to front:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                fwrite(entry,elen,1,stdout);
            } else {
                printf("%lld", value);
            }
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate from back to front, deleting all items:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                fwrite(entry,elen,1,stdout);
            } else {
                printf("%lld", value);
            }
            zl = ziplistDelete(zl,&p);
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Delete inclusive range 0,0:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 1);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 0,1:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 2);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 1,2:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 2);
        ziplistRepr(zl);
    }

    printf("Delete with start index out of range:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 5, 1);
        ziplistRepr(zl);
    }

    printf("Delete with num overflow:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 5);
        ziplistRepr(zl);
    }

    printf("Delete foo while iterating:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value)) {
            if (entry && strncmp("foo", entry, elen) == 0) {
                printf("Delete foo\n");
                zl = ziplistDelete(zl, &p, ZIPLIST_TAIL);
            } else {
                printf("Entry: ");
                if (entry) {
                    fwrite(entry,elen,1,stdout);
                } else {
                    printf("%lld", value);
                }
                p = ziplistNext(p);
                printf("\n");
            }
        }
        printf("\n");
        ziplistRepr(zl);
    }

    printf("Create long list and check indices:\n");
    {
        zl = ziplistNew();
        char buf[32];
        int i,len;
        for (i = 0; i < 1000; i++) {
            len = sprintf(buf,"%d",i);
            zl = ziplistPush(zl,buf,len,ZIPLIST_TAIL);
        }
        for (i = 0; i < 1000; i++) {
            p = ziplistIndex(zl,i);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(i == value);

            p = ziplistIndex(zl,-i-1);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(999-i == value);
        }
        printf("SUCCESS\n\n");
    }

    printf("Compare strings with ziplist entries:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        if (!ziplistCompare(p,"hello",5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p,"hella",5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl, 3);
        if (!ziplistCompare(p,"1024",4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p,"1025",4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }
        printf("SUCCESS\n");
    }

    return 0;
}
#endif
