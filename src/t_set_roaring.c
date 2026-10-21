/* Experimental hybrid Roaring backend for integer and string set members. */
#ifndef DISABLENEON
#define DISABLENEON 1
#endif
#ifndef ROARING_DISABLE_X64
#define ROARING_DISABLE_X64 1
#endif
#include "server.h"
#include "bitroar.h"
#include "setroaring.h"
#include "t_set_encoding.h"

static int srRawAdd(robj *o, char *str, size_t len, int64_t *llvalp,
                    int str_is_sds, int after_convert, int *target_enc) {
    UNUSED(after_convert);
    UNUSED(target_enc);
    setRoaring *set = o->ptr;
    long long value;
    if (llvalp) return setRoaringAddInt(set, *llvalp);
    if (string2ll(str, len, &value)) return setRoaringAddInt(set, value);
    sds member = str_is_sds ? sdsdup((sds)str) : sdsnewlen(str, len);
    if (setRoaringAddString(set, member)) return 1;
    sdsfree(member);
    return 0;
}

static int srRawRemove(robj *o, char *str, size_t len, int64_t llval,
                       int str_is_sds) {
    UNUSED(str_is_sds);
    if (!str) return setRoaringRemoveInt(o->ptr, llval);
    long long value;
    if (string2ll(str, len, &value)) return setRoaringRemoveInt(o->ptr, value);
    return setRoaringRemoveString(o->ptr, str, len);
}

static int srIsMember(robj *o, char *str, size_t len, int64_t llval,
                      int str_is_sds) {
    UNUSED(str_is_sds);
    if (!str) return setRoaringFindInt(o->ptr, llval);
    long long value;
    if (string2ll(str, len, &value)) return setRoaringFindInt(o->ptr, value);
    return setRoaringFindString(o->ptr, str, len);
}

static void srIterInit(setTypeIterator *si) {
    setRoaring *set = si->subject->ptr;
    si->roaring_phase = 0;
    si->roaring_iter = bitroarRawIteratorCreate(set->integers);
    si->roaring_sparse_entry = NULL;
    si->roaring_sparse_index = 0;
    dictInitIterator(&si->roaring_sparse_di, setRoaringSparse(set));
    dictInitIterator(&si->di, setRoaringStrings(set));
}

static void srIterReset(setTypeIterator *si) {
    if (si->roaring_iter) bitroarRawIteratorFree(si->roaring_iter);
    si->roaring_iter = NULL;
    dictResetIterator(&si->roaring_sparse_di);
    dictResetIterator(&si->di);
}

static int srIterNext(setTypeIterator *si, char **str, size_t *len,
                      int64_t *llele) {
    if (si->roaring_phase == 0) {
        uint64_t encoded;
        if (bitroarRawIteratorNext(si->roaring_iter, &encoded)) {
            *llele = setRoaringDecodeValue(encoded);
            *str = NULL;
            return 0;
        }
        bitroarRawIteratorFree(si->roaring_iter);
        si->roaring_iter = NULL;
        si->roaring_phase = 1;
    }
    if (si->roaring_phase == 1) {
        if (si->roaring_sparse_entry) {
            setRoaringSparseBucket *bucket = dictGetVal(si->roaring_sparse_entry);
            if (si->roaring_sparse_index < bucket->count) {
                uint64_t encoded = setRoaringSparseEncodedValue(si->roaring_sparse_entry,
                    bucket->values[si->roaring_sparse_index++]);
                *llele = setRoaringDecodeValue(encoded);
                *str = NULL;
                return 0;
            }
        }
        si->roaring_sparse_entry = dictNext(&si->roaring_sparse_di);
        si->roaring_sparse_index = 0;
        if (si->roaring_sparse_entry) {
            setRoaringSparseBucket *bucket = dictGetVal(si->roaring_sparse_entry);
            uint64_t encoded = setRoaringSparseEncodedValue(si->roaring_sparse_entry,
                bucket->values[si->roaring_sparse_index++]);
            *llele = setRoaringDecodeValue(encoded);
            *str = NULL;
            return 0;
        }
        si->roaring_phase = 2;
    }
    dictEntry *de = dictNext(&si->di);
    if (!de) return -1;
    *str = dictGetKey(de);
    *len = sdslen(*str);
    *llele = -123456789;
    return 0;
}

static void srRandomElement(robj *o, char **str, size_t *len, int64_t *llele) {
    setRoaring *set = o->ptr;
    uint64_t ints = setRoaringIntCount(set), total = setRoaringSize(set);
    uint64_t rank = (uint64_t)rand() % total;
    if (rank < ints) {
        serverAssert(setRoaringSelectInt(set, rank, llele));
        *str = NULL;
    } else {
        dictEntry *de = dictGetFairRandomKey(setRoaringStrings(set));
        *str = dictGetKey(de);
        *len = sdslen(*str);
        *llele = -123456789;
    }
}

static unsigned long srSize(const robj *o) { return setRoaringSize(o->ptr); }
static size_t srAllocSize(const robj *o) { return setRoaringAllocSize(o->ptr); }

static robj *srDup(robj *o) {
    robj *copy = createObject(OBJ_SET, setRoaringDup(o->ptr));
    copy->encoding = OBJ_ENCODING_SET_ROARING;
    return copy;
}

static void srFree(robj *o) { setRoaringFree(o->ptr); }

static void *srConvertFrom(robj *o, unsigned long cap, int panic) {
    UNUSED(panic);
    setRoaring *set = setRoaringCreate();
    if (!set) return NULL;
    if (cap) dictTryExpand(setRoaringStrings(set), cap);
    setTypeIterator si;
    setTypeInitIterator(&si, o);
    char *str;
    size_t len = 0;
    int64_t value = 0;
    while (setTypeNext(&si, &str, &len, &value) != -1) {
        if (!str || string2ll(str, len, (long long *)&value)) {
            setRoaringAddInt(set, value);
        } else {
            serverAssert(setRoaringAddString(set, sdsnewlen(str, len)));
        }
    }
    setTypeResetIterator(&si);
    return set;
}

const setTypeOps setTypeOpsRoaring = {
    .rawAdd = srRawAdd, .rawRemove = srRawRemove, .isMember = srIsMember,
    .iterInit = srIterInit, .iterReset = srIterReset, .iterNext = srIterNext,
    .randomElement = srRandomElement, .size = srSize, .allocSize = srAllocSize,
    .dup = srDup, .free = srFree, .convertFrom = srConvertFrom,
};
