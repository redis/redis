#define ZIP_BIGLEN 254
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

/* Resize the zip* structure. */
static unsigned char *zipResize(unsigned char *z, unsigned int len) {
    z = zrealloc(z,len);
    z[len-1] = ZIP_END;
    return z;
}
