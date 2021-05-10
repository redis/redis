/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef _PACKEDLIST_H
#define _PACKEDLIST_H

/* Each entry in the ziplist is either a string or an integer. */
typedef struct {
    /* When string is used, it is provided with the length (slen). */
    unsigned char *sval;
    unsigned int slen;
    /* When integer is used, 'sval' is NULL, and lval holds the value. */
    long long lval;
} ziplistEntry;

typedef struct packedClass {
    long (*listLen)(unsigned char *l);
    size_t (*listBlobLen)(unsigned char *l);
    unsigned int (*listGet)(unsigned char *p, unsigned char **vstr, unsigned int *vlen, long long *vll);
    unsigned char *(*listIndex)(unsigned char *l, long index);
    unsigned char *(*listNext)(unsigned char *l, unsigned char *p);
    unsigned char *(*listPrev)(unsigned char *l, unsigned char *p);
    unsigned char *(*listPushHead)(unsigned char *l, unsigned char *s, uint32_t slen);
    unsigned char *(*listPushTail)(unsigned char *l, unsigned char *s, uint32_t slen);
    unsigned char *(*listReplace)(unsigned char *l, unsigned char *p, unsigned char *s, uint32_t slen);
    unsigned char *(*listDelete)(unsigned char *l, unsigned char **p);
    unsigned char *(*listFind)(unsigned char *lp, unsigned char *p, unsigned char *s, unsigned int slen, unsigned int skip);
    void (*listRandomPair)(unsigned char *l, unsigned long total_count, ziplistEntry *key, ziplistEntry *val);
    void (*listRandomPairs)(unsigned char *zl, unsigned int count, ziplistEntry *keys, ziplistEntry *vals);
    unsigned int  (*listRandomPairsUnique)(unsigned char *zl, unsigned int count, ziplistEntry *keys, ziplistEntry *vals);
} packedClass;

#endif /* _PACKEDLIST_H */
