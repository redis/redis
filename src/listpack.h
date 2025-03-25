/* Listpack -- A lists of strings serialization format
 *
 * This file implements the specification you can find at:
 *
 *  https://github.com/antirez/listpack
 *
 * Copyright (c) 2017-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of the Redis Source Available License 2.0
 * (RSALv2) or the Server Side Public License v1 (SSPLv1).
 */

#ifndef __LISTPACK_H
#define __LISTPACK_H

#include <stdlib.h>
#include <stdint.h>

#define LP_INTBUF_SIZE 21 /* 20 digits of -2^63 + 1 null term = 21. */

/* lpInsert() where argument possible values: */
#define LP_BEFORE 0
#define LP_AFTER 1
#define LP_REPLACE 2

/* Each entry in the listpack is either a string or an integer. */
typedef struct {
    /* When string is used, it is provided with the length (slen). */
    unsigned char *sval;
    uint32_t slen;
    /* When integer is used, 'sval' is NULL, and lval holds the value. */
    long long lval;
} listpackEntry;

unsigned char *lpNew(size_t capacity);
void lpFree(unsigned char *lp);
void lpFreeGeneric(void *lp);
unsigned char* lpShrinkToFit(unsigned char *lp);
unsigned char *lpInsertString(unsigned char *lp, unsigned char *s, uint32_t slen,
                              unsigned char *p, int where, unsigned char **newp);
unsigned char *lpInsertInteger(unsigned char *lp, long long lval,
                               unsigned char *p, int where, unsigned char **newp);
unsigned char *lpPrepend(unsigned char *lp, unsigned char *s, uint32_t slen);
unsigned char *lpPrependInteger(unsigned char *lp, long long lval);
unsigned char *lpAppend(unsigned char *lp, unsigned char *s, uint32_t slen);
unsigned char *lpAppendInteger(unsigned char *lp, long long lval);
unsigned char *lpReplace(unsigned char *lp, unsigned char **p, unsigned char *s, uint32_t slen);
unsigned char *lpReplaceInteger(unsigned char *lp, unsigned char **p, long long lval);
unsigned char *lpDelete(unsigned char *lp, unsigned char *p, unsigned char **newp);
unsigned char *lpDeleteRangeWithEntry(unsigned char *lp, unsigned char **p, unsigned long num);
unsigned char *lpDeleteRange(unsigned char *lp, long index, unsigned long num);
unsigned char *lpBatchAppend(unsigned char *lp, listpackEntry *entries, unsigned long len);
unsigned char *lpBatchInsert(unsigned char *lp, unsigned char *p, int where,
                             listpackEntry *entries, unsigned int len, unsigned char **newp);
unsigned char *lpBatchDelete(unsigned char *lp, unsigned char **ps, unsigned long count);
unsigned char *lpMerge(unsigned char **first, unsigned char **second);
unsigned char *lpDup(unsigned char *lp);
unsigned long lpLength(unsigned char *lp);
unsigned char *lpGet(unsigned char *p, int64_t *count, unsigned char *intbuf);
unsigned char *lpGetValue(unsigned char *p, unsigned int *slen, long long *lval);
int lpGetIntegerValue(unsigned char *p, long long *lval);
unsigned char *lpFind(unsigned char *lp, unsigned char *p, unsigned char *s, uint32_t slen, unsigned int skip);
typedef int (*lpCmp)(const unsigned char *lp, unsigned char *p, void *user, unsigned char *s, long long slen);
unsigned char *lpFindCb(unsigned char *lp, unsigned char *p, void *user, lpCmp cmp, unsigned int skip);
unsigned char *lpFirst(unsigned char *lp);
unsigned char *lpLast(unsigned char *lp);
unsigned char *lpNext(unsigned char *lp, unsigned char *p);
unsigned char *lpNextWithBytes(unsigned char *lp, unsigned char *p, const size_t lpbytes);
unsigned char *lpPrev(unsigned char *lp, unsigned char *p);
size_t lpBytes(unsigned char *lp);
size_t lpEntrySizeInteger(long long lval);
size_t lpEstimateBytesRepeatedInteger(long long lval, unsigned long rep);
unsigned char *lpSeek(unsigned char *lp, long index);
typedef int (*listpackValidateEntryCB)(unsigned char *p, unsigned int head_count, void *userdata);
int lpValidateIntegrity(unsigned char *lp, size_t size, int deep,
                        listpackValidateEntryCB entry_cb, void *cb_userdata);
unsigned char *lpValidateFirst(unsigned char *lp);
int lpValidateNext(unsigned char *lp, unsigned char **pp, size_t lpbytes);
unsigned int lpCompare(unsigned char *p, unsigned char *s, uint32_t slen);
void lpRandomPair(unsigned char *lp, unsigned long total_count,
                  listpackEntry *key, listpackEntry *val, int tuple_len);
void lpRandomPairs(unsigned char *lp, unsigned int count,
                   listpackEntry *keys, listpackEntry *vals, int tuple_len);
unsigned int lpRandomPairsUnique(unsigned char *lp, unsigned int count,
                                 listpackEntry *keys, listpackEntry *vals, int tuple_len);
void lpRandomEntries(unsigned char *lp, unsigned int count, listpackEntry *entries);
unsigned char *lpNextRandom(unsigned char *lp, unsigned char *p, unsigned int *index,
                            unsigned int remaining, int tuple_len);
int lpSafeToAdd(unsigned char* lp, size_t add);
void lpRepr(unsigned char *lp);

#ifdef REDIS_TEST
int listpackTest(int argc, char *argv[], int flags);
#endif

#endif
