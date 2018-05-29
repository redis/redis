/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

#ifndef _ZIPLIST_H
#define _ZIPLIST_H

#include "alloc.h"

#define ZIPLIST_HEAD 0
#define ZIPLIST_TAIL 1

unsigned char *ziplistNewA(alloc a);
static inline unsigned char *ziplistNew(void) { return ziplistNewA(z_alloc); }
static inline unsigned char *ziplistNewM(void) { return ziplistNewA(m_alloc); }
unsigned char *ziplistMergeA(unsigned char **first, unsigned char **second, alloc a);
static inline unsigned char *ziplistMerge(unsigned char **first, unsigned char **second) {
	return ziplistMergeA(first, second, z_alloc);
}
static inline unsigned char *ziplistMergeM(unsigned char **first, unsigned char **second) {
	return ziplistMergeA(first, second, m_alloc);
}
unsigned char *ziplistPushA(unsigned char *zl, unsigned char *s, unsigned int slen, int where, alloc a);
static inline unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {
	return ziplistPushA(zl, s, slen, where, z_alloc);
}
static inline unsigned char *ziplistPushM(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {
	return ziplistPushA(zl, s, slen, where, m_alloc);
}
unsigned char *ziplistIndex(unsigned char *zl, int index);
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p);
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p);
unsigned int ziplistGet(unsigned char *p, unsigned char **sval, unsigned int *slen, long long *lval);
unsigned char *ziplistInsertA(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen, alloc a);
static inline unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
	return ziplistInsertA(zl, p, s, slen, z_alloc);
}
static inline unsigned char *ziplistInsertM(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
	return ziplistInsertA(zl, p, s, slen, m_alloc);
}
unsigned char *ziplistDeleteA(unsigned char *zl, unsigned char **p, alloc a);
static inline unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {
	return ziplistDeleteA(zl, p, z_alloc);
}
static inline unsigned char *ziplistDeleteM(unsigned char *zl, unsigned char **p) {
	return ziplistDeleteA(zl, p, m_alloc);
}
unsigned char *ziplistDeleteRangeA(unsigned char *zl, int index, unsigned int num, alloc a);
static inline unsigned char *ziplistDeleteRange(unsigned char *zl, int index, unsigned int num) {
	return ziplistDeleteRangeA(zl, index, num, z_alloc);
}
static inline unsigned char *ziplistDeleteRangeM(unsigned char *zl, int index, unsigned int num) {
	return ziplistDeleteRangeA(zl, index, num, m_alloc);
}
unsigned int ziplistCompare(unsigned char *p, unsigned char *s, unsigned int slen);
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip);
unsigned int ziplistLen(unsigned char *zl);
size_t ziplistBlobLen(unsigned char *zl);
void ziplistRepr(unsigned char *zl);

#ifdef REDIS_TEST
int ziplistTest(int argc, char *argv[]);
#endif

#endif /* _ZIPLIST_H */
