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
#include <string.h>
#include <assert.h>
#include "zmalloc.h"
#include "sds.h"
#include "ziplist.h"
#include "zip.c"

#define ZIPLIST_BYTES(zl) (*((unsigned int*)(zl)))
#define ZIPLIST_LENGTH(zl) (*((zl)+sizeof(unsigned int)))
#define ZIPLIST_HEADER_SIZE (sizeof(unsigned int)+1)

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
    unsigned int curlen = ZIPLIST_BYTES(zl);
    unsigned int reqlen = zipEncodeLength(NULL,elen)+elen;
    unsigned char *p;

    /* Resize the ziplist */
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

    /* Increase length */
    if (ZIPLIST_LENGTH(zl) < ZIP_BIGLEN) ZIPLIST_LENGTH(zl)++;

    /* Write the entry */
    p += zipEncodeLength(p,elen);
    memcpy(p,entry,elen);
    return zl;
}

unsigned char *ziplistPop(unsigned char *zl, sds *value, int where) {
    unsigned int curlen = ZIPLIST_BYTES(zl), len, rlen;
    unsigned char *p;
    if (value) *value = NULL;

    /* Get pointer to element to remove */
    p = (where == ZIPLIST_HEAD) ? ziplistHead(zl) : ziplistTail(zl);
    if (*p == ZIP_END) return zl;
    len = zipDecodeLength(p);
    if (value) *value = sdsnewlen(p+zipEncodeLength(NULL,len),len);

    /* Move list to front when popping from the head */
    rlen = zipRawEntryLength(p);
    if (where == ZIPLIST_HEAD) {
        memmove(p,p+rlen,curlen-ZIPLIST_HEADER_SIZE-len);
    }

    /* Resize and update length */
    zl = ziplistResize(zl,curlen-rlen);
    if (ZIPLIST_LENGTH(zl) < ZIP_BIGLEN) ZIPLIST_LENGTH(zl)--;
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
    if (*p == ZIP_END) return NULL;
    if (entry) {
        *elen = zipDecodeLength(p);
        *entry = p+ZIP_LEN_BYTES(*elen);
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
        if (ZIPLIST_LENGTH(zl) < ZIP_BIGLEN) ZIPLIST_LENGTH(zl) -= deleted;
    }
    return zl;
}

void ziplistRepr(unsigned char *zl) {
    unsigned char *p;
    unsigned int l;

    printf("{bytes %d} {length %u}\n",ZIPLIST_BYTES(zl), ZIPLIST_LENGTH(zl));
    p = ziplistHead(zl);
    while(*p != ZIP_END) {
        l = zipDecodeLength(p);
        printf("{key %u}",l);
        p += zipEncodeLength(NULL,l);
        fwrite(p,l,1,stdout);
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

int main(int argc, char **argv) {
    unsigned char *zl, *p, *entry;
    unsigned int elen;
    sds s;

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

    return 0;
}
#endif
