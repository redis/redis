/*
 * Copyright Redis Ltd. 2026 - present
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 *
 * WHAT IS BITROAR?
 * ----------------
 * bitroar backs the OBJ_ENCODING_BITMAP_ROARING encoding of bitmap objects
 * with a 64-bit Roaring bitmap (CRoaring). It exposes the operations the
 * bitmap commands (SETBIT, GETBIT, BITCOUNT, BITPOS, BITFIELD, BITOP, ...)
 * and the object machinery (dup, free, defrag, dismiss, RDB persistence)
 * need, while keeping the string-bitmap semantics: a logical byte length,
 * bit offsets addressed from the most significant bit of byte 0, and
 * materialization back to a flat string when required.
 */

#ifndef __BITROAR_H
#define __BITROAR_H

#include "sds.h"    /* sds */
#include "object.h" /* robj */

#include <stdint.h>
#include <sys/types.h>

/* Internal representability bound for Roaring bitmap logical length. Command
 * handlers enforce the user-configured proto-max-bulk-len limit separately;
 * this bound only protects signed range and length arithmetic. */
#define BITROAR_MAX_BYTES_RAW (INT64_MAX >> 3)
#define BITROAR_MAX_BYTES ((uint64_t)BITROAR_MAX_BYTES_RAW)

/* BITOP NOT fills every missing 2^16-bit Roaring chunk in the logical range.
 * Reject new work above 2^16 chunks (512 MiB) so a compact bitmap with a high
 * logical length cannot amplify into unbounded allocations. This fixed limit
 * is independent of proto-max-bulk-len, which may be changed after a value is
 * created. Master and AOF clients bypass admission limits so commands accepted
 * by an older version can still be replayed. */
#define BITROAR_BITOP_NOT_MAX_BYTES (512ULL * 1024 * 1024)

/* Bitwise operations supported by bitroarApplyOp() (the BITOP command). */
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

/* Called on invocation, per-bit visitor for bitroarVisitSetBitRanges().
 * Reports one maximal run of set bits as [start, end) bit offsets. */
typedef void bitroarRangeCallback(uint64_t start, uint64_t end, void *privdata);

/* Initialization (once at server startup: plugs zmalloc into CRoaring) */
void bitroarInit(void);

/* Object lifecycle */
robj *bitroarCreate(void);
robj *bitroarCreateFromString(const unsigned char *buf, size_t len);
robj *bitroarCreateFromPortable(const unsigned char *buf, size_t len, uint64_t byte_len);
robj *bitroarDup(const robj *o);
void bitroarFree(robj *o);

/* Memory management (fork-child dismissal, active defrag, accounting) */
void bitroarDismiss(robj *o, size_t size_hint);
void bitroarDefrag(robj *o);
unsigned long bitroarDefragIncremental(robj *o, unsigned long cursor);
size_t bitroarAllocSize(const robj *o);
size_t bitroarContainerCount(const robj *o);

/* Read operations */
uint64_t bitroarLen(const robj *o);
uint64_t bitroarCardinality(const robj *o);
uint64_t bitroarRangeCardinality(const robj *o, uint64_t start, uint64_t end);
void bitroarVisitSetBitRanges(const robj *o, bitroarRangeCallback *callback, void *privdata);
long long bitroarBitpos(const robj *o, int bit, uint64_t start, uint64_t end, int end_given);
int bitroarCanRepresentBit(uint64_t bitoffset);
int bitroarGetBit(const robj *o, uint64_t bitoffset);
uint64_t bitroarGetUnsignedBitfield(const robj *o, uint64_t offset, uint64_t bits);

/* Write operations */
int bitroarSetBit(robj *o, uint64_t bitoffset, int on);
int bitroarSetUnsignedBitfield(robj *o, uint64_t offset, uint64_t bits, uint64_t value);
robj *bitroarApplyOp(bitroarOp op, robj **objects, size_t numkeys, uint64_t maxlen);

/* Serialization. MaterializeForDebug flattens to the logical raw string bytes
 * for DEBUG BITMAP-RAW and rejects lengths above proto-max-bulk-len.
 * SerializePortable emits the RoaringFormatSpec 64-bit portable format, whose
 * size tracks resident data rather than the logical length. */
sds bitroarMaterializeForDebug(const robj *o);
sds bitroarSerializePortable(const robj *o);

#endif /* __BITROAR_H */
