/*
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */

#include "server.h"
#include "lzf.h"    /* LZF compression library */
#include "zipmap.h"
#include "endianconv.h"
#include "fpconv_dtoa.h"
#include "stream.h"
#include "functions.h"
#include "intset.h"  /* Compact integer set structure */
#include "bio.h"

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/param.h>

/* This macro is called when the internal RDB structure is corrupt */
#define rdbReportCorruptRDB(...) rdbReportError(1, __LINE__,__VA_ARGS__)
/* This macro is called when RDB read failed (possibly a short read) */
#define rdbReportReadError(...) rdbReportError(0, __LINE__,__VA_ARGS__)

/* This macro tells if we are in the context of a RESTORE command, and not loading an RDB or AOF. */
#define isRestoreContext() \
    ((server.current_client == NULL || server.current_client->id == CLIENT_ID_AOF) ? 0 : 1)

char* rdbFileBeingLoaded = NULL; /* used for rdb checking on read error */
extern int rdbCheckMode;
void rdbCheckError(const char *fmt, ...);
void rdbCheckSetError(const char *fmt, ...);

#ifdef __GNUC__
void rdbReportError(int corruption_error, int linenum, char *reason, ...) __attribute__ ((format (printf, 3, 4)));
#endif
void rdbReportError(int corruption_error, int linenum, char *reason, ...) {
    va_list ap;
    char msg[1024];
    int len;

    len = snprintf(msg,sizeof(msg),
        "Internal error in RDB reading offset %llu, function at rdb.c:%d -> ",
        (unsigned long long)server.loading_loaded_bytes, linenum);
    va_start(ap,reason);
    vsnprintf(msg+len,sizeof(msg)-len,reason,ap);
    va_end(ap);

    if (isRestoreContext()) {
        /* If we're in the context of a RESTORE command, just propagate the error. */
        /* log in VERBOSE, and return (don't exit). */
        serverLog(LL_VERBOSE, "%s", msg);
        return;
    } else if (rdbCheckMode) {
        /* If we're inside the rdb checker, let it handle the error. */
        rdbCheckError("%s",msg);
    } else if (rdbFileBeingLoaded) {
        /* If we're loading an rdb file form disk, run rdb check (and exit) */
        serverLog(LL_WARNING, "%s", msg);
        char *argv[2] = {"",rdbFileBeingLoaded};
        if (anetIsFifo(argv[1])) {
            /* Cannot check RDB FIFO because we cannot reopen the FIFO and check already streamed data. */
            rdbCheckError("Cannot check RDB that is a FIFO: %s", argv[1]);
            return;
        }
        redis_check_rdb_main(2,argv,NULL);
    } else if (corruption_error) {
        /* In diskless loading, in case of corrupt file, log and exit. */
        serverLog(LL_WARNING, "%s. Failure loading rdb format", msg);
    } else {
        /* In diskless loading, in case of a short read (not a corrupt
         * file), log and proceed (don't exit). */
        serverLog(LL_WARNING, "%s. Failure loading rdb format from socket, assuming connection error, resuming operation.", msg);
        return;
    }
    serverLog(LL_WARNING, "Terminating server after rdb file reading failure.");
    exit(1);
}

ssize_t rdbWriteRaw(rio *rdb, void *p, size_t len) {
    if (rdb && rioWrite(rdb,p,len) == 0)
        return -1;
    return len;
}

int rdbSaveType(rio *rdb, unsigned char type) {
    return rdbWriteRaw(rdb,&type,1);
}

/* Load a "type" in RDB format, that is a one byte unsigned integer.
 * This function is not only used to load object types, but also special
 * "types" like the end-of-file type, the EXPIRE type, and so forth. */
int rdbLoadType(rio *rdb) {
    unsigned char type;
    if (rioRead(rdb,&type,1) == 0) return -1;
    return type;
}

/* This is only used to load old databases stored with the RDB_OPCODE_EXPIRETIME
 * opcode. New versions of Redis store using the RDB_OPCODE_EXPIRETIME_MS
 * opcode. On error -1 is returned, however this could be a valid time, so
 * to check for loading errors the caller should call rioGetReadError() after
 * calling this function. */
time_t rdbLoadTime(rio *rdb) {
    int32_t t32;
    if (rioRead(rdb,&t32,4) == 0) return -1;
    return (time_t)t32;
}

ssize_t rdbSaveMillisecondTime(rio *rdb, long long t) {
    int64_t t64 = (int64_t) t;
    memrev64ifbe(&t64); /* Store in little endian. */
    return rdbWriteRaw(rdb,&t64,8);
}

/* This function loads a time from the RDB file. It gets the version of the
 * RDB because, unfortunately, before Redis 5 (RDB version 9), the function
 * failed to convert data to/from little endian, so RDB files with keys having
 * expires could not be shared between big endian and little endian systems
 * (because the expire time will be totally wrong). The fix for this is just
 * to call memrev64ifbe(), however if we fix this for all the RDB versions,
 * this call will introduce an incompatibility for big endian systems:
 * after upgrading to Redis version 5 they will no longer be able to load their
 * own old RDB files. Because of that, we instead fix the function only for new
 * RDB versions, and load older RDB versions as we used to do in the past,
 * allowing big endian systems to load their own old RDB files.
 *
 * On I/O error the function returns LLONG_MAX, however if this is also a
 * valid stored value, the caller should use rioGetReadError() to check for
 * errors after calling this function. */
long long rdbLoadMillisecondTime(rio *rdb, int rdbver) {
    int64_t t64;
    if (rioRead(rdb,&t64,8) == 0) return LLONG_MAX;
    if (rdbver >= 9) /* Check the top comment of this function. */
        memrev64ifbe(&t64); /* Convert in big endian if the system is BE. */
    return (long long)t64;
}

/* Saves an encoded length. The first two bits in the first byte are used to
 * hold the encoding type. See the RDB_* definitions for more information
 * on the types of encoding. */
int rdbSaveLen(rio *rdb, uint64_t len) {
    unsigned char buf[2];
    size_t nwritten;

    if (len < (1<<6)) {
        /* Save a 6 bit len */
        buf[0] = (len&0xFF)|(RDB_6BITLEN<<6);
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        nwritten = 1;
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        buf[0] = ((len>>8)&0xFF)|(RDB_14BITLEN<<6);
        buf[1] = len&0xFF;
        if (rdbWriteRaw(rdb,buf,2) == -1) return -1;
        nwritten = 2;
    } else if (len <= UINT32_MAX) {
        /* Save a 32 bit len */
        buf[0] = RDB_32BITLEN;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        uint32_t len32 = htonl(len);
        if (rdbWriteRaw(rdb,&len32,4) == -1) return -1;
        nwritten = 1+4;
    } else {
        /* Save a 64 bit len */
        buf[0] = RDB_64BITLEN;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        len = htonu64(len);
        if (rdbWriteRaw(rdb,&len,8) == -1) return -1;
        nwritten = 1+8;
    }
    return nwritten;
}


/* Load an encoded length. If the loaded length is a normal length as stored
 * with rdbSaveLen(), the read length is set to '*lenptr'. If instead the
 * loaded length describes a special encoding that follows, then '*isencoded'
 * is set to 1 and the encoding format is stored at '*lenptr'.
 *
 * See the RDB_ENC_* definitions in rdb.h for more information on special
 * encodings.
 *
 * The function returns -1 on error, 0 on success. */
int rdbLoadLenByRef(rio *rdb, int *isencoded, uint64_t *lenptr) {
    unsigned char buf[2];
    int type;

    if (isencoded) *isencoded = 0;
    if (rioRead(rdb,buf,1) == 0) return -1;
    type = (buf[0]&0xC0)>>6;
    if (type == RDB_ENCVAL) {
        /* Read a 6 bit encoding type. */
        if (isencoded) *isencoded = 1;
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_6BITLEN) {
        /* Read a 6 bit len. */
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_14BITLEN) {
        /* Read a 14 bit len. */
        if (rioRead(rdb,buf+1,1) == 0) return -1;
        *lenptr = ((buf[0]&0x3F)<<8)|buf[1];
    } else if (buf[0] == RDB_32BITLEN) {
        /* Read a 32 bit len. */
        uint32_t len;
        if (rioRead(rdb,&len,4) == 0) return -1;
        *lenptr = ntohl(len);
    } else if (buf[0] == RDB_64BITLEN) {
        /* Read a 64 bit len. */
        uint64_t len;
        if (rioRead(rdb,&len,8) == 0) return -1;
        *lenptr = ntohu64(len);
    } else {
        rdbReportCorruptRDB(
            "Unknown length encoding %d in rdbLoadLen()",type);
        return -1; /* Never reached. */
    }
    return 0;
}

/* This is like rdbLoadLenByRef() but directly returns the value read
 * from the RDB stream, signaling an error by returning RDB_LENERR
 * (since it is a too large count to be applicable in any Redis data
 * structure). */
uint64_t rdbLoadLen(rio *rdb, int *isencoded) {
    uint64_t len;

    if (rdbLoadLenByRef(rdb,isencoded,&len) == -1) return RDB_LENERR;
    return len;
}

/* Encodes the "value" argument as integer when it fits in the supported ranges
 * for encoded types. If the function successfully encodes the integer, the
 * representation is stored in the buffer pointer to by "enc" and the string
 * length is returned. Otherwise 0 is returned. */
int rdbEncodeInteger(long long value, unsigned char *enc) {
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* Loads an integer-encoded object with the specified encoding type "enctype".
 * The returned value changes according to the flags, see
 * rdbGenericLoadStringObject() for more info. */
void *rdbLoadIntegerObject(rio *rdb, int enctype, int flags, size_t *lenptr) {
    int plainFlag = flags & RDB_LOAD_PLAIN;
    int sdsFlag = flags & RDB_LOAD_SDS;
    int hfldFlag = flags & (RDB_LOAD_HFLD|RDB_LOAD_HFLD_TTL);
    int encode = flags & RDB_LOAD_ENC;
    unsigned char enc[4];
    long long val;

    if (enctype == RDB_ENC_INT8) {
        if (rioRead(rdb,enc,1) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == RDB_ENC_INT16) {
        uint16_t v;
        if (rioRead(rdb,enc,2) == 0) return NULL;
        v = ((uint32_t)enc[0])|
            ((uint32_t)enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == RDB_ENC_INT32) {
        uint32_t v;
        if (rioRead(rdb,enc,4) == 0) return NULL;
        v = ((uint32_t)enc[0])|
            ((uint32_t)enc[1]<<8)|
            ((uint32_t)enc[2]<<16)|
            ((uint32_t)enc[3]<<24);
        val = (int32_t)v;
    } else {
        rdbReportCorruptRDB("Unknown RDB integer encoding type %d",enctype);
        return NULL; /* Never reached. */
    }
    if (plainFlag || sdsFlag || hfldFlag) {
        char buf[LONG_STR_SIZE], *p;
        int len = ll2string(buf,sizeof(buf),val);
        if (lenptr) *lenptr = len;
        if (plainFlag) {
            p = zmalloc(len);
        } else if (sdsFlag) {
            p = sdsnewlen(SDS_NOINIT,len);
        } else { /* hfldFlag */
            p = hfieldNew(NULL, len, (flags&RDB_LOAD_HFLD) ? 0 : 1);
        }
        memcpy(p,buf,len);
        return p;
    } else if (encode) {
        return createStringObjectFromLongLongForValue(val);
    } else {
        return createStringObjectFromLongLongWithSds(val);
    }
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space */
int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    if (string2ll(s, len, &value)) {
        return rdbEncodeInteger(value, enc);
    } else {
        return 0;
    }
}

ssize_t rdbSaveLzfBlob(rio *rdb, void *data, size_t compress_len,
                       size_t original_len) {
    unsigned char byte;
    ssize_t n, nwritten = 0;

    /* Data compressed! Let's save it on disk */
    byte = (RDB_ENCVAL<<6)|RDB_ENC_LZF;
    if ((n = rdbWriteRaw(rdb,&byte,1)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbSaveLen(rdb,compress_len)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbSaveLen(rdb,original_len)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbWriteRaw(rdb,data,compress_len)) == -1) goto writeerr;
    nwritten += n;

    return nwritten;

writeerr:
    return -1;
}

ssize_t rdbSaveLzfStringObject(rio *rdb, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    void *out;

    /* We require at least four bytes compression for this to be worth it */
    if (len <= 4) return 0;
    outlen = len-4;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;
    comprlen = lzf_compress(s, len, out, outlen);
    if (comprlen == 0) {
        zfree(out);
        return 0;
    }
    ssize_t nwritten = rdbSaveLzfBlob(rdb, out, comprlen, len);
    zfree(out);
    return nwritten;
}

/* Load an LZF compressed string in RDB format. The returned value
 * changes according to 'flags'. For more info check the
 * rdbGenericLoadStringObject() function. */
void *rdbLoadLzfStringObject(rio *rdb, int flags, size_t *lenptr) {
    int plainFlag = flags & RDB_LOAD_PLAIN;
    int sdsFlag = flags & RDB_LOAD_SDS;
    int hfldFlag = flags & (RDB_LOAD_HFLD | RDB_LOAD_HFLD_TTL);
    int robjFlag = (!(plainFlag || sdsFlag || hfldFlag)); /* not plain/sds/hfld */

    uint64_t len, clen;
    unsigned char *c = NULL;
    char *val = NULL;

    if ((clen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    if ((c = ztrymalloc(clen)) == NULL) {
        serverLog(isRestoreContext()? LL_VERBOSE: LL_WARNING, "rdbLoadLzfStringObject failed allocating %llu bytes", (unsigned long long)clen);
        goto err;
    }

    /* Allocate our target according to the uncompressed size. */
    if (plainFlag) {
        val = ztrymalloc(len);
    } else if (sdsFlag || robjFlag) {
        val = sdstrynewlen(SDS_NOINIT,len);
    } else { /* hfldFlag */
        val = hfieldTryNew(NULL, len, (flags&RDB_LOAD_HFLD) ? 0 : 1);
    }

    if (!val) {
        serverLog(isRestoreContext()? LL_VERBOSE: LL_WARNING, "rdbLoadLzfStringObject failed allocating %llu bytes", (unsigned long long)len);
        goto err;
    }

    if (lenptr) *lenptr = len;

    /* Load the compressed representation and uncompress it to target. */
    if (rioRead(rdb,c,clen) == 0) goto err;
    if (lzf_decompress(c,clen,val,len) != len) {
        rdbReportCorruptRDB("Invalid LZF compressed string");
        goto err;
    }
    zfree(c);

    return (robjFlag) ? createObject(OBJ_STRING,val) : (void *) val;

err:
    zfree(c);
    if (plainFlag) {
        zfree(val);
    } else if (sdsFlag || robjFlag) {
        sdsfree(val);
    } else { /* hfldFlag*/
        hfieldFree(val);
    }
    return NULL;
}

/* Save a string object as [len][data] on disk. If the object is a string
 * representation of an integer value we try to save it in a special form */
ssize_t rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
    int enclen;
    ssize_t n, nwritten = 0;

    /* Try integer encoding */
    if (len <= 11) {
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
            if (rdbWriteRaw(rdb,buf,enclen) == -1) return -1;
            return enclen;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it */
    if (server.rdb_compression && len > 20) {
        n = rdbSaveLzfStringObject(rdb,s,len);
        if (n == -1) return -1;
        if (n > 0) return n;
        /* Return value of 0 means data can't be compressed, save the old way */
    }

    /* Store verbatim */
    if ((n = rdbSaveLen(rdb,len)) == -1) return -1;
    nwritten += n;
    if (len > 0) {
        if (rdbWriteRaw(rdb,s,len) == -1) return -1;
        nwritten += len;
    }
    return nwritten;
}

/* Save a long long value as either an encoded string or a string. */
ssize_t rdbSaveLongLongAsStringObject(rio *rdb, long long value) {
    unsigned char buf[32];
    ssize_t n, nwritten = 0;
    int enclen = rdbEncodeInteger(value,buf);
    if (enclen > 0) {
        return rdbWriteRaw(rdb,buf,enclen);
    } else {
        /* Encode as string */
        enclen = ll2string((char*)buf,32,value);
        serverAssert(enclen < 32);
        if ((n = rdbSaveLen(rdb,enclen)) == -1) return -1;
        nwritten += n;
        if ((n = rdbWriteRaw(rdb,buf,enclen)) == -1) return -1;
        nwritten += n;
    }
    return nwritten;
}

/* Like rdbSaveRawString() gets a Redis object instead. */
ssize_t rdbSaveStringObject(rio *rdb, robj *obj) {
    /* Avoid to decode the object, then encode it again, if the
     * object is already integer encoded. */
    if (obj->encoding == OBJ_ENCODING_INT) {
        return rdbSaveLongLongAsStringObject(rdb,(long)obj->ptr);
    } else {
        serverAssertWithInfo(NULL,obj,sdsEncodedObject(obj));
        return rdbSaveRawString(rdb,obj->ptr,sdslen(obj->ptr));
    }
}

/* Load a string object from an RDB file according to flags:
 *
 * RDB_LOAD_NONE (no flags): load an RDB object, unencoded.
 * RDB_LOAD_ENC: If the returned type is a Redis object, try to
 *               encode it in a special way to be more memory
 *               efficient. When this flag is passed the function
 *               no longer guarantees that obj->ptr is an SDS string.
 * RDB_LOAD_PLAIN: Return a plain string allocated with zmalloc()
 *                 instead of a Redis object with an sds in it.
 * RDB_LOAD_SDS: Return an SDS string instead of a Redis object.
 * RDB_LOAD_HFLD: Return a hash field object (mstr)
 * RDB_LOAD_HFLD_TTL: Return a hash field with TTL metadata reserved
 *
 * On I/O error NULL is returned.
 */
void *rdbGenericLoadStringObject(rio *rdb, int flags, size_t *lenptr) {
    void *buf;
    int plainFlag = flags & RDB_LOAD_PLAIN;
    int sdsFlag = flags & RDB_LOAD_SDS;
    int hfldFlag = flags & (RDB_LOAD_HFLD|RDB_LOAD_HFLD_TTL);
    int robjFlag = (!(plainFlag || sdsFlag || hfldFlag)); /* not plain/sds/hfld */

    int isencoded;
    unsigned long long len;

    len = rdbLoadLen(rdb,&isencoded);
    if (len == RDB_LENERR) return NULL;

    if (isencoded) {
        switch(len) {
        case RDB_ENC_INT8:
        case RDB_ENC_INT16:
        case RDB_ENC_INT32:
            return rdbLoadIntegerObject(rdb,len,flags,lenptr);
        case RDB_ENC_LZF:
            return rdbLoadLzfStringObject(rdb,flags,lenptr);
        default:
            rdbReportCorruptRDB("Unknown RDB string encoding type %llu",len);
            return NULL;
        }
    }

    /* return robj */
    if (robjFlag) {
        robj *o = tryCreateStringObject(SDS_NOINIT,len);
        if (!o) {
            serverLog(isRestoreContext()? LL_VERBOSE: LL_WARNING, "rdbGenericLoadStringObject failed allocating %llu bytes", len);
            return NULL;
        }
        if (len && rioRead(rdb,o->ptr,len) == 0) {
            decrRefCount(o);
            return NULL;
        }
        return o;
    }

    /* plain/sds/hfld */
    if (plainFlag) {
        buf = ztrymalloc(len);
    } else if (sdsFlag) {
        buf = sdstrynewlen(SDS_NOINIT,len);
    }  else { /* hfldFlag */
        buf = hfieldTryNew(NULL, len, (flags&RDB_LOAD_HFLD) ? 0 : 1);
    }
    if (!buf) {
        serverLog(isRestoreContext()? LL_VERBOSE: LL_WARNING, "rdbGenericLoadStringObject failed allocating %llu bytes", len);
        return NULL;
    }

    if (lenptr) *lenptr = len;
    if (len && rioRead(rdb,buf,len) == 0) {
        if (plainFlag)
            zfree(buf);
        else if (sdsFlag) {
            sdsfree(buf);
        } else { /* hfldFlag */
            hfieldFree(buf);
        }
        return NULL;
    }
    return buf;
}

robj *rdbLoadStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
}

robj *rdbLoadEncodedStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_ENC,NULL);
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifying the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 */
ssize_t rdbSaveDoubleValue(rio *rdb, double val) {
    unsigned char buf[128];
    int len;

    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
    } else {
        long long lvalue;
        /* Integer printing function is much faster, check if we can safely use it. */
        if (double2ll(val, &lvalue))
            ll2string((char*)buf+1,sizeof(buf)-1,lvalue);
        else {
            const int dlen = fpconv_dtoa(val, (char*)buf+1);
            buf[dlen+1] = '\0';
        }
        buf[0] = strlen((char*)buf+1);
        len = buf[0]+1;
    }
    return rdbWriteRaw(rdb,buf,len);
}

/* For information about double serialization check rdbSaveDoubleValue() */
int rdbLoadDoubleValue(rio *rdb, double *val) {
    char buf[256];
    unsigned char len;

    if (rioRead(rdb,&len,1) == 0) return -1;
    switch(len) {
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;
    default:
        if (rioRead(rdb,buf,len) == 0) return -1;
        buf[len] = '\0';
        if (sscanf(buf, "%lg", val)!=1) return -1;
        return 0;
    }
}

/* Saves a double for RDB 8 or greater, where IE754 binary64 format is assumed.
 * We just make sure the integer is always stored in little endian, otherwise
 * the value is copied verbatim from memory to disk.
 *
 * Return -1 on error, the size of the serialized value on success. */
int rdbSaveBinaryDoubleValue(rio *rdb, double val) {
    memrev64ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Loads a double from RDB 8 or greater. See rdbSaveBinaryDoubleValue() for
 * more info. On error -1 is returned, otherwise 0. */
int rdbLoadBinaryDoubleValue(rio *rdb, double *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev64ifbe(val);
    return 0;
}

/* Like rdbSaveBinaryDoubleValue() but single precision. */
int rdbSaveBinaryFloatValue(rio *rdb, float val) {
    memrev32ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Like rdbLoadBinaryDoubleValue() but single precision. */
int rdbLoadBinaryFloatValue(rio *rdb, float *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev32ifbe(val);
    return 0;
}

/* Save the object type of object "o". */
int rdbSaveObjectType(rio *rdb, robj *o) {
    switch (o->type) {
    case OBJ_STRING:
        return rdbSaveType(rdb,RDB_TYPE_STRING);
    case OBJ_LIST:
        if (o->encoding == OBJ_ENCODING_QUICKLIST || o->encoding == OBJ_ENCODING_LISTPACK)
            return rdbSaveType(rdb, RDB_TYPE_LIST_QUICKLIST_2);
        else
            serverPanic("Unknown list encoding");
    case OBJ_SET:
        if (o->encoding == OBJ_ENCODING_INTSET)
            return rdbSaveType(rdb,RDB_TYPE_SET_INTSET);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_SET);
        else if (o->encoding == OBJ_ENCODING_LISTPACK)
            return rdbSaveType(rdb,RDB_TYPE_SET_LISTPACK);
        else
            serverPanic("Unknown set encoding");
    case OBJ_ZSET:
        if (o->encoding == OBJ_ENCODING_LISTPACK)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_LISTPACK);
        else if (o->encoding == OBJ_ENCODING_SKIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_2);
        else
            serverPanic("Unknown sorted set encoding");
    case OBJ_HASH:
        if (o->encoding == OBJ_ENCODING_LISTPACK)
            return rdbSaveType(rdb,RDB_TYPE_HASH_LISTPACK);
        else if (o->encoding == OBJ_ENCODING_LISTPACK_EX)
            return rdbSaveType(rdb,RDB_TYPE_HASH_LISTPACK_EX);
        else if (o->encoding == OBJ_ENCODING_HT) {
            if (hashTypeGetMinExpire(o, /*accurate*/ 1) == EB_EXPIRE_TIME_INVALID)
                return rdbSaveType(rdb,RDB_TYPE_HASH);
            else
                return rdbSaveType(rdb,RDB_TYPE_HASH_METADATA);
        } else
            serverPanic("Unknown hash encoding");
    case OBJ_STREAM:
        return rdbSaveType(rdb,RDB_TYPE_STREAM_LISTPACKS_3);
    case OBJ_MODULE:
        return rdbSaveType(rdb,RDB_TYPE_MODULE_2);
    default:
        serverPanic("Unknown object type");
    }
    return -1; /* avoid warning */
}

/* Use rdbLoadType() to load a TYPE in RDB format, but returns -1 if the
 * type is not specifically a valid Object Type. */
int rdbLoadObjectType(rio *rdb) {
    int type;
    if ((type = rdbLoadType(rdb)) == -1) return -1;
    if (!rdbIsObjectType(type)) return -1;
    return type;
}

/* This helper function serializes a consumer group Pending Entries List (PEL)
 * into the RDB file. The 'nacks' argument tells the function if also persist
 * the information about the not acknowledged message, or if to persist
 * just the IDs: this is useful because for the global consumer group PEL
 * we serialized the NACKs as well, but when serializing the local consumer
 * PELs we just add the ID, that will be resolved inside the global PEL to
 * put a reference to the same structure. */
ssize_t rdbSaveStreamPEL(rio *rdb, rax *pel, int nacks) {
    ssize_t n, nwritten = 0;

    /* Number of entries in the PEL. */
    if ((n = rdbSaveLen(rdb,raxSize(pel))) == -1) return -1;
    nwritten += n;

    /* Save each entry. */
    raxIterator ri;
    raxStart(&ri,pel);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        /* We store IDs in raw form as 128 big big endian numbers, like
         * they are inside the radix tree key. */
        if ((n = rdbWriteRaw(rdb,ri.key,sizeof(streamID))) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        if (nacks) {
            streamNACK *nack = ri.data;
            if ((n = rdbSaveMillisecondTime(rdb,nack->delivery_time)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            if ((n = rdbSaveLen(rdb,nack->delivery_count)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            /* We don't save the consumer name: we'll save the pending IDs
             * for each consumer in the consumer PEL, and resolve the consumer
             * at loading time. */
        }
    }
    raxStop(&ri);
    return nwritten;
}

/* Serialize the consumers of a stream consumer group into the RDB. Helper
 * function for the stream data type serialization. What we do here is to
 * persist the consumer metadata, and it's PEL, for each consumer. */
size_t rdbSaveStreamConsumers(rio *rdb, streamCG *cg) {
    ssize_t n, nwritten = 0;

    /* Number of consumers in this consumer group. */
    if ((n = rdbSaveLen(rdb,raxSize(cg->consumers))) == -1) return -1;
    nwritten += n;

    /* Save each consumer. */
    raxIterator ri;
    raxStart(&ri,cg->consumers);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        streamConsumer *consumer = ri.data;

        /* Consumer name. */
        if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        /* Seen time. */
        if ((n = rdbSaveMillisecondTime(rdb,consumer->seen_time)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        /* Active time. */
        if ((n = rdbSaveMillisecondTime(rdb,consumer->active_time)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        /* Consumer PEL, without the ACKs (see last parameter of the function
         * passed with value of 0), at loading time we'll lookup the ID
         * in the consumer group global PEL and will put a reference in the
         * consumer local PEL. */
        if ((n = rdbSaveStreamPEL(rdb,consumer->pel,0)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;
    }
    raxStop(&ri);
    return nwritten;
}

/* Save a Redis object.
 * Returns -1 on error, number of bytes written on success. */
ssize_t rdbSaveObject(rio *rdb, robj *o, robj *key, int dbid) {
    ssize_t n = 0, nwritten = 0;

    if (o->type == OBJ_STRING) {
        /* Save a string value */
        if ((n = rdbSaveStringObject(rdb,o)) == -1) return -1;
        nwritten += n;
    } else if (o->type == OBJ_LIST) {
        /* Save a list value */
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
            quicklist *ql = o->ptr;
            quicklistNode *node = ql->head;

            if ((n = rdbSaveLen(rdb,ql->len)) == -1) return -1;
            nwritten += n;

            while(node) {
                if ((n = rdbSaveLen(rdb,node->container)) == -1) return -1;
                nwritten += n;

                if (quicklistNodeIsCompressed(node)) {
                    void *data;
                    size_t compress_len = quicklistGetLzf(node, &data);
                    if ((n = rdbSaveLzfBlob(rdb,data,compress_len,node->sz)) == -1) return -1;
                    nwritten += n;
                } else {
                    if ((n = rdbSaveRawString(rdb,node->entry,node->sz)) == -1) return -1;
                    nwritten += n;
                }
                node = node->next;
            }
        } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
            unsigned char *lp = o->ptr;

            /* Save list listpack as a fake quicklist that only has a single node. */
            if ((n = rdbSaveLen(rdb,1)) == -1) return -1;
            nwritten += n;
            if ((n = rdbSaveLen(rdb,QUICKLIST_NODE_CONTAINER_PACKED)) == -1) return -1;
            nwritten += n;
            if ((n = rdbSaveRawString(rdb,lp,lpBytes(lp))) == -1) return -1;
            nwritten += n;
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (o->type == OBJ_SET) {
        /* Save a set value */
        if (o->encoding == OBJ_ENCODING_HT) {
            dict *set = o->ptr;
            dictIterator *di = dictGetIterator(set);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize(set))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                sds ele = dictGetKey(de);
                if ((n = rdbSaveRawString(rdb,(unsigned char*)ele,sdslen(ele)))
                    == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
            size_t l = intsetBlobLen((intset*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
            size_t l = lpBytes((unsigned char *)o->ptr);
            if ((n = rdbSaveRawString(rdb, o->ptr, l)) == -1) return -1;
            nwritten += n;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (o->type == OBJ_ZSET) {
        /* Save a sorted set value */
        if (o->encoding == OBJ_ENCODING_LISTPACK) {
            size_t l = lpBytes((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = o->ptr;
            zskiplist *zsl = zs->zsl;

            if ((n = rdbSaveLen(rdb,zsl->length)) == -1) return -1;
            nwritten += n;

            /* We save the skiplist elements from the greatest to the smallest
             * (that's trivial since the elements are already ordered in the
             * skiplist): this improves the load process, since the next loaded
             * element will always be the smaller, so adding to the skiplist
             * will always immediately stop at the head, making the insertion
             * O(1) instead of O(log(N)). */
            zskiplistNode *zn = zsl->tail;
            while (zn != NULL) {
                if ((n = rdbSaveRawString(rdb,
                    (unsigned char*)zn->ele,sdslen(zn->ele))) == -1)
                {
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveBinaryDoubleValue(rdb,zn->score)) == -1)
                    return -1;
                nwritten += n;
                zn = zn->backward;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (o->type == OBJ_HASH) {
        /* Save a hash value */
        if ((o->encoding == OBJ_ENCODING_LISTPACK) ||
            (o->encoding == OBJ_ENCODING_LISTPACK_EX))
        {
            /* Save min/next HFE expiration time if needed */
            if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
                uint64_t minExpire = hashTypeGetMinExpire(o, 0);
                /* if invalid time then save 0 */
                if (minExpire == EB_EXPIRE_TIME_INVALID)
                    minExpire = 0;
                if (rdbSaveMillisecondTime(rdb, minExpire) == -1)
                    return -1;
            }
            unsigned char *lp_ptr = hashTypeListpackGetLp(o);
            size_t l = lpBytes(lp_ptr);

            if ((n = rdbSaveRawString(rdb,lp_ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == OBJ_ENCODING_HT) {
            int hashWithMeta = 0;  /* RDB_TYPE_HASH_METADATA */
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;
            /* Determine the hash layout to use based on the presence of at least
             * one field with a valid TTL. If such a field exists, employ the
             * RDB_TYPE_HASH_METADATA layout, including tuples of [ttl][field][value].
             * Otherwise, use the standard RDB_TYPE_HASH layout containing only
             * the tuples [field][value]. */
            uint64_t minExpire = hashTypeGetMinExpire(o, 1);

            /* if RDB_TYPE_HASH_METADATA (Can have TTLs on fields) */
            if (minExpire != EB_EXPIRE_TIME_INVALID) {
                hashWithMeta = 1;
                /* Save next field expire time of hash */
                if (rdbSaveMillisecondTime(rdb, minExpire) == -1) {
                    dictReleaseIterator(di);
                    return -1;
                }
            }

            /* save number of fields in hash */
            if ((n = rdbSaveLen(rdb,dictSize((dict*)o->ptr))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;

            /* save all hash fields */
            while((de = dictNext(di)) != NULL) {
                hfield field = dictGetKey(de);
                sds value = dictGetVal(de);

                /* save the TTL */
                if (hashWithMeta) {
                    uint64_t ttl, expiryTime= hfieldGetExpireTime(field);

                    /* Saved TTL value:
                     *  - 0: Indicates no TTL. This is common case so we keep it small.
                     *  - Otherwise: TTL is relative to minExpire (with +1 to avoid 0 that already taken)
                     */
                    ttl = (expiryTime == EB_EXPIRE_TIME_INVALID) ? 0 : expiryTime - minExpire + 1;
                    if ((n = rdbSaveLen(rdb, ttl)) == -1) {
                        dictReleaseIterator(di);
                        return -1;
                    }
                    nwritten += n;
                }

                /* save the key */
                if ((n = rdbSaveRawString(rdb,(unsigned char*)field,
                        hfieldlen(field))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;

                /* save the value */
                if ((n = rdbSaveRawString(rdb,(unsigned char*)value,
                        sdslen(value))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (o->type == OBJ_STREAM) {
        /* Store how many listpacks we have inside the radix tree. */
        stream *s = o->ptr;
        rax *rax = s->rax;
        if ((n = rdbSaveLen(rdb,raxSize(rax))) == -1) return -1;
        nwritten += n;

        /* Serialize all the listpacks inside the radix tree as they are,
         * when loading back, we'll use the first entry of each listpack
         * to insert it back into the radix tree. */
        raxIterator ri;
        raxStart(&ri,rax);
        raxSeek(&ri,"^",NULL,0);
        while (raxNext(&ri)) {
            unsigned char *lp = ri.data;
            size_t lp_bytes = lpBytes(lp);
            if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            if ((n = rdbSaveRawString(rdb,lp,lp_bytes)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
        }
        raxStop(&ri);

        /* Save the number of elements inside the stream. We cannot obtain
         * this easily later, since our macro nodes should be checked for
         * number of items: not a great CPU / space tradeoff. */
        if ((n = rdbSaveLen(rdb,s->length)) == -1) return -1;
        nwritten += n;
        /* Save the last entry ID. */
        if ((n = rdbSaveLen(rdb,s->last_id.ms)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->last_id.seq)) == -1) return -1;
        nwritten += n;
        /* Save the first entry ID. */
        if ((n = rdbSaveLen(rdb,s->first_id.ms)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->first_id.seq)) == -1) return -1;
        nwritten += n;
        /* Save the maximal tombstone ID. */
        if ((n = rdbSaveLen(rdb,s->max_deleted_entry_id.ms)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->max_deleted_entry_id.seq)) == -1) return -1;
        nwritten += n;
        /* Save the offset. */
        if ((n = rdbSaveLen(rdb,s->entries_added)) == -1) return -1;
        nwritten += n;

        /* The consumer groups and their clients are part of the stream
         * type, so serialize every consumer group. */

        /* Save the number of groups. */
        size_t num_cgroups = s->cgroups ? raxSize(s->cgroups) : 0;
        if ((n = rdbSaveLen(rdb,num_cgroups)) == -1) return -1;
        nwritten += n;

        if (num_cgroups) {
            /* Serialize each consumer group. */
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                streamCG *cg = ri.data;

                /* Save the group name. */
                if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Last ID. */
                if ((n = rdbSaveLen(rdb,cg->last_id.ms)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveLen(rdb,cg->last_id.seq)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;
                
                /* Save the group's logical reads counter. */
                if ((n = rdbSaveLen(rdb,cg->entries_read)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Save the global PEL. */
                if ((n = rdbSaveStreamPEL(rdb,cg->pel,1)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Save the consumers of this group. */
                if ((n = rdbSaveStreamConsumers(rdb,cg)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;
            }
            raxStop(&ri);
        }
    } else if (o->type == OBJ_MODULE) {
        /* Save a module-specific value. */
        RedisModuleIO io;
        moduleValue *mv = o->ptr;
        moduleType *mt = mv->type;

        /* Write the "module" identifier as prefix, so that we'll be able
         * to call the right module during loading. */
        int retval = rdbSaveLen(rdb,mt->id);
        if (retval == -1) return -1;
        moduleInitIOContext(io,mt,rdb,key,dbid);
        io.bytes += retval;

        /* Then write the module-specific representation + EOF marker. */
        mt->rdb_save(&io,mv->value);
        retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
        if (retval == -1)
            io.error = 1;
        else
            io.bytes += retval;

        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }
        return io.error ? -1 : (ssize_t)io.bytes;
    } else {
        serverPanic("Unknown object type");
    }
    return nwritten;
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. */
size_t rdbSavedObjectLen(robj *o, robj *key, int dbid) {
    ssize_t len = rdbSaveObject(NULL,o,key,dbid);
    serverAssertWithInfo(NULL,o,len != -1);
    return len;
}

/* Save a key-value pair, with expire time, type, key, value.
 * On error -1 is returned.
 * On success if the key was actually saved 1 is returned. */
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime, int dbid) {
    int savelru = server.maxmemory_policy & MAXMEMORY_FLAG_LRU;
    int savelfu = server.maxmemory_policy & MAXMEMORY_FLAG_LFU;

    /* Save the expire time */
    if (expiretime != -1) {
        if (rdbSaveType(rdb,RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;
    }

    /* Save the LRU info. */
    if (savelru) {
        uint64_t idletime = estimateObjectIdleTime(val);
        idletime /= 1000; /* Using seconds is enough and requires less space.*/
        if (rdbSaveType(rdb,RDB_OPCODE_IDLE) == -1) return -1;
        if (rdbSaveLen(rdb,idletime) == -1) return -1;
    }

    /* Save the LFU info. */
    if (savelfu) {
        uint8_t buf[1];
        buf[0] = LFUDecrAndReturn(val);
        /* We can encode this in exactly two bytes: the opcode and an 8
         * bit counter, since the frequency is logarithmic with a 0-255 range.
         * Note that we do not store the halving time because to reset it
         * a single time when loading does not affect the frequency much. */
        if (rdbSaveType(rdb,RDB_OPCODE_FREQ) == -1) return -1;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
    }

    /* Save type, key, value */
    if (rdbSaveObjectType(rdb,val) == -1) return -1;
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    if (rdbSaveObject(rdb,val,key,dbid) == -1) return -1;

    /* Delay return if required (for testing) */
    if (server.rdb_key_save_delay)
        debugDelay(server.rdb_key_save_delay);

    return 1;
}

/* Save an AUX field. */
ssize_t rdbSaveAuxField(rio *rdb, void *key, size_t keylen, void *val, size_t vallen) {
    ssize_t ret, len = 0;
    if ((ret = rdbSaveType(rdb,RDB_OPCODE_AUX)) == -1) return -1;
    len += ret;
    if ((ret = rdbSaveRawString(rdb,key,keylen)) == -1) return -1;
    len += ret;
    if ((ret = rdbSaveRawString(rdb,val,vallen)) == -1) return -1;
    len += ret;
    return len;
}

/* Wrapper for rdbSaveAuxField() used when key/val length can be obtained
 * with strlen(). */
ssize_t rdbSaveAuxFieldStrStr(rio *rdb, char *key, char *val) {
    return rdbSaveAuxField(rdb,key,strlen(key),val,strlen(val));
}

/* Wrapper for strlen(key) + integer type (up to long long range). */
ssize_t rdbSaveAuxFieldStrInt(rio *rdb, char *key, long long val) {
    char buf[LONG_STR_SIZE];
    int vlen = ll2string(buf,sizeof(buf),val);
    return rdbSaveAuxField(rdb,key,strlen(key),buf,vlen);
}

/* Save a few default AUX fields with information about the RDB generated. */
int rdbSaveInfoAuxFields(rio *rdb, int rdbflags, rdbSaveInfo *rsi) {
    int redis_bits = (sizeof(void*) == 8) ? 64 : 32;
    int aof_base = (rdbflags & RDBFLAGS_AOF_PREAMBLE) != 0;

    /* Add a few fields about the state when the RDB was created. */
    if (rdbSaveAuxFieldStrStr(rdb,"redis-ver",REDIS_VERSION) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"redis-bits",redis_bits) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"ctime",time(NULL)) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"used-mem",zmalloc_used_memory()) == -1) return -1;

    /* Handle saving options that generate aux fields. */
    if (rsi) {
        if (rdbSaveAuxFieldStrInt(rdb,"repl-stream-db",rsi->repl_stream_db)
            == -1) return -1;
        if (rdbSaveAuxFieldStrStr(rdb,"repl-id",server.replid)
            == -1) return -1;
        if (rdbSaveAuxFieldStrInt(rdb,"repl-offset",server.master_repl_offset)
            == -1) return -1;
    }
    if (rdbSaveAuxFieldStrInt(rdb, "aof-base", aof_base) == -1) return -1;
    return 1;
}

ssize_t rdbSaveSingleModuleAux(rio *rdb, int when, moduleType *mt) {
    /* Save a module-specific aux value. */
    RedisModuleIO io;
    int retval = 0;
    moduleInitIOContext(io,mt,rdb,NULL,-1);

    /* We save the AUX field header in a temporary buffer so we can support aux_save2 API.
     * If aux_save2 is used the buffer will be flushed at the first time the module will perform
     * a write operation to the RDB and will be ignored is case there was no writes. */
    rio aux_save_headers_rio;
    rioInitWithBuffer(&aux_save_headers_rio, sdsempty());

    if (rdbSaveType(&aux_save_headers_rio, RDB_OPCODE_MODULE_AUX) == -1) goto error;

    /* Write the "module" identifier as prefix, so that we'll be able
     * to call the right module during loading. */
    if (rdbSaveLen(&aux_save_headers_rio,mt->id) == -1) goto error;

    /* write the 'when' so that we can provide it on loading. add a UINT opcode
     * for backwards compatibility, everything after the MT needs to be prefixed
     * by an opcode. */
    if (rdbSaveLen(&aux_save_headers_rio,RDB_MODULE_OPCODE_UINT) == -1) goto error;
    if (rdbSaveLen(&aux_save_headers_rio,when) == -1) goto error;

    /* Then write the module-specific representation + EOF marker. */
    if (mt->aux_save2) {
        io.pre_flush_buffer = aux_save_headers_rio.io.buffer.ptr;
        mt->aux_save2(&io,when);
        if (io.pre_flush_buffer) {
            /* aux_save did not save any data to the RDB.
             * We will avoid saving any data related to this aux type
             * to allow loading this RDB if the module is not present. */
            sdsfree(io.pre_flush_buffer);
            io.pre_flush_buffer = NULL;
            return 0;
        }
    } else {
        /* Write headers now, aux_save does not do lazy saving of the headers. */
        retval = rdbWriteRaw(rdb, aux_save_headers_rio.io.buffer.ptr, sdslen(aux_save_headers_rio.io.buffer.ptr));
        if (retval == -1) goto error;
        io.bytes += retval;
        sdsfree(aux_save_headers_rio.io.buffer.ptr);
        mt->aux_save(&io,when);
    }
    retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
    serverAssert(!io.pre_flush_buffer);
    if (retval == -1)
        io.error = 1;
    else
        io.bytes += retval;

    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    if (io.error)
        return -1;
    return io.bytes;
error:
    sdsfree(aux_save_headers_rio.io.buffer.ptr);
    return -1;
}

ssize_t rdbSaveFunctions(rio *rdb) {
    dict *functions = functionsLibGet();
    dictIterator *iter = dictGetIterator(functions);
    dictEntry *entry = NULL;
    ssize_t written = 0;
    ssize_t ret;
    while ((entry = dictNext(iter))) {
        if ((ret = rdbSaveType(rdb, RDB_OPCODE_FUNCTION2)) < 0) goto werr;
        written += ret;
        functionLibInfo *li = dictGetVal(entry);
        if ((ret = rdbSaveRawString(rdb, (unsigned char *) li->code, sdslen(li->code))) < 0) goto werr;
        written += ret;
    }
    dictReleaseIterator(iter);
    return written;

werr:
    dictReleaseIterator(iter);
    return -1;
}

ssize_t rdbSaveDb(rio *rdb, int dbid, int rdbflags, long *key_counter) {
    dictEntry *de;
    ssize_t written = 0;
    ssize_t res;
    kvstoreIterator *kvs_it = NULL;
    static long long info_updated_time = 0;
    char *pname = (rdbflags & RDBFLAGS_AOF_PREAMBLE) ? "AOF rewrite" :  "RDB";

    redisDb *db = server.db + dbid;
    unsigned long long int db_size = kvstoreSize(db->keys);
    if (db_size == 0) return 0;

    /* Write the SELECT DB opcode */
    if ((res = rdbSaveType(rdb,RDB_OPCODE_SELECTDB)) < 0) goto werr;
    written += res;
    if ((res = rdbSaveLen(rdb, dbid)) < 0) goto werr;
    written += res;

    /* Write the RESIZE DB opcode. */
    unsigned long long expires_size = kvstoreSize(db->expires);
    if ((res = rdbSaveType(rdb,RDB_OPCODE_RESIZEDB)) < 0) goto werr;
    written += res;
    if ((res = rdbSaveLen(rdb,db_size)) < 0) goto werr;
    written += res;
    if ((res = rdbSaveLen(rdb,expires_size)) < 0) goto werr;
    written += res;

    kvs_it = kvstoreIteratorInit(db->keys);
    int last_slot = -1;
    /* Iterate this DB writing every entry */
    while ((de = kvstoreIteratorNext(kvs_it)) != NULL) {
        int curr_slot = kvstoreIteratorGetCurrentDictIndex(kvs_it);
        /* Save slot info. */
        if (server.cluster_enabled && curr_slot != last_slot) {
            if ((res = rdbSaveType(rdb, RDB_OPCODE_SLOT_INFO)) < 0) goto werr;
            written += res;
            if ((res = rdbSaveLen(rdb, curr_slot)) < 0) goto werr;
            written += res;
            if ((res = rdbSaveLen(rdb, kvstoreDictSize(db->keys, curr_slot))) < 0) goto werr;
            written += res;
            if ((res = rdbSaveLen(rdb, kvstoreDictSize(db->expires, curr_slot))) < 0) goto werr;
            written += res;
            last_slot = curr_slot;
        }
        sds keystr = dictGetKey(de);
        robj key, *o = dictGetVal(de);
        long long expire;
        size_t rdb_bytes_before_key = rdb->processed_bytes;

        initStaticStringObject(key,keystr);
        expire = getExpire(db,&key);
        if ((res = rdbSaveKeyValuePair(rdb, &key, o, expire, dbid)) < 0) goto werr;
        written += res;

        /* In fork child process, we can try to release memory back to the
         * OS and possibly avoid or decrease COW. We give the dismiss
         * mechanism a hint about an estimated size of the object we stored. */
        size_t dump_size = rdb->processed_bytes - rdb_bytes_before_key;
        if (server.in_fork_child) dismissObject(o, dump_size);

        /* Update child info every 1 second (approximately).
         * in order to avoid calling mstime() on each iteration, we will
         * check the diff every 1024 keys */
        if (((*key_counter)++ & 1023) == 0) {
            long long now = mstime();
            if (now - info_updated_time >= 1000) {
                sendChildInfo(CHILD_INFO_TYPE_CURRENT_INFO, *key_counter, pname);
                info_updated_time = now;
            }
        }
    }
    kvstoreIteratorRelease(kvs_it);
    return written;

werr:
    if (kvs_it) kvstoreIteratorRelease(kvs_it);
    return -1;
}

/* Produces a dump of the database in RDB format sending it to the specified
 * Redis I/O channel. On success C_OK is returned, otherwise C_ERR
 * is returned and part of the output, or all the output, can be
 * missing because of I/O errors.
 *
 * When the function returns C_ERR and if 'error' is not NULL, the
 * integer pointed by 'error' is set to the value of errno just after the I/O
 * error. */
int rdbSaveRio(int req, rio *rdb, int *error, int rdbflags, rdbSaveInfo *rsi) {
    char magic[10];
    uint64_t cksum;
    long key_counter = 0;
    int j;

    if (server.rdb_checksum)
        rdb->update_cksum = rioGenericUpdateChecksum;
    snprintf(magic,sizeof(magic),"REDIS%04d",RDB_VERSION);
    if (rdbWriteRaw(rdb,magic,9) == -1) goto werr;
    if (rdbSaveInfoAuxFields(rdb,rdbflags,rsi) == -1) goto werr;
    if (!(req & SLAVE_REQ_RDB_EXCLUDE_DATA) && rdbSaveModulesAux(rdb, REDISMODULE_AUX_BEFORE_RDB) == -1) goto werr;

    /* save functions */
    if (!(req & SLAVE_REQ_RDB_EXCLUDE_FUNCTIONS) && rdbSaveFunctions(rdb) == -1) goto werr;

    /* save all databases, skip this if we're in functions-only mode */
    if (!(req & SLAVE_REQ_RDB_EXCLUDE_DATA)) {
        for (j = 0; j < server.dbnum; j++) {
            if (rdbSaveDb(rdb, j, rdbflags, &key_counter) == -1) goto werr;
        }
    }

    if (!(req & SLAVE_REQ_RDB_EXCLUDE_DATA) && rdbSaveModulesAux(rdb, REDISMODULE_AUX_AFTER_RDB) == -1) goto werr;

    /* EOF opcode */
    if (rdbSaveType(rdb,RDB_OPCODE_EOF) == -1) goto werr;

    /* CRC64 checksum. It will be zero if checksum computation is disabled, the
     * loading code skips the check in this case. */
    cksum = rdb->cksum;
    memrev64ifbe(&cksum);
    if (rioWrite(rdb,&cksum,8) == 0) goto werr;
    return C_OK;

werr:
    if (error) *error = errno;
    return C_ERR;
}

/* This helper function is only used for diskless replication.
 * This is just a wrapper to rdbSaveRio() that additionally adds a prefix
 * and a suffix to the generated RDB dump. The prefix is:
 *
 * $EOF:<40 bytes unguessable hex string>\r\n
 *
 * While the suffix is the 40 bytes hex string we announced in the prefix.
 * This way processes receiving the payload can understand when it ends
 * without doing any processing of the content. */
int rdbSaveRioWithEOFMark(int req, rio *rdb, int *error, rdbSaveInfo *rsi) {
    char eofmark[RDB_EOF_MARK_SIZE];

    startSaving(RDBFLAGS_REPLICATION);
    getRandomHexChars(eofmark,RDB_EOF_MARK_SIZE);
    if (error) *error = 0;
    if (rioWrite(rdb,"$EOF:",5) == 0) goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    if (rioWrite(rdb,"\r\n",2) == 0) goto werr;
    if (rdbSaveRio(req,rdb,error,RDBFLAGS_REPLICATION,rsi) == C_ERR) goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    stopSaving(1);
    return C_OK;

werr: /* Write error. */
    /* Set 'error' only if not already set by rdbSaveRio() call. */
    if (error && *error == 0) *error = errno;
    stopSaving(0);
    return C_ERR;
}

static int rdbSaveInternal(int req, const char *filename, rdbSaveInfo *rsi, int rdbflags) {
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. */
    rio rdb;
    int error = 0;
    int saved_errno;
    char *err_op;    /* For a detailed log */

    FILE *fp = fopen(filename,"w");
    if (!fp) {
        saved_errno = errno;
        char *str_err = strerror(errno);
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Failed opening the temp RDB file %s (in server root dir %s) "
            "for saving: %s",
            filename,
            cwdp ? cwdp : "unknown",
            str_err);
        errno = saved_errno;
        return C_ERR;
    }

    rioInitWithFile(&rdb,fp);

    if (server.rdb_save_incremental_fsync) {
        rioSetAutoSync(&rdb,REDIS_AUTOSYNC_BYTES);
        if (!(rdbflags & RDBFLAGS_KEEP_CACHE)) rioSetReclaimCache(&rdb,1);
    }

    if (rdbSaveRio(req,&rdb,&error,rdbflags,rsi) == C_ERR) {
        errno = error;
        err_op = "rdbSaveRio";
        goto werr;
    }

    /* Make sure data will not remain on the OS's output buffers */
    if (fflush(fp)) { err_op = "fflush"; goto werr; }
    if (fsync(fileno(fp))) { err_op = "fsync"; goto werr; }
    if (!(rdbflags & RDBFLAGS_KEEP_CACHE) && reclaimFilePageCache(fileno(fp), 0, 0) == -1) {
        serverLog(LL_NOTICE,"Unable to reclaim cache after saving RDB: %s", strerror(errno));
    }
    if (fclose(fp)) { fp = NULL; err_op = "fclose"; goto werr; }

    return C_OK;

werr:
    saved_errno = errno;
    serverLog(LL_WARNING,"Write error while saving DB to the disk(%s): %s", err_op, strerror(errno));
    if (fp) fclose(fp);
    unlink(filename);
    errno = saved_errno;
    return C_ERR;
}

/* Save DB to the file. Similar to rdbSave() but this function won't use a
 * temporary file and won't update the metrics. */
int rdbSaveToFile(const char *filename) {
    startSaving(RDBFLAGS_NONE);

    if (rdbSaveInternal(SLAVE_REQ_NONE,filename,NULL,RDBFLAGS_NONE) != C_OK) {
        int saved_errno = errno;
        stopSaving(0);
        errno = saved_errno;
        return C_ERR;
    }

    stopSaving(1);
    return C_OK;
}

/* Save the DB on disk. Return C_ERR on error, C_OK on success. */
int rdbSave(int req, char *filename, rdbSaveInfo *rsi, int rdbflags) {
    char tmpfile[256];
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. */

    startSaving(rdbflags);
    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());

    if (rdbSaveInternal(req,tmpfile,rsi,rdbflags) != C_OK) {
        stopSaving(0);
        return C_ERR;
    }
    
    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        char *str_err = strerror(errno);
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Error moving temp DB file %s on the final "
            "destination %s (in server root dir %s): %s",
            tmpfile,
            filename,
            cwdp ? cwdp : "unknown",
            str_err);
        unlink(tmpfile);
        stopSaving(0);
        return C_ERR;
    }
    if (fsyncFileDir(filename) != 0) {
        serverLog(LL_WARNING,
            "Failed to fsync directory while saving DB: %s", strerror(errno));
        stopSaving(0);
        return C_ERR;
    }

    serverLog(LL_NOTICE,"DB saved on disk");
    server.dirty = 0;
    server.lastsave = time(NULL);
    server.lastbgsave_status = C_OK;
    stopSaving(1);
    return C_OK;
}

int rdbSaveBackground(int req, char *filename, rdbSaveInfo *rsi, int rdbflags) {
    pid_t childpid;

    if (hasActiveChildProcess()) return C_ERR;
    server.stat_rdb_saves++;

    server.dirty_before_bgsave = server.dirty;
    server.lastbgsave_try = time(NULL);

    if ((childpid = redisFork(CHILD_TYPE_RDB)) == 0) {
        int retval;

        /* Child */
        redisSetProcTitle("redis-rdb-bgsave");
        redisSetCpuAffinity(server.bgsave_cpulist);
        retval = rdbSave(req, filename,rsi,rdbflags);
        if (retval == C_OK) {
            sendChildCowInfo(CHILD_INFO_TYPE_RDB_COW_SIZE, "RDB");
        }
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent */
        if (childpid == -1) {
            server.lastbgsave_status = C_ERR;
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return C_ERR;
        }
        serverLog(LL_NOTICE,"Background saving started by pid %ld",(long) childpid);
        server.rdb_save_time_start = time(NULL);
        server.rdb_child_type = RDB_CHILD_TYPE_DISK;
        return C_OK;
    }
    return C_OK; /* unreached */
}

/* Note that we may call this function in signal handle 'sigShutdownHandler',
 * so we need guarantee all functions we call are async-signal-safe.
 * If we call this function from signal handle, we won't call bg_unlink that
 * is not async-signal-safe. */
void rdbRemoveTempFile(pid_t childpid, int from_signal) {
    char tmpfile[256];
    char pid[32];

    /* Generate temp rdb file name using async-signal safe functions. */
    ll2string(pid, sizeof(pid), childpid);
    redis_strlcpy(tmpfile, "temp-", sizeof(tmpfile));
    redis_strlcat(tmpfile, pid, sizeof(tmpfile));
    redis_strlcat(tmpfile, ".rdb", sizeof(tmpfile));

    if (from_signal) {
        /* bg_unlink is not async-signal-safe, but in this case we don't really
         * need to close the fd, it'll be released when the process exists. */
        int fd = open(tmpfile, O_RDONLY|O_NONBLOCK);
        UNUSED(fd);
        unlink(tmpfile);
    } else {
        bg_unlink(tmpfile);
    }
}

/* This function is called by rdbLoadObject() when the code is in RDB-check
 * mode and we find a module value of type 2 that can be parsed without
 * the need of the actual module. The value is parsed for errors, finally
 * a dummy redis object is returned just to conform to the API. */
robj *rdbLoadCheckModuleValue(rio *rdb, char *modulename) {
    uint64_t opcode;
    while((opcode = rdbLoadLen(rdb,NULL)) != RDB_MODULE_OPCODE_EOF) {
        if (opcode == RDB_MODULE_OPCODE_SINT ||
            opcode == RDB_MODULE_OPCODE_UINT)
        {
            uint64_t len;
            if (rdbLoadLenByRef(rdb,NULL,&len) == -1) {
                rdbReportCorruptRDB(
                    "Error reading integer from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_STRING) {
            robj *o = rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
            if (o == NULL) {
                rdbReportCorruptRDB(
                    "Error reading string from module %s value", modulename);
            }
            decrRefCount(o);
        } else if (opcode == RDB_MODULE_OPCODE_FLOAT) {
            float val;
            if (rdbLoadBinaryFloatValue(rdb,&val) == -1) {
                rdbReportCorruptRDB(
                    "Error reading float from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_DOUBLE) {
            double val;
            if (rdbLoadBinaryDoubleValue(rdb,&val) == -1) {
                rdbReportCorruptRDB(
                    "Error reading double from module %s value", modulename);
            }
        }
    }
    return createStringObject("module-dummy-value",18);
}

/* callback for hashZiplistConvertAndValidateIntegrity.
 * Check that the ziplist doesn't have duplicate hash field names.
 * The ziplist element pointed by 'p' will be converted and stored into listpack. */
static int _ziplistPairsEntryConvertAndValidate(unsigned char *p, unsigned int head_count, void *userdata) {
    unsigned char *str;
    unsigned int slen;
    long long vll;

    struct {
        long count;
        dict *fields;
        unsigned char **lp;
    } *data = userdata;

    if (data->fields == NULL) {
        data->fields = dictCreate(&hashDictType);
        dictExpand(data->fields, head_count/2);
    }

    if (!ziplistGet(p, &str, &slen, &vll))
        return 0;

    /* Even records are field names, add to dict and check that's not a dup */
    if (((data->count) & 1) == 0) {
        sds field = str? sdsnewlen(str, slen): sdsfromlonglong(vll);
        if (dictAdd(data->fields, field, NULL) != DICT_OK) {
            /* Duplicate, return an error */
            sdsfree(field);
            return 0;
        }
    }

    if (str) {
        *(data->lp) = lpAppend(*(data->lp), (unsigned char*)str, slen);
    } else {
        *(data->lp) = lpAppendInteger(*(data->lp), vll);
    }

    (data->count)++;
    return 1;
}

/* Validate the integrity of the data structure while converting it to 
 * listpack and storing it at 'lp'.
 * The function is safe to call on non-validated ziplists, it returns 0
 * when encounter an integrity validation issue. */
int ziplistPairsConvertAndValidateIntegrity(unsigned char *zl, size_t size, unsigned char **lp) {
    /* Keep track of the field names to locate duplicate ones */
    struct {
        long count;
        dict *fields; /* Initialisation at the first callback. */
        unsigned char **lp;
    } data = {0, NULL, lp};

    int ret = ziplistValidateIntegrity(zl, size, 1, _ziplistPairsEntryConvertAndValidate, &data);

    /* make sure we have an even number of records. */
    if (data.count & 1)
        ret = 0;

    if (data.fields) dictRelease(data.fields);
    return ret;
}

/* callback for ziplistValidateIntegrity.
 * The ziplist element pointed by 'p' will be converted and stored into listpack. */
static int _ziplistEntryConvertAndValidate(unsigned char *p, unsigned int head_count, void *userdata) {
    UNUSED(head_count);
    unsigned char *str;
    unsigned int slen;
    long long vll;
    unsigned char **lp = (unsigned char**)userdata;

    if (!ziplistGet(p, &str, &slen, &vll)) return 0;

    if (str)
        *lp = lpAppend(*lp, (unsigned char*)str, slen);
    else
        *lp = lpAppendInteger(*lp, vll);

    return 1;
}

/* callback for ziplistValidateIntegrity.
 * The ziplist element pointed by 'p' will be converted and stored into quicklist. */
static int _listZiplistEntryConvertAndValidate(unsigned char *p, unsigned int head_count, void *userdata) {
    UNUSED(head_count);
    unsigned char *str;
    unsigned int slen;
    long long vll;
    char longstr[32] = {0};
    quicklist *ql = (quicklist*)userdata;

    if (!ziplistGet(p, &str, &slen, &vll)) return 0;
    if (!str) {
        /* Write the longval as a string so we can re-add it */
        slen = ll2string(longstr, sizeof(longstr), vll);
        str = (unsigned char *)longstr;
    }
    quicklistPushTail(ql, str, slen);
    return 1;
}

/* callback for to check the listpack doesn't have duplicate records */
static int _lpEntryValidation(unsigned char *p, unsigned int head_count, void *userdata) {
    struct {
        int tuple_len;
        long count;
        dict *fields;
        long long last_expireat;
    } *data = userdata;

    if (data->fields == NULL) {
        data->fields = dictCreate(&hashDictType);
        dictExpand(data->fields, head_count/data->tuple_len);
    }

    /* If we're checking pairs, then even records are field names. Otherwise
     * we're checking all elements. Add to dict and check that's not a dup */
    if (data->count % data->tuple_len == 0) {
        unsigned char *str;
        int64_t slen;
        unsigned char buf[LP_INTBUF_SIZE];

        str = lpGet(p, &slen, buf);
        sds field = sdsnewlen(str, slen);
        if (dictAdd(data->fields, field, NULL) != DICT_OK) {
            /* Duplicate, return an error */
            sdsfree(field);
            return 0;
        }
    }

    /* Validate TTL field, only for listpackex. */
    if (data->count % data->tuple_len == 2) {
        long long expire_at;
        /* Must be an integer. */
        if (!lpGetIntegerValue(p, &expire_at)) return 0;
        /* Must be less than EB_EXPIRE_TIME_MAX. */
        if (expire_at < 0 || (unsigned long long)expire_at > EB_EXPIRE_TIME_MAX) return 0;
        /* TTL fields are ordered. If the current field has TTL, the previous field must
         * also have one, and the current TTL must be greater than the previous one. */
        if (expire_at != 0 && (data->last_expireat == 0 || expire_at < data->last_expireat)) return 0;
        data->last_expireat = expire_at;
    }

    (data->count)++;
    return 1;
}

/* Validate the integrity of the listpack structure.
 * when `deep` is 0, only the integrity of the header is validated.
 * when `deep` is 1, we scan all the entries one by one.
 * tuple_len indicates what is a logical entry tuple size.
 * Whether tuple is of size 1 (set), 2 (feild-value) or 3 (field-value[-ttl]),
 * first element in the tuple must be unique */
int lpValidateIntegrityAndDups(unsigned char *lp, size_t size, int deep, int tuple_len) {
    if (!deep)
        return lpValidateIntegrity(lp, size, 0, NULL, NULL);

    /* Keep track of the field names to locate duplicate ones */
    struct {
        int tuple_len;
        long count;
        dict *fields; /* Initialisation at the first callback. */
        long long last_expireat; /* Last field's expiry time to ensure order in TTL fields. */
    } data = {tuple_len, 0, NULL, -1};

    int ret = lpValidateIntegrity(lp, size, 1, _lpEntryValidation, &data);

    /* the number of records should be a multiple of the tuple length */
    if (data.count % tuple_len != 0)
        ret = 0;

    if (data.fields) dictRelease(data.fields);
    return ret;
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL.
 *
 * error - When the function returns NULL and if 'error' is not NULL, the
 *   integer pointed by 'error' is set to the type of error that occurred
 * minExpiredField - If loading a hash with expiration on fields, then this value
 *   will be set to the minimum expire time found in the hash fields. If there are
 *   no fields with expiration or it is not a hash, then it will set be to
 *   EB_EXPIRE_TIME_INVALID.
 */
robj *rdbLoadObject(int rdbtype, rio *rdb, sds key, int dbid, int *error)
{
    robj *o = NULL, *ele, *dec;
    uint64_t len;
    unsigned int i;

    /* Set default error of load object, it will be set to 0 on success. */
    if (error) *error = RDB_LOAD_ERR_OTHER;

    int deep_integrity_validation = server.sanitize_dump_payload == SANITIZE_DUMP_YES;
    if (server.sanitize_dump_payload == SANITIZE_DUMP_CLIENTS) {
        /* Skip sanitization when loading (an RDB), or getting a RESTORE command
         * from either the master or a client using an ACL user with the skip-sanitize-payload flag. */
        int skip = server.loading ||
            (server.current_client && (server.current_client->flags & CLIENT_MASTER));
        if (!skip && server.current_client && server.current_client->user)
            skip = !!(server.current_client->user->flags & USER_FLAG_SANITIZE_PAYLOAD_SKIP);
        deep_integrity_validation = !skip;
    }

    if (rdbtype == RDB_TYPE_STRING) {
        /* Read string value */
        if ((o = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
        o = tryObjectEncodingEx(o, 0);
    } else if (rdbtype == RDB_TYPE_LIST) {
        /* Read list value */
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        if (len == 0) goto emptykey;

        o = createQuicklistObject(server.list_max_listpack_size, server.list_compress_depth);

        /* Load every single element of the list */
        while(len--) {
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            dec = getDecodedObject(ele);
            size_t len = sdslen(dec->ptr);
            quicklistPushTail(o->ptr, dec->ptr, len);
            decrRefCount(dec);
            decrRefCount(ele);
        }

        listTypeTryConversion(o, LIST_CONV_AUTO, NULL, NULL);
    } else if (rdbtype == RDB_TYPE_SET) {
        /* Read Set value */
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        if (len == 0) goto emptykey;

        /* Use a regular set when there are too many entries. */
        size_t max_entries = server.set_max_intset_entries;
        if (max_entries >= 1<<30) max_entries = 1<<30;
        if (len > max_entries) {
            o = createSetObject();
            /* It's faster to expand the dict to the right size asap in order
             * to avoid rehashing */
            if (len > DICT_HT_INITIAL_SIZE && dictTryExpand(o->ptr, len) != DICT_OK) {
                rdbReportCorruptRDB("OOM in dictTryExpand %llu", (unsigned long long)len);
                decrRefCount(o);
                return NULL;
            }
        } else {
            o = createIntsetObject();
        }

        /* Load every single element of the set */
        size_t maxelelen = 0, sumelelen = 0;
        for (i = 0; i < len; i++) {
            long long llval;
            sds sdsele;

            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            size_t elelen = sdslen(sdsele);
            sumelelen += elelen;
            if (elelen > maxelelen) maxelelen = elelen;

            if (o->encoding == OBJ_ENCODING_INTSET) {
                /* Fetch integer value from element. */
                if (isSdsRepresentableAsLongLong(sdsele,&llval) == C_OK) {
                    uint8_t success;
                    o->ptr = intsetAdd(o->ptr, llval, &success);
                    if (!success) {
                        rdbReportCorruptRDB("Duplicate set members detected");
                        decrRefCount(o);
                        sdsfree(sdsele);
                        return NULL;
                    }
                } else if (setTypeSize(o) < server.set_max_listpack_entries &&
                           maxelelen <= server.set_max_listpack_value &&
                           lpSafeToAdd(NULL, sumelelen))
                {
                    /* We checked if it's safe to add one large element instead
                     * of many small ones. It's OK since lpSafeToAdd doesn't
                     * care about individual elements, only the total size. */
                    setTypeConvert(o, OBJ_ENCODING_LISTPACK);
                } else if (setTypeConvertAndExpand(o, OBJ_ENCODING_HT, len, 0) != C_OK) {
                    rdbReportCorruptRDB("OOM in dictTryExpand %llu", (unsigned long long)len);
                    sdsfree(sdsele);
                    decrRefCount(o);
                    return NULL;
                }
            }

            /* This will also be called when the set was just converted
             * to a listpack encoded set. */
            if (o->encoding == OBJ_ENCODING_LISTPACK) {
                if (setTypeSize(o) < server.set_max_listpack_entries &&
                    elelen <= server.set_max_listpack_value &&
                    lpSafeToAdd(o->ptr, elelen))
                {
                    unsigned char *p = lpFirst(o->ptr);
                    if (p && lpFind(o->ptr, p, (unsigned char*)sdsele, elelen, 0)) {
                        rdbReportCorruptRDB("Duplicate set members detected");
                        decrRefCount(o);
                        sdsfree(sdsele);
                        return NULL;
                    }
                    o->ptr = lpAppend(o->ptr, (unsigned char *)sdsele, elelen);
                } else if (setTypeConvertAndExpand(o, OBJ_ENCODING_HT, len, 0) != C_OK) {
                    rdbReportCorruptRDB("OOM in dictTryExpand %llu",
                                        (unsigned long long)len);
                    sdsfree(sdsele);
                    decrRefCount(o);
                    return NULL;
                }
            }

            /* This will also be called when the set was just converted
             * to a regular hash table encoded set. */
            if (o->encoding == OBJ_ENCODING_HT) {
                if (dictAdd((dict*)o->ptr, sdsele, NULL) != DICT_OK) {
                    rdbReportCorruptRDB("Duplicate set members detected");
                    decrRefCount(o);
                    sdsfree(sdsele);
                    return NULL;
                }
            } else {
                sdsfree(sdsele);
            }
        }
    } else if (rdbtype == RDB_TYPE_ZSET_2 || rdbtype == RDB_TYPE_ZSET) {
        /* Read sorted set value. */
        uint64_t zsetlen;
        size_t maxelelen = 0, totelelen = 0;
        zset *zs;

        if ((zsetlen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        if (zsetlen == 0) goto emptykey;

        o = createZsetObject();
        zs = o->ptr;

        if (zsetlen > DICT_HT_INITIAL_SIZE && dictTryExpand(zs->dict,zsetlen) != DICT_OK) {
            rdbReportCorruptRDB("OOM in dictTryExpand %llu", (unsigned long long)zsetlen);
            decrRefCount(o);
            return NULL;
        }

        /* Load every single element of the sorted set. */
        while(zsetlen--) {
            sds sdsele;
            double score;
            zskiplistNode *znode;

            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }

            if (rdbtype == RDB_TYPE_ZSET_2) {
                if (rdbLoadBinaryDoubleValue(rdb,&score) == -1) {
                    decrRefCount(o);
                    sdsfree(sdsele);
                    return NULL;
                }
            } else {
                if (rdbLoadDoubleValue(rdb,&score) == -1) {
                    decrRefCount(o);
                    sdsfree(sdsele);
                    return NULL;
                }
            }

            if (isnan(score)) {
                rdbReportCorruptRDB("Zset with NAN score detected");
                decrRefCount(o);
                sdsfree(sdsele);
                return NULL;
            }

            /* Don't care about integer-encoded strings. */
            if (sdslen(sdsele) > maxelelen) maxelelen = sdslen(sdsele);
            totelelen += sdslen(sdsele);

            znode = zslInsert(zs->zsl,score,sdsele);
            if (dictAdd(zs->dict,sdsele,&znode->score) != DICT_OK) {
                rdbReportCorruptRDB("Duplicate zset fields detected");
                decrRefCount(o);
                /* no need to free 'sdsele', will be released by zslFree together with 'o' */
                return NULL;
            }
        }

        /* Convert *after* loading, since sorted sets are not stored ordered. */
        if (zsetLength(o) <= server.zset_max_listpack_entries &&
            maxelelen <= server.zset_max_listpack_value &&
            lpSafeToAdd(NULL, totelelen))
        {
            zsetConvert(o, OBJ_ENCODING_LISTPACK);
        }
    } else if (rdbtype == RDB_TYPE_HASH) {
        uint64_t len;
        int ret;
        sds value;
        hfield field;
        dict *dupSearchDict = NULL;

        len = rdbLoadLen(rdb, NULL);
        if (len == RDB_LENERR) return NULL;
        if (len == 0) goto emptykey;

        o = createHashObject();

        /* Too many entries? Use a hash table right from the start. */
        if (len > server.hash_max_listpack_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT, NULL);
        else if (deep_integrity_validation) {
            /* In this mode, we need to guarantee that the server won't crash
             * later when the ziplist is converted to a dict.
             * Create a set (dict with no values) to for a dup search.
             * We can dismiss it as soon as we convert the ziplist to a hash. */
            dupSearchDict = dictCreate(&hashDictType);
        }

        /* Load every field and value into the listpack */
        while (o->encoding == OBJ_ENCODING_LISTPACK && len > 0) {
            len--;
            /* Load raw strings */
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_HFLD,NULL)) == NULL) {
                decrRefCount(o);
                if (dupSearchDict) dictRelease(dupSearchDict);
                return NULL;
            }
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                hfieldFree(field);
                decrRefCount(o);
                if (dupSearchDict) dictRelease(dupSearchDict);
                return NULL;
            }

            if (dupSearchDict) {
                sds field_dup = sdsnewlen(field, hfieldlen(field));

                if (dictAdd(dupSearchDict, field_dup, NULL) != DICT_OK) {
                    rdbReportCorruptRDB("Hash with dup elements");
                    dictRelease(dupSearchDict);
                    decrRefCount(o);
                    sdsfree(field_dup);
                    hfieldFree(field);
                    sdsfree(value);
                    return NULL;
                }
            }

            /* Convert to hash table if size threshold is exceeded */
            if (hfieldlen(field) > server.hash_max_listpack_value ||
                sdslen(value) > server.hash_max_listpack_value ||
                !lpSafeToAdd(o->ptr, hfieldlen(field) + sdslen(value)))
            {
                hashTypeConvert(o, OBJ_ENCODING_HT, NULL);
                dictUseStoredKeyApi((dict *)o->ptr, 1);
                ret = dictAdd((dict*)o->ptr, field, value);
                dictUseStoredKeyApi((dict *)o->ptr, 0);
                if (ret == DICT_ERR) {
                    rdbReportCorruptRDB("Duplicate hash fields detected");
                    if (dupSearchDict) dictRelease(dupSearchDict);
                    sdsfree(value);
                    hfieldFree(field);
                    decrRefCount(o);
                    return NULL;
                }
                break;
            }

            /* Add pair to listpack */
            o->ptr = lpAppend(o->ptr, (unsigned char*)field, hfieldlen(field));
            o->ptr = lpAppend(o->ptr, (unsigned char*)value, sdslen(value));

            hfieldFree(field);
            sdsfree(value);
        }

        if (dupSearchDict) {
            /* We no longer need this, from now on the entries are added
             * to a dict so the check is performed implicitly. */
            dictRelease(dupSearchDict);
            dupSearchDict = NULL;
        }

        if (o->encoding == OBJ_ENCODING_HT && len > DICT_HT_INITIAL_SIZE) {
            if (dictTryExpand(o->ptr, len) != DICT_OK) {
                rdbReportCorruptRDB("OOM in dictTryExpand %llu", (unsigned long long)len);
                decrRefCount(o);
                return NULL;
            }
        }

        /* Load remaining fields and values into the hash table */
        while (o->encoding == OBJ_ENCODING_HT && len > 0) {
            len--;
            /* Load encoded strings */
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_HFLD,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                hfieldFree(field);
                decrRefCount(o);
                return NULL;
            }

            /* Add pair to hash table */
            dict *d = o->ptr;
            dictUseStoredKeyApi(d, 1);
            ret = dictAdd(d, field, value);
            dictUseStoredKeyApi(d, 0);
            if (ret == DICT_ERR) {
                rdbReportCorruptRDB("Duplicate hash fields detected");
                sdsfree(value);
                hfieldFree(field);
                decrRefCount(o);
                return NULL;
            }
        }

        /* All pairs should be read by now */
        serverAssert(len == 0);
    } else if (rdbtype == RDB_TYPE_HASH_METADATA || rdbtype == RDB_TYPE_HASH_METADATA_PRE_GA) {
        sds value;
        hfield field;
        uint64_t ttl, expireAt, minExpire = EB_EXPIRE_TIME_INVALID;
        dict *dupSearchDict = NULL;

        /* If hash with TTLs, load next/min expiration time
         *
         * - This value is serialized for future use-case of streaming the object
         *   directly to FLASH (while keeping in mem its next expiration time).
         * - It is also being used to keep only relative TTL for fields in RDB file.
         */
        if (rdbtype == RDB_TYPE_HASH_METADATA) {
            minExpire = rdbLoadMillisecondTime(rdb, RDB_VERSION);
            if (rioGetReadError(rdb)) {
                rdbReportCorruptRDB("Hash failed loading minExpire");
                return NULL;
            }
            if (minExpire > EB_EXPIRE_TIME_INVALID) {
                rdbReportCorruptRDB("Hash read invalid minExpire value");
            }
        }

        len = rdbLoadLen(rdb, NULL);
        if (len == RDB_LENERR) return NULL;
        if (len == 0) goto emptykey;
        /* TODO: create listpackEx or HT directly*/
        o = createHashObject();
        /* Too many entries? Use a hash table right from the start. */
        if (len > server.hash_max_listpack_entries) {
            hashTypeConvert(o, OBJ_ENCODING_HT, NULL);
            dictTypeAddMeta((dict**)&o->ptr, &mstrHashDictTypeWithHFE);
            initDictExpireMetadata(key, o);
        } else {
            hashTypeConvert(o, OBJ_ENCODING_LISTPACK_EX, NULL);
            if (deep_integrity_validation) {
                /* In this mode, we need to guarantee that the server won't crash
                * later when the listpack is converted to a dict.
                * Create a set (dict with no values) for dup search.
                * We can dismiss it as soon as we convert the listpack to a hash. */
                dupSearchDict = dictCreate(&hashDictType);
            }
        }

        while (len > 0) {
            len--;

            /* read the TTL */
            if (rdbLoadLenByRef(rdb, NULL, &ttl) == -1) {
                serverLog(LL_WARNING, "failed reading hash TTL");
                decrRefCount(o);
                if (dupSearchDict != NULL) dictRelease(dupSearchDict);
                return NULL;
            }


            if (rdbtype == RDB_TYPE_HASH_METADATA) {
                /* Loaded TTL value:
                 *  - 0: Indicates no TTL. This is common case so we keep it small.
                 *  - Otherwise: TTL is relative to minExpire (with +1 to avoid 0 that already taken)
                 */
                expireAt = (ttl != 0) ? (ttl + minExpire - 1) : 0;
            } else { /* RDB_TYPE_HASH_METADATA_PRE_GA */
                expireAt = ttl; /* Value is absolute */
            }

            if (expireAt > EB_EXPIRE_TIME_MAX) {
                rdbReportCorruptRDB("invalid expireAt time: %llu",
                                    (unsigned long long) expireAt);
                decrRefCount(o);
                if (dupSearchDict != NULL) dictRelease(dupSearchDict);
                return NULL;
            }

            /* if needed create field with TTL metadata  */
            if (expireAt !=0)
                field = rdbGenericLoadStringObject(rdb, RDB_LOAD_HFLD_TTL, NULL);
            else
                field = rdbGenericLoadStringObject(rdb, RDB_LOAD_HFLD, NULL);

            if (field == NULL) {
                serverLog(LL_WARNING, "failed reading hash field");
                decrRefCount(o);
                if (dupSearchDict != NULL) dictRelease(dupSearchDict);
                return NULL;
            }

            /* read the value */
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                serverLog(LL_WARNING, "failed reading hash value");
                decrRefCount(o);
                if (dupSearchDict != NULL) dictRelease(dupSearchDict);
                hfieldFree(field);
                return NULL;
            }

            /* store the values read - either to listpack or dict */
            if (o->encoding == OBJ_ENCODING_LISTPACK_EX) {
                /* integrity - check for key duplication (if required) */
                if (dupSearchDict) {
                    sds field_dup = sdsnewlen(field, hfieldlen(field));

                    if (dictAdd(dupSearchDict, field_dup, NULL) != DICT_OK) {
                        rdbReportCorruptRDB("Hash with dup elements");
                        dictRelease(dupSearchDict);
                        decrRefCount(o);
                        sdsfree(field_dup);
                        sdsfree(value);
                        hfieldFree(field);
                        return NULL;
                    }
                }

                /* check if the values can be saved to listpack (or should convert to dict encoding) */
                if (hfieldlen(field) > server.hash_max_listpack_value ||
                    sdslen(value) > server.hash_max_listpack_value ||
                    !lpSafeToAdd(((listpackEx*)o->ptr)->lp, hfieldlen(field) + sdslen(value) + lpEntrySizeInteger(expireAt)))
                {
                    /* convert to hash */
                    hashTypeConvert(o, OBJ_ENCODING_HT, NULL);

                    if (len > DICT_HT_INITIAL_SIZE) { /* TODO: this is NOT the original len, but this is also the case for simple hash, is this a bug? */
                        if (dictTryExpand(o->ptr, len) != DICT_OK) {
                            rdbReportCorruptRDB("OOM in dictTryExpand %llu", (unsigned long long)len);
                            decrRefCount(o);
                            if (dupSearchDict != NULL) dictRelease(dupSearchDict);
                            sdsfree(value);
                            hfieldFree(field);
                            return NULL;
                        }
                    }

                    /* don't add the values to the new hash: the next if will catch and the values will be added there */
                } else {
                    listpackExAddNew(o, field, hfieldlen(field),
                                     value, sdslen(value), expireAt);
                    hfieldFree(field);
                    sdsfree(value);
                }
            }

            if (o->encoding == OBJ_ENCODING_HT) {
                /* Add pair to hash table */
                dict *d = o->ptr;
                dictUseStoredKeyApi(d, 1);
                int ret = dictAdd(d, field, value);
                dictUseStoredKeyApi(d, 0);

                /* Attach expiry to the hash field and register in hash private HFE DS */
                if ((ret != DICT_ERR) && expireAt) {
                    dictExpireMetadata *m = (dictExpireMetadata *) dictMetadata(d);
                    ret = ebAdd(&m->hfe, &hashFieldExpireBucketsType, field, expireAt);
                }

                if (ret == DICT_ERR) {
                    rdbReportCorruptRDB("Duplicate hash fields detected");
                    sdsfree(value);
                    hfieldFree(field);
                    decrRefCount(o);
                    return NULL;
                }
            }
        }

        if (dupSearchDict != NULL) dictRelease(dupSearchDict);

    } else if (rdbtype == RDB_TYPE_LIST_QUICKLIST || rdbtype == RDB_TYPE_LIST_QUICKLIST_2) {
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        if (len == 0) goto emptykey;

        o = createQuicklistObject(server.list_max_listpack_size, server.list_compress_depth);
        uint64_t container = QUICKLIST_NODE_CONTAINER_PACKED;
        while (len--) {
            unsigned char *lp;
            size_t encoded_len;

            if (rdbtype == RDB_TYPE_LIST_QUICKLIST_2) {
                if ((container = rdbLoadLen(rdb,NULL)) == RDB_LENERR) {
                    decrRefCount(o);
                    return NULL;
                }

                if (container != QUICKLIST_NODE_CONTAINER_PACKED && container != QUICKLIST_NODE_CONTAINER_PLAIN) {
                    rdbReportCorruptRDB("Quicklist integrity check failed.");
                    decrRefCount(o);
                    return NULL;
                }
            }

            unsigned char *data =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,&encoded_len);
            if (data == NULL || (encoded_len == 0)) {
                zfree(data);
                decrRefCount(o);
                return NULL;
            }

            if (container == QUICKLIST_NODE_CONTAINER_PLAIN) {
                quicklistAppendPlainNode(o->ptr, data, encoded_len);
                continue;
            }

            if (rdbtype == RDB_TYPE_LIST_QUICKLIST_2) {
                lp = data;
                if (deep_integrity_validation) server.stat_dump_payload_sanitizations++;
                if (!lpValidateIntegrity(lp, encoded_len, deep_integrity_validation, NULL, NULL)) {
                    rdbReportCorruptRDB("Listpack integrity check failed.");
                    decrRefCount(o);
                    zfree(lp);
                    return NULL;
                }
            } else {
                lp = lpNew(encoded_len);
                if (!ziplistValidateIntegrity(data, encoded_len, 1,
                        _ziplistEntryConvertAndValidate, &lp))
                {
                    rdbReportCorruptRDB("Ziplist integrity check failed.");
                    decrRefCount(o);
                    zfree(data);
                    zfree(lp);
                    return NULL;
                }
                zfree(data);
                lp = lpShrinkToFit(lp);
            }

            /* Silently skip empty ziplists, if we'll end up with empty quicklist we'll fail later. */
            if (lpLength(lp) == 0) {
                zfree(lp);
                continue;
            } else {
                quicklistAppendListpack(o->ptr, lp);
            }
        }

        if (quicklistCount(o->ptr) == 0) {
            decrRefCount(o);
            goto emptykey;
        }

        listTypeTryConversion(o, LIST_CONV_AUTO, NULL, NULL);
    } else if (rdbtype == RDB_TYPE_HASH_ZIPMAP  ||
               rdbtype == RDB_TYPE_LIST_ZIPLIST ||
               rdbtype == RDB_TYPE_SET_INTSET   ||
               rdbtype == RDB_TYPE_SET_LISTPACK ||
               rdbtype == RDB_TYPE_ZSET_ZIPLIST ||
               rdbtype == RDB_TYPE_ZSET_LISTPACK ||
               rdbtype == RDB_TYPE_HASH_ZIPLIST ||
               rdbtype == RDB_TYPE_HASH_LISTPACK ||
               rdbtype == RDB_TYPE_HASH_LISTPACK_EX_PRE_GA ||
               rdbtype == RDB_TYPE_HASH_LISTPACK_EX)
    {
        size_t encoded_len;

        /* If Hash TTLs, Load next/min expiration time before the `encoded` */
        if (rdbtype == RDB_TYPE_HASH_LISTPACK_EX) {
            uint64_t minExpire = rdbLoadMillisecondTime(rdb, RDB_VERSION);
            /* This value was serialized for future use-case of streaming the object
             * directly to FLASH (while keeping in mem its next expiration time) */
            UNUSED(minExpire);
            if (rioGetReadError(rdb)) {
                rdbReportCorruptRDB( "Hash listpackex integrity check failed.");
                return NULL;
            }
        }

        unsigned char *encoded =
            rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,&encoded_len);
        if (encoded == NULL) return NULL;

        o = createObject(OBJ_STRING, encoded); /* Obj type fixed below. */

        /* Fix the object encoding, and make sure to convert the encoded
         * data type into the base type if accordingly to the current
         * configuration there are too many elements in the encoded data
         * type. Note that we only check the length and not max element
         * size as this is an O(N) scan. Eventually everything will get
         * converted. */
        switch(rdbtype) {
            case RDB_TYPE_HASH_ZIPMAP:
                /* Since we don't keep zipmaps anymore, the rdb loading for these
                 * is O(n) anyway, use `deep` validation. */
                if (!zipmapValidateIntegrity(encoded, encoded_len, 1)) {
                    rdbReportCorruptRDB("Zipmap integrity check failed.");
                    zfree(encoded);
                    o->ptr = NULL;
                    decrRefCount(o);
                    return NULL;
                }
                /* Convert to ziplist encoded hash. This must be deprecated
                 * when loading dumps created by Redis 2.4 gets deprecated. */
                {
                    unsigned char *lp = lpNew(0);
                    unsigned char *zi = zipmapRewind(o->ptr);
                    unsigned char *fstr, *vstr;
                    unsigned int flen, vlen;
                    unsigned int maxlen = 0;
                    dict *dupSearchDict = dictCreate(&hashDictType);

                    while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen)) != NULL) {
                        if (flen > maxlen) maxlen = flen;
                        if (vlen > maxlen) maxlen = vlen;

                        /* search for duplicate records */
                        sds field = sdstrynewlen(fstr, flen);
                        if (!field || dictAdd(dupSearchDict, field, NULL) != DICT_OK ||
                            !lpSafeToAdd(lp, (size_t)flen + vlen)) {
                            rdbReportCorruptRDB("Hash zipmap with dup elements, or big length (%u)", flen);
                            dictRelease(dupSearchDict);
                            sdsfree(field);
                            zfree(encoded);
                            o->ptr = NULL;
                            decrRefCount(o);
                            return NULL;
                        }

                        lp = lpAppend(lp, fstr, flen);
                        lp = lpAppend(lp, vstr, vlen);
                    }

                    dictRelease(dupSearchDict);
                    zfree(o->ptr);
                    o->ptr = lp;
                    o->type = OBJ_HASH;
                    o->encoding = OBJ_ENCODING_LISTPACK;

                    if (hashTypeLength(o, 0) > server.hash_max_listpack_entries ||
                        maxlen > server.hash_max_listpack_value)
                    {
                        hashTypeConvert(o, OBJ_ENCODING_HT, NULL);
                    }
                }
                break;
            case RDB_TYPE_LIST_ZIPLIST:
                {
                    quicklist *ql = quicklistNew(server.list_max_listpack_size,
                                                 server.list_compress_depth);

                    if (!ziplistValidateIntegrity(encoded, encoded_len, 1,
                            _listZiplistEntryConvertAndValidate, ql))
                    {
                        rdbReportCorruptRDB("List ziplist integrity check failed.");
                        zfree(encoded);
                        o->ptr = NULL;
                        decrRefCount(o);
                        quicklistRelease(ql);
                        return NULL;
                    }

                    if (ql->len == 0) {
                        zfree(encoded);
                        o->ptr = NULL;
                        decrRefCount(o);
                        quicklistRelease(ql);
                        goto emptykey;
                    }

                    zfree(encoded);
                    o->type = OBJ_LIST;
                    o->ptr = ql;
                    o->encoding = OBJ_ENCODING_QUICKLIST;
                    break;
                }
            case RDB_TYPE_SET_INTSET:
                if (deep_integrity_validation) server.stat_dump_payload_sanitizations++;
                if (!intsetValidateIntegrity(encoded, encoded_len, deep_integrity_validation)) {
                    rdbReportCorruptRDB("Intset integrity check failed.");
                    zfree(encoded);
                    o->ptr = NULL;
                    decrRefCount(o);
                    return NULL;
                }
                o->type = OBJ_SET;
                o->encoding = OBJ_ENCODING_INTSET;
                if (intsetLen(o->ptr) > server.set_max_intset_entries)
                    setTypeConvert(o, OBJ_ENCODING_HT);
                break;
            case RDB_TYPE_SET_LISTPACK:
                if (deep_integrity_validation) server.stat_dump_payload_sanitizations++;
                if (!lpValidateIntegrityAndDups(encoded, encoded_len, deep_integrity_validation, 1)) {
                    rdbReportCorruptRDB("Set listpack integrity check failed.");
                    zfree(encoded);
                    o->ptr = NULL;
                    decrRefCount(o);
                    return NULL;
                }
                o->type = OBJ_SET;
                o->encoding = OBJ_ENCODING_LISTPACK;

                if (setTypeSize(o) == 0) {
                    zfree(encoded);
                    o->ptr = NULL;
                    decrRefCount(o);
                    goto emptykey;
                }
                if (setTypeSize(o) > server.set_max_listpack_entries)
                    setTypeConvert(o, OBJ_ENCODING_HT);
                break;
            case RDB_TYPE_ZSET_ZIPLIST:
                {
                    unsigned char *lp = lpNew(encoded_len);
                    if (!ziplistPairsConvertAndValidateIntegrity(encoded, encoded_len, &lp)) {
                        rdbReportCorruptRDB("Zset ziplist integrity check failed.");
                        zfree(lp);
                        zfree(encoded);
                        o->ptr = NULL;
                        decrRefCount(o);
                        return NULL;
                    }

                    zfree(o->ptr);
                    o->type = OBJ_ZSET;
                    o->ptr = lp;
                    o->encoding = OBJ_ENCODING_LISTPACK;
                    if (zsetLength(o) == 0) {
                        decrRefCount(o);
                        goto emptykey;
                    }

                    if (zsetLength(o) > server.zset_max_listpack_entries)
                        zsetConvert(o, OBJ_ENCODING_SKIPLIST);
                    else
                        o->ptr = lpShrinkToFit(o->ptr);
                    break;
                }
            case RDB_TYPE_ZSET_LISTPACK:
                if (deep_integrity_validation) server.stat_dump_payload_sanitizations++;
                if (!lpValidateIntegrityAndDups(encoded, encoded_len, deep_integrity_validation, 2)) {
                    rdbReportCorruptRDB("Zset listpack integrity check failed.");
                    zfree(encoded);
                    o->ptr = NULL;
                    decrRefCount(o);
                    return NULL;
                }
                o->type = OBJ_ZSET;
                o->encoding = OBJ_ENCODING_LISTPACK;
                if (zsetLength(o) == 0) {
                    decrRefCount(o);
                    goto emptykey;
                }

                if (zsetLength(o) > server.zset_max_listpack_entries)
                    zsetConvert(o, OBJ_ENCODING_SKIPLIST);
                break;
            case RDB_TYPE_HASH_ZIPLIST:
                {
                    unsigned char *lp = lpNew(encoded_len);
                    if (!ziplistPairsConvertAndValidateIntegrity(encoded, encoded_len, &lp)) {
                        rdbReportCorruptRDB("Hash ziplist integrity check failed.");
                        zfree(lp);
                        zfree(encoded);
                        o->ptr = NULL;
                        decrRefCount(o);
                        return NULL;
                    }

                    zfree(o->ptr);
                    o->ptr = lp;
                    o->type = OBJ_HASH;
                    o->encoding = OBJ_ENCODING_LISTPACK;
                    if (hashTypeLength(o, 0) == 0) {
                        decrRefCount(o);
                        goto emptykey;
                    }

                    if (hashTypeLength(o, 0) > server.hash_max_listpack_entries)
                        hashTypeConvert(o, OBJ_ENCODING_HT, NULL);
                    else
                        o->ptr = lpShrinkToFit(o->ptr);
                    break;
                }
            case RDB_TYPE_HASH_LISTPACK:
            case RDB_TYPE_HASH_LISTPACK_EX_PRE_GA:
            case RDB_TYPE_HASH_LISTPACK_EX:
                /* listpack-encoded hash with TTL requires its own struct
                 * pointed to by o->ptr */
                o->type = OBJ_HASH;
                if ( (rdbtype == RDB_TYPE_HASH_LISTPACK_EX) ||
                     (rdbtype == RDB_TYPE_HASH_LISTPACK_EX_PRE_GA) ) {
                    listpackEx *lpt = listpackExCreate();
                    lpt->lp = encoded;
                    lpt->key = key;
                    o->ptr = lpt;
                    o->encoding = OBJ_ENCODING_LISTPACK_EX;
                } else
                    o->encoding = OBJ_ENCODING_LISTPACK;

                /* tuple_len is the number of elements for each key:
                 * key + value for simple hash, key + value + tll for hash with TTL*/
                int tuple_len = (rdbtype == RDB_TYPE_HASH_LISTPACK ? 2 : 3);
                /* validate read data */
                if (deep_integrity_validation) server.stat_dump_payload_sanitizations++;
                if (!lpValidateIntegrityAndDups(encoded, encoded_len,
                                                deep_integrity_validation, tuple_len)) {
                    rdbReportCorruptRDB("Hash listpack integrity check failed.");
                    decrRefCount(o);
                    return NULL;
                }

                /* if listpack is empty, delete it */
                if (hashTypeLength(o, 0) == 0) {
                    decrRefCount(o);
                    goto emptykey;
                }

                /* Convert listpack to hash table without registering in global HFE DS,
                 * if has HFEs, since the listpack is not connected yet to the DB */
                if (hashTypeLength(o, 0) > server.hash_max_listpack_entries)
                    hashTypeConvert(o, OBJ_ENCODING_HT, NULL /*db->hexpires*/);

                break;
            default:
                /* totally unreachable */
                rdbReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
                break;
        }
    } else if (rdbtype == RDB_TYPE_STREAM_LISTPACKS ||
               rdbtype == RDB_TYPE_STREAM_LISTPACKS_2 ||
               rdbtype == RDB_TYPE_STREAM_LISTPACKS_3)
    {
        o = createStreamObject();
        stream *s = o->ptr;
        uint64_t listpacks = rdbLoadLen(rdb,NULL);
        if (listpacks == RDB_LENERR) {
            rdbReportReadError("Stream listpacks len loading failed.");
            decrRefCount(o);
            return NULL;
        }

        while(listpacks--) {
            /* Get the master ID, the one we'll use as key of the radix tree
             * node: the entries inside the listpack itself are delta-encoded
             * relatively to this ID. */
            sds nodekey = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (nodekey == NULL) {
                rdbReportReadError("Stream master ID loading failed: invalid encoding or I/O error.");
                decrRefCount(o);
                return NULL;
            }
            if (sdslen(nodekey) != sizeof(streamID)) {
                rdbReportCorruptRDB("Stream node key entry is not the "
                                        "size of a stream ID");
                sdsfree(nodekey);
                decrRefCount(o);
                return NULL;
            }

            /* Load the listpack. */
            size_t lp_size;
            unsigned char *lp =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,&lp_size);
            if (lp == NULL) {
                rdbReportReadError("Stream listpacks loading failed.");
                sdsfree(nodekey);
                decrRefCount(o);
                return NULL;
            }
            if (deep_integrity_validation) server.stat_dump_payload_sanitizations++;
            if (!streamValidateListpackIntegrity(lp, lp_size, deep_integrity_validation)) {
                rdbReportCorruptRDB("Stream listpack integrity check failed.");
                sdsfree(nodekey);
                decrRefCount(o);
                zfree(lp);
                return NULL;
            }

            unsigned char *first = lpFirst(lp);
            if (first == NULL) {
                /* Serialized listpacks should never be empty, since on
                 * deletion we should remove the radix tree key if the
                 * resulting listpack is empty. */
                rdbReportCorruptRDB("Empty listpack inside stream");
                sdsfree(nodekey);
                decrRefCount(o);
                zfree(lp);
                return NULL;
            }

            /* Insert the key in the radix tree. */
            int retval = raxTryInsert(s->rax,
                (unsigned char*)nodekey,sizeof(streamID),lp,NULL);
            sdsfree(nodekey);
            if (!retval) {
                rdbReportCorruptRDB("Listpack re-added with existing key");
                decrRefCount(o);
                zfree(lp);
                return NULL;
            }
        }
        /* Load total number of items inside the stream. */
        s->length = rdbLoadLen(rdb,NULL);

        /* Load the last entry ID. */
        s->last_id.ms = rdbLoadLen(rdb,NULL);
        s->last_id.seq = rdbLoadLen(rdb,NULL);

        if (rdbtype >= RDB_TYPE_STREAM_LISTPACKS_2) {
            /* Load the first entry ID. */
            s->first_id.ms = rdbLoadLen(rdb,NULL);
            s->first_id.seq = rdbLoadLen(rdb,NULL);

            /* Load the maximal deleted entry ID. */
            s->max_deleted_entry_id.ms = rdbLoadLen(rdb,NULL);
            s->max_deleted_entry_id.seq = rdbLoadLen(rdb,NULL);

            /* Load the offset. */
            s->entries_added = rdbLoadLen(rdb,NULL);
        } else {
            /* During migration the offset can be initialized to the stream's
             * length. At this point, we also don't care about tombstones
             * because CG offsets will be later initialized as well. */
            s->max_deleted_entry_id.ms = 0;
            s->max_deleted_entry_id.seq = 0;
            s->entries_added = s->length;

            /* Since the rax is already loaded, we can find the first entry's
             * ID. */
            streamGetEdgeID(s,1,1,&s->first_id);
        }

        if (rioGetReadError(rdb)) {
            rdbReportReadError("Stream object metadata loading failed.");
            decrRefCount(o);
            return NULL;
        }

        if (s->length && !raxSize(s->rax)) {
            rdbReportCorruptRDB("Stream length inconsistent with rax entries");
            decrRefCount(o);
            return NULL;
        }

        /* Consumer groups loading */
        uint64_t cgroups_count = rdbLoadLen(rdb,NULL);
        if (cgroups_count == RDB_LENERR) {
            rdbReportReadError("Stream cgroup count loading failed.");
            decrRefCount(o);
            return NULL;
        }
        while(cgroups_count--) {
            /* Get the consumer group name and ID. We can then create the
             * consumer group ASAP and populate its structure as
             * we read more data. */
            streamID cg_id;
            sds cgname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (cgname == NULL) {
                rdbReportReadError(
                    "Error reading the consumer group name from Stream");
                decrRefCount(o);
                return NULL;
            }

            cg_id.ms = rdbLoadLen(rdb,NULL);
            cg_id.seq = rdbLoadLen(rdb,NULL);
            if (rioGetReadError(rdb)) {
                rdbReportReadError("Stream cgroup ID loading failed.");
                sdsfree(cgname);
                decrRefCount(o);
                return NULL;
            }
            
            /* Load group offset. */
            uint64_t cg_offset;
            if (rdbtype >= RDB_TYPE_STREAM_LISTPACKS_2) {
                cg_offset = rdbLoadLen(rdb,NULL);
                if (rioGetReadError(rdb)) {
                    rdbReportReadError("Stream cgroup offset loading failed.");
                    sdsfree(cgname);
                    decrRefCount(o);
                    return NULL;
                }
            } else {
                cg_offset = streamEstimateDistanceFromFirstEverEntry(s,&cg_id);
            }

            streamCG *cgroup = streamCreateCG(s,cgname,sdslen(cgname),&cg_id,cg_offset);
            if (cgroup == NULL) {
                rdbReportCorruptRDB("Duplicated consumer group name %s",
                                         cgname);
                decrRefCount(o);
                sdsfree(cgname);
                return NULL;
            }
            sdsfree(cgname);

            /* Load the global PEL for this consumer group, however we'll
             * not yet populate the NACK structures with the message
             * owner, since consumers for this group and their messages will
             * be read as a next step. So for now leave them not resolved
             * and later populate it. */
            uint64_t pel_size = rdbLoadLen(rdb,NULL);
            if (pel_size == RDB_LENERR) {
                rdbReportReadError("Stream PEL size loading failed.");
                decrRefCount(o);
                return NULL;
            }
            while(pel_size--) {
                unsigned char rawid[sizeof(streamID)];
                if (rioRead(rdb,rawid,sizeof(rawid)) == 0) {
                    rdbReportReadError("Stream PEL ID loading failed.");
                    decrRefCount(o);
                    return NULL;
                }
                streamNACK *nack = streamCreateNACK(NULL);
                nack->delivery_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                nack->delivery_count = rdbLoadLen(rdb,NULL);
                if (rioGetReadError(rdb)) {
                    rdbReportReadError("Stream PEL NACK loading failed.");
                    decrRefCount(o);
                    streamFreeNACK(nack);
                    return NULL;
                }
                if (!raxTryInsert(cgroup->pel,rawid,sizeof(rawid),nack,NULL)) {
                    rdbReportCorruptRDB("Duplicated global PEL entry "
                                            "loading stream consumer group");
                    decrRefCount(o);
                    streamFreeNACK(nack);
                    return NULL;
                }
            }

            /* Now that we loaded our global PEL, we need to load the
             * consumers and their local PELs. */
            uint64_t consumers_num = rdbLoadLen(rdb,NULL);
            if (consumers_num == RDB_LENERR) {
                rdbReportReadError("Stream consumers num loading failed.");
                decrRefCount(o);
                return NULL;
            }
            while(consumers_num--) {
                sds cname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
                if (cname == NULL) {
                    rdbReportReadError(
                        "Error reading the consumer name from Stream group.");
                    decrRefCount(o);
                    return NULL;
                }
                streamConsumer *consumer = streamCreateConsumer(cgroup,cname,NULL,0,
                                                        SCC_NO_NOTIFY|SCC_NO_DIRTIFY);
                sdsfree(cname);
                if (!consumer) {
                    rdbReportCorruptRDB("Duplicate stream consumer detected.");
                    decrRefCount(o);
                    return NULL;
                }

                consumer->seen_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                if (rioGetReadError(rdb)) {
                    rdbReportReadError("Stream short read reading seen time.");
                    decrRefCount(o);
                    return NULL;
                }

                if (rdbtype >= RDB_TYPE_STREAM_LISTPACKS_3) {
                    consumer->active_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                    if (rioGetReadError(rdb)) {
                        rdbReportReadError("Stream short read reading active time.");
                        decrRefCount(o);
                        return NULL;
                    }
                } else {
                    /* That's the best estimate we got */
                    consumer->active_time = consumer->seen_time;
                }

                /* Load the PEL about entries owned by this specific
                 * consumer. */
                pel_size = rdbLoadLen(rdb,NULL);
                if (pel_size == RDB_LENERR) {
                    rdbReportReadError(
                        "Stream consumer PEL num loading failed.");
                    decrRefCount(o);
                    return NULL;
                }
                while(pel_size--) {
                    unsigned char rawid[sizeof(streamID)];
                    if (rioRead(rdb,rawid,sizeof(rawid)) == 0) {
                        rdbReportReadError(
                            "Stream short read reading PEL streamID.");
                        decrRefCount(o);
                        return NULL;
                    }
                    void *result;
                    if (!raxFind(cgroup->pel,rawid,sizeof(rawid),&result)) {
                        rdbReportCorruptRDB("Consumer entry not found in "
                                                "group global PEL");
                        decrRefCount(o);
                        return NULL;
                    }
                    streamNACK *nack = result;

                    /* Set the NACK consumer, that was left to NULL when
                     * loading the global PEL. Then set the same shared
                     * NACK structure also in the consumer-specific PEL. */
                    nack->consumer = consumer;
                    if (!raxTryInsert(consumer->pel,rawid,sizeof(rawid),nack,NULL)) {
                        rdbReportCorruptRDB("Duplicated consumer PEL entry "
                                                " loading a stream consumer "
                                                "group");
                        decrRefCount(o);
                        streamFreeNACK(nack);
                        return NULL;
                    }
                }
            }

            /* Verify that each PEL eventually got a consumer assigned to it. */
            if (deep_integrity_validation) {
                raxIterator ri_cg_pel;
                raxStart(&ri_cg_pel,cgroup->pel);
                raxSeek(&ri_cg_pel,"^",NULL,0);
                while(raxNext(&ri_cg_pel)) {
                    streamNACK *nack = ri_cg_pel.data;
                    if (!nack->consumer) {
                        raxStop(&ri_cg_pel);
                        rdbReportCorruptRDB("Stream CG PEL entry without consumer");
                        decrRefCount(o);
                        return NULL;
                    }
                }
                raxStop(&ri_cg_pel);
            }
        }
    } else if (rdbtype == RDB_TYPE_MODULE_PRE_GA) {
            rdbReportCorruptRDB("Pre-release module format not supported");
            return NULL;
    } else if (rdbtype == RDB_TYPE_MODULE_2) {
        uint64_t moduleid = rdbLoadLen(rdb,NULL);
        if (rioGetReadError(rdb)) {
            rdbReportReadError("Short read module id");
            return NULL;
        }
        moduleType *mt = moduleTypeLookupModuleByID(moduleid);

        if (rdbCheckMode) {
            char name[10];
            moduleTypeNameByID(name,moduleid);
            return rdbLoadCheckModuleValue(rdb,name);
        }

        if (mt == NULL) {
            char name[10];
            moduleTypeNameByID(name,moduleid);
            rdbReportCorruptRDB("The RDB file contains module data I can't load: no matching module type '%s'", name);
            return NULL;
        }
        RedisModuleIO io;
        robj keyobj;
        initStaticStringObject(keyobj,key);
        moduleInitIOContext(io,mt,rdb,&keyobj,dbid);
        /* Call the rdb_load method of the module providing the 10 bit
         * encoding version in the lower 10 bits of the module ID. */
        void *ptr = mt->rdb_load(&io,moduleid&1023);
        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }

        /* Module v2 serialization has an EOF mark at the end. */
        uint64_t eof = rdbLoadLen(rdb,NULL);
        if (eof == RDB_LENERR) {
            if (ptr) {
                o = createModuleObject(mt, ptr); /* creating just in order to easily destroy */
                decrRefCount(o);
            }
            return NULL;
        }
        if (eof != RDB_MODULE_OPCODE_EOF) {
            rdbReportCorruptRDB("The RDB file contains module data for the module '%s' that is not terminated by "
                                "the proper module value EOF marker", moduleTypeModuleName(mt));
            if (ptr) {
                o = createModuleObject(mt, ptr); /* creating just in order to easily destroy */
                decrRefCount(o);
            }
            return NULL;
        }

        if (ptr == NULL) {
            rdbReportCorruptRDB("The RDB file contains module data for the module type '%s', that the responsible "
                                "module is not able to load. Check for modules log above for additional clues.",
                                moduleTypeModuleName(mt));
            return NULL;
        }
        o = createModuleObject(mt, ptr);
    } else {
        rdbReportReadError("Unknown RDB encoding type %d",rdbtype);
        return NULL;
    }

    if (error) *error = 0;
    return o;

emptykey:
    if (error) *error = RDB_LOAD_ERR_EMPTY_KEY;
    return NULL;
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats. */
void startLoading(size_t size, int rdbflags, int async) {
    loadingSetFlags(NULL, size, async);
    loadingFireEvent(rdbflags);
}

/* Initialize stats, set loading flags and filename if provided. */
void loadingSetFlags(char *filename, size_t size, int async) {
    rdbFileBeingLoaded = filename;
    server.loading = 1;
    if (async == 1) server.async_loading = 1;
    server.loading_start_time = time(NULL);
    server.loading_loaded_bytes = 0;
    server.loading_total_bytes = size;
    server.loading_rdb_used_mem = 0;
    server.rdb_last_load_keys_expired = 0;
    server.rdb_last_load_keys_loaded = 0;
    blockingOperationStarts();
}

void loadingFireEvent(int rdbflags) {
    /* Fire the loading modules start event. */
    int subevent;
    if (rdbflags & RDBFLAGS_AOF_PREAMBLE)
        subevent = REDISMODULE_SUBEVENT_LOADING_AOF_START;
    else if(rdbflags & RDBFLAGS_REPLICATION)
        subevent = REDISMODULE_SUBEVENT_LOADING_REPL_START;
    else
        subevent = REDISMODULE_SUBEVENT_LOADING_RDB_START;
    moduleFireServerEvent(REDISMODULE_EVENT_LOADING,subevent,NULL);
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats.
 * 'filename' is optional and used for rdb-check on error */
void startLoadingFile(size_t size, char* filename, int rdbflags) {
    loadingSetFlags(filename, size, 0);
    loadingFireEvent(rdbflags);
}

/* Refresh the absolute loading progress info */
void loadingAbsProgress(off_t pos) {
    server.loading_loaded_bytes = pos;
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Refresh the incremental loading progress info */
void loadingIncrProgress(off_t size) {
    server.loading_loaded_bytes += size;
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Update the file name currently being loaded */
void updateLoadingFileName(char* filename) {
    rdbFileBeingLoaded = filename;
}

/* Loading finished */
void stopLoading(int success) {
    server.loading = 0;
    server.async_loading = 0;
    blockingOperationEnds();
    rdbFileBeingLoaded = NULL;

    /* Fire the loading modules end event. */
    moduleFireServerEvent(REDISMODULE_EVENT_LOADING,
                          success?
                            REDISMODULE_SUBEVENT_LOADING_ENDED:
                            REDISMODULE_SUBEVENT_LOADING_FAILED,
                           NULL);
}

void startSaving(int rdbflags) {
    /* Fire the persistence modules start event. */
    int subevent;
    if (rdbflags & RDBFLAGS_AOF_PREAMBLE && getpid() != server.pid)
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_AOF_START;
    else if (rdbflags & RDBFLAGS_AOF_PREAMBLE)
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_AOF_START;
    else if (getpid()!=server.pid)
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_RDB_START;
    else
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START;
    moduleFireServerEvent(REDISMODULE_EVENT_PERSISTENCE,subevent,NULL);
}

void stopSaving(int success) {
    /* Fire the persistence modules end event. */
    moduleFireServerEvent(REDISMODULE_EVENT_PERSISTENCE,
                          success?
                            REDISMODULE_SUBEVENT_PERSISTENCE_ENDED:
                            REDISMODULE_SUBEVENT_PERSISTENCE_FAILED,
                          NULL);
}

/* Track loading progress in order to serve client's from time to time
   and if needed calculate rdb checksum  */
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len) {
    if (server.rdb_checksum)
        rioGenericUpdateChecksum(r, buf, len);
    if (server.loading_process_events_interval_bytes &&
        (r->processed_bytes + len)/server.loading_process_events_interval_bytes > r->processed_bytes/server.loading_process_events_interval_bytes)
    {
        if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER)
            replicationSendNewlineToMaster();
        loadingAbsProgress(r->processed_bytes);
        processEventsWhileBlocked();
        processModuleLoadingProgressEvent(0);
    }
    if (server.repl_state == REPL_STATE_TRANSFER && rioCheckType(r) == RIO_TYPE_CONN) {
        atomicIncr(server.stat_net_repl_input_bytes, len);
    }
}

/* Save the given functions_ctx to the rdb.
 * The err output parameter is optional and will be set with relevant error
 * message on failure, it is the caller responsibility to free the error
 * message on failure.
 *
 * The lib_ctx argument is also optional. If NULL is given, only verify rdb
 * structure with out performing the actual functions loading. */
int rdbFunctionLoad(rio *rdb, int ver, functionsLibCtx* lib_ctx, int rdbflags, sds *err) {
    UNUSED(ver);
    sds error = NULL;
    sds final_payload = NULL;
    int res = C_ERR;
    if (!(final_payload = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL))) {
        error = sdsnew("Failed loading library payload");
        goto done;
    }

    if (lib_ctx) {
        sds library_name = NULL;
        if (!(library_name = functionsCreateWithLibraryCtx(final_payload, rdbflags & RDBFLAGS_ALLOW_DUP, &error, lib_ctx, 0))) {
            if (!error) {
                error = sdsnew("Failed creating the library");
            }
            goto done;
        }
        sdsfree(library_name);
    }

    res = C_OK;

done:
    if (final_payload) sdsfree(final_payload);
    if (error) {
        if (err) {
            *err = error;
        } else {
            serverLog(LL_WARNING, "Failed creating function, %s", error);
            sdsfree(error);
        }
    }
    return res;
}

/* Load an RDB file from the rio stream 'rdb'. On success C_OK is returned,
 * otherwise C_ERR is returned and 'errno' is set accordingly. */
int rdbLoadRio(rio *rdb, int rdbflags, rdbSaveInfo *rsi) {
    functionsLibCtx* functions_lib_ctx = functionsLibCtxGetCurrent();
    rdbLoadingCtx loading_ctx = { .dbarray = server.db, .functions_lib_ctx = functions_lib_ctx };
    int retval = rdbLoadRioWithLoadingCtx(rdb,rdbflags,rsi,&loading_ctx);
    return retval;
}

/* Load an RDB file from the rio stream 'rdb'. On success C_OK is returned,
 * otherwise C_ERR is returned.
 * The rdb_loading_ctx argument holds objects to which the rdb will be loaded to,
 * currently it only allow to set db object and functionLibCtx to which the data
 * will be loaded (in the future it might contains more such objects). */
int rdbLoadRioWithLoadingCtx(rio *rdb, int rdbflags, rdbSaveInfo *rsi, rdbLoadingCtx *rdb_loading_ctx) {
    uint64_t dbid = 0;
    int type, rdbver;
    uint64_t db_size = 0, expires_size = 0;
    int should_expand_db = 0;
    redisDb *db = rdb_loading_ctx->dbarray+0;
    char buf[1024];
    int error;
    long long empty_keys_skipped = 0;

    rdb->update_cksum = rdbLoadProgressCallback;
    rdb->max_processing_chunk = server.loading_process_events_interval_bytes;
    if (rioRead(rdb,buf,9) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        serverLog(LL_WARNING,"Wrong signature trying to load DB from file");
        return C_ERR;
    }
    rdbver = atoi(buf+5);
    if (rdbver < 1 || rdbver > RDB_VERSION) {
        serverLog(LL_WARNING,"Can't handle RDB format version %d",rdbver);
        return C_ERR;
    }

    /* Key-specific attributes, set by opcodes before the key type. */
    long long lru_idle = -1, lfu_freq = -1, expiretime = -1, now = mstime();
    long long lru_clock = LRU_CLOCK();

    while(1) {
        sds key;
        robj *val;

        /* Read type. */
        if ((type = rdbLoadType(rdb)) == -1) goto eoferr;

        /* Handle special types. */
        if (type == RDB_OPCODE_EXPIRETIME) {
            /* EXPIRETIME: load an expire associated with the next key
             * to load. Note that after loading an expire we need to
             * load the actual type, and continue. */
            expiretime = rdbLoadTime(rdb);
            expiretime *= 1000;
            if (rioGetReadError(rdb)) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
            /* EXPIRETIME_MS: milliseconds precision expire times introduced
             * with RDB v3. Like EXPIRETIME but no with more precision. */
            expiretime = rdbLoadMillisecondTime(rdb,rdbver);
            if (rioGetReadError(rdb)) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_FREQ) {
            /* FREQ: LFU frequency. */
            uint8_t byte;
            if (rioRead(rdb,&byte,1) == 0) goto eoferr;
            lfu_freq = byte;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_IDLE) {
            /* IDLE: LRU idle time. */
            uint64_t qword;
            if ((qword = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            lru_idle = qword;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EOF) {
            /* EOF: End of file, exit the main loop. */
            break;
        } else if (type == RDB_OPCODE_SELECTDB) {
            /* SELECTDB: Select the specified database. */
            if ((dbid = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            if (dbid >= (unsigned)server.dbnum) {
                serverLog(LL_WARNING,
                    "FATAL: Data file was created with a Redis "
                    "server configured to handle more than %d "
                    "databases. Exiting\n", server.dbnum);
                exit(1);
            }
            db = rdb_loading_ctx->dbarray+dbid;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_RESIZEDB) {
            /* RESIZEDB: Hint about the size of the keys in the currently
             * selected data base, in order to avoid useless rehashing. */
            if ((db_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            if ((expires_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            should_expand_db = 1;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_SLOT_INFO) {
            uint64_t slot_id, slot_size, expires_slot_size;
            if ((slot_id = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            if ((slot_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            if ((expires_slot_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            if (!server.cluster_enabled) {
                continue; /* Ignore gracefully. */
            }
            /* In cluster mode we resize individual slot specific dictionaries based on the number of keys that slot holds. */
            kvstoreDictExpand(db->keys, slot_id, slot_size);
            kvstoreDictExpand(db->expires, slot_id, expires_slot_size);
            should_expand_db = 0;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_AUX) {
            /* AUX: generic string-string fields. Use to add state to RDB
             * which is backward compatible. Implementations of RDB loading
             * are required to skip AUX fields they don't understand.
             *
             * An AUX field is composed of two strings: key and value. */
            robj *auxkey, *auxval;
            if ((auxkey = rdbLoadStringObject(rdb)) == NULL) goto eoferr;
            if ((auxval = rdbLoadStringObject(rdb)) == NULL) {
                decrRefCount(auxkey);
                goto eoferr;
            }

            if (((char*)auxkey->ptr)[0] == '%') {
                /* All the fields with a name staring with '%' are considered
                 * information fields and are logged at startup with a log
                 * level of NOTICE. */
                serverLog(LL_NOTICE,"RDB '%s': %s",
                    (char*)auxkey->ptr,
                    (char*)auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-stream-db")) {
                if (rsi) rsi->repl_stream_db = atoi(auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-id")) {
                if (rsi && sdslen(auxval->ptr) == CONFIG_RUN_ID_SIZE) {
                    memcpy(rsi->repl_id,auxval->ptr,CONFIG_RUN_ID_SIZE+1);
                    rsi->repl_id_is_set = 1;
                }
            } else if (!strcasecmp(auxkey->ptr,"repl-offset")) {
                if (rsi) rsi->repl_offset = strtoll(auxval->ptr,NULL,10);
            } else if (!strcasecmp(auxkey->ptr,"lua")) {
                /* Won't load the script back in memory anymore. */
            } else if (!strcasecmp(auxkey->ptr,"redis-ver")) {
                serverLog(LL_NOTICE,"Loading RDB produced by version %s",
                    (char*)auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"ctime")) {
                time_t age = time(NULL)-strtol(auxval->ptr,NULL,10);
                if (age < 0) age = 0;
                serverLog(LL_NOTICE,"RDB age %ld seconds",
                    (unsigned long) age);
            } else if (!strcasecmp(auxkey->ptr,"used-mem")) {
                long long usedmem = strtoll(auxval->ptr,NULL,10);
                serverLog(LL_NOTICE,"RDB memory usage when created %.2f Mb",
                    (double) usedmem / (1024*1024));
                server.loading_rdb_used_mem = usedmem;
            } else if (!strcasecmp(auxkey->ptr,"aof-preamble")) {
                long long haspreamble = strtoll(auxval->ptr,NULL,10);
                if (haspreamble) serverLog(LL_NOTICE,"RDB has an AOF tail");
            } else if (!strcasecmp(auxkey->ptr, "aof-base")) {
                long long isbase = strtoll(auxval->ptr, NULL, 10);
                if (isbase) serverLog(LL_NOTICE, "RDB is base AOF");
            } else if (!strcasecmp(auxkey->ptr,"redis-bits")) {
                /* Just ignored. */
            } else {
                /* We ignore fields we don't understand, as by AUX field
                 * contract. */
                serverLog(LL_DEBUG,"Unrecognized RDB AUX field: '%s'",
                    (char*)auxkey->ptr);
            }

            decrRefCount(auxkey);
            decrRefCount(auxval);
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_MODULE_AUX) {
            /* Load module data that is not related to the Redis key space.
             * Such data can be potentially be stored both before and after the
             * RDB keys-values section. */
            uint64_t moduleid = rdbLoadLen(rdb,NULL);
            int when_opcode = rdbLoadLen(rdb,NULL);
            int when = rdbLoadLen(rdb,NULL);
            if (rioGetReadError(rdb)) goto eoferr;
            if (when_opcode != RDB_MODULE_OPCODE_UINT) {
                rdbReportReadError("bad when_opcode");
                goto eoferr;
            }
            moduleType *mt = moduleTypeLookupModuleByID(moduleid);
            char name[10];
            moduleTypeNameByID(name,moduleid);

            if (!rdbCheckMode && mt == NULL) {
                /* Unknown module. */
                serverLog(LL_WARNING,"The RDB file contains AUX module data I can't load: no matching module '%s'", name);
                exit(1);
            } else if (!rdbCheckMode && mt != NULL) {
                if (!mt->aux_load) {
                    /* Module doesn't support AUX. */
                    serverLog(LL_WARNING,"The RDB file contains module AUX data, but the module '%s' doesn't seem to support it.", name);
                    exit(1);
                }

                RedisModuleIO io;
                moduleInitIOContext(io,mt,rdb,NULL,-1);
                /* Call the rdb_load method of the module providing the 10 bit
                 * encoding version in the lower 10 bits of the module ID. */
                int rc = mt->aux_load(&io,moduleid&1023, when);
                if (io.ctx) {
                    moduleFreeContext(io.ctx);
                    zfree(io.ctx);
                }
                if (rc != REDISMODULE_OK || io.error) {
                    moduleTypeNameByID(name,moduleid);
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module type '%s', that the responsible module is not able to load. Check for modules log above for additional clues.", name);
                    goto eoferr;
                }
                uint64_t eof = rdbLoadLen(rdb,NULL);
                if (eof != RDB_MODULE_OPCODE_EOF) {
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module '%s' that is not terminated by the proper module value EOF marker", name);
                    goto eoferr;
                }
                continue;
            } else {
                /* RDB check mode. */
                robj *aux = rdbLoadCheckModuleValue(rdb,name);
                decrRefCount(aux);
                continue; /* Read next opcode. */
            }
        } else if (type == RDB_OPCODE_FUNCTION_PRE_GA) {
            rdbReportCorruptRDB("Pre-release function format not supported.");
            exit(1);
        } else if (type == RDB_OPCODE_FUNCTION2) {
            sds err = NULL;
            if (rdbFunctionLoad(rdb, rdbver, rdb_loading_ctx->functions_lib_ctx, rdbflags, &err) != C_OK) {
                serverLog(LL_WARNING,"Failed loading library, %s", err);
                sdsfree(err);
                goto eoferr;
            }
            continue;
        }

        /* If there is no slot info, it means that it's either not cluster mode or we are trying to load legacy RDB file.
         * In this case we want to estimate number of keys per slot and resize accordingly. */
        if (should_expand_db) {
            dbExpand(db, db_size, 0);
            dbExpandExpires(db, expires_size, 0);
            should_expand_db = 0;
        }

        /* Read key */
        if ((key = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL)
            goto eoferr;
        /* Read value */
        val = rdbLoadObject(type,rdb,key,db->id,&error);

        /* Check if the key already expired. This function is used when loading
         * an RDB file from disk, either at startup, or when an RDB was
         * received from the master. In the latter case, the master is
         * responsible for key expiry. If we would expire keys here, the
         * snapshot taken by the master may not be reflected on the slave.
         * Similarly, if the base AOF is RDB format, we want to load all
         * the keys they are, since the log of operations in the incr AOF
         * is assumed to work in the exact keyspace state. */
        if (val == NULL) {
            /* Since we used to have bug that could lead to empty keys
             * (See #8453), we rather not fail when empty key is encountered
             * in an RDB file, instead we will silently discard it and
             * continue loading. */
            if (error == RDB_LOAD_ERR_EMPTY_KEY) {
                if(empty_keys_skipped++ < 10)
                    serverLog(LL_NOTICE, "rdbLoadObject skipping empty key: %s", key);
                sdsfree(key);
            } else {
                sdsfree(key);
                goto eoferr;
            }
        } else if (iAmMaster() &&
            !(rdbflags&RDBFLAGS_AOF_PREAMBLE) &&
            expiretime != -1 && expiretime < now)
        {
            if (rdbflags & RDBFLAGS_FEED_REPL) {
                /* Caller should have created replication backlog,
                 * and now this path only works when rebooting,
                 * so we don't have replicas yet. */
                serverAssert(server.repl_backlog != NULL && listLength(server.slaves) == 0);
                robj keyobj;
                initStaticStringObject(keyobj,key);
                robj *argv[2];
                argv[0] = server.lazyfree_lazy_expire ? shared.unlink : shared.del;
                argv[1] = &keyobj;
                replicationFeedSlaves(server.slaves,dbid,argv,2);
            }
            sdsfree(key);
            decrRefCount(val);
            server.rdb_last_load_keys_expired++;
        } else {
            robj keyobj;
            initStaticStringObject(keyobj,key);

            /* Add the new object in the hash table */
            int added = dbAddRDBLoad(db,key,val);
            server.rdb_last_load_keys_loaded++;
            if (!added) {
                if (rdbflags & RDBFLAGS_ALLOW_DUP) {
                    /* This flag is useful for DEBUG RELOAD special modes.
                     * When it's set we allow new keys to replace the current
                     * keys with the same name. */
                    dbSyncDelete(db,&keyobj);
                    dbAddRDBLoad(db,key,val);
                } else {
                    serverLog(LL_WARNING,
                        "RDB has duplicated key '%s' in DB %d",key,db->id);
                    serverPanic("Duplicated key found in RDB file");
                }
            }

            /* If minExpiredField was set, then the object is hash with expiration
             * on fields and need to register it in global HFE DS */
            if (val->type == OBJ_HASH) {
                uint64_t minExpiredField = hashTypeGetMinExpire(val, 1);
                if (minExpiredField != EB_EXPIRE_TIME_INVALID)
                    hashTypeAddToExpires(db, key, val, minExpiredField);
            }

            /* Set the expire time if needed */
            if (expiretime != -1) {
                setExpire(NULL,db,&keyobj,expiretime);
            }

            /* Set usage information (for eviction). */
            objectSetLRUOrLFU(val,lfu_freq,lru_idle,lru_clock,1000);

            /* call key space notification on key loaded for modules only */
            moduleNotifyKeyspaceEvent(NOTIFY_LOADED, "loaded", &keyobj, db->id);
        }

        /* Loading the database more slowly is useful in order to test
         * certain edge cases. */
        if (server.key_load_delay)
            debugDelay(server.key_load_delay);

        /* Reset the state that is key-specified and is populated by
         * opcodes before the key, so that we start from scratch again. */
        expiretime = -1;
        lfu_freq = -1;
        lru_idle = -1;
    }
    /* Verify the checksum if RDB version is >= 5 */
    if (rdbver >= 5) {
        uint64_t cksum, expected = rdb->cksum;

        if (rioRead(rdb,&cksum,8) == 0) goto eoferr;
        if (server.rdb_checksum && !server.skip_checksum_validation) {
            memrev64ifbe(&cksum);
            if (cksum == 0) {
                serverLog(LL_NOTICE,"RDB file was saved with checksum disabled: no check performed.");
            } else if (cksum != expected) {
                serverLog(LL_WARNING,"Wrong RDB checksum expected: (%llx) but "
                    "got (%llx). Aborting now.",
                        (unsigned long long)expected,
                        (unsigned long long)cksum);
                rdbReportCorruptRDB("RDB CRC error");
                return C_ERR;
            }
        }
    }

    if (empty_keys_skipped) {
        serverLog(LL_NOTICE,
            "Done loading RDB, keys loaded: %lld, keys expired: %lld, empty keys skipped: %lld.",
                server.rdb_last_load_keys_loaded, server.rdb_last_load_keys_expired, empty_keys_skipped);
    } else {
        serverLog(LL_NOTICE,
            "Done loading RDB, keys loaded: %lld, keys expired: %lld.",
                server.rdb_last_load_keys_loaded, server.rdb_last_load_keys_expired);
    }
    return C_OK;

    /* Unexpected end of file is handled here calling rdbReportReadError():
     * this will in turn either abort Redis in most cases, or if we are loading
     * the RDB file from a socket during initial SYNC (diskless replica mode),
     * we'll report the error to the caller, so that we can retry. */
eoferr:
    serverLog(LL_WARNING,
        "Short read or OOM loading DB. Unrecoverable error, aborting now.");
    rdbReportReadError("Unexpected EOF reading RDB file");
    return C_ERR;
}

int rdbLoad(char *filename, rdbSaveInfo *rsi, int rdbflags) {
    return rdbLoadWithEmptyFunc(filename, rsi, rdbflags, NULL);
}

/* Like rdbLoadRio() but takes a filename instead of a rio stream. The
 * filename is open for reading and a rio stream object created in order
 * to do the actual loading. Moreover the ETA displayed in the INFO
 * output is initialized and finalized.
 *
 * If you pass an 'rsi' structure initialized with RDB_SAVE_INFO_INIT, the
 * loading code will fill the information fields in the structure.
 *
 * If emptyDbFunc is not NULL, it will be called to flush old db or to
 * discard partial db on error. */
int rdbLoadWithEmptyFunc(char *filename, rdbSaveInfo *rsi, int rdbflags, void (*emptyDbFunc)(void)) {
    FILE *fp;
    rio rdb;
    int retval;
    struct stat sb;
    int rdb_fd;

    fp = fopen(filename, "r");
    if (fp == NULL) {
        if (errno == ENOENT) return RDB_NOT_EXIST;

        serverLog(LL_WARNING,"Fatal error: can't open the RDB file %s for reading: %s", filename, strerror(errno));
        return RDB_FAILED;
    }

    if (fstat(fileno(fp), &sb) == -1)
        sb.st_size = 0;

    loadingSetFlags(filename, sb.st_size, 0);
    /* Note that inside loadingSetFlags(), server.loading is set.
     * emptyDbCallback() may yield back to event-loop to reply -LOADING. */
    if (emptyDbFunc)
        emptyDbFunc(); /* Flush existing db. */
    loadingFireEvent(rdbflags);
    rioInitWithFile(&rdb,fp);

    retval = rdbLoadRio(&rdb,rdbflags,rsi);

    fclose(fp);
    if (retval != C_OK && emptyDbFunc)
        emptyDbFunc(); /* Clean up partial db. */

    stopLoading(retval==C_OK);
    /* Reclaim the cache backed by rdb */
    if (retval == C_OK && !(rdbflags & RDBFLAGS_KEEP_CACHE)) {
        /* TODO: maybe we could combine the fopen and open into one in the future */
        rdb_fd = open(filename, O_RDONLY);
        if (rdb_fd >= 0) bioCreateCloseJob(rdb_fd, 0, 1);
    }
    return (retval==C_OK) ? RDB_OK : RDB_FAILED;
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of actual BGSAVEs. */
static void backgroundSaveDoneHandlerDisk(int exitcode, int bysignal, time_t save_end) {
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background saving terminated with success");
        server.dirty = server.dirty - server.dirty_before_bgsave;
        server.lastsave = save_end;
        server.lastbgsave_status = C_OK;
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background saving error");
        server.lastbgsave_status = C_ERR;
    } else {
        mstime_t latency;

        serverLog(LL_WARNING,
            "Background saving terminated by signal %d", bysignal);
        latencyStartMonitor(latency);
        rdbRemoveTempFile(server.child_pid, 0);
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("rdb-unlink-temp-file",latency);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * triggering an error condition. */
        if (bysignal != SIGUSR1)
            server.lastbgsave_status = C_ERR;
    }
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of RDB -> Slaves socket transfers for
 * diskless replication. */
static void backgroundSaveDoneHandlerSocket(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background RDB transfer terminated with success");
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background transfer error");
    } else {
        serverLog(LL_WARNING,
            "Background transfer terminated by signal %d", bysignal);
    }
    if (server.rdb_child_exit_pipe!=-1)
        close(server.rdb_child_exit_pipe);
    if (server.rdb_pipe_read != -1) {
        aeDeleteFileEvent(server.el, server.rdb_pipe_read, AE_READABLE);
        close(server.rdb_pipe_read);
    }
    server.rdb_child_exit_pipe = -1;
    server.rdb_pipe_read = -1;
    zfree(server.rdb_pipe_conns);
    server.rdb_pipe_conns = NULL;
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    zfree(server.rdb_pipe_buff);
    server.rdb_pipe_buff = NULL;
    server.rdb_pipe_bufflen = 0;
}

/* When a background RDB saving/transfer terminates, call the right handler. */
void backgroundSaveDoneHandler(int exitcode, int bysignal) {
    int type = server.rdb_child_type;
    time_t save_end = time(NULL);
    if (server.bgsave_aborted)
        bysignal = SIGUSR1;
    switch(server.rdb_child_type) {
    case RDB_CHILD_TYPE_DISK:
        backgroundSaveDoneHandlerDisk(exitcode,bysignal,save_end);
        break;
    case RDB_CHILD_TYPE_SOCKET:
        backgroundSaveDoneHandlerSocket(exitcode,bysignal);
        break;
    default:
        serverPanic("Unknown RDB child type.");
        break;
    }

    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_last = save_end-server.rdb_save_time_start;
    server.rdb_save_time_start = -1;
    server.bgsave_aborted = 0;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, type);
}

/* Kill the RDB saving child using SIGUSR1 (so that the parent will know
 * the child did not exit for an error, but because we wanted), and performs
 * the cleanup needed. */
void killRDBChild(void) {
    kill(server.child_pid, SIGUSR1);
    /* Because we are not using here waitpid (like we have in killAppendOnlyChild
     * and TerminateModuleForkChild), all the cleanup operations is done by
     * checkChildrenDone, that later will find that the process killed.
     * This includes:
     * - resetChildState
     * - rdbRemoveTempFile */

    /* However, there's a chance the child already exited (or about to exit), and will
     * not receive the signal, in that case it could result in success and the done
     * handler will override some server metrics (e.g. the dirty counter) which it
     * shouldn't (e.g. in case of FLUSHALL), or the synchronously created RDB file. */
     server.bgsave_aborted = 1;
}

/* Spawn an RDB child that writes the RDB to the sockets of the slaves
 * that are currently in SLAVE_STATE_WAIT_BGSAVE_START state. */
int rdbSaveToSlavesSockets(int req, rdbSaveInfo *rsi) {
    listNode *ln;
    listIter li;
    pid_t childpid;
    int pipefds[2], rdb_pipe_write = 0, safe_to_exit_pipe = 0;
    int rdb_channel = (req & SLAVE_REQ_RDB_CHANNEL);

    if (hasActiveChildProcess()) return C_ERR;

    /* Even if the previous fork child exited, don't start a new one until we
     * drained the pipe. */
    if (server.rdb_pipe_conns) return C_ERR;

    if (!rdb_channel) {
        /* Before to fork, create a pipe that is used to transfer the rdb bytes to
         * the parent, we can't let it write directly to the sockets, since in case
         * of TLS we must let the parent handle a continuous TLS state when the
         * child terminates and parent takes over. */
        if (anetPipe(pipefds, O_NONBLOCK, 0) == -1) return C_ERR;
        server.rdb_pipe_read = pipefds[0]; /* read end */
        rdb_pipe_write = pipefds[1]; /* write end */

        /* create another pipe that is used by the parent to signal to the child
         * that it can exit. */
        if (anetPipe(pipefds, 0, 0) == -1) {
            close(rdb_pipe_write);
            close(server.rdb_pipe_read);
            return C_ERR;
        }
        safe_to_exit_pipe = pipefds[0]; /* read end */
        server.rdb_child_exit_pipe = pipefds[1]; /* write end */
    }

    /* Collect the connections of the replicas we want to transfer
     * the RDB to, which are in WAIT_BGSAVE_START state. */
    int numconns = 0;
    connection **conns = zmalloc(sizeof(*conns) * listLength(server.slaves));
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            /* Check slave has the exact requirements */
            if (slave->slave_req != req)
                continue;
            replicationSetupSlaveForFullResync(slave, getPsyncInitialOffset());
            conns[numconns++] = slave->conn;
            if (rdb_channel) {
                /* Put the socket in blocking mode to simplify RDB transfer. */
                connSendTimeout(slave->conn, server.repl_timeout * 1000);
                connBlock(slave->conn);
            }
        }
    }

    if (!rdb_channel) {
        server.rdb_pipe_conns = conns;
        server.rdb_pipe_numconns = numconns;
        server.rdb_pipe_numconns_writing = 0;
    }

    /* Create the child process. */
    if ((childpid = redisFork(CHILD_TYPE_RDB)) == 0) {
        /* Child */
        int retval, dummy;
        rio rdb;

        if (rdb_channel) {
            rioInitWithConnset(&rdb, conns, numconns);
        } else {
            rioInitWithFd(&rdb,rdb_pipe_write);
            /* Close the reading part, so that if the parent crashes, the child
             * will get a write error and exit. */
            close(server.rdb_pipe_read);
        }

        redisSetProcTitle("redis-rdb-to-slaves");
        redisSetCpuAffinity(server.bgsave_cpulist);

        retval = rdbSaveRioWithEOFMark(req,&rdb,NULL,rsi);
        if (retval == C_OK && rioFlush(&rdb) == 0)
            retval = C_ERR;

        if (retval == C_OK) {
            sendChildCowInfo(CHILD_INFO_TYPE_RDB_COW_SIZE, "RDB");
        }

        if (rdb_channel) {
            rioFreeConnset(&rdb);
        } else {
            rioFreeFd(&rdb);
            /* wake up the reader, tell it we're done. */
            close(rdb_pipe_write);
            close(server.rdb_child_exit_pipe); /* close write end so that we can detect the close on the parent. */
            /* hold exit until the parent tells us it's safe. we're not expecting
             * to read anything, just get the error when the pipe is closed. */
            dummy = read(safe_to_exit_pipe, pipefds, 1);
            UNUSED(dummy);
        }
        zfree(conns);
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent */
        if (childpid == -1) {
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));

            /* Undo the state change. The caller will perform cleanup on
             * all the slaves in BGSAVE_START state, but an early call to
             * replicationSetupSlaveForFullResync() turned it into BGSAVE_END */
            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                client *slave = ln->value;
                if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
                    slave->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
                }
            }

            if (!rdb_channel)  {
                close(rdb_pipe_write);
                close(server.rdb_pipe_read);
                close(server.rdb_child_exit_pipe);
                zfree(server.rdb_pipe_conns);
                server.rdb_pipe_conns = NULL;
                server.rdb_pipe_numconns = 0;
                server.rdb_pipe_numconns_writing = 0;
            }
        } else {
            serverLog(LL_NOTICE, "Background RDB transfer started by pid %ld to %s", (long)childpid,
                      rdb_channel ? "replica socket" : "parent process pipe");
            server.rdb_save_time_start = time(NULL);
            server.rdb_child_type = RDB_CHILD_TYPE_SOCKET;
            if (!rdb_channel) {
                close(rdb_pipe_write); /* close write in parent so that it can detect the close on the child. */
                if (aeCreateFileEvent(server.el, server.rdb_pipe_read, AE_READABLE, rdbPipeReadHandler,NULL) == AE_ERR) {
                    serverPanic("Unrecoverable error creating server.rdb_pipe_read file event.");
                }
            }
        }
        if (rdb_channel)
            zfree(conns);
        else
            close(safe_to_exit_pipe);

        return (childpid == -1) ? C_ERR : C_OK;
    }
    return C_OK; /* Unreached. */
}

void saveCommand(client *c) {
    if (server.child_type == CHILD_TYPE_RDB) {
        addReplyError(c,"Background save already in progress");
        return;
    }

    server.stat_rdb_saves++;

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    if (rdbSave(SLAVE_REQ_NONE,server.rdb_filename,rsiptr,RDBFLAGS_NONE) == C_OK) {
        addReply(c,shared.ok);
    } else {
        addReplyErrorObject(c,shared.err);
    }
}

/* BGSAVE [SCHEDULE] */
void bgsaveCommand(client *c) {
    int schedule = 0;

    /* The SCHEDULE option changes the behavior of BGSAVE when an AOF rewrite
     * is in progress. Instead of returning an error a BGSAVE gets scheduled. */
    if (c->argc > 1) {
        if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"schedule")) {
            schedule = 1;
        } else {
            addReplyErrorObject(c,shared.syntaxerr);
            return;
        }
    }

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);

    if (server.child_type == CHILD_TYPE_RDB) {
        addReplyError(c,"Background save already in progress");
    } else if (hasActiveChildProcess() || server.in_exec) {
        if (schedule || server.in_exec) {
            server.rdb_bgsave_scheduled = 1;
            addReplyStatus(c,"Background saving scheduled");
        } else {
            addReplyError(c,
            "Another child process is active (AOF?): can't BGSAVE right now. "
            "Use BGSAVE SCHEDULE in order to schedule a BGSAVE whenever "
            "possible.");
        }
    } else if (rdbSaveBackground(SLAVE_REQ_NONE,server.rdb_filename,rsiptr,RDBFLAGS_NONE) == C_OK) {
        addReplyStatus(c,"Background saving started");
    } else {
        addReplyErrorObject(c,shared.err);
    }
}

/* Populate the rdbSaveInfo structure used to persist the replication
 * information inside the RDB file. Currently the structure explicitly
 * contains just the currently selected DB from the master stream, however
 * if the rdbSave*() family functions receive a NULL rsi structure also
 * the Replication ID/offset is not saved. The function populates 'rsi'
 * that is normally stack-allocated in the caller, returns the populated
 * pointer if the instance has a valid master client, otherwise NULL
 * is returned, and the RDB saving will not persist any replication related
 * information. */
rdbSaveInfo *rdbPopulateSaveInfo(rdbSaveInfo *rsi) {
    rdbSaveInfo rsi_init = RDB_SAVE_INFO_INIT;
    *rsi = rsi_init;

    /* If the instance is a master, we can populate the replication info
     * only when repl_backlog is not NULL. If the repl_backlog is NULL,
     * it means that the instance isn't in any replication chains. In this
     * scenario the replication info is useless, because when a slave
     * connects to us, the NULL repl_backlog will trigger a full
     * synchronization, at the same time we will use a new replid and clear
     * replid2. */
    if (!server.masterhost && server.repl_backlog) {
        /* Note that when server.slaveseldb is -1, it means that this master
         * didn't apply any write commands after a full synchronization.
         * So we can let repl_stream_db be 0, this allows a restarted slave
         * to reload replication ID/offset, it's safe because the next write
         * command must generate a SELECT statement. */
        rsi->repl_stream_db = server.slaveseldb == -1 ? 0 : server.slaveseldb;
        return rsi;
    }

    /* If the instance is a slave we need a connected master
     * in order to fetch the currently selected DB. */
    if (server.master) {
        rsi->repl_stream_db = server.master->db->id;
        return rsi;
    }

    /* If we have a cached master we can use it in order to populate the
     * replication selected DB info inside the RDB file: the slave can
     * increment the master_repl_offset only from data arriving from the
     * master, so if we are disconnected the offset in the cached master
     * is valid. */
    if (server.cached_master) {
        rsi->repl_stream_db = server.cached_master->db->id;
        return rsi;
    }
    return NULL;
}
