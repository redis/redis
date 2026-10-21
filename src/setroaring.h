/* Experimental set storage: signed integers in per-group CRoaring32, strings in a dict. */
#ifndef __SETROARING_H
#define __SETROARING_H

#include "dict.h"
#include "object.h"
#include "sds.h"
#include <stddef.h>
#include <stdint.h>
#ifndef DISABLENEON
#define DISABLENEON 1
#endif
#ifndef ROARING_DISABLE_X64
#define ROARING_DISABLE_X64 1
#endif

typedef struct setRoaring {
    robj *integers; /* The existing Redis Roaring64 bitmap, storing encoded integers. */
    uint64_t promoted_count;
    dict *sparse;
    uint64_t sparse_count;
    dict *strings;
    uint8_t group_bits; /* Number of low bits kept in each sparse slice. */
} setRoaring;

/* Unpromoted low-bit values sharing one high-bit prefix. */
typedef struct setRoaringSparseBucket {
    size_t count;
    size_t capacity;
    uint32_t min_value;
    uint32_t max_value;
    uint32_t values[];
} setRoaringSparseBucket;

void setRoaringInit(void);
setRoaring *setRoaringCreate(void);
setRoaring *setRoaringDup(const setRoaring *set);
void setRoaringFree(setRoaring *set);
void setRoaringDismiss(setRoaring *set, size_t size_hint);
int setRoaringAddInt(setRoaring *set, int64_t value);
int setRoaringRemoveInt(setRoaring *set, int64_t value);
int setRoaringFindInt(const setRoaring *set, int64_t value);
int setRoaringAddString(setRoaring *set, sds value);
int setRoaringRemoveString(setRoaring *set, const char *value, size_t len);
int setRoaringFindString(const setRoaring *set, const char *value, size_t len);
uint64_t setRoaringIntCount(const setRoaring *set);
uint64_t setRoaringSize(const setRoaring *set);
int setRoaringSelectInt(const setRoaring *set, uint64_t rank, int64_t *value);
dict *setRoaringSparse(setRoaring *set);
robj *setRoaringIntegers(setRoaring *set);
uint64_t setRoaringSparseEncodedValue(const setRoaring *set, const dictEntry *entry, uint32_t low);
int64_t setRoaringDecodeValue(uint64_t value);
uint64_t setRoaringAllocSize(const setRoaring *set);
dict *setRoaringStrings(setRoaring *set);

#endif
