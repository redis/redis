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
#include "zip.c"

#define ZIPLIST_BYTES(zl) (*((unsigned int*)(zl)))
#define ZIPLIST_LENGTH(zl) (*((zl)+sizeof(unsigned int)))
#define ZIPLIST_HEADER_SIZE (sizeof(unsigned int)+1)
#define ZIPLIST_INCR_LENGTH(zl,incr) { \
    if (ZIPLIST_LENGTH(zl) < (ZIP_END-1)) ZIPLIST_LENGTH(zl)+=incr; }

/* Create a new empty ziplist. */
unsigned char *ziplistNew(void) {
    unsigned int bytes = ZIPLIST_HEADER_SIZE+1;
    unsigned char *zl = zmalloc(bytes);
    ZIPLIST_BYTES(zl) = bytes;
    ZIPLIST_LENGTH(zl) = 0;
    zl[bytes-1] = ZIP_END;
    return zl;
}

static unsigned char *ziplistResize(unsigned char *zl, unsigned int len) {
    zl = zipResize(zl,len);
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
