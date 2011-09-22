#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#include <stdio.h>
#include "sds.h"

struct _rio {
    /* Backend functions.
     * Since this functions do not tolerate short writes or reads the return
     * value is simplified to: zero on error, non zero on complete success. */
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

void rioInitWithFile(rio *r, FILE *fp);
void rioInitWithBuffer(rio *r, sds s);

size_t rioWriteBulkCount(rio *r, char prefix, int count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, long long l);
size_t rioWriteBulkDouble(rio *r, double d);

#endif
