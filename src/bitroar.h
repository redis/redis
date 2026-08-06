#ifndef __BITROAR_H
#define __BITROAR_H

#include "sds.h"
#include "object.h"
#include "rio.h"

#include <stdint.h>
#include <sys/types.h>

/* Internal representability bound for Roaring bitmap logical length. Command
 * handlers enforce the user-configured proto-max-bulk-len limit separately;
 * this bound only protects signed range and length arithmetic. */
#define BITROAR_MAX_BYTES_RAW (INT64_MAX >> 3)
#define BITROAR_MAX_BYTES ((uint64_t)BITROAR_MAX_BYTES_RAW)

typedef enum bitroarOp {
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
} bitroarOp;

void bitroarInit(void);
robj *bitroarCreate(void);
robj *bitroarCreateFromString(const unsigned char *buf, size_t len);
robj *bitroarCreateFromPortable(const unsigned char *buf, size_t len,
                                uint64_t byte_len);
robj *bitroarDup(const robj *o);
void bitroarFree(robj *o);
void bitroarDismiss(robj *o, size_t size_hint);
void bitroarDefrag(robj *o);
unsigned long bitroarDefragIncremental(robj *o, unsigned long cursor);
size_t bitroarContainerCount(const robj *o);

uint64_t bitroarLen(const robj *o);
size_t bitroarAllocSize(const robj *o);
uint64_t bitroarCardinality(const robj *o);
uint64_t bitroarRangeCardinality(const robj *o, uint64_t start, uint64_t end);
typedef void bitroarRangeCallback(uint64_t start, uint64_t end, void *privdata);
void bitroarVisitSetBitRanges(const robj *o, bitroarRangeCallback *callback, void *privdata);
long long bitroarBitpos(const robj *o, int bit, uint64_t start, uint64_t end, int end_given);
int bitroarCanRepresentBit(uint64_t bitoffset);
int bitroarGetBit(const robj *o, uint64_t bitoffset);
int bitroarSetBit(robj *o, uint64_t bitoffset, int on);
uint64_t bitroarGetUnsignedBitfield(const robj *o, uint64_t offset, uint64_t bits);
int bitroarSetUnsignedBitfield(robj *o, uint64_t offset, uint64_t bits, uint64_t value);
void bitroarOptimize(robj *o);
sds bitroarMaterialize(const robj *o);
sds bitroarSerializePortable(const robj *o);
robj *bitroarApplyOp(bitroarOp op, robj **objects, size_t numkeys, uint64_t maxlen);

#endif /* __BITROAR_H */
