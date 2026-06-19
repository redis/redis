#ifndef __BITMAP_ROARING_H
#define __BITMAP_ROARING_H

#include "sds.h"
#include "object.h"

#include <stdint.h>
#include <sys/types.h>

/* Internal sanity cap for native bitmap logical length. Public bitmap commands
 * keep the legacy proto-max-bulk-len offset limit. */
#define BITMAP_OBJECT_MAX_BYTES_RAW (INT64_MAX >> 3)
#define BITMAP_OBJECT_MAX_BYTES ((uint64_t)BITMAP_OBJECT_MAX_BYTES_RAW)
#define BITMAP_OBJECT_MAX_BITOFFSET (BITMAP_OBJECT_MAX_BYTES * 8 - 1)

void bitmapRoaringInit(void);
robj *createBitmapObject(void);
robj *createBitmapObjectFromString(const unsigned char *buf, size_t len);
robj *bitmapTypeDup(const robj *o);
void freeBitmapObject(robj *o);
void dismissBitmapObject(robj *o, size_t size_hint);
void bitmapObjectDefrag(robj *o);
unsigned long bitmapObjectDefragIncremental(robj *o, unsigned long cursor);
size_t bitmapObjectContainerCount(const robj *o);

typedef enum bitmapBitop {
    BITMAP_BITOP_AND = 0,
    BITMAP_BITOP_OR,
    BITMAP_BITOP_XOR,
    BITMAP_BITOP_NOT,
    BITMAP_BITOP_DIFF,
    BITMAP_BITOP_DIFF1,
    BITMAP_BITOP_ANDOR,
    BITMAP_BITOP_ONE
} bitmapBitop;

uint64_t bitmapObjectLen(const robj *o);
size_t bitmapObjectAllocSize(const robj *o);
uint64_t bitmapObjectCardinality(const robj *o);
uint64_t bitmapObjectRangeCardinality(const robj *o, uint64_t start, uint64_t end);
typedef void bitmapObjectRangeCallback(uint64_t start, uint64_t end, void *privdata);
void bitmapObjectVisitSetBitRanges(const robj *o,
                                   bitmapObjectRangeCallback *callback,
                                   void *privdata);
long long bitmapObjectBitpos(const robj *o, int bit, uint64_t start, uint64_t end, int end_given);
int bitmapObjectCanRepresentBit(uint64_t bitoffset);
int bitmapObjectGetBit(const robj *o, uint64_t bitoffset);
int bitmapObjectSetBit(robj *o, uint64_t bitoffset, int on);
sds bitmapObjectMaterialize(const robj *o);
robj *bitmapObjectsBitopBitmap(bitmapBitop op, robj **objects, size_t numkeys, uint64_t maxlen);

#endif /* __BITMAP_ROARING_H */
