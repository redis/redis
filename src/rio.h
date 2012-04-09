#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#include <stdio.h>
#include <stdint.h>
#include "sds.h"

struct _rio {
    /* Backend functions.
     * Since this functions do not tolerate short writes or reads the return
     * value is simplified to: zero on error, non zero on complete success. */
    size_t (*read)(struct _rio *, void *buf, size_t len);
    size_t (*write)(struct _rio *, const void *buf, size_t len);
    off_t (*tell)(struct _rio *);
    /* The update_cksum method if not NULL is used to compute the checksum of all the
     * data that was read or written so far. The method should be designed so that
     * can be called with the current checksum, and the buf and len fields pointing
     * to the new block of data to add to the checksum computation. */
    void (*update_cksum)(struct _rio *, const void *buf, size_t len);

    /* The current checksum */
    uint64_t cksum;

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

/* The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */

inline size_t rioWrite(rio *r, const void *buf, size_t len) {
    if (r->update_cksum) r->update_cksum(r,buf,len);
    return r->write(r,buf,len);
}

inline size_t rioRead(rio *r, void *buf, size_t len) {
    if (r->read(r,buf,len) == 1) {
        if (r->update_cksum) r->update_cksum(r,buf,len);
        return 1;
    }
    return 0;
}

inline off_t rioTell(rio *r) {
    return r->tell(r);
}

void rioInitWithFile(rio *r, FILE *fp);
void rioInitWithBuffer(rio *r, sds s);

size_t rioWriteBulkCount(rio *r, char prefix, int count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, long long l);
size_t rioWriteBulkDouble(rio *r, double d);

void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len);

#endif
