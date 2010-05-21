#define ZIP_BIGLEN 254
#define ZIP_END 255

/* The following macro returns the number of bytes needed to encode the length
 * for the integer value _l, that is, 1 byte for lengths < ZIP_BIGLEN and
 * 5 bytes for all the other lengths. */
#define ZIP_LEN_BYTES(_l) (((_l) < ZIP_BIGLEN) ? 1 : sizeof(unsigned int)+1)

/* Decode the encoded length pointed by 'p' */
static unsigned int zipDecodeLength(unsigned char *p) {
    unsigned int len = *p;

    if (len < ZIP_BIGLEN) return len;
    memcpy(&len,p+1,sizeof(unsigned int));
    return len;
}

/* Encode the length 'l' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. */
static unsigned int zipEncodeLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        return ZIP_LEN_BYTES(len);
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

/* Return the total amount used by an entry (encoded length + payload). */
static unsigned int zipRawEntryLength(unsigned char *p) {
    unsigned int l = zipDecodeLength(p);
    return zipEncodeLength(NULL,l) + l;
}

/* Resize the zip* structure. */
static unsigned char *zipResize(unsigned char *z, unsigned int len) {
    z = zrealloc(z,len);
    z[len-1] = ZIP_END;
    return z;
}
