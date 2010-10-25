/* Synchronous socket and file I/O operations useful across the core.
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"

/* ----------------- Blocking sockets I/O with timeouts --------------------- */

/* Redis performs most of the I/O in a nonblocking way, with the exception
 * of the SYNC command where the slave does it in a blocking way, and
 * the MIGRATE command that must be blocking in order to be atomic from the
 * point of view of the two instances (one migrating the key and one receiving
 * the key). This is why need the following blocking I/O functions. */

int syncWrite(int fd, char *ptr, ssize_t size, int timeout) {
    ssize_t nwritten, ret = size;
    time_t start = time(NULL);

    timeout++;
    while(size) {
        if (aeWait(fd,AE_WRITABLE,1000) & AE_WRITABLE) {
            nwritten = write(fd,ptr,size);
            if (nwritten == -1) return -1;
            ptr += nwritten;
            size -= nwritten;
        }
        if ((time(NULL)-start) > timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
    return ret;
}

int syncRead(int fd, char *ptr, ssize_t size, int timeout) {
    ssize_t nread, totread = 0;
    time_t start = time(NULL);

    timeout++;
    while(size) {
        if (aeWait(fd,AE_READABLE,1000) & AE_READABLE) {
            nread = read(fd,ptr,size);
            if (nread <= 0) return -1;
            ptr += nread;
            size -= nread;
            totread += nread;
        }
        if ((time(NULL)-start) > timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
    return totread;
}

int syncReadLine(int fd, char *ptr, ssize_t size, int timeout) {
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
    }
    return nread;
}

/* ----------------- Blocking sockets I/O with timeouts --------------------- */

/* Write binary-safe string into a file in the bulkformat
 * $<count>\r\n<payload>\r\n */
int fwriteBulkString(FILE *fp, char *s, unsigned long len) {
    char cbuf[128];
    int clen;
    cbuf[0] = '$';
    clen = 1+ll2string(cbuf+1,sizeof(cbuf)-1,len);
    cbuf[clen++] = '\r';
    cbuf[clen++] = '\n';
    if (fwrite(cbuf,clen,1,fp) == 0) return 0;
    if (len > 0 && fwrite(s,len,1,fp) == 0) return 0;
    if (fwrite("\r\n",2,1,fp) == 0) return 0;
    return 1;
}

/* Write a double value in bulk format $<count>\r\n<payload>\r\n */
int fwriteBulkDouble(FILE *fp, double d) {
    char buf[128], dbuf[128];

    snprintf(dbuf,sizeof(dbuf),"%.17g\r\n",d);
    snprintf(buf,sizeof(buf),"$%lu\r\n",(unsigned long)strlen(dbuf)-2);
    if (fwrite(buf,strlen(buf),1,fp) == 0) return 0;
    if (fwrite(dbuf,strlen(dbuf),1,fp) == 0) return 0;
    return 1;
}

/* Write a long value in bulk format $<count>\r\n<payload>\r\n */
int fwriteBulkLongLong(FILE *fp, long long l) {
    char bbuf[128], lbuf[128];
    unsigned int blen, llen;
    llen = ll2string(lbuf,32,l);
    blen = snprintf(bbuf,sizeof(bbuf),"$%u\r\n%s\r\n",llen,lbuf);
    if (fwrite(bbuf,blen,1,fp) == 0) return 0;
    return 1;
}

/* Delegate writing an object to writing a bulk string or bulk long long. */
int fwriteBulkObject(FILE *fp, robj *obj) {
    /* Avoid using getDecodedObject to help copy-on-write (we are often
     * in a child process when this function is called). */
    if (obj->encoding == REDIS_ENCODING_INT) {
        return fwriteBulkLongLong(fp,(long)obj->ptr);
    } else if (obj->encoding == REDIS_ENCODING_RAW) {
        return fwriteBulkString(fp,obj->ptr,sdslen(obj->ptr));
    } else {
        redisPanic("Unknown string encoding");
    }
}


