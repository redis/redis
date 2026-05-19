/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Sparse Array - A memory-efficient sparse array with 64-bit index space.
 *
 * This data structure was designed and implemented by Salvatore Sanfilippo.
 */

#ifndef __SPARSEARRAY_H
#define __SPARSEARRAY_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ============================================================================
 * SPARSE ARRAY OVERVIEW
 * ============================================================================
 *
 * Sparse arrays are random-access sequences indexed by non-negative 64-bit
 * integers. They support O(1) get/set operations and efficient iteration.
 *
 * MEMORY LAYOUT
 * -------------
 * The array uses a two-level structure: a directory pointing to "slices",
 * which contain just a range of elements. For very large/sparse arrays, a
 * three-level "superdir" structure is used.
 *
 * SLICE TYPES
 * -----------
 * Each slice holds up to slice_size elements and can be:
 *
 * - Sparse: Sorted array of (offset, value) pairs. Memory-efficient when
 *   elements are scattered within the slice.
 *
 * - Dense: Contiguous array with a sliding window. Used when the slice
 *   has many elements.
 *
 * VALUE ENCODING (Tagged Pointers)
 * --------------------------------
 * Values are stored in tagged pointer-sized words, using the low 2 bits as a
 * tag. The exact immediate encoding depends on pointer width:
 *
 * 64-bit builds:
 *   Tag 00: arString pointer (heap-allocated, 8+ byte strings)
 *   Tag 01: Immediate signed integer in the 62-bit payload
 *   Tag 10: Immediate double (low 2 bits of the IEEE-754 payload cleared)
 *   Tag 11: Inline small string (0-7 bytes)
 *
 * 32-bit builds:
 *   Tag 00: arString pointer
 *   Tag 01: Immediate signed integer in the 30-bit payload
 *   Tag 10: Immediate float (low 2 bits of the IEEE-754 payload cleared)
 *   Tag 11: Inline small string (0-3 bytes)
 *
 * RDB persistence is architecture-neutral: values are saved as logical ints,
 * doubles and strings, never as raw tagged words.
 * ========================================================================== */

/* ----------------------------------------------------------------------------
 * Configuration defaults
 * -------------------------------------------------------------------------- */

#define AR_SLICE_SIZE_DEFAULT     4096
#define AR_SLICE_SIZE_MIN         256
#define AR_SLICE_SIZE_MAX         65536
#define AR_SPARSE_KMAX_DEFAULT    10
#define AR_SPARSE_KMIN_DEFAULT    5

/* Superdir: fixed-size blocks of slice pointers. Each block holds 2048
 * pointers to actual array slices, which uses about 8 KB on 32-bit builds
 * and 16 KB on 64-bit builds. This keeps very large indices from forcing
 * catastrophic flat-directory growth. */
#define AR_SUPER_BLOCK_SLOTS      2048

/* Internal constants */
#define AR_SLICE_MIN_ALLOC        8         /* Initial dense window allocation */
#define AR_INSERT_IDX_NONE        UINT64_MAX  /* No insert performed yet */

/* Slice encoding types */
#define AR_SLICE_DENSE   0
#define AR_SLICE_SPARSE  1

/* Tagged value encoding (low 2 bits). NULL (0) means empty slot. */
#define AR_TAG_PTR       ((uintptr_t)0)   /* arString pointer (low 2 bits = 00) */
#define AR_TAG_INT       ((uintptr_t)1)   /* Immediate signed integer (01) */
#define AR_TAG_FLOAT     ((uintptr_t)2)   /* Immediate float (10) */
#define AR_TAG_STR       ((uintptr_t)3)   /* Inline small string (11) */
#define AR_TAG_MASK      ((uintptr_t)3)

#if UINTPTR_MAX == UINT64_MAX
#define AR_SMALLSTR_MAXLEN   7
#define AR_SMALLSTR_LEN_MASK 0x7u
#elif UINTPTR_MAX == UINT32_MAX
#define AR_SMALLSTR_MAXLEN   3
#define AR_SMALLSTR_LEN_MASK 0x3u
#else
#error "Unsupported pointer size"
#endif

/* RDB type tags for array elements */
#define AR_RDB_TAG_SDS      0
#define AR_RDB_TAG_INT      1
#define AR_RDB_TAG_FLOAT    2
#define AR_RDB_TAG_SMALLSTR 3

/* Buffer size for inline types (int/float/smallstr) */
#define AR_INLINE_BUFSIZE   64

/* ----------------------------------------------------------------------------
 * Data structures
 * -------------------------------------------------------------------------- */

/* Array slice: holds a range of elements. Single allocation with payload. */
typedef struct arSlice {
    uint8_t encoding;       /* 0=dense, 1=sparse */
    uint8_t _pad1[3];
    uint32_t count;         /* Non-empty items in this slice */
    union {
        struct {
            uint32_t offset;    /* First logical offset in window */
            uint32_t winsize;   /* Window size (power of two) */
            uint32_t max_idx;   /* Highest offset with a value */
            void **items;       /* Points into payload */
        } dense;
        struct {
            uint32_t cap;       /* Capacity */
            uint16_t *offsets;  /* Points into payload */
            void **values;      /* Points into payload (aligned) */
        } sparse;
    } layout;
} arSlice;

/* Super-directory entry: groups slices into fixed-size pointer blocks. */
typedef struct arSDirEntry {
    uint64_t block_id;      /* slice_id / AR_SUPER_BLOCK_SLOTS */
    uint32_t count;         /* Non-NULL slots in this block */
    uint32_t _pad;
    arSlice **slots;        /* AR_SUPER_BLOCK_SLOTS pointers to slices */
} arSDirEntry;

/* Array header */
typedef struct redisArray {
    uint64_t count;             /* Total non-empty items */
    uint64_t insert_idx;        /* Last insert index, or UINT64_MAX if none */
    uint64_t dir_alloc;         /* Flat directory length (flat mode) */
    uint64_t dir_highest_used;  /* Highest non-NULL slice index */
    uint64_t num_slices;        /* Number of allocated slices */
    size_t alloc_size;          /* Tracked total allocation (for slot stats) */
    uint32_t slice_size;        /* Slice size (power of two) */
    uint32_t sdir_len;          /* Superdir entries count */
    uint32_t sdir_cap;          /* Superdir capacity */
    uint32_t _pad;
    arSlice **dir;              /* Flat directory or NULL */
    arSDirEntry *superdir;      /* Super-directory or NULL */
} redisArray;

/* ----------------------------------------------------------------------------
 * Inline helpers: index arithmetic
 * -------------------------------------------------------------------------- */

/* Compute bits needed to address elements within a slice. */
static inline int arSliceBits(uint32_t slice_size) {
    if (slice_size == 4096) return 12;  /* Fast path for default */
    int bits = 0;
    uint32_t x = slice_size;
    while (x > 1) { x >>= 1; bits++; }
    return bits;
}

static inline uint64_t arSliceId(uint64_t idx, uint32_t slice_size) {
    return idx >> arSliceBits(slice_size);
}

static inline uint32_t arSliceOff(uint64_t idx, uint32_t slice_size) {
    return (uint32_t)(idx & (slice_size - 1));
}

static inline uint64_t arMakeIdx(uint64_t slice_id, uint32_t off, uint32_t slice_size) {
    return (slice_id << arSliceBits(slice_size)) | off;
}

/* ----------------------------------------------------------------------------
 * Inline helpers: tagged value encoding
 * -------------------------------------------------------------------------- */

static inline int arIsEmpty(void *v) { return v == NULL; }

static inline int arIsPtr(void *v) {
    return v != NULL && ((uintptr_t)v & AR_TAG_MASK) == AR_TAG_PTR;
}

static inline int arIsInt(void *v) {
    return ((uintptr_t)v & AR_TAG_MASK) == AR_TAG_INT;
}

static inline int64_t arToInt(void *v) {
    return (int64_t)(intptr_t)v >> 2;  /* Arithmetic shift preserves sign */
}

static inline void *arFromInt(int64_t ival) {
    return (void *)(((uintptr_t)ival << 2) | AR_TAG_INT);
}

static inline int arIntFits(int64_t ival) {
#if UINTPTR_MAX == UINT64_MAX
    return ival >= -(1LL << 61) && ival <= (1LL << 61) - 1;
#else
    return ival >= -(1LL << 29) && ival <= (1LL << 29) - 1;
#endif
}

static inline int arIsFloat(void *v) {
    return ((uintptr_t)v & AR_TAG_MASK) == AR_TAG_FLOAT;
}

static inline double arToDouble(void *v) {
#if UINTPTR_MAX == UINT64_MAX
    uint64_t bits = (uintptr_t)v & ~AR_TAG_MASK;
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
#else
    uint32_t bits = (uint32_t)((uintptr_t)v & ~(uintptr_t)AR_TAG_MASK);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return (double)f;
#endif
}

static inline void *arFromFloatBits(uint64_t bits_trunc) {
#if UINTPTR_MAX == UINT64_MAX
    return (void *)((bits_trunc & ~AR_TAG_MASK) | AR_TAG_FLOAT);
#else
    uint32_t bits32 = (uint32_t)bits_trunc;
    return (void *)(uintptr_t)((bits32 & ~(uint32_t)AR_TAG_MASK) | AR_TAG_FLOAT);
#endif
}

static inline int arIsSmallStr(void *v) {
    return ((uintptr_t)v & AR_TAG_MASK) == AR_TAG_STR;
}

static inline int arSmallStrLen(void *v) {
    return (int)(((uintptr_t)v >> 2) & AR_SMALLSTR_LEN_MASK);
}

static inline int arToSmallStr(void *v, char *buf) {
    int len = arSmallStrLen(v);
    uintptr_t val = (uintptr_t)v;
    for (int i = 0; i < len; i++) {
        buf[i] = (char)((val >> (8 * (i + 1))) & 0xFF);
    }
    buf[len] = '\0';
    return len;
}

static inline void *arFromSmallStr(const char *s, int len) {
    uintptr_t v = AR_TAG_STR | ((uintptr_t)len << 2);
    for (int i = 0; i < len; i++) {
        v |= ((uintptr_t)(uint8_t)s[i]) << (8 * (i + 1));
    }
    return (void *)v;
}

/* ----------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/* Lifecycle */
redisArray *arNew(void);
void arFree(redisArray *ar);
redisArray *arDup(redisArray *ar);
void arDismiss(redisArray *ar, size_t size_hint);

/* Element access */
void *arGet(redisArray *ar, uint64_t idx);
void arSet(redisArray *ar, uint64_t idx, void *v);
int arDel(redisArray *ar, uint64_t idx);

/* Value encoding/decoding */
void *arEncode(const char *s, size_t len);
const char *arDecode(void *v, char *buf, size_t bufsize, size_t *outlen);
int arFormatFloat(double d, char *buf, size_t bufsize);
size_t arStringLen(const void *ptr);
const char *arStringData(const void *ptr);
void *arValueFromRdbInt(int64_t ival);
void *arValueFromRdbFloat(double d);
void *arValueFromRdbSmallStr(const char *s, size_t len);

/* Queries */
uint64_t arCount(redisArray *ar);
uint64_t arLen(redisArray *ar);

/* Bulk operations */
uint64_t arDeleteRange(redisArray *ar, uint64_t lo, uint64_t hi);
void arTruncate(redisArray *ar, uint64_t limit);
void arMayPromoteToDenseForRangeSet(redisArray *ar, uint64_t lo, uint64_t hi);

/* Utilities */
uint32_t arSparseFindPos(arSlice *s, uint16_t rel_idx, int *found);
uint32_t arSuperDirFind(redisArray *ar, uint64_t block_id, int *found);
redisArray *arDefrag(redisArray *ar, void *(*defragfn)(void *));
unsigned long arDefragIncremental(redisArray **arref, unsigned long cursor,
                                  void *(*defragfn)(void *));

#endif /* __SPARSEARRAY_H */
