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
    *value = NULL;

    /* Get pointer to element to remove */
    p = (where == ZIPLIST_HEAD) ? ziplistHead(zl) : ziplistTail(zl);
    if (*p == ZIP_END) return zl;
    len = zipDecodeLength(p);
    *value = sdsnewlen(p+zipEncodeLength(NULL,len),len);

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
int main(int argc, char **argv) {
    unsigned char *zl;
    sds s;

    zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char*)"foo", 3, ZIPLIST_TAIL);
    ziplistRepr(zl);
    zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
    ziplistRepr(zl);
    zl = ziplistPush(zl, (unsigned char*)"hello", 5, ZIPLIST_HEAD);
    ziplistRepr(zl);

    zl = ziplistPop(zl, &s, ZIPLIST_TAIL);
    printf("Pop tail: %s (length %ld)\n", s, sdslen(s));
    ziplistRepr(zl);

    zl = ziplistPop(zl, &s, ZIPLIST_HEAD);
    printf("Pop head: %s (length %ld)\n", s, sdslen(s));
    ziplistRepr(zl);

    return 0;
}
#endif
