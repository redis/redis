/* Synchronous socket and file I/O operations useful across the core.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"

/* ----------------- Blocking sockets I/O with timeouts --------------------- */

/* Redis performs most of the I/O in a nonblocking way, with the exception
 * of the SYNC command where the slave does it in a blocking way, and
 * the MIGRATE command that must be blocking in order to be atomic from the
 * point of view of the two instances (one migrating the key and one receiving
 * the key). This is why need the following blocking I/O functions.
 *
 * All the functions take the timeout in milliseconds. */

#define SYNCIO__RESOLUTION 10 /* Resolution in milliseconds */

/* Write the specified payload to 'fd'. If writing the whole payload will be
 * done within 'timeout' milliseconds the operation succeeds and 'size' is
 * returned. Otherwise the operation fails, -1 is returned, and an unspecified
 * partial write could be performed against the file descriptor. */
ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout) {
    ssize_t nwritten, ret = size;
    long long start = mstime();
    long long remaining = timeout;

    while(1) {
        long long wait = (remaining > SYNCIO__RESOLUTION) ?
                          remaining : SYNCIO__RESOLUTION;
        long long elapsed;

        /* Optimistically try to write before checking if the file descriptor
         * is actually writable. At worst we get EAGAIN. */
        nwritten = write(fd,ptr,size);
        if (nwritten == -1) {
            if (errno != EAGAIN) return -1;
        } else {
            ptr += nwritten;
            size -= nwritten;
        }
        if (size == 0) return ret;

        /* Wait */
        aeWait(fd,AE_WRITABLE,wait);
        elapsed = mstime() - start;
        if (elapsed >= timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
        remaining = timeout - elapsed;
    }
}

/* Read the specified amount of bytes from 'fd'. If all the bytes are read
 * within 'timeout' milliseconds the operation succeed and 'size' is returned.
 * Otherwise the operation fails, -1 is returned, and an unspecified amount of
 * data could be read from the file descriptor. */
ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout) {
    ssize_t nread, totread = 0;
    long long start = mstime();
    long long remaining = timeout;

    if (size == 0) return 0;
    while(1) {
        long long wait = (remaining > SYNCIO__RESOLUTION) ?
                          remaining : SYNCIO__RESOLUTION;
        long long elapsed;

        /* Optimistically try to read before checking if the file descriptor
         * is actually readable. At worst we get EAGAIN. */
        nread = read(fd,ptr,size);
        if (nread == 0) return -1; /* short read. */
        if (nread == -1) {
            if (errno != EAGAIN) return -1;
        } else {
            ptr += nread;
            size -= nread;
            totread += nread;
        }
        if (size == 0) return totread;

        /* Wait */
        aeWait(fd,AE_READABLE,wait);
        elapsed = mstime() - start;
        if (elapsed >= timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
        remaining = timeout - elapsed;
    }
}

/* Read a line making sure that every char will not require more than 'timeout'
 * milliseconds to be read.
 *
 * On success the number of bytes read is returned, otherwise -1.
 * On success the string is always correctly terminated with a 0 byte. */
ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout) {
    ssize_t nread = 0;

    size--;
    while(size) {
        char c;

        if (syncRead(fd,&c,1,timeout) == -1) return -1;
        if (c == '\n') {
            *ptr = '\0';
            if (nread && *(ptr-1) == '\r') *(ptr-1) = '\0';
            return nread;
        } else {
            *ptr++ = c;
            *ptr = '\0';
            nread++;
        }
        size--;
    }
    return nread;
}
