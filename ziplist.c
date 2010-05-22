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

#define ZIP_END 255

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
#define ZIPLIST_LENGTH(zl) (*((zl)+sizeof(unsigned int)))
#define ZIPLIST_HEADER_SIZE (sizeof(unsigned int)+1)
#define ZIPLIST_INCR_LENGTH(zl,incr) { \
    if (ZIPLIST_LENGTH(zl) < (ZIP_END-1)) ZIPLIST_LENGTH(zl)+=incr; }

/* Return bytes needed to store integer encoded by 'encoding' */
static unsigned int zipEncodingSize(char encoding) {
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

/* Check if string pointed to by 'entry' can be encoded as an integer.
 * Stores the integer value in 'v' and its encoding in 'encoding'.
 * Warning: this function requires a NULL-terminated string! */
static int zipTryEncoding(unsigned char *entry, long long *v, char *encoding) {
    long long value;
    char *eptr;

    if (entry[0] == '-' || (entry[0] >= '0' && entry[0] <= '9')) {
        value = strtoll(entry,&eptr,10);
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
static void zipSaveInteger(unsigned char *p, long long value, char encoding) {
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
static long long zipLoadInteger(unsigned char *p, char encoding) {
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

/* Return the total amount used by an entry (encoded length + payload). */
static unsigned int zipRawEntryLength(unsigned char *p) {
    unsigned int lensize, len;
    len = zipDecodeLength(p, &lensize);
    return lensize + len;
}

/* Create a new empty ziplist. */
unsigned char *ziplistNew(void) {
    unsigned int bytes = ZIPLIST_HEADER_SIZE+1;
    unsigned char *zl = zmalloc(bytes);
    ZIPLIST_BYTES(zl) = bytes;
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

static unsigned char *ziplistHead(unsigned char *zl) {
    return zl+ZIPLIST_HEADER_SIZE;
}

static unsigned char *ziplistTail(unsigned char *zl) {
    unsigned char *p, *q;
    p = q = ziplistHead(zl);
    while (*p != ZIP_END) {
        q = p;
        p += zipRawEntryLength(p);
    }
    return q;
}

unsigned char *ziplistPush(unsigned char *zl, unsigned char *entry, unsigned int elen, int where) {
    unsigned int curlen = ZIPLIST_BYTES(zl), reqlen;
    unsigned char *p;
    char encoding = ZIP_ENC_RAW;
    long long value;

    /* See if the entry can be encoded */
    if (zipTryEncoding(entry,&value,&encoding)) {
        reqlen = zipEncodingSize(encoding);
    } else {
        reqlen = elen;
    }
    reqlen += zipEncodeLength(NULL,encoding,elen);

    /* Resize the ziplist and move if needed */
    zl = ziplistResize(zl,curlen+reqlen);
    if (where == ZIPLIST_HEAD) {
        p = zl+ZIPLIST_HEADER_SIZE;
        if (*p != ZIP_END) {
            /* Subtract one because of the ZIP_END bytes */
            memmove(p+reqlen,p,curlen-ZIPLIST_HEADER_SIZE-1);
        }
    } else {
        p = zl+curlen-1;
    }

    /* Write the entry */
    p += zipEncodeLength(p,encoding,elen);
    if (encoding != ZIP_ENC_RAW) {
        zipSaveInteger(p,value,encoding);
    } else {
        memcpy(p,entry,elen);
    }
    ZIPLIST_INCR_LENGTH(zl,1);
    return zl;
}

unsigned char *ziplistPop(unsigned char *zl, sds *target, int where) {
    unsigned int curlen = ZIPLIST_BYTES(zl), rawlen;
    unsigned int len, lensize;
    unsigned char *p;
    long long value;
    if (target) *target = NULL;

    /* Get pointer to element to remove */
    p = (where == ZIPLIST_HEAD) ? ziplistHead(zl) : ziplistTail(zl);
    if (*p == ZIP_END) return zl;
    len = zipDecodeLength(p,&lensize);
    if (target) {
        if (ZIP_ENCODING(p) == ZIP_ENC_RAW) {
            *target = sdsnewlen(p+lensize,len);
        } else {
            value = zipLoadInteger(p+lensize,ZIP_ENCODING(p));
            *target = sdscatprintf(sdsempty(), "%lld", value);
        }
    }

    /* Move list to front when popping from the head */
    rawlen = lensize+len;
    if (where == ZIPLIST_HEAD) {
        memmove(p,p+rawlen,curlen-ZIPLIST_HEADER_SIZE-len);
    }

    /* Resize and update length */
    zl = ziplistResize(zl,curlen-rawlen);
    ZIPLIST_INCR_LENGTH(zl,-1);
    return zl;
}

/* Returns an offset to use for iterating with ziplistNext. */
unsigned char *ziplistIndex(unsigned char *zl, unsigned int index) {
    unsigned char *p = zl+ZIPLIST_HEADER_SIZE;
    unsigned int i = 0;
    for (; i < index; i++) {
        if (*p == ZIP_END) break;
        p += zipRawEntryLength(p);
    }
    return p;
}

/* Store entry at current position in sds *value and return pointer
 * to the next entry. */
unsigned char *ziplistNext(unsigned char *p, unsigned char **q, unsigned char **entry, unsigned int *elen) {
    unsigned int lensize;
    if (*p == ZIP_END) return NULL;
    if (entry) {
        *elen = zipDecodeLength(p,&lensize);
        *entry = p+lensize;
    }
    if (q != NULL) *q = p;
    p += zipRawEntryLength(p);
    return p;
}

/* Delete a range of entries from the ziplist. */
unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index, unsigned int num) {
    unsigned char *p, *first = ziplistIndex(zl, index);
    unsigned int i, deleted = 0, totlen, newlen;
    for (p = first, i = 0; *p != ZIP_END && i < num; i++) {
        p += zipRawEntryLength(p);
        deleted++;
    }

    totlen = p-first;
    if (totlen > 0) {
        /* Move current tail to the new tail when there *is* a tail */
        if (*p != ZIP_END) memmove(first,p,ZIPLIST_BYTES(zl)-(p-zl)-1);

        /* Resize and update length */
        zl = ziplistResize(zl, ZIPLIST_BYTES(zl)-totlen);
        ZIPLIST_INCR_LENGTH(zl,-deleted);
    }
    return zl;
}

/* Delete a single entry from the ziplist, pointed to by *p.
 * Also update *p in place, to be able to iterate over the
 * ziplist, while deleting entries. */
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {
    unsigned int offset = *p-zl, tail, len;
    len = zipRawEntryLength(*p);
    tail = ZIPLIST_BYTES(zl)-offset-len-1;

    /* Move current tail to the new tail when there *is* a tail */
    if (tail > 0) memmove(*p,*p+len,tail);

    /* Resize and update length */
    zl = ziplistResize(zl, ZIPLIST_BYTES(zl)-len);
    ZIPLIST_INCR_LENGTH(zl,-1);

    /* Store new pointer to current element in p.
     * This needs to be done because zl can change on realloc. */
    *p = zl+offset;
    return zl;
}

void ziplistRepr(unsigned char *zl) {
    unsigned char *p, encoding;
    unsigned int l, lsize;
    long long value;

    printf("{total bytes %d} {length %u}\n",ZIPLIST_BYTES(zl), ZIPLIST_LENGTH(zl));
    p = ziplistHead(zl);
    while(*p != ZIP_END) {
        l = zipDecodeLength(p,&lsize);
        printf("{header %u, payload %u} ",lsize,l);
        encoding = ZIP_ENCODING(p);
        p += lsize;
        if (encoding == ZIP_ENC_RAW) {
            fwrite(p,l,1,stdout);
        } else {
            printf("%lld", zipLoadInteger(p,encoding));
        }
        printf("\n");
        p += l;
    }
    printf("{end}\n\n");
}

#ifdef ZIPLIST_TEST_MAIN

unsigned char *createList() {
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char*)"foo", 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"hello", 5, ZIPLIST_HEAD);
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
    unsigned char *zl, *p, *q, *entry;
    unsigned int elen;
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

    printf("Iterate list from 0 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while ((p = ziplistNext(p, NULL, &entry, &elen)) != NULL) {
            printf("Entry: ");
            fwrite(entry,elen,1,stdout);
            printf(" (length %d)\n", elen);
        }
        printf("\n");
    }

    printf("Iterate list from 1 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 1);
        while ((p = ziplistNext(p, NULL, &entry, &elen)) != NULL) {
            printf("Entry: ");
            fwrite(entry,elen,1,stdout);
            printf(" (length %d)\n", elen);
        }
        printf("\n");
    }

    printf("Iterate list from 2 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 2);
        while ((p = ziplistNext(p, NULL, &entry, &elen)) != NULL) {
            printf("Entry: ");
            fwrite(entry,elen,1,stdout);
            printf(" (length %d)\n", elen);
        }
        printf("\n");
    }

    printf("Iterate starting out of range:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 3);
        if (ziplistNext(p, &entry, NULL, &elen) == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR\n");
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
        while ((p = ziplistNext(p, &q, &entry, &elen)) != NULL) {
            if (strncmp("foo", entry, elen) == 0) {
                printf("Delete foo\n");
                zl = ziplistDelete(zl, &q);
                p = q;
            } else {
                printf("Entry: ");
                fwrite(entry,elen,1,stdout);
                printf(" (length %d)\n", elen);
            }
        }
        printf("\n");
        ziplistRepr(zl);
        printf("\n");
    }

    return 0;
}
#endif
