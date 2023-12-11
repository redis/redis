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

#ifndef HIREDIS_SDS_H
#define HIREDIS_SDS_H

#define HI_SDS_MAX_PREALLOC (1024*1024)
#ifdef _MSC_VER
typedef long long ssize_t;
#define SSIZE_MAX (LLONG_MAX >> 1)
#ifndef __clang__
#define __attribute__(x)
#endif
#endif

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>

typedef char *hisds;

/* Note: sdshdr5 is never used, we just access the flags byte directly.
 * However is here to document the layout of type 5 SDS strings. */
struct __attribute__ ((__packed__)) hisdshdr5 {
    unsigned char flags; /* 3 lsb of type, and 5 msb of string length */
    char buf[];
};
struct __attribute__ ((__packed__)) hisdshdr8 {
    uint8_t len; /* used */
    uint8_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) hisdshdr16 {
    uint16_t len; /* used */
    uint16_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) hisdshdr32 {
    uint32_t len; /* used */
    uint32_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) hisdshdr64 {
    uint64_t len; /* used */
    uint64_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

#define HI_SDS_TYPE_5  0
#define HI_SDS_TYPE_8  1
#define HI_SDS_TYPE_16 2
#define HI_SDS_TYPE_32 3
#define HI_SDS_TYPE_64 4
#define HI_SDS_TYPE_MASK 7
#define HI_SDS_TYPE_BITS 3
#define HI_SDS_HDR_VAR(T,s) struct hisdshdr##T *sh = (struct hisdshdr##T *)((s)-(sizeof(struct hisdshdr##T)));
#define HI_SDS_HDR(T,s) ((struct hisdshdr##T *)((s)-(sizeof(struct hisdshdr##T))))
#define HI_SDS_TYPE_5_LEN(f) ((f)>>HI_SDS_TYPE_BITS)

static inline size_t hi_sdslen(const hisds s) {
    unsigned char flags = s[-1];
    switch(flags & HI_SDS_TYPE_MASK) {
        case HI_SDS_TYPE_5:
            return HI_SDS_TYPE_5_LEN(flags);
        case HI_SDS_TYPE_8:
            return HI_SDS_HDR(8,s)->len;
        case HI_SDS_TYPE_16:
            return HI_SDS_HDR(16,s)->len;
        case HI_SDS_TYPE_32:
            return HI_SDS_HDR(32,s)->len;
        case HI_SDS_TYPE_64:
            return HI_SDS_HDR(64,s)->len;
    }
    return 0;
}

static inline size_t hi_sdsavail(const hisds s) {
    unsigned char flags = s[-1];
    switch(flags&HI_SDS_TYPE_MASK) {
        case HI_SDS_TYPE_5: {
            return 0;
        }
        case HI_SDS_TYPE_8: {
            HI_SDS_HDR_VAR(8,s);
            return sh->alloc - sh->len;
        }
        case HI_SDS_TYPE_16: {
            HI_SDS_HDR_VAR(16,s);
            return sh->alloc - sh->len;
        }
        case HI_SDS_TYPE_32: {
            HI_SDS_HDR_VAR(32,s);
            return sh->alloc - sh->len;
        }
        case HI_SDS_TYPE_64: {
            HI_SDS_HDR_VAR(64,s);
            return sh->alloc - sh->len;
        }
    }
    return 0;
}

static inline void hi_sdssetlen(hisds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&HI_SDS_TYPE_MASK) {
        case HI_SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                *fp = (unsigned char)(HI_SDS_TYPE_5 | (newlen << HI_SDS_TYPE_BITS));
            }
            break;
        case HI_SDS_TYPE_8:
            HI_SDS_HDR(8,s)->len = (uint8_t)newlen;
            break;
        case HI_SDS_TYPE_16:
            HI_SDS_HDR(16,s)->len = (uint16_t)newlen;
            break;
        case HI_SDS_TYPE_32:
            HI_SDS_HDR(32,s)->len = (uint32_t)newlen;
            break;
        case HI_SDS_TYPE_64:
            HI_SDS_HDR(64,s)->len = (uint64_t)newlen;
            break;
    }
}

static inline void hi_sdsinclen(hisds s, size_t inc) {
    unsigned char flags = s[-1];
    switch(flags&HI_SDS_TYPE_MASK) {
        case HI_SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                unsigned char newlen = HI_SDS_TYPE_5_LEN(flags)+(unsigned char)inc;
                *fp = HI_SDS_TYPE_5 | (newlen << HI_SDS_TYPE_BITS);
            }
            break;
        case HI_SDS_TYPE_8:
            HI_SDS_HDR(8,s)->len += (uint8_t)inc;
            break;
        case HI_SDS_TYPE_16:
            HI_SDS_HDR(16,s)->len += (uint16_t)inc;
            break;
        case HI_SDS_TYPE_32:
            HI_SDS_HDR(32,s)->len += (uint32_t)inc;
            break;
        case HI_SDS_TYPE_64:
            HI_SDS_HDR(64,s)->len += (uint64_t)inc;
            break;
    }
}

/* hi_sdsalloc() = hi_sdsavail() + hi_sdslen() */
static inline size_t hi_sdsalloc(const hisds s) {
    unsigned char flags = s[-1];
    switch(flags & HI_SDS_TYPE_MASK) {
        case HI_SDS_TYPE_5:
            return HI_SDS_TYPE_5_LEN(flags);
        case HI_SDS_TYPE_8:
            return HI_SDS_HDR(8,s)->alloc;
        case HI_SDS_TYPE_16:
            return HI_SDS_HDR(16,s)->alloc;
        case HI_SDS_TYPE_32:
            return HI_SDS_HDR(32,s)->alloc;
        case HI_SDS_TYPE_64:
            return HI_SDS_HDR(64,s)->alloc;
    }
    return 0;
}

static inline void hi_sdssetalloc(hisds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&HI_SDS_TYPE_MASK) {
        case HI_SDS_TYPE_5:
            /* Nothing to do, this type has no total allocation info. */
            break;
        case HI_SDS_TYPE_8:
            HI_SDS_HDR(8,s)->alloc = (uint8_t)newlen;
            break;
        case HI_SDS_TYPE_16:
            HI_SDS_HDR(16,s)->alloc = (uint16_t)newlen;
            break;
        case HI_SDS_TYPE_32:
            HI_SDS_HDR(32,s)->alloc = (uint32_t)newlen;
            break;
        case HI_SDS_TYPE_64:
            HI_SDS_HDR(64,s)->alloc = (uint64_t)newlen;
            break;
    }
}

hisds hi_sdsnewlen(const void *init, size_t initlen);
hisds hi_sdsnew(const char *init);
hisds hi_sdsempty(void);
hisds hi_sdsdup(const hisds s);
void  hi_sdsfree(hisds s);
hisds hi_sdsgrowzero(hisds s, size_t len);
hisds hi_sdscatlen(hisds s, const void *t, size_t len);
hisds hi_sdscat(hisds s, const char *t);
hisds hi_sdscatsds(hisds s, const hisds t);
hisds hi_sdscpylen(hisds s, const char *t, size_t len);
hisds hi_sdscpy(hisds s, const char *t);

hisds hi_sdscatvprintf(hisds s, const char *fmt, va_list ap);
#ifdef __GNUC__
hisds hi_sdscatprintf(hisds s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
hisds hi_sdscatprintf(hisds s, const char *fmt, ...);
#endif

hisds hi_sdscatfmt(hisds s, char const *fmt, ...);
hisds hi_sdstrim(hisds s, const char *cset);
int hi_sdsrange(hisds s, ssize_t start, ssize_t end);
void hi_sdsupdatelen(hisds s);
void hi_sdsclear(hisds s);
int hi_sdscmp(const hisds s1, const hisds s2);
hisds *hi_sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count);
void hi_sdsfreesplitres(hisds *tokens, int count);
void hi_sdstolower(hisds s);
void hi_sdstoupper(hisds s);
hisds hi_sdsfromlonglong(long long value);
hisds hi_sdscatrepr(hisds s, const char *p, size_t len);
hisds *hi_sdssplitargs(const char *line, int *argc);
hisds hi_sdsmapchars(hisds s, const char *from, const char *to, size_t setlen);
hisds hi_sdsjoin(char **argv, int argc, char *sep);
hisds hi_sdsjoinsds(hisds *argv, int argc, const char *sep, size_t seplen);

/* Low level functions exposed to the user API */
hisds hi_sdsMakeRoomFor(hisds s, size_t addlen);
void hi_sdsIncrLen(hisds s, int incr);
hisds hi_sdsRemoveFreeSpace(hisds s);
size_t hi_sdsAllocSize(hisds s);
void *hi_sdsAllocPtr(hisds s);

/* Export the allocator used by SDS to the program using SDS.
 * Sometimes the program SDS is linked to, may use a different set of
 * allocators, but may want to allocate or free things that SDS will
 * respectively free or allocate. */
void *hi_sds_malloc(size_t size);
void *hi_sds_realloc(void *ptr, size_t size);
void hi_sds_free(void *ptr);

#ifdef REDIS_TEST
int hi_sdsTest(int argc, char *argv[]);
#endif

#endif /* HIREDIS_SDS_H */
