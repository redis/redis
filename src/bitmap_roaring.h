#ifndef __BITMAP_ROARING_H
#define __BITMAP_ROARING_H

#include "sds.h"
#include "object.h"
#include "rio.h"

#include <stdint.h>
#include <sys/types.h>

/* Internal representability cap for native bitmap logical length. Keep v1
 * native bitmaps inside the same 32-bit bit index space as bounded Roaring
 * bitmaps: 512 MiB of logical bytes, max bit offset UINT32_MAX. Command
 * handlers still enforce the client-visible proto-max-bulk-len limit for
 * writes, dense/materializing paths, and legacy string reads when it is lower;
 * read-only native lookups may exceed that lower runtime limit inside this
 * cap. */
#define BITMAP_OBJECT_MAX_BYTES_RAW (512LL*1024*1024)
#define BITMAP_OBJECT_MAX_BYTES ((uint64_t)BITMAP_OBJECT_MAX_BYTES_RAW)
#define BITMAP_OBJECT_MAX_BITOFFSET (BITMAP_OBJECT_MAX_BYTES * 8 - 1)

void bitmapRoaringInit(void);
robj *createBitmapObject(void);
robj *createBitmapObjectWithLen(uint64_t byte_len);
robj *createBitmapObjectFromString(const unsigned char *buf, size_t len);
robj *createBitmapObjectFromStringNoOptimize(const unsigned char *buf,
                                             size_t len);
robj *createBitmapObjectFromPortable(const unsigned char *buf, size_t len,
                                     uint64_t byte_len);
robj *bitmapTypeDup(const robj *o);
void freeBitmapObject(robj *o);
void dismissBitmapObject(robj *o, size_t size_hint);
void bitmapObjectDefrag(robj *o);
unsigned long bitmapObjectDefragIncremental(robj *o, unsigned long cursor);
size_t bitmapObjectContainerCount(const robj *o);

typedef enum bitmapBitop {
    BITOP_AND = 0,
    BITOP_OR,
    BITOP_XOR,
    BITOP_NOT,
    BITOP_DIFF,
    BITOP_DIFF1,
    BITOP_ANDOR,
    /* ONE(A1, A2, ..., An) = X.
     * If X[i] is the i-th bit of X then:
     * X[i] == 1 if and only if there is m such that:
     * Am[i] == 1 and Al[i] == 0 for all l != m. */
    BITOP_ONE
} bitmapBitop;

uint64_t bitmapObjectLen(const robj *o);
size_t bitmapObjectAllocSize(const robj *o);
uint64_t bitmapObjectCardinality(const robj *o);
uint64_t bitmapObjectRangeCardinality(const robj *o, uint64_t start, uint64_t end);
typedef struct bitmapObjectBenchResult {
    uint64_t iterations;
    uint64_t sink;
    long long direct_cardinality_us;
    long long wrapper_cardinality_us;
    long long direct_range_cardinality_us;
    long long wrapper_range_cardinality_us;
    long long direct_minimum_us;
    long long wrapper_bitpos_one_us;
    long long wrapper_bitpos_zero_us;
} bitmapObjectBenchResult;
void bitmapObjectBench(const robj *o, uint64_t iterations,
                       bitmapObjectBenchResult *result);
typedef void bitmapObjectRangeCallback(uint64_t start, uint64_t end, void *privdata);
void bitmapObjectVisitSetBitRanges(const robj *o, bitmapObjectRangeCallback *callback, void *privdata);
long long bitmapObjectBitpos(const robj *o, int bit, uint64_t start, uint64_t end, int end_given);
int bitmapObjectCanRepresentBit(uint64_t bitoffset);
int bitmapObjectGetBit(const robj *o, uint64_t bitoffset);
int bitmapObjectSetBit(robj *o, uint64_t bitoffset, int on);
uint64_t bitmapObjectGetUnsignedBitfield(const robj *o, uint64_t offset,
                                         uint64_t bits);
int bitmapObjectSetUnsignedBitfield(robj *o, uint64_t offset, uint64_t bits,
                                    uint64_t value);
int bitmapObjectAddRange(robj *o, uint64_t start, uint64_t end);
void bitmapObjectOptimize(robj *o);
sds bitmapObjectMaterialize(const robj *o);
sds bitmapObjectMaterializeForRDB(const robj *o);
sds bitmapObjectSerializePortable(const robj *o);
ssize_t bitmapObjectSaveRdbContainers(rio *rdb, const robj *o);
robj *createBitmapObjectFromRdbContainers(rio *rdb, uint64_t byte_len);
int bitmapObjectEndianRoundtripCheck(const robj *o);
robj *bitmapObjectsBitopBitmap(bitmapBitop op, robj **objects, size_t numkeys, uint64_t maxlen);

#endif /* __BITMAP_ROARING_H */
