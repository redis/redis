#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#include <stdio.h>
#include "sds.h"

struct _rio {
    /* Backend functions. Both read and write should return 0 for short reads
     * or writes, identical to the return values of fread/fwrite. */
    size_t (*read)(struct _rio *, void *buf, size_t len);
    size_t (*write)(struct _rio *, const void *buf, size_t len);
    off_t (*tell)(struct _rio *);

    /* Backend-specific vars. */
    union {
        struct {
            sds ptr;
            off_t pos;
        } buffer;
        struct {
            FILE *fp;
        } file;
    } io;
};

typedef struct _rio rio;

#define rioWrite(rio,buf,len) ((rio)->write((rio),(buf),(len)))
#define rioRead(rio,buf,len) ((rio)->read((rio),(buf),(len)))

rio rioInitWithFile(FILE *fp);
rio rioInitWithBuffer(sds s);

size_t rioWriteBulkCount(rio *r, char prefix, int count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, long long l);
size_t rioWriteBulkDouble(rio *r, double d);

#endif
