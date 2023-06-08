/* SDSLib 2.0 -- A C dynamic strings library
 *
 * Copyright (c) 2006-2015, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2015, Oran Agra
 * Copyright (c) 2015, Redis Labs, Inc
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

#include "fmacros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include "sds.h"
#include "sdsalloc.h"

static inline int hi_sdsHdrSize(char type) {
    switch(type&HI_SDS_TYPE_MASK) {
        case HI_SDS_TYPE_5:
            return sizeof(struct hisdshdr5);
        case HI_SDS_TYPE_8:
            return sizeof(struct hisdshdr8);
        case HI_SDS_TYPE_16:
            return sizeof(struct hisdshdr16);
        case HI_SDS_TYPE_32:
            return sizeof(struct hisdshdr32);
        case HI_SDS_TYPE_64:
            return sizeof(struct hisdshdr64);
    }
    return 0;
}

static inline char hi_sdsReqType(size_t string_size) {
    if (string_size < 32)
        return HI_SDS_TYPE_5;
    if (string_size < 0xff)
        return HI_SDS_TYPE_8;
    if (string_size < 0xffff)
        return HI_SDS_TYPE_16;
    if (string_size < 0xffffffff)
        return HI_SDS_TYPE_32;
    return HI_SDS_TYPE_64;
}

/* Create a new hisds string with the content specified by the 'init' pointer
 * and 'initlen'.
 * If NULL is used for 'init' the string is initialized with zero bytes.
 *
 * The string is always null-terminated (all the hisds strings are, always) so
 * even if you create an hisds string with:
 *
 * mystring = hi_sdsnewlen("abc",3);
 *
 * You can print the string with printf() as there is an implicit \0 at the
 * end of the string. However the string is binary safe and can contain
 * \0 characters in the middle, as the length is stored in the hisds header. */
hisds hi_sdsnewlen(const void *init, size_t initlen) {
    void *sh;
    hisds s;
    char type = hi_sdsReqType(initlen);
    /* Empty strings are usually created in order to append. Use type 8
     * since type 5 is not good at this. */
    if (type == HI_SDS_TYPE_5 && initlen == 0) type = HI_SDS_TYPE_8;
    int hdrlen = hi_sdsHdrSize(type);
    unsigned char *fp; /* flags pointer. */

    if (hdrlen+initlen+1 <= initlen) return NULL; /* Catch size_t overflow */
    sh = hi_s_malloc(hdrlen+initlen+1);
    if (sh == NULL) return NULL;
    if (!init)
        memset(sh, 0, hdrlen+initlen+1);
    s = (char*)sh+hdrlen;
    fp = ((unsigned char*)s)-1;
    switch(type) {
        case HI_SDS_TYPE_5: {
            *fp = type | (initlen << HI_SDS_TYPE_BITS);
            break;
        }
        case HI_SDS_TYPE_8: {
            HI_SDS_HDR_VAR(8,s);
            sh->len = initlen;
            sh->alloc = initlen;
            *fp = type;
            break;
        }
        case HI_SDS_TYPE_16: {
            HI_SDS_HDR_VAR(16,s);
            sh->len = initlen;
            sh->alloc = initlen;
            *fp = type;
            break;
        }
        case HI_SDS_TYPE_32: {
            HI_SDS_HDR_VAR(32,s);
            sh->len = initlen;
            sh->alloc = initlen;
            *fp = type;
            break;
        }
        case HI_SDS_TYPE_64: {
            HI_SDS_HDR_VAR(64,s);
            sh->len = initlen;
            sh->alloc = initlen;
            *fp = type;
            break;
        }
    }
    if (initlen && init)
        memcpy(s, init, initlen);
    s[initlen] = '\0';
    return s;
}

/* Create an empty (zero length) hisds string. Even in this case the string
 * always has an implicit null term. */
hisds hi_sdsempty(void) {
    return hi_sdsnewlen("",0);
}

/* Create a new hisds string starting from a null terminated C string. */
hisds hi_sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return hi_sdsnewlen(init, initlen);
}

/* Duplicate an hisds string. */
hisds hi_sdsdup(const hisds s) {
    return hi_sdsnewlen(s, hi_sdslen(s));
}

/* Free an hisds string. No operation is performed if 's' is NULL. */
void hi_sdsfree(hisds s) {
    if (s == NULL) return;
    hi_s_free((char*)s-hi_sdsHdrSize(s[-1]));
}

/* Set the hisds string length to the length as obtained with strlen(), so
 * considering as content only up to the first null term character.
 *
 * This function is useful when the hisds string is hacked manually in some
 * way, like in the following example:
 *
 * s = hi_sdsnew("foobar");
 * s[2] = '\0';
 * hi_sdsupdatelen(s);
 * printf("%d\n", hi_sdslen(s));
 *
 * The output will be "2", but if we comment out the call to hi_sdsupdatelen()
 * the output will be "6" as the string was modified but the logical length
 * remains 6 bytes. */
void hi_sdsupdatelen(hisds s) {
    size_t reallen = strlen(s);
    hi_sdssetlen(s, reallen);
}

/* Modify an hisds string in-place to make it empty (zero length).
 * However all the existing buffer is not discarded but set as free space
 * so that next append operations will not require allocations up to the
 * number of bytes previously available. */
void hi_sdsclear(hisds s) {
    hi_sdssetlen(s, 0);
    s[0] = '\0';
}

/* Enlarge the free space at the end of the hisds string so that the caller
 * is sure that after calling this function can overwrite up to addlen
 * bytes after the end of the string, plus one more byte for nul term.
 *
 * Note: this does not change the *length* of the hisds string as returned
 * by hi_sdslen(), but only the free buffer space we have. */
hisds hi_sdsMakeRoomFor(hisds s, size_t addlen) {
    void *sh, *newsh;
    size_t avail = hi_sdsavail(s);
    size_t len, newlen, reqlen;
    char type, oldtype = s[-1] & HI_SDS_TYPE_MASK;
    int hdrlen;

    /* Return ASAP if there is enough space left. */
    if (avail >= addlen) return s;

    len = hi_sdslen(s);
    sh = (char*)s-hi_sdsHdrSize(oldtype);
    reqlen = newlen = (len+addlen);
    if (newlen <= len) return NULL; /* Catch size_t overflow */
    if (newlen < HI_SDS_MAX_PREALLOC)
        newlen *= 2;
    else
        newlen += HI_SDS_MAX_PREALLOC;

    type = hi_sdsReqType(newlen);

    /* Don't use type 5: the user is appending to the string and type 5 is
     * not able to remember empty space, so hi_sdsMakeRoomFor() must be called
     * at every appending operation. */
    if (type == HI_SDS_TYPE_5) type = HI_SDS_TYPE_8;

    hdrlen = hi_sdsHdrSize(type);
    if (hdrlen+newlen+1 <= reqlen) return NULL; /* Catch size_t overflow */
    if (oldtype==type) {
        newsh = hi_s_realloc(sh, hdrlen+newlen+1);
        if (newsh == NULL) return NULL;
        s = (char*)newsh+hdrlen;
    } else {
        /* Since the header size changes, need to move the string forward,
         * and can't use realloc */
        newsh = hi_s_malloc(hdrlen+newlen+1);
        if (newsh == NULL) return NULL;
        memcpy((char*)newsh+hdrlen, s, len+1);
        hi_s_free(sh);
        s = (char*)newsh+hdrlen;
        s[-1] = type;
        hi_sdssetlen(s, len);
    }
    hi_sdssetalloc(s, newlen);
    return s;
}

/* Reallocate the hisds string so that it has no free space at the end. The
 * contained string remains not altered, but next concatenation operations
 * will require a reallocation.
 *
 * After the call, the passed hisds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
hisds hi_sdsRemoveFreeSpace(hisds s) {
    void *sh, *newsh;
    char type, oldtype = s[-1] & HI_SDS_TYPE_MASK;
    int hdrlen;
    size_t len = hi_sdslen(s);
    sh = (char*)s-hi_sdsHdrSize(oldtype);

    type = hi_sdsReqType(len);
    hdrlen = hi_sdsHdrSize(type);
    if (oldtype==type) {
        newsh = hi_s_realloc(sh, hdrlen+len+1);
        if (newsh == NULL) return NULL;
        s = (char*)newsh+hdrlen;
    } else {
        newsh = hi_s_malloc(hdrlen+len+1);
        if (newsh == NULL) return NULL;
        memcpy((char*)newsh+hdrlen, s, len+1);
        hi_s_free(sh);
        s = (char*)newsh+hdrlen;
        s[-1] = type;
        hi_sdssetlen(s, len);
    }
    hi_sdssetalloc(s, len);
    return s;
}

/* Return the total size of the allocation of the specifed hisds string,
 * including:
 * 1) The hisds header before the pointer.
 * 2) The string.
 * 3) The free buffer at the end if any.
 * 4) The implicit null term.
 */
size_t hi_sdsAllocSize(hisds s) {
    size_t alloc = hi_sdsalloc(s);
    return hi_sdsHdrSize(s[-1])+alloc+1;
}

/* Return the pointer of the actual SDS allocation (normally SDS strings
 * are referenced by the start of the string buffer). */
void *hi_sdsAllocPtr(hisds s) {
    return (void*) (s-hi_sdsHdrSize(s[-1]));
}

/* Increment the hisds length and decrements the left free space at the
 * end of the string according to 'incr'. Also set the null term
 * in the new end of the string.
 *
 * This function is used in order to fix the string length after the
 * user calls hi_sdsMakeRoomFor(), writes something after the end of
 * the current string, and finally needs to set the new length.
 *
 * Note: it is possible to use a negative increment in order to
 * right-trim the string.
 *
 * Usage example:
 *
 * Using hi_sdsIncrLen() and hi_sdsMakeRoomFor() it is possible to mount the
 * following schema, to cat bytes coming from the kernel to the end of an
 * hisds string without copying into an intermediate buffer:
 *
 * oldlen = hi_hi_sdslen(s);
 * s = hi_sdsMakeRoomFor(s, BUFFER_SIZE);
 * nread = read(fd, s+oldlen, BUFFER_SIZE);
 * ... check for nread <= 0 and handle it ...
 * hi_sdsIncrLen(s, nread);
 */
void hi_sdsIncrLen(hisds s, int incr) {
    unsigned char flags = s[-1];
    size_t len;
    switch(flags&HI_SDS_TYPE_MASK) {
        case HI_SDS_TYPE_5: {
            unsigned char *fp = ((unsigned char*)s)-1;
            unsigned char oldlen = HI_SDS_TYPE_5_LEN(flags);
            assert((incr > 0 && oldlen+incr < 32) || (incr < 0 && oldlen >= (unsigned int)(-incr)));
            *fp = HI_SDS_TYPE_5 | ((oldlen+incr) << HI_SDS_TYPE_BITS);
            len = oldlen+incr;
            break;
        }
        case HI_SDS_TYPE_8: {
            HI_SDS_HDR_VAR(8,s);
            assert((incr >= 0 && sh->alloc-sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case HI_SDS_TYPE_16: {
            HI_SDS_HDR_VAR(16,s);
            assert((incr >= 0 && sh->alloc-sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case HI_SDS_TYPE_32: {
            HI_SDS_HDR_VAR(32,s);
            assert((incr >= 0 && sh->alloc-sh->len >= (unsigned int)incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case HI_SDS_TYPE_64: {
            HI_SDS_HDR_VAR(64,s);
            assert((incr >= 0 && sh->alloc-sh->len >= (uint64_t)incr) || (incr < 0 && sh->len >= (uint64_t)(-incr)));
            len = (sh->len += incr);
            break;
        }
        default: len = 0; /* Just to avoid compilation warnings. */
    }
    s[len] = '\0';
}

/* Grow the hisds to have the specified length. Bytes that were not part of
 * the original length of the hisds will be set to zero.
 *
 * if the specified length is smaller than the current length, no operation
 * is performed. */
hisds hi_sdsgrowzero(hisds s, size_t len) {
    size_t curlen = hi_sdslen(s);

    if (len <= curlen) return s;
    s = hi_sdsMakeRoomFor(s,len-curlen);
    if (s == NULL) return NULL;

    /* Make sure added region doesn't contain garbage */
    memset(s+curlen,0,(len-curlen+1)); /* also set trailing \0 byte */
    hi_sdssetlen(s, len);
    return s;
}

/* Append the specified binary-safe string pointed by 't' of 'len' bytes to the
 * end of the specified hisds string 's'.
 *
 * After the call, the passed hisds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
hisds hi_sdscatlen(hisds s, const void *t, size_t len) {
    size_t curlen = hi_sdslen(s);

    s = hi_sdsMakeRoomFor(s,len);
    if (s == NULL) return NULL;
    memcpy(s+curlen, t, len);
    hi_sdssetlen(s, curlen+len);
    s[curlen+len] = '\0';
    return s;
}

/* Append the specified null termianted C string to the hisds string 's'.
 *
 * After the call, the passed hisds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
hisds hi_sdscat(hisds s, const char *t) {
    return hi_sdscatlen(s, t, strlen(t));
}

/* Append the specified hisds 't' to the existing hisds 's'.
 *
 * After the call, the modified hisds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
hisds hi_sdscatsds(hisds s, const hisds t) {
    return hi_sdscatlen(s, t, hi_sdslen(t));
}

/* Destructively modify the hisds string 's' to hold the specified binary
 * safe string pointed by 't' of length 'len' bytes. */
hisds hi_sdscpylen(hisds s, const char *t, size_t len) {
    if (hi_sdsalloc(s) < len) {
        s = hi_sdsMakeRoomFor(s,len-hi_sdslen(s));
        if (s == NULL) return NULL;
    }
    memcpy(s, t, len);
    s[len] = '\0';
    hi_sdssetlen(s, len);
    return s;
}

/* Like hi_sdscpylen() but 't' must be a null-terminated string so that the length
 * of the string is obtained with strlen(). */
hisds hi_sdscpy(hisds s, const char *t) {
    return hi_sdscpylen(s, t, strlen(t));
}

/* Helper for hi_sdscatlonglong() doing the actual number -> string
 * conversion. 's' must point to a string with room for at least
 * HI_SDS_LLSTR_SIZE bytes.
 *
 * The function returns the length of the null-terminated string
 * representation stored at 's'. */
#define HI_SDS_LLSTR_SIZE 21
int hi_sdsll2str(char *s, long long value) {
    char *p, aux;
    unsigned long long v;
    size_t l;

    /* Generate the string representation, this method produces
     * an reversed string. */
    v = (value < 0) ? -value : value;
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);
    if (value < 0) *p++ = '-';

    /* Compute length and add null term. */
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Identical hi_sdsll2str(), but for unsigned long long type. */
int hi_sdsull2str(char *s, unsigned long long v) {
    char *p, aux;
    size_t l;

    /* Generate the string representation, this method produces
     * an reversed string. */
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);

    /* Compute length and add null term. */
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Create an hisds string from a long long value. It is much faster than:
 *
 * hi_sdscatprintf(hi_sdsempty(),"%lld\n", value);
 */
hisds hi_sdsfromlonglong(long long value) {
    char buf[HI_SDS_LLSTR_SIZE];
    int len = hi_sdsll2str(buf,value);

    return hi_sdsnewlen(buf,len);
}

/* Like hi_sdscatprintf() but gets va_list instead of being variadic. */
hisds hi_sdscatvprintf(hisds s, const char *fmt, va_list ap) {
    va_list cpy;
    char staticbuf[1024], *buf = staticbuf, *t;
    size_t buflen = strlen(fmt)*2;

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    if (buflen > sizeof(staticbuf)) {
        buf = hi_s_malloc(buflen);
        if (buf == NULL) return NULL;
    } else {
        buflen = sizeof(staticbuf);
    }

    /* Try with buffers two times bigger every time we fail to
     * fit the string in the current buffer size. */
    while(1) {
        buf[buflen-2] = '\0';
        va_copy(cpy,ap);
        vsnprintf(buf, buflen, fmt, cpy);
        va_end(cpy);
        if (buf[buflen-2] != '\0') {
            if (buf != staticbuf) hi_s_free(buf);
            buflen *= 2;
            buf = hi_s_malloc(buflen);
            if (buf == NULL) return NULL;
            continue;
        }
        break;
    }

    /* Finally concat the obtained string to the SDS string and return it. */
    t = hi_sdscat(s, buf);
    if (buf != staticbuf) hi_s_free(buf);
    return t;
}

/* Append to the hisds string 's' a string obtained using printf-alike format
 * specifier.
 *
 * After the call, the modified hisds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = hi_sdsnew("Sum is: ");
 * s = hi_sdscatprintf(s,"%d+%d = %d",a,b,a+b).
 *
 * Often you need to create a string from scratch with the printf-alike
 * format. When this is the need, just use hi_sdsempty() as the target string:
 *
 * s = hi_sdscatprintf(hi_sdsempty(), "... your format ...", args);
 */
hisds hi_sdscatprintf(hisds s, const char *fmt, ...) {
    va_list ap;
    char *t;
    va_start(ap, fmt);
    t = hi_sdscatvprintf(s,fmt,ap);
    va_end(ap);
    return t;
}

/* This function is similar to hi_sdscatprintf, but much faster as it does
 * not rely on sprintf() family functions implemented by the libc that
 * are often very slow. Moreover directly handling the hisds string as
 * new data is concatenated provides a performance improvement.
 *
 * However this function only handles an incompatible subset of printf-alike
 * format specifiers:
 *
 * %s - C String
 * %S - SDS string
 * %i - signed int
 * %I - 64 bit signed integer (long long, int64_t)
 * %u - unsigned int
 * %U - 64 bit unsigned integer (unsigned long long, uint64_t)
 * %% - Verbatim "%" character.
 */
hisds hi_sdscatfmt(hisds s, char const *fmt, ...) {
    const char *f = fmt;
    long i;
    va_list ap;

    va_start(ap,fmt);
    i = hi_sdslen(s); /* Position of the next byte to write to dest str. */
    while(*f) {
        char next, *str;
        size_t l;
        long long num;
        unsigned long long unum;

        /* Make sure there is always space for at least 1 char. */
        if (hi_sdsavail(s)==0) {
            s = hi_sdsMakeRoomFor(s,1);
            if (s == NULL) goto fmt_error;
        }

        switch(*f) {
        case '%':
            next = *(f+1);
            f++;
            switch(next) {
            case 's':
            case 'S':
                str = va_arg(ap,char*);
                l = (next == 's') ? strlen(str) : hi_sdslen(str);
                if (hi_sdsavail(s) < l) {
                    s = hi_sdsMakeRoomFor(s,l);
                    if (s == NULL) goto fmt_error;
                }
                memcpy(s+i,str,l);
                hi_sdsinclen(s,l);
                i += l;
                break;
            case 'i':
            case 'I':
                if (next == 'i')
                    num = va_arg(ap,int);
                else
                    num = va_arg(ap,long long);
                {
                    char buf[HI_SDS_LLSTR_SIZE];
                    l = hi_sdsll2str(buf,num);
                    if (hi_sdsavail(s) < l) {
                        s = hi_sdsMakeRoomFor(s,l);
                        if (s == NULL) goto fmt_error;
                    }
                    memcpy(s+i,buf,l);
                    hi_sdsinclen(s,l);
                    i += l;
                }
                break;
            case 'u':
            case 'U':
                if (next == 'u')
                    unum = va_arg(ap,unsigned int);
                else
                    unum = va_arg(ap,unsigned long long);
                {
                    char buf[HI_SDS_LLSTR_SIZE];
                    l = hi_sdsull2str(buf,unum);
                    if (hi_sdsavail(s) < l) {
                        s = hi_sdsMakeRoomFor(s,l);
                        if (s == NULL) goto fmt_error;
                    }
                    memcpy(s+i,buf,l);
                    hi_sdsinclen(s,l);
                    i += l;
                }
                break;
            default: /* Handle %% and generally %<unknown>. */
                s[i++] = next;
                hi_sdsinclen(s,1);
                break;
            }
            break;
        default:
            s[i++] = *f;
            hi_sdsinclen(s,1);
            break;
        }
        f++;
    }
    va_end(ap);

    /* Add null-term */
    s[i] = '\0';
    return s;

fmt_error:
    va_end(ap);
    return NULL;
}

/* Remove the part of the string from left and from right composed just of
 * contiguous characters found in 'cset', that is a null terminted C string.
 *
 * After the call, the modified hisds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = hi_sdsnew("AA...AA.a.aa.aHelloWorld     :::");
 * s = hi_sdstrim(s,"Aa. :");
 * printf("%s\n", s);
 *
 * Output will be just "Hello World".
 */
hisds hi_sdstrim(hisds s, const char *cset) {
    char *start, *end, *sp, *ep;
    size_t len;

    sp = start = s;
    ep = end = s+hi_sdslen(s)-1;
    while(sp <= end && strchr(cset, *sp)) sp++;
    while(ep > sp && strchr(cset, *ep)) ep--;
    len = (sp > ep) ? 0 : ((ep-sp)+1);
    if (s != sp) memmove(s, sp, len);
    s[len] = '\0';
    hi_sdssetlen(s,len);
    return s;
}

/* Turn the string into a smaller (or equal) string containing only the
 * substring specified by the 'start' and 'end' indexes.
 *
 * start and end can be negative, where -1 means the last character of the
 * string, -2 the penultimate character, and so forth.
 *
 * The interval is inclusive, so the start and end characters will be part
 * of the resulting string.
 *
 * The string is modified in-place.
 *
 * Return value:
 * -1 (error) if hi_sdslen(s) is larger than maximum positive ssize_t value.
 *  0 on success.
 *
 * Example:
 *
 * s = hi_sdsnew("Hello World");
 * hi_sdsrange(s,1,-1); => "ello World"
 */
int hi_sdsrange(hisds s, ssize_t start, ssize_t end) {
    size_t newlen, len = hi_sdslen(s);
    if (len > SSIZE_MAX) return -1;

    if (len == 0) return 0;
    if (start < 0) {
        start = len+start;
        if (start < 0) start = 0;
    }
    if (end < 0) {
        end = len+end;
        if (end < 0) end = 0;
    }
    newlen = (start > end) ? 0 : (end-start)+1;
    if (newlen != 0) {
        if (start >= (ssize_t)len) {
            newlen = 0;
        } else if (end >= (ssize_t)len) {
            end = len-1;
            newlen = (start > end) ? 0 : (end-start)+1;
        }
    } else {
        start = 0;
    }
    if (start && newlen) memmove(s, s+start, newlen);
    s[newlen] = 0;
    hi_sdssetlen(s,newlen);
    return 0;
}

/* Apply tolower() to every character of the sds string 's'. */
void hi_sdstolower(hisds s) {
    size_t len = hi_sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = tolower(s[j]);
}

/* Apply toupper() to every character of the sds string 's'. */
void hi_sdstoupper(hisds s) {
    size_t len = hi_sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = toupper(s[j]);
}

/* Compare two hisds strings s1 and s2 with memcmp().
 *
 * Return value:
 *
 *     positive if s1 > s2.
 *     negative if s1 < s2.
 *     0 if s1 and s2 are exactly the same binary string.
 *
 * If two strings share exactly the same prefix, but one of the two has
 * additional characters, the longer string is considered to be greater than
 * the smaller one. */
int hi_sdscmp(const hisds s1, const hisds s2) {
    size_t l1, l2, minlen;
    int cmp;

    l1 = hi_sdslen(s1);
    l2 = hi_sdslen(s2);
    minlen = (l1 < l2) ? l1 : l2;
    cmp = memcmp(s1,s2,minlen);
    if (cmp == 0) return l1-l2;
    return cmp;
}

/* Split 's' with separator in 'sep'. An array
 * of hisds strings is returned. *count will be set
 * by reference to the number of tokens returned.
 *
 * On out of memory, zero length string, zero length
 * separator, NULL is returned.
 *
 * Note that 'sep' is able to split a string using
 * a multi-character separator. For example
 * hi_sdssplit("foo_-_bar","_-_"); will return two
 * elements "foo" and "bar".
 *
 * This version of the function is binary-safe but
 * requires length arguments. hi_sdssplit() is just the
 * same function but for zero-terminated strings.
 */
hisds *hi_sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count) {
    int elements = 0, slots = 5, start = 0, j;
    hisds *tokens;

    if (seplen < 1 || len < 0) return NULL;

    tokens = hi_s_malloc(sizeof(hisds)*slots);
    if (tokens == NULL) return NULL;

    if (len == 0) {
        *count = 0;
        return tokens;
    }
    for (j = 0; j < (len-(seplen-1)); j++) {
        /* make sure there is room for the next element and the final one */
        if (slots < elements+2) {
            hisds *newtokens;

            slots *= 2;
            newtokens = hi_s_realloc(tokens,sizeof(hisds)*slots);
            if (newtokens == NULL) goto cleanup;
            tokens = newtokens;
        }
        /* search the separator */
        if ((seplen == 1 && *(s+j) == sep[0]) || (memcmp(s+j,sep,seplen) == 0)) {
            tokens[elements] = hi_sdsnewlen(s+start,j-start);
            if (tokens[elements] == NULL) goto cleanup;
            elements++;
            start = j+seplen;
            j = j+seplen-1; /* skip the separator */
        }
    }
    /* Add the final element. We are sure there is room in the tokens array. */
    tokens[elements] = hi_sdsnewlen(s+start,len-start);
    if (tokens[elements] == NULL) goto cleanup;
    elements++;
    *count = elements;
    return tokens;

cleanup:
    {
        int i;
        for (i = 0; i < elements; i++) hi_sdsfree(tokens[i]);
        hi_s_free(tokens);
        *count = 0;
        return NULL;
    }
}

/* Free the result returned by hi_sdssplitlen(), or do nothing if 'tokens' is NULL. */
void hi_sdsfreesplitres(hisds *tokens, int count) {
    if (!tokens) return;
    while(count--)
        hi_sdsfree(tokens[count]);
    hi_s_free(tokens);
}

/* Append to the hisds string "s" an escaped string representation where
 * all the non-printable characters (tested with isprint()) are turned into
 * escapes in the form "\n\r\a...." or "\x<hex-number>".
 *
 * After the call, the modified hisds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
hisds hi_sdscatrepr(hisds s, const char *p, size_t len) {
    s = hi_sdscatlen(s,"\"",1);
    while(len--) {
        switch(*p) {
        case '\\':
        case '"':
            s = hi_sdscatprintf(s,"\\%c",*p);
            break;
        case '\n': s = hi_sdscatlen(s,"\\n",2); break;
        case '\r': s = hi_sdscatlen(s,"\\r",2); break;
        case '\t': s = hi_sdscatlen(s,"\\t",2); break;
        case '\a': s = hi_sdscatlen(s,"\\a",2); break;
        case '\b': s = hi_sdscatlen(s,"\\b",2); break;
        default:
            if (isprint(*p))
                s = hi_sdscatprintf(s,"%c",*p);
            else
                s = hi_sdscatprintf(s,"\\x%02x",(unsigned char)*p);
            break;
        }
        p++;
    }
    return hi_sdscatlen(s,"\"",1);
}

/* Helper function for hi_sdssplitargs() that converts a hex digit into an
 * integer from 0 to 15 */
static int hi_hex_digit_to_int(char c) {
    switch(c) {
    case '0': return 0;
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'a': case 'A': return 10;
    case 'b': case 'B': return 11;
    case 'c': case 'C': return 12;
    case 'd': case 'D': return 13;
    case 'e': case 'E': return 14;
    case 'f': case 'F': return 15;
    default: return 0;
    }
}

/* Split a line into arguments, where every argument can be in the
 * following programming-language REPL-alike form:
 *
 * foo bar "newline are supported\n" and "\xff\x00otherstuff"
 *
 * The number of arguments is stored into *argc, and an array
 * of hisds is returned.
 *
 * The caller should free the resulting array of hisds strings with
 * hi_sdsfreesplitres().
 *
 * Note that hi_sdscatrepr() is able to convert back a string into
 * a quoted string in the same format hi_sdssplitargs() is able to parse.
 *
 * The function returns the allocated tokens on success, even when the
 * input string is empty, or NULL if the input contains unbalanced
 * quotes or closed quotes followed by non space characters
 * as in: "foo"bar or "foo'
 */
hisds *hi_sdssplitargs(const char *line, int *argc) {
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while(1) {
        /* skip blanks */
        while(*p && isspace(*p)) p++;
        if (*p) {
            /* get a token */
            int inq=0;  /* set to 1 if we are in "quotes" */
            int insq=0; /* set to 1 if we are in 'single quotes' */
            int done=0;

            if (current == NULL) current = hi_sdsempty();
            while(!done) {
                if (inq) {
                    if (*p == '\\' && *(p+1) == 'x' &&
                                             isxdigit(*(p+2)) &&
                                             isxdigit(*(p+3)))
                    {
                        unsigned char byte;

                        byte = (hi_hex_digit_to_int(*(p+2))*16)+
                                hi_hex_digit_to_int(*(p+3));
                        current = hi_sdscatlen(current,(char*)&byte,1);
                        p += 3;
                    } else if (*p == '\\' && *(p+1)) {
                        char c;

                        p++;
                        switch(*p) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'b': c = '\b'; break;
                        case 'a': c = '\a'; break;
                        default: c = *p; break;
                        }
                        current = hi_sdscatlen(current,&c,1);
                    } else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = hi_sdscatlen(current,p,1);
                    }
                } else if (insq) {
                    if (*p == '\\' && *(p+1) == '\'') {
                        p++;
                        current = hi_sdscatlen(current,"'",1);
                    } else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = hi_sdscatlen(current,p,1);
                    }
                } else {
                    switch(*p) {
                    case ' ':
                    case '\n':
                    case '\r':
                    case '\t':
                    case '\0':
                        done=1;
                        break;
                    case '"':
                        inq=1;
                        break;
                    case '\'':
                        insq=1;
                        break;
                    default:
                        current = hi_sdscatlen(current,p,1);
                        break;
                    }
                }
                if (*p) p++;
            }
            /* add the token to the vector */
            {
                char **new_vector = hi_s_realloc(vector,((*argc)+1)*sizeof(char*));
                if (new_vector == NULL) {
                    hi_s_free(vector);
                    return NULL;
                }

                vector = new_vector;
                vector[*argc] = current;
                (*argc)++;
                current = NULL;
            }
        } else {
            /* Even on empty input string return something not NULL. */
            if (vector == NULL) vector = hi_s_malloc(sizeof(void*));
            return vector;
        }
    }

err:
    while((*argc)--)
        hi_sdsfree(vector[*argc]);
    hi_s_free(vector);
    if (current) hi_sdsfree(current);
    *argc = 0;
    return NULL;
}

/* Modify the string substituting all the occurrences of the set of
 * characters specified in the 'from' string to the corresponding character
 * in the 'to' array.
 *
 * For instance: hi_sdsmapchars(mystring, "ho", "01", 2)
 * will have the effect of turning the string "hello" into "0ell1".
 *
 * The function returns the hisds string pointer, that is always the same
 * as the input pointer since no resize is needed. */
hisds hi_sdsmapchars(hisds s, const char *from, const char *to, size_t setlen) {
    size_t j, i, l = hi_sdslen(s);

    for (j = 0; j < l; j++) {
        for (i = 0; i < setlen; i++) {
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

/* Join an array of C strings using the specified separator (also a C string).
 * Returns the result as an hisds string. */
hisds hi_sdsjoin(char **argv, int argc, char *sep) {
    hisds join = hi_sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = hi_sdscat(join, argv[j]);
        if (j != argc-1) join = hi_sdscat(join,sep);
    }
    return join;
}

/* Like hi_sdsjoin, but joins an array of SDS strings. */
hisds hi_sdsjoinsds(hisds *argv, int argc, const char *sep, size_t seplen) {
    hisds join = hi_sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = hi_sdscatsds(join, argv[j]);
        if (j != argc-1) join = hi_sdscatlen(join,sep,seplen);
    }
    return join;
}

/* Wrappers to the allocators used by SDS. Note that SDS will actually
 * just use the macros defined into sdsalloc.h in order to avoid to pay
 * the overhead of function calls. Here we define these wrappers only for
 * the programs SDS is linked to, if they want to touch the SDS internals
 * even if they use a different allocator. */
void *hi_sds_malloc(size_t size) { return hi_s_malloc(size); }
void *hi_sds_realloc(void *ptr, size_t size) { return hi_s_realloc(ptr,size); }
void hi_sds_free(void *ptr) { hi_s_free(ptr); }

#if defined(HI_SDS_TEST_MAIN)
#include <stdio.h>
#include "testhelp.h"
#include "limits.h"

#define UNUSED(x) (void)(x)
int hi_sdsTest(void) {
    {
        hisds x = hi_sdsnew("foo"), y;

        test_cond("Create a string and obtain the length",
            hi_sdslen(x) == 3 && memcmp(x,"foo\0",4) == 0)

        hi_sdsfree(x);
        x = hi_sdsnewlen("foo",2);
        test_cond("Create a string with specified length",
            hi_sdslen(x) == 2 && memcmp(x,"fo\0",3) == 0)

        x = hi_sdscat(x,"bar");
        test_cond("Strings concatenation",
            hi_sdslen(x) == 5 && memcmp(x,"fobar\0",6) == 0);

        x = hi_sdscpy(x,"a");
        test_cond("hi_sdscpy() against an originally longer string",
            hi_sdslen(x) == 1 && memcmp(x,"a\0",2) == 0)

        x = hi_sdscpy(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        test_cond("hi_sdscpy() against an originally shorter string",
            hi_sdslen(x) == 33 &&
            memcmp(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0",33) == 0)

        hi_sdsfree(x);
        x = hi_sdscatprintf(hi_sdsempty(),"%d",123);
        test_cond("hi_sdscatprintf() seems working in the base case",
            hi_sdslen(x) == 3 && memcmp(x,"123\0",4) == 0)

        hi_sdsfree(x);
        x = hi_sdsnew("--");
        x = hi_sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN,LLONG_MAX);
        test_cond("hi_sdscatfmt() seems working in the base case",
            hi_sdslen(x) == 60 &&
            memcmp(x,"--Hello Hi! World -9223372036854775808,"
                     "9223372036854775807--",60) == 0)
        printf("[%s]\n",x);

        hi_sdsfree(x);
        x = hi_sdsnew("--");
        x = hi_sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
        test_cond("hi_sdscatfmt() seems working with unsigned numbers",
            hi_sdslen(x) == 35 &&
            memcmp(x,"--4294967295,18446744073709551615--",35) == 0)

        hi_sdsfree(x);
        x = hi_sdsnew(" x ");
        hi_sdstrim(x," x");
        test_cond("hi_sdstrim() works when all chars match",
            hi_sdslen(x) == 0)

        hi_sdsfree(x);
        x = hi_sdsnew(" x ");
        hi_sdstrim(x," ");
        test_cond("hi_sdstrim() works when a single char remains",
            hi_sdslen(x) == 1 && x[0] == 'x')

        hi_sdsfree(x);
        x = hi_sdsnew("xxciaoyyy");
        hi_sdstrim(x,"xy");
        test_cond("hi_sdstrim() correctly trims characters",
            hi_sdslen(x) == 4 && memcmp(x,"ciao\0",5) == 0)

        y = hi_sdsdup(x);
        hi_sdsrange(y,1,1);
        test_cond("hi_sdsrange(...,1,1)",
            hi_sdslen(y) == 1 && memcmp(y,"i\0",2) == 0)

        hi_sdsfree(y);
        y = hi_sdsdup(x);
        hi_sdsrange(y,1,-1);
        test_cond("hi_sdsrange(...,1,-1)",
            hi_sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        hi_sdsfree(y);
        y = hi_sdsdup(x);
        hi_sdsrange(y,-2,-1);
        test_cond("hi_sdsrange(...,-2,-1)",
            hi_sdslen(y) == 2 && memcmp(y,"ao\0",3) == 0)

        hi_sdsfree(y);
        y = hi_sdsdup(x);
        hi_sdsrange(y,2,1);
        test_cond("hi_sdsrange(...,2,1)",
            hi_sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        hi_sdsfree(y);
        y = hi_sdsdup(x);
        hi_sdsrange(y,1,100);
        test_cond("hi_sdsrange(...,1,100)",
            hi_sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        hi_sdsfree(y);
        y = hi_sdsdup(x);
        hi_sdsrange(y,100,100);
        test_cond("hi_sdsrange(...,100,100)",
            hi_sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        hi_sdsfree(y);
        hi_sdsfree(x);
        x = hi_sdsnew("foo");
        y = hi_sdsnew("foa");
        test_cond("hi_sdscmp(foo,foa)", hi_sdscmp(x,y) > 0)

        hi_sdsfree(y);
        hi_sdsfree(x);
        x = hi_sdsnew("bar");
        y = hi_sdsnew("bar");
        test_cond("hi_sdscmp(bar,bar)", hi_sdscmp(x,y) == 0)

        hi_sdsfree(y);
        hi_sdsfree(x);
        x = hi_sdsnew("aar");
        y = hi_sdsnew("bar");
        test_cond("hi_sdscmp(bar,bar)", hi_sdscmp(x,y) < 0)

        hi_sdsfree(y);
        hi_sdsfree(x);
        x = hi_sdsnewlen("\a\n\0foo\r",7);
        y = hi_sdscatrepr(hi_sdsempty(),x,hi_sdslen(x));
        test_cond("hi_sdscatrepr(...data...)",
            memcmp(y,"\"\\a\\n\\x00foo\\r\"",15) == 0)

        {
            unsigned int oldfree;
            char *p;
            int step = 10, j, i;

            hi_sdsfree(x);
            hi_sdsfree(y);
            x = hi_sdsnew("0");
            test_cond("hi_sdsnew() free/len buffers", hi_sdslen(x) == 1 && hi_sdsavail(x) == 0);

            /* Run the test a few times in order to hit the first two
             * SDS header types. */
            for (i = 0; i < 10; i++) {
                int oldlen = hi_sdslen(x);
                x = hi_sdsMakeRoomFor(x,step);
                int type = x[-1]&HI_SDS_TYPE_MASK;

                test_cond("sdsMakeRoomFor() len", hi_sdslen(x) == oldlen);
                if (type != HI_SDS_TYPE_5) {
                    test_cond("hi_sdsMakeRoomFor() free", hi_sdsavail(x) >= step);
                    oldfree = hi_sdsavail(x);
                }
                p = x+oldlen;
                for (j = 0; j < step; j++) {
                    p[j] = 'A'+j;
                }
                hi_sdsIncrLen(x,step);
            }
            test_cond("hi_sdsMakeRoomFor() content",
                memcmp("0ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJ",x,101) == 0);
            test_cond("sdsMakeRoomFor() final length",hi_sdslen(x)==101);

            hi_sdsfree(x);
        }
    }
    test_report();
    return 0;
}
#endif

#ifdef HI_SDS_TEST_MAIN
int main(void) {
    return hi_sdsTest();
}
#endif
