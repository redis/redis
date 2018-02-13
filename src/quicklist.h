/* quicklist.h - A generic doubly linked quicklist implementation
 *
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this quicklist of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this quicklist of conditions and the following disclaimer in the
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

#ifndef __QUICKLIST_H__
#define __QUICKLIST_H__

#include "alloc.h"

/* Node, quicklist, and Iterator are the only data structures used currently. */

/* quicklistNode is a 32 byte struct describing a ziplist for a quicklist.
 * We use bit fields keep the quicklistNode at 32 bytes.
 * count: 16 bits, max 65536 (max zl bytes is 65k, so max count actually < 32k).
 * encoding: 2 bits, RAW=1, LZF=2.
 * container: 2 bits, NONE=1, ZIPLIST=2.
 * recompress: 1 bit, bool, true if node is temporarry decompressed for usage.
 * attempted_compress: 1 bit, boolean, used for verifying during testing.
 * extra: 12 bits, free for future use; pads out the remainder of 32 bits */
typedef struct quicklistNode {
    struct quicklistNode *prev;
    struct quicklistNode *next;
    unsigned char *zl;
    unsigned int sz;             /* ziplist size in bytes */
    unsigned int count : 16;     /* count of items in ziplist */
    unsigned int encoding : 2;   /* RAW==1 or LZF==2 */
    unsigned int container : 2;  /* NONE==1 or ZIPLIST==2 */
    unsigned int recompress : 1; /* was this node previous compressed? */
    unsigned int attempted_compress : 1; /* node can't compress; too small */
    unsigned int extra : 10; /* more bits to steal for future usage */
} quicklistNode;

/* quicklistLZF is a 4+N byte struct holding 'sz' followed by 'compressed'.
 * 'sz' is byte length of 'compressed' field.
 * 'compressed' is LZF data with total (compressed) length 'sz'
 * NOTE: uncompressed length is stored in quicklistNode->sz.
 * When quicklistNode->zl is compressed, node->zl points to a quicklistLZF */
typedef struct quicklistLZF {
    unsigned int sz; /* LZF size in bytes*/
    char compressed[];
} quicklistLZF;

/* quicklist is a 40 byte struct (on 64-bit systems) describing a quicklist.
 * 'count' is the number of total entries.
 * 'len' is the number of quicklist nodes.
 * 'compress' is: -1 if compression disabled, otherwise it's the number
 *                of quicklistNodes to leave uncompressed at ends of quicklist.
 * 'fill' is the user-requested (or default) fill factor. */
typedef struct quicklist {
    quicklistNode *head;
    quicklistNode *tail;
    unsigned long count;        /* total count of all entries in all ziplists */
    unsigned long len;          /* number of quicklistNodes */
    int fill : 16;              /* fill factor for individual nodes */
    unsigned int compress : 16; /* depth of end nodes not to compress;0=off */
} quicklist;

typedef struct quicklistIter {
    const quicklist *quicklist;
    quicklistNode *current;
    unsigned char *zi;
    long offset; /* offset in current ziplist */
    int direction;
} quicklistIter;

typedef struct quicklistEntry {
    const quicklist *quicklist;
    quicklistNode *node;
    unsigned char *zi;
    unsigned char *value;
    long long longval;
    unsigned int sz;
    int offset;
} quicklistEntry;

#define QUICKLIST_HEAD 0
#define QUICKLIST_TAIL -1

/* quicklist node encodings */
#define QUICKLIST_NODE_ENCODING_RAW 1
#define QUICKLIST_NODE_ENCODING_LZF 2

/* quicklist compression disable */
#define QUICKLIST_NOCOMPRESS 0

/* quicklist container formats */
#define QUICKLIST_NODE_CONTAINER_NONE 1
#define QUICKLIST_NODE_CONTAINER_ZIPLIST 2

#define quicklistNodeIsCompressed(node)                                        \
    ((node)->encoding == QUICKLIST_NODE_ENCODING_LZF)

/* Prototypes */
quicklist *quicklistCreateA(alloc a);
static inline quicklist *quicklistCreate(void) { return quicklistCreateA(z_alloc); }
//static inline quicklist *quicklistCreateM(void) { return quicklistCreateA(m_alloc); }
quicklist *quicklistNew(int fill, int compress);
void quicklistSetCompressDepth(quicklist *quicklist, int depth);
void quicklistSetFill(quicklist *quicklist, int fill);
void quicklistSetOptions(quicklist *quicklist, int fill, int depth);
void quicklistReleaseA(quicklist *quicklist, alloc a);
//static inline void quicklistRelease(quicklist *quicklist) { quicklistReleaseA(quicklist, z_alloc); }
//static inline void quicklistReleaseM(quicklist *quicklist) { quicklistReleaseA(quicklist, m_alloc); }
int quicklistPushHeadA(quicklist *quicklist, void *value, const size_t sz, alloc a);
static inline int quicklistPushHead(quicklist *quicklist, void *value, const size_t sz) {
	return quicklistPushHeadA(quicklist, value, sz, z_alloc);
}
//static inline int quicklistPushHeadM(quicklist *quicklist, void *value, const size_t sz) {
//	return quicklistPushHeadA(quicklist, value, sz, m_alloc);
//}
int quicklistPushTailA(quicklist *quicklist, void *value, const size_t sz, alloc a);
static inline int quicklistPushTail(quicklist *quicklist, void *value, const size_t sz) {
	return quicklistPushTailA(quicklist, value, sz, z_alloc);
}
//static inline int quicklistPushTailM(quicklist *quicklist, void *value, const size_t sz) {
//	return quicklistPushTailA(quicklist, value, sz, m_alloc);
//}
void quicklistPushA(quicklist *quicklist, void *value, const size_t sz,
                   int where, alloc a);
void quicklistAppendZiplist(quicklist *quicklist, unsigned char *zl);
quicklist *quicklistAppendValuesFromZiplist(quicklist *quicklist,
                                            unsigned char *zl);
quicklist *quicklistCreateFromZiplist(int fill, int compress,
                                      unsigned char *zl);
void quicklistInsertAfterA(quicklist *quicklist, quicklistEntry *node,
                          void *value, const size_t sz, alloc a);
//static inline void quicklistInsertAfter(quicklist *quicklist, quicklistEntry *node,
//                          void *value, const size_t sz) {
//	quicklistInsertAfterA(quicklist, node, value, sz, z_alloc);
//}
//static inline void quicklistInsertAfterM(quicklist *quicklist, quicklistEntry *node,
//                          void *value, const size_t sz) {
//	quicklistInsertAfterA(quicklist, node, value, sz, m_alloc);
//}
void quicklistInsertBeforeA(quicklist *quicklist, quicklistEntry *node,
                           void *value, const size_t sz, alloc a);
//static inline void quicklistInsertBefore(quicklist *quicklist, quicklistEntry *node,
//                           void *value, const size_t sz) {
//	quicklistInsertBeforeA(quicklist, node, value, sz, z_alloc);
//}
//static inline void quicklistInsertBeforeM(quicklist *quicklist, quicklistEntry *node,
//                           void *value, const size_t sz) {
//	quicklistInsertBeforeA(quicklist, node, value, sz, m_alloc);
//}
void quicklistDelEntryA(quicklistIter *iter, quicklistEntry *entry, alloc a);
//static inline void quicklistDelEntry(quicklistIter *iter, quicklistEntry *entry) {
//	quicklistDelEntryA(iter, entry, z_alloc);
//}
//static inline void quicklistDelEntryM(quicklistIter *iter, quicklistEntry *entry) {
//	quicklistDelEntryA(iter, entry, m_alloc);
//}
int quicklistReplaceAtIndexA(quicklist *quicklist, long index, void *data,
                            int sz, alloc a);
//static inline int quicklistReplaceAtIndex(quicklist *quicklist, long index, void *data,
//                            int sz) {
//	return quicklistReplaceAtIndexA(quicklist, index, data, sz, z_alloc);
//}
static inline int quicklistReplaceAtIndexM(quicklist *quicklist, long index, void *data,
                            int sz) {
	return quicklistReplaceAtIndexA(quicklist, index, data, sz, m_alloc);
}
int quicklistDelRangeA(quicklist *quicklist, const long start, const long stop, alloc a);
//static inline int quicklistDelRange(quicklist *quicklist, const long start, const long stop) {
//	return quicklistDelRangeA(quicklist, start, stop, z_alloc);
//}
static inline int quicklistDelRangeM(quicklist *quicklist, const long start, const long stop) {
	return quicklistDelRangeA(quicklist, start, stop, m_alloc);
}
quicklistIter *quicklistGetIterator(const quicklist *quicklist, int direction);
quicklistIter *quicklistGetIteratorAtIdxA(const quicklist *quicklist,
                                         int direction, const long long idx, alloc a);
static inline quicklistIter *quicklistGetIteratorAtIdx(const quicklist *quicklist,
                                         int direction, const long long idx) {
	return quicklistGetIteratorAtIdxA(quicklist, direction, idx, z_alloc);
}
int quicklistNextA(quicklistIter *iter, quicklistEntry *node, alloc a);
static inline int quicklistNext(quicklistIter *iter, quicklistEntry *node) {
	return quicklistNextA(iter, node, z_alloc);
}
//static inline int quicklistNextM(quicklistIter *iter, quicklistEntry *node) {
//	return quicklistNextA(iter, node, m_alloc);
//}
void quicklistReleaseIterator(quicklistIter *iter);
quicklist *quicklistDup(quicklist *orig);
int quicklistIndexA(const quicklist *quicklist, const long long index,
                   quicklistEntry *entry, alloc a);
//static inline int quicklistIndex(const quicklist *quicklist, const long long index,
//                   quicklistEntry *entry) {
//	return quicklistIndexA(quicklist, index, entry, z_alloc);
//}
static inline int quicklistIndexM(const quicklist *quicklist, const long long index,
                   quicklistEntry *entry) {
	return quicklistIndexA(quicklist, index, entry, m_alloc);
}
void quicklistRewind(quicklist *quicklist, quicklistIter *li);
void quicklistRewindTail(quicklist *quicklist, quicklistIter *li);
void quicklistRotate(quicklist *quicklist);
int quicklistPopCustomA(quicklist *quicklist, int where, unsigned char **data,
                       unsigned int *sz, long long *sval,
                       void *(*saver)(unsigned char *data, unsigned int sz), alloc a);
static inline int quicklistPopCustom(quicklist *quicklist, int where, unsigned char **data,
                       unsigned int *sz, long long *sval,
                       void *(*saver)(unsigned char *data, unsigned int sz)) {
	return quicklistPopCustomA(quicklist, where, data, sz, sval, saver, z_alloc);
}
//static inline int quicklistPopCustomM(quicklist *quicklist, int where, unsigned char **data,
//                       unsigned int *sz, long long *sval,
//                       void *(*saver)(unsigned char *data, unsigned int sz)) {
//	return quicklistPopCustomA(quicklist, where, data, sz, sval, saver, m_alloc);
//}
int quicklistPop(quicklist *quicklist, int where, unsigned char **data,
                 unsigned int *sz, long long *slong);
unsigned long quicklistCount(const quicklist *ql);
int quicklistCompare(unsigned char *p1, unsigned char *p2, int p2_len);
size_t quicklistGetLzf(const quicklistNode *node, void **data);

#ifdef REDIS_TEST
int quicklistTest(int argc, char *argv[]);
#endif

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __QUICKLIST_H__ */
