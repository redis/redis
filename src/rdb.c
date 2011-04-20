#include "redis.h"
#include "lzf.h"    /* LZF compression library */

#include <math.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/stat.h>

/* Convenience wrapper around fwrite, that returns the number of bytes written
 * to the file instead of the number of objects (see fwrite(3)) and -1 in the
 * case of an error. It also supports a NULL *fp to skip writing altogether
 * instead of writing to /dev/null. */
static int rdbWriteRaw(FILE *fp, void *p, size_t len) {
    if (fp != NULL && fwrite(p,len,1,fp) == 0) return -1;
    return len;
}

int rdbSaveType(FILE *fp, unsigned char type) {
    return rdbWriteRaw(fp,&type,1);
}

int rdbSaveTime(FILE *fp, time_t t) {
    int32_t t32 = (int32_t) t;
    return rdbWriteRaw(fp,&t32,4);
}

/* check rdbLoadLen() comments for more info */
int rdbSaveLen(FILE *fp, uint32_t len) {
    unsigned char buf[2];
    int nwritten;

    if (len < (1<<6)) {
        /* Save a 6 bit len */
        buf[0] = (len&0xFF)|(REDIS_RDB_6BITLEN<<6);
        if (rdbWriteRaw(fp,buf,1) == -1) return -1;
        nwritten = 1;
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        buf[0] = ((len>>8)&0xFF)|(REDIS_RDB_14BITLEN<<6);
        buf[1] = len&0xFF;
        if (rdbWriteRaw(fp,buf,2) == -1) return -1;
        nwritten = 2;
    } else {
        /* Save a 32 bit len */
        buf[0] = (REDIS_RDB_32BITLEN<<6);
        if (rdbWriteRaw(fp,buf,1) == -1) return -1;
        len = htonl(len);
        if (rdbWriteRaw(fp,&len,4) == -1) return -1;
        nwritten = 1+4;
    }
    return nwritten;
}

/* Encode 'value' as an integer if possible (if integer will fit the
 * supported range). If the function sucessful encoded the integer
 * then the (up to 5 bytes) encoded representation is written in the
 * string pointed by 'enc' and the length is returned. Otherwise
 * 0 is returned. */
int rdbEncodeInteger(long long value, unsigned char *enc) {
    /* Finally check if it fits in our ranges */
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space */
int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    char *endptr, buf[32];

    /* Check if it's possible to encode this value as a number */
    value = strtoll(s, &endptr, 10);
    if (endptr[0] != '\0') return 0;
    ll2string(buf,32,value);

    /* If the number converted back into a string is not identical
     * then it's not possible to encode the string as integer */
    if (strlen(buf) != len || memcmp(buf,s,len)) return 0;

    return rdbEncodeInteger(value,enc);
}

int rdbSaveLzfStringObject(FILE *fp, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    unsigned char byte;
    int n, nwritten = 0;
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
    /* Data compressed! Let's save it on disk */
    byte = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_LZF;
    if ((n = rdbWriteRaw(fp,&byte,1)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbSaveLen(fp,comprlen)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbSaveLen(fp,len)) == -1) goto writeerr;
    nwritten += n;

    if ((n = rdbWriteRaw(fp,out,comprlen)) == -1) goto writeerr;
    nwritten += n;

    zfree(out);
    return nwritten;

writeerr:
    zfree(out);
    return -1;
}

/* Save a string objet as [len][data] on disk. If the object is a string
 * representation of an integer value we try to save it in a special form */
int rdbSaveRawString(FILE *fp, unsigned char *s, size_t len) {
    int enclen;
    int n, nwritten = 0;

    /* Try integer encoding */
    if (len <= 11) {
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
            if (rdbWriteRaw(fp,buf,enclen) == -1) return -1;
            return enclen;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it */
    if (server.rdbcompression && len > 20) {
        n = rdbSaveLzfStringObject(fp,s,len);
        if (n == -1) return -1;
        if (n > 0) return n;
        /* Return value of 0 means data can't be compressed, save the old way */
    }

    /* Store verbatim */
    if ((n = rdbSaveLen(fp,len)) == -1) return -1;
    nwritten += n;
    if (len > 0) {
        if (rdbWriteRaw(fp,s,len) == -1) return -1;
        nwritten += len;
    }
    return nwritten;
}

/* Save a long long value as either an encoded string or a string. */
int rdbSaveLongLongAsStringObject(FILE *fp, long long value) {
    unsigned char buf[32];
    int n, nwritten = 0;
    int enclen = rdbEncodeInteger(value,buf);
    if (enclen > 0) {
        return rdbWriteRaw(fp,buf,enclen);
    } else {
        /* Encode as string */
        enclen = ll2string((char*)buf,32,value);
        redisAssert(enclen < 32);
        if ((n = rdbSaveLen(fp,enclen)) == -1) return -1;
        nwritten += n;
        if ((n = rdbWriteRaw(fp,buf,enclen)) == -1) return -1;
        nwritten += n;
    }
    return nwritten;
}

/* Like rdbSaveStringObjectRaw() but handle encoded objects */
int rdbSaveStringObject(FILE *fp, robj *obj) {
    /* Avoid to decode the object, then encode it again, if the
     * object is alrady integer encoded. */
    if (obj->encoding == REDIS_ENCODING_INT) {
        return rdbSaveLongLongAsStringObject(fp,(long)obj->ptr);
    } else {
        redisAssert(obj->encoding == REDIS_ENCODING_RAW);
        return rdbSaveRawString(fp,obj->ptr,sdslen(obj->ptr));
    }
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifing the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 */
int rdbSaveDoubleValue(FILE *fp, double val) {
    unsigned char buf[128];
    int len;

    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
    } else {
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
        /* Check if the float is in a safe range to be casted into a
         * long long. We are assuming that long long is 64 bit here.
         * Also we are assuming that there are no implementations around where
         * double has precision < 52 bit.
         *
         * Under this assumptions we test if a double is inside an interval
         * where casting to long long is safe. Then using two castings we
         * make sure the decimal part is zero. If all this is true we use
         * integer printing function that is much faster. */
        double min = -4503599627370495; /* (2^52)-1 */
        double max = 4503599627370496; /* -(2^52) */
        if (val > min && val < max && val == ((double)((long long)val)))
            ll2string((char*)buf+1,sizeof(buf),(long long)val);
        else
#endif
            snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = strlen((char*)buf+1);
        len = buf[0]+1;
    }
    return rdbWriteRaw(fp,buf,len);
}

/* Save a Redis object. */
int rdbSaveObject(FILE *fp, robj *o) {
    int n, nwritten = 0;

    if (o->type == REDIS_STRING) {
        /* Save a string value */
        if ((n = rdbSaveStringObject(fp,o)) == -1) return -1;
        nwritten += n;
    } else if (o->type == REDIS_LIST) {
        /* Save a list value */
        if (o->encoding == REDIS_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(fp,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
            list *list = o->ptr;
            listIter li;
            listNode *ln;

            if ((n = rdbSaveLen(fp,listLength(list))) == -1) return -1;
            nwritten += n;

            listRewind(list,&li);
            while((ln = listNext(&li))) {
                robj *eleobj = listNodeValue(ln);
                if ((n = rdbSaveStringObject(fp,eleobj)) == -1) return -1;
                nwritten += n;
            }
        } else {
            redisPanic("Unknown list encoding");
        }
    } else if (o->type == REDIS_SET) {
        /* Save a set value */
        if (o->encoding == REDIS_ENCODING_HT) {
            dict *set = o->ptr;
            dictIterator *di = dictGetIterator(set);
            dictEntry *de;

            if ((n = rdbSaveLen(fp,dictSize(set))) == -1) return -1;
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                robj *eleobj = dictGetEntryKey(de);
                if ((n = rdbSaveStringObject(fp,eleobj)) == -1) return -1;
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else if (o->encoding == REDIS_ENCODING_INTSET) {
            size_t l = intsetBlobLen((intset*)o->ptr);

            if ((n = rdbSaveRawString(fp,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else {
            redisPanic("Unknown set encoding");
        }
    } else if (o->type == REDIS_ZSET) {
        /* Save a sorted set value */
        if (o->encoding == REDIS_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(fp,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == REDIS_ENCODING_SKIPLIST) {
            zset *zs = o->ptr;
            dictIterator *di = dictGetIterator(zs->dict);
            dictEntry *de;

            if ((n = rdbSaveLen(fp,dictSize(zs->dict))) == -1) return -1;
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                robj *eleobj = dictGetEntryKey(de);
                double *score = dictGetEntryVal(de);

                if ((n = rdbSaveStringObject(fp,eleobj)) == -1) return -1;
                nwritten += n;
                if ((n = rdbSaveDoubleValue(fp,*score)) == -1) return -1;
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else {
            redisPanic("Unknown sorted set encoding");
        }
    } else if (o->type == REDIS_HASH) {
        /* Save a hash value */
        if (o->encoding == REDIS_ENCODING_ZIPMAP) {
            size_t l = zipmapBlobLen((unsigned char*)o->ptr);

            if ((n = rdbSaveRawString(fp,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else {
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;

            if ((n = rdbSaveLen(fp,dictSize((dict*)o->ptr))) == -1) return -1;
            nwritten += n;

            while((de = dictNext(di)) != NULL) {
                robj *key = dictGetEntryKey(de);
                robj *val = dictGetEntryVal(de);

                if ((n = rdbSaveStringObject(fp,key)) == -1) return -1;
                nwritten += n;
                if ((n = rdbSaveStringObject(fp,val)) == -1) return -1;
                nwritten += n;
            }
            dictReleaseIterator(di);
        }
    } else {
        redisPanic("Unknown object type");
    }
    return nwritten;
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. */
off_t rdbSavedObjectLen(robj *o) {
    int len = rdbSaveObject(NULL,o);
    redisAssert(len != -1);
    return len;
}

/* Return the number of pages required to save this object in the swap file */
off_t rdbSavedObjectPages(robj *o) {
    off_t bytes = rdbSavedObjectLen(o);
    return (bytes+(server.vm_page_size-1))/server.vm_page_size;
}

/* Save the DB on disk. Return REDIS_ERR on error, REDIS_OK on success */
int rdbSave(char *filename) {
    dictIterator *di = NULL;
    dictEntry *de;
    FILE *fp;
    char tmpfile[256];
    int j;
    time_t now = time(NULL);

    /* Wait for I/O therads to terminate, just in case this is a
     * foreground-saving, to avoid seeking the swap file descriptor at the
     * same time. */
    if (server.vm_enabled)
        waitEmptyIOJobsQueue();

    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        redisLog(REDIS_WARNING, "Failed saving the DB: %s", strerror(errno));
        return REDIS_ERR;
    }
    if (fwrite("REDIS0001",9,1,fp) == 0) goto werr;
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetIterator(d);
        if (!di) {
            fclose(fp);
            return REDIS_ERR;
        }

        /* Write the SELECT DB opcode */
        if (rdbSaveType(fp,REDIS_SELECTDB) == -1) goto werr;
        if (rdbSaveLen(fp,j) == -1) goto werr;

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            sds keystr = dictGetEntryKey(de);
            robj key, *o = dictGetEntryVal(de);
            time_t expiretime;
            
            initStaticStringObject(key,keystr);
            expiretime = getExpire(db,&key);

            /* Save the expire time */
            if (expiretime != -1) {
                /* If this key is already expired skip it */
                if (expiretime < now) continue;
                if (rdbSaveType(fp,REDIS_EXPIRETIME) == -1) goto werr;
                if (rdbSaveTime(fp,expiretime) == -1) goto werr;
            }
            /* Save the key and associated value. This requires special
             * handling if the value is swapped out. */
            if (!server.vm_enabled || o->storage == REDIS_VM_MEMORY ||
                                      o->storage == REDIS_VM_SWAPPING) {
                int otype = o->type;

                /* Fix the type id for specially encoded data types */
                if (otype == REDIS_HASH && o->encoding == REDIS_ENCODING_ZIPMAP)
                    otype = REDIS_HASH_ZIPMAP;
                else if (otype == REDIS_LIST &&
                         o->encoding == REDIS_ENCODING_ZIPLIST)
                    otype = REDIS_LIST_ZIPLIST;
                else if (otype == REDIS_SET &&
                         o->encoding == REDIS_ENCODING_INTSET)
                    otype = REDIS_SET_INTSET;
                /* Save type, key, value */
                if (rdbSaveType(fp,otype) == -1) goto werr;
                if (rdbSaveStringObject(fp,&key) == -1) goto werr;
                if (rdbSaveObject(fp,o) == -1) goto werr;
            } else {
                /* REDIS_VM_SWAPPED or REDIS_VM_LOADING */
                robj *po;
                /* Get a preview of the object in memory */
                po = vmPreviewObject(o);
                /* Save type, key, value */
                if (rdbSaveType(fp,po->type) == -1) goto werr;
                if (rdbSaveStringObject(fp,&key) == -1) goto werr;
                if (rdbSaveObject(fp,po) == -1) goto werr;
                /* Remove the loaded object from memory */
                decrRefCount(po);
            }
        }
        dictReleaseIterator(di);
    }
    /* EOF opcode */
    if (rdbSaveType(fp,REDIS_EOF) == -1) goto werr;

    /* Make sure data will not remain on the OS's output buffers */
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        redisLog(REDIS_WARNING,"Error moving temp DB file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        return REDIS_ERR;
    }
    redisLog(REDIS_NOTICE,"DB saved on disk");
    server.dirty = 0;
    server.lastsave = time(NULL);
    return REDIS_OK;

werr:
    fclose(fp);
    unlink(tmpfile);
    redisLog(REDIS_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    if (di) dictReleaseIterator(di);
    return REDIS_ERR;
}

int rdbSaveBackground(char *filename) {
    pid_t childpid;

    if (server.bgsavechildpid != -1) return REDIS_ERR;
    if (server.vm_enabled) waitEmptyIOJobsQueue();
    server.dirty_before_bgsave = server.dirty;
    if ((childpid = fork()) == 0) {
        /* Child */
        if (server.vm_enabled) vmReopenSwapFile();
        if (server.ipfd > 0) close(server.ipfd);
        if (server.sofd > 0) close(server.sofd);
        if (rdbSave(filename) == REDIS_OK) {
            _exit(0);
        } else {
            _exit(1);
        }
    } else {
        /* Parent */
        if (childpid == -1) {
            redisLog(REDIS_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        redisLog(REDIS_NOTICE,"Background saving started by pid %d",childpid);
        server.bgsavechildpid = childpid;
        updateDictResizePolicy();
        return REDIS_OK;
    }
    return REDIS_OK; /* unreached */
}

void rdbRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-%d.rdb", (int) childpid);
    unlink(tmpfile);
}

int rdbLoadType(FILE *fp) {
    unsigned char type;
    if (fread(&type,1,1,fp) == 0) return -1;
    return type;
}

time_t rdbLoadTime(FILE *fp) {
    int32_t t32;
    if (fread(&t32,4,1,fp) == 0) return -1;
    return (time_t) t32;
}

/* Load an encoded length from the DB, see the REDIS_RDB_* defines on the top
 * of this file for a description of how this are stored on disk.
 *
 * isencoded is set to 1 if the readed length is not actually a length but
 * an "encoding type", check the above comments for more info */
uint32_t rdbLoadLen(FILE *fp, int *isencoded) {
    unsigned char buf[2];
    uint32_t len;
    int type;

    if (isencoded) *isencoded = 0;
    if (fread(buf,1,1,fp) == 0) return REDIS_RDB_LENERR;
    type = (buf[0]&0xC0)>>6;
    if (type == REDIS_RDB_6BITLEN) {
        /* Read a 6 bit len */
        return buf[0]&0x3F;
    } else if (type == REDIS_RDB_ENCVAL) {
        /* Read a 6 bit len encoding type */
        if (isencoded) *isencoded = 1;
        return buf[0]&0x3F;
    } else if (type == REDIS_RDB_14BITLEN) {
        /* Read a 14 bit len */
        if (fread(buf+1,1,1,fp) == 0) return REDIS_RDB_LENERR;
        return ((buf[0]&0x3F)<<8)|buf[1];
    } else {
        /* Read a 32 bit len */
        if (fread(&len,4,1,fp) == 0) return REDIS_RDB_LENERR;
        return ntohl(len);
    }
}

/* Load an integer-encoded object from file 'fp', with the specified
 * encoding type 'enctype'. If encode is true the function may return
 * an integer-encoded object as reply, otherwise the returned object
 * will always be encoded as a raw string. */
robj *rdbLoadIntegerObject(FILE *fp, int enctype, int encode) {
    unsigned char enc[4];
    long long val;

    if (enctype == REDIS_RDB_ENC_INT8) {
        if (fread(enc,1,1,fp) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == REDIS_RDB_ENC_INT16) {
        uint16_t v;
        if (fread(enc,2,1,fp) == 0) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == REDIS_RDB_ENC_INT32) {
        uint32_t v;
        if (fread(enc,4,1,fp) == 0) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        val = 0; /* anti-warning */
        redisPanic("Unknown RDB integer encoding type");
    }
    if (encode)
        return createStringObjectFromLongLong(val);
    else
        return createObject(REDIS_STRING,sdsfromlonglong(val));
}

robj *rdbLoadLzfStringObject(FILE*fp) {
    unsigned int len, clen;
    unsigned char *c = NULL;
    sds val = NULL;

    if ((clen = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((len = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((c = zmalloc(clen)) == NULL) goto err;
    if ((val = sdsnewlen(NULL,len)) == NULL) goto err;
    if (fread(c,clen,1,fp) == 0) goto err;
    if (lzf_decompress(c,clen,val,len) == 0) goto err;
    zfree(c);
    return createObject(REDIS_STRING,val);
err:
    zfree(c);
    sdsfree(val);
    return NULL;
}

robj *rdbGenericLoadStringObject(FILE*fp, int encode) {
    int isencoded;
    uint32_t len;
    sds val;

    len = rdbLoadLen(fp,&isencoded);
    if (isencoded) {
        switch(len) {
        case REDIS_RDB_ENC_INT8:
        case REDIS_RDB_ENC_INT16:
        case REDIS_RDB_ENC_INT32:
            return rdbLoadIntegerObject(fp,len,encode);
        case REDIS_RDB_ENC_LZF:
            return rdbLoadLzfStringObject(fp);
        default:
            redisPanic("Unknown RDB encoding type");
        }
    }

    if (len == REDIS_RDB_LENERR) return NULL;
    val = sdsnewlen(NULL,len);
    if (len && fread(val,len,1,fp) == 0) {
        sdsfree(val);
        return NULL;
    }
    return createObject(REDIS_STRING,val);
}

robj *rdbLoadStringObject(FILE *fp) {
    return rdbGenericLoadStringObject(fp,0);
}

robj *rdbLoadEncodedStringObject(FILE *fp) {
    return rdbGenericLoadStringObject(fp,1);
}

/* For information about double serialization check rdbSaveDoubleValue() */
int rdbLoadDoubleValue(FILE *fp, double *val) {
    char buf[128];
    unsigned char len;

    if (fread(&len,1,1,fp) == 0) return -1;
    switch(len) {
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;
    default:
        if (fread(buf,len,1,fp) == 0) return -1;
        buf[len] = '\0';
        sscanf(buf, "%lg", val);
        return 0;
    }
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL. */
robj *rdbLoadObject(int type, FILE *fp) {
    robj *o, *ele, *dec;
    size_t len;
    unsigned int i;

    redisLog(REDIS_DEBUG,"LOADING OBJECT %d (at %d)\n",type,ftell(fp));
    if (type == REDIS_STRING) {
        /* Read string value */
        if ((o = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
        o = tryObjectEncoding(o);
    } else if (type == REDIS_LIST) {
        /* Read list value */
        if ((len = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;

        /* Use a real list when there are too many entries */
        if (len > server.list_max_ziplist_entries) {
            o = createListObject();
        } else {
            o = createZiplistObject();
        }

        /* Load every single element of the list */
        while(len--) {
            if ((ele = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;

            /* If we are using a ziplist and the value is too big, convert
             * the object to a real list. */
            if (o->encoding == REDIS_ENCODING_ZIPLIST &&
                ele->encoding == REDIS_ENCODING_RAW &&
                sdslen(ele->ptr) > server.list_max_ziplist_value)
                    listTypeConvert(o,REDIS_ENCODING_LINKEDLIST);

            if (o->encoding == REDIS_ENCODING_ZIPLIST) {
                dec = getDecodedObject(ele);
                o->ptr = ziplistPush(o->ptr,dec->ptr,sdslen(dec->ptr),REDIS_TAIL);
                decrRefCount(dec);
                decrRefCount(ele);
            } else {
                ele = tryObjectEncoding(ele);
                listAddNodeTail(o->ptr,ele);
            }
        }
    } else if (type == REDIS_SET) {
        /* Read list/set value */
        if ((len = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;

        /* Use a regular set when there are too many entries. */
        if (len > server.set_max_intset_entries) {
            o = createSetObject();
            /* It's faster to expand the dict to the right size asap in order
             * to avoid rehashing */
            if (len > DICT_HT_INITIAL_SIZE)
                dictExpand(o->ptr,len);
        } else {
            o = createIntsetObject();
        }

        /* Load every single element of the list/set */
        for (i = 0; i < len; i++) {
            long long llval;
            if ((ele = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
            ele = tryObjectEncoding(ele);

            if (o->encoding == REDIS_ENCODING_INTSET) {
                /* Fetch integer value from element */
                if (isObjectRepresentableAsLongLong(ele,&llval) == REDIS_OK) {
                    o->ptr = intsetAdd(o->ptr,llval,NULL);
                } else {
                    setTypeConvert(o,REDIS_ENCODING_HT);
                    dictExpand(o->ptr,len);
                }
            }

            /* This will also be called when the set was just converted
             * to regular hashtable encoded set */
            if (o->encoding == REDIS_ENCODING_HT) {
                dictAdd((dict*)o->ptr,ele,NULL);
            } else {
                decrRefCount(ele);
            }
        }
    } else if (type == REDIS_ZSET) {
        /* Read list/set value */
        size_t zsetlen;
        size_t maxelelen = 0;
        zset *zs;

        if ((zsetlen = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = createZsetObject();
        zs = o->ptr;

        /* Load every single element of the list/set */
        while(zsetlen--) {
            robj *ele;
            double score;
            zskiplistNode *znode;

            if ((ele = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
            ele = tryObjectEncoding(ele);
            if (rdbLoadDoubleValue(fp,&score) == -1) return NULL;

            /* Don't care about integer-encoded strings. */
            if (ele->encoding == REDIS_ENCODING_RAW &&
                sdslen(ele->ptr) > maxelelen)
                    maxelelen = sdslen(ele->ptr);

            znode = zslInsert(zs->zsl,score,ele);
            dictAdd(zs->dict,ele,&znode->score);
            incrRefCount(ele); /* added to skiplist */
        }

        /* Convert *after* loading, since sorted sets are not stored ordered. */
        if (zsetLength(o) <= server.zset_max_ziplist_entries &&
            maxelelen <= server.zset_max_ziplist_value)
                zsetConvert(o,REDIS_ENCODING_ZIPLIST);
    } else if (type == REDIS_HASH) {
        size_t hashlen;

        if ((hashlen = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR) return NULL;
        o = createHashObject();
        /* Too many entries? Use an hash table. */
        if (hashlen > server.hash_max_zipmap_entries)
            convertToRealHash(o);
        /* Load every key/value, then set it into the zipmap or hash
         * table, as needed. */
        while(hashlen--) {
            robj *key, *val;

            if ((key = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
            if ((val = rdbLoadEncodedStringObject(fp)) == NULL) return NULL;
            /* If we are using a zipmap and there are too big values
             * the object is converted to real hash table encoding. */
            if (o->encoding != REDIS_ENCODING_HT &&
               ((key->encoding == REDIS_ENCODING_RAW &&
                sdslen(key->ptr) > server.hash_max_zipmap_value) ||
                (val->encoding == REDIS_ENCODING_RAW &&
                sdslen(val->ptr) > server.hash_max_zipmap_value)))
            {
                    convertToRealHash(o);
            }

            if (o->encoding == REDIS_ENCODING_ZIPMAP) {
                unsigned char *zm = o->ptr;
                robj *deckey, *decval;

                /* We need raw string objects to add them to the zipmap */
                deckey = getDecodedObject(key);
                decval = getDecodedObject(val);
                zm = zipmapSet(zm,deckey->ptr,sdslen(deckey->ptr),
                                  decval->ptr,sdslen(decval->ptr),NULL);
                o->ptr = zm;
                decrRefCount(deckey);
                decrRefCount(decval);
                decrRefCount(key);
                decrRefCount(val);
            } else {
                key = tryObjectEncoding(key);
                val = tryObjectEncoding(val);
                dictAdd((dict*)o->ptr,key,val);
            }
        }
    } else if (type == REDIS_HASH_ZIPMAP ||
               type == REDIS_LIST_ZIPLIST ||
               type == REDIS_SET_INTSET ||
               type == REDIS_ZSET_ZIPLIST)
    {
        robj *aux = rdbLoadStringObject(fp);

        if (aux == NULL) return NULL;
        o = createObject(REDIS_STRING,NULL); /* string is just placeholder */
        o->ptr = zmalloc(sdslen(aux->ptr));
        memcpy(o->ptr,aux->ptr,sdslen(aux->ptr));
        decrRefCount(aux);

        /* Fix the object encoding, and make sure to convert the encoded
         * data type into the base type if accordingly to the current
         * configuration there are too many elements in the encoded data
         * type. Note that we only check the length and not max element
         * size as this is an O(N) scan. Eventually everything will get
         * converted. */
        switch(type) {
            case REDIS_HASH_ZIPMAP:
                o->type = REDIS_HASH;
                o->encoding = REDIS_ENCODING_ZIPMAP;
                if (zipmapLen(o->ptr) > server.hash_max_zipmap_entries)
                    convertToRealHash(o);
                break;
            case REDIS_LIST_ZIPLIST:
                o->type = REDIS_LIST;
                o->encoding = REDIS_ENCODING_ZIPLIST;
                if (ziplistLen(o->ptr) > server.list_max_ziplist_entries)
                    listTypeConvert(o,REDIS_ENCODING_LINKEDLIST);
                break;
            case REDIS_SET_INTSET:
                o->type = REDIS_SET;
                o->encoding = REDIS_ENCODING_INTSET;
                if (intsetLen(o->ptr) > server.set_max_intset_entries)
                    setTypeConvert(o,REDIS_ENCODING_HT);
                break;
            case REDIS_ZSET_ZIPLIST:
                o->type = REDIS_ZSET;
                o->encoding = REDIS_ENCODING_ZIPLIST;
                if (zsetLength(o) > server.zset_max_ziplist_entries)
                    zsetConvert(o,REDIS_ENCODING_SKIPLIST);
                break;
            default:
                redisPanic("Unknown encoding");
                break;
        }
    } else {
        redisPanic("Unknown object type");
    }
    return o;
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats. */
void startLoading(FILE *fp) {
    struct stat sb;

    /* Load the DB */
    server.loading = 1;
    server.loading_start_time = time(NULL);
    if (fstat(fileno(fp), &sb) == -1) {
        server.loading_total_bytes = 1; /* just to avoid division by zero */
    } else {
        server.loading_total_bytes = sb.st_size;
    }
}

/* Refresh the loading progress info */
void loadingProgress(off_t pos) {
    server.loading_loaded_bytes = pos;
}

/* Loading finished */
void stopLoading(void) {
    server.loading = 0;
}

int rdbLoad(char *filename) {
    FILE *fp;
    uint32_t dbid;
    int type, retval, rdbver;
    int swap_all_values = 0;
    redisDb *db = server.db+0;
    char buf[1024];
    time_t expiretime, now = time(NULL);
    long loops = 0;

    fp = fopen(filename,"r");
    if (!fp) return REDIS_ERR;
    if (fread(buf,9,1,fp) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Wrong signature trying to load DB from file");
        return REDIS_ERR;
    }
    rdbver = atoi(buf+5);
    if (rdbver != 1) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Can't handle RDB format version %d",rdbver);
        return REDIS_ERR;
    }

    startLoading(fp);
    while(1) {
        robj *key, *val;
        int force_swapout;

        expiretime = -1;

        /* Serve the clients from time to time */
        if (!(loops++ % 1000)) {
            loadingProgress(ftello(fp));
            aeProcessEvents(server.el, AE_FILE_EVENTS|AE_DONT_WAIT);
        }

        /* Read type. */
        if ((type = rdbLoadType(fp)) == -1) goto eoferr;
        if (type == REDIS_EXPIRETIME) {
            if ((expiretime = rdbLoadTime(fp)) == -1) goto eoferr;
            /* We read the time so we need to read the object type again */
            if ((type = rdbLoadType(fp)) == -1) goto eoferr;
        }
        if (type == REDIS_EOF) break;
        /* Handle SELECT DB opcode as a special case */
        if (type == REDIS_SELECTDB) {
            if ((dbid = rdbLoadLen(fp,NULL)) == REDIS_RDB_LENERR)
                goto eoferr;
            if (dbid >= (unsigned)server.dbnum) {
                redisLog(REDIS_WARNING,"FATAL: Data file was created with a Redis server configured to handle more than %d databases. Exiting\n", server.dbnum);
                exit(1);
            }
            db = server.db+dbid;
            continue;
        }
        /* Read key */
        if ((key = rdbLoadStringObject(fp)) == NULL) goto eoferr;
        /* Read value */
        if ((val = rdbLoadObject(type,fp)) == NULL) goto eoferr;
        /* Check if the key already expired */
        if (expiretime != -1 && expiretime < now) {
            decrRefCount(key);
            decrRefCount(val);
            continue;
        }
        /* Add the new object in the hash table */
        retval = dbAdd(db,key,val);
        if (retval == REDIS_ERR) {
            redisLog(REDIS_WARNING,"Loading DB, duplicated key (%s) found! Unrecoverable error, exiting now.", key->ptr);
            exit(1);
        }
        /* Set the expire time if needed */
        if (expiretime != -1) setExpire(db,key,expiretime);

        /* Handle swapping while loading big datasets when VM is on */

        /* If we detecter we are hopeless about fitting something in memory
         * we just swap every new key on disk. Directly...
         * Note that's important to check for this condition before resorting
         * to random sampling, otherwise we may try to swap already
         * swapped keys. */
        if (swap_all_values) {
            dictEntry *de = dictFind(db->dict,key->ptr);

            /* de may be NULL since the key already expired */
            if (de) {
                vmpointer *vp;
                val = dictGetEntryVal(de);

                if (val->refcount == 1 &&
                    (vp = vmSwapObjectBlocking(val)) != NULL)
                    dictGetEntryVal(de) = vp;
            }
            decrRefCount(key);
            continue;
        }
        decrRefCount(key);

        /* Flush data on disk once 32 MB of additional RAM are used... */
        force_swapout = 0;
        if ((zmalloc_used_memory() - server.vm_max_memory) > 1024*1024*32)
            force_swapout = 1;

        /* If we have still some hope of having some value fitting memory
         * then we try random sampling. */
        if (!swap_all_values && server.vm_enabled && force_swapout) {
            while (zmalloc_used_memory() > server.vm_max_memory) {
                if (vmSwapOneObjectBlocking() == REDIS_ERR) break;
            }
            if (zmalloc_used_memory() > server.vm_max_memory)
                swap_all_values = 1; /* We are already using too much mem */
        }
    }
    fclose(fp);
    stopLoading();
    return REDIS_OK;

eoferr: /* unexpected end of file is handled here with a fatal exit */
    redisLog(REDIS_WARNING,"Short read or OOM loading DB. Unrecoverable error, aborting now.");
    exit(1);
    return REDIS_ERR; /* Just to avoid warning */
}

/* A background saving child (BGSAVE) terminated its work. Handle this. */
void backgroundSaveDoneHandler(int statloc) {
    int exitcode = WEXITSTATUS(statloc);
    int bysignal = WIFSIGNALED(statloc);

    if (!bysignal && exitcode == 0) {
        redisLog(REDIS_NOTICE,
            "Background saving terminated with success");
        server.dirty = server.dirty - server.dirty_before_bgsave;
        server.lastsave = time(NULL);
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "Background saving error");
    } else {
        redisLog(REDIS_WARNING,
            "Background saving terminated by signal %d", WTERMSIG(statloc));
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    server.bgsavechildpid = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    updateSlavesWaitingBgsave(exitcode == 0 ? REDIS_OK : REDIS_ERR);
}
