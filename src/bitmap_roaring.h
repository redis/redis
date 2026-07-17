#ifndef __BITMAP_ROARING_H
#define __BITMAP_ROARING_H

#include "sds.h"
#include "object.h"
#include "rio.h"

#include <stdint.h>
#include <sys/types.h>

/* Representability cap for Roaring bitmap logical length. Keep v1 Roaring
 * bitmaps inside the same 32-bit bit index space as bounded Roaring bitmaps:
 * 512 MiB of logical bytes, max bit offset UINT32_MAX. Command handlers still
 * enforce the client-visible proto-max-bulk-len limit for writes,
 * dense/materializing paths, and legacy string reads when it is lower;
 * read-only roaring lookups may exceed that lower runtime limit inside this
 * cap. */
#define BITMAP_OBJECT_MAX_BYTES (512ULL*1024*1024)

typedef enum bitmapBitop {
    BITOP_AND = 0,
    BITOP_OR,
    BITOP_XOR,
    BITOP_NOT,
    BITOP_DIFF,  /* DIFF(X, A1, A2, ..., An) = X & !(A1 | A2 | ... | An) */
    BITOP_DIFF1, /* DIFF1(X, A1, A2, ..., An) = !X & (A1 | A2 | ... | An) */
    BITOP_ANDOR, /* ANDOR(X, A1, A2, ..., An) = X & (A1 | A2 | ... | An) */
    /* ONE(A1, A2, ..., An) = X.
     * If X[i] is the i-th bit of X then:
     * X[i] == 1 if and only if there is m such that:
     * Am[i] == 1 and Al[i] == 0 for all l != m. */
    BITOP_ONE
} bitmapBitop;

void bitmapRoaringInit(void);
robj *createBitmapObject(void);
robj *createBitmapObjectFromString(const unsigned char *buf, size_t len);
robj *bitmapTypeDup(const robj *o);
void freeBitmapObject(robj *o);
void dismissBitmapObject(robj *o, size_t size_hint);
void bitmapObjectDefrag(robj *o);
unsigned long bitmapObjectDefragIncremental(robj *o, unsigned long cursor);
size_t bitmapObjectContainerCount(const robj *o);

uint64_t bitmapObjectLen(const robj *o);
size_t bitmapObjectAllocSize(const robj *o);
uint64_t bitmapObjectCardinality(const robj *o);
uint64_t bitmapObjectRangeCardinality(const robj *o, uint64_t start, uint64_t end);
typedef void bitmapObjectRangeCallback(uint64_t start, uint64_t end, void *privdata);
void bitmapObjectVisitSetBitRanges(const robj *o, bitmapObjectRangeCallback *callback, void *privdata);
long long bitmapObjectBitpos(const robj *o, int bit, uint64_t start, uint64_t end, int end_given);
int bitmapObjectCanRepresentBit(uint64_t bitoffset);
int bitmapObjectGetBit(const robj *o, uint64_t bitoffset);
int bitmapObjectSetBit(robj *o, uint64_t bitoffset, int on);
uint64_t bitmapObjectGetUnsignedBitfield(const robj *o, uint64_t offset, uint64_t bits);
int bitmapObjectSetUnsignedBitfield(robj *o, uint64_t offset, uint64_t bits, uint64_t value);
void bitmapObjectOptimize(robj *o);
sds bitmapObjectMaterialize(const robj *o);
sds bitmapObjectMaterializeForRDB(const robj *o);
robj *bitmapObjectsBitop(bitmapBitop op, robj **objects, size_t numkeys, uint64_t maxlen);

#endif /* __BITMAP_ROARING_H */
