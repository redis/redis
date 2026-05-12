/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Sparse Array - A memory-efficient sparse array with 64-bit index space.
 * Originally authored by: Salvatore Sanfilippo.
 *
 * This data structure was designed and implemented by Salvatore Sanfilippo.
 */

#include "server.h"
#include <math.h>
#include <float.h>

/******************************************************************************
 * SPARSE ARRAY IMPLEMENTATION
 *
 * Sparse arrays are random-access sequences indexed by non-negative 64-bit
 * integers. They support O(1) get/set operations and efficient iteration.
 *
 * Arrays use tagged pointer-sized values. 64-bit builds inline more payload,
 * while 32-bit builds use narrower immediate encodings and fall back to
 * arString more often. SDS strings are not used as values since the final
 * bits of SDS pointers are not guaranteed to be zero.
 *
 * See sparsearray.h for data structure documentation and inline helpers.
 *
 *****************************************************************************/

/* ----------------------------------------------------------------------------
 * Configuration - mapped to Redis server struct for easy standalone adaptation
 * -------------------------------------------------------------------------- */

#define ArraySliceSize  server.array_slice_size
#define ArraySparseKMax server.array_sparse_kmax
#define ArraySparseKMin server.array_sparse_kmin

/* ----------------------------------------------------------------------------
 * Allocation size tracking
 *
 * Every zmalloc/zfree/zrealloc that contributes to the array's footprint is
 * tracked in ar->alloc_size so that kvobjAllocSize() can return an O(1)
 * answer. When ar is NULL (e.g. during arFree) tracking is skipped.
 * -------------------------------------------------------------------------- */

static inline void *arAllocAndTrack(redisArray *ar, size_t size) {
    size_t usable;
    void *ptr = zmalloc_usable(size, &usable);
    if (ar) ar->alloc_size += usable;
    return ptr;
}
static inline void *arCallocAndTrack(redisArray *ar, size_t size) {
    size_t usable;
    void *ptr = zcalloc_usable(size, &usable);
    if (ar) ar->alloc_size += usable;
    return ptr;
}
static inline void arFreeAndTrack(redisArray *ar, void *ptr) {
    size_t usable;
    zfree_usable(ptr, &usable);
    if (ar) ar->alloc_size -= usable;
}
static inline void *arReallocAndTrack(redisArray *ar, void *ptr, size_t size) {
    size_t usable, old_usable;
    void *newptr = zrealloc_usable(ptr, size, &usable, &old_usable);
    if (ar) ar->alloc_size += usable - old_usable;
    return newptr;
}

/* Track a tagged value entering/leaving the array (arString bookkeeping). */
static inline void arTrackValueIn(redisArray *ar, void *v) {
    if (ar && arIsPtr(v)) ar->alloc_size += zmalloc_size(v);
}
static inline void arTrackValueOut(redisArray *ar, void *v) {
    if (ar && arIsPtr(v)) ar->alloc_size -= zmalloc_size(v);
}

/* ----------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static inline size_t arStringHeaderSize(size_t len) {
    return (len <= 32767) ? 2 : 8;
}

size_t arStringLen(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    if (p[0] & 0x80) {
        return ((size_t)(p[0] & 0x7F) << 8) | p[1];
    } else {
        size_t len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | p[i];
        return len;
    }
}

const char *arStringData(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    return (const char *)(p + ((p[0] & 0x80) ? 2 : 8));
}

static inline size_t arSparseAllocSize(uint32_t cap) {
    size_t offsets_size = cap * sizeof(uint16_t);
    size_t padding = (sizeof(void *) - (offsets_size % sizeof(void *))) % sizeof(void *);
    return sizeof(arSlice) + offsets_size + padding + cap * sizeof(void *);
}

static inline size_t arDenseAllocSize(uint32_t winsize) {
    return sizeof(arSlice) + winsize * sizeof(void *);
}

static inline uint32_t arSliceMaxIdx(arSlice *s) {
    if (s->encoding == AR_SLICE_DENSE) {
        return s->layout.dense.max_idx;
    } else {
        return s->layout.sparse.offsets[s->count - 1];
    }
}

/* ----------------------------------------------------------------------------
 * arString type
 * -------------------------------------------------------------------------- */

/* Allocate a new arString with the given content.
 *
 * We use arString instead of SDS because SDS pointers are not guaranteed to
 * have the low bits zero (SDS points inside an allocation, after the header).
 * Our tagged pointer scheme needs tag 00 for heap strings, so we need aligned
 * pointers. zmalloc guarantees sufficient alignment.
 *
 * arString has two header formats:
 *
 * 1. Short header (2 bytes): lengths up to 32767 bytes.
 *    The top bit of the first byte is set, and the remaining 15 bits store
 *    the length in big-endian form.
 *
 *      +--------+--------+-------------------+
 *      |1LLLLLLL|LLLLLLLL|      payload      |
 *      +--------+--------+-------------------+
 *       byte 0   byte 1
 *
 * 2. Long header (8 bytes): lengths up to 2^63-1 bytes.
 *    The top bit of the first byte is clear, and the remaining 63 bits store
 *    the length in big-endian form.
 *
 *      +--------+--------+--------+--------+--------+--------+--------+--//
 *      |0LLLLLLL|LLLLLLLL|LLLLLLLL|LLLLLLLL|LLLLLLLL|LLLLLLLL|LLLLLLLL|
 *      +--------+--------+--------+--------+--------+--------+--------+--//
 *       byte 0   byte 1   byte 2   byte 3   byte 4   byte 5   byte 6
 *
 *      //--+--------+-------------------+
 *          |LLLLLLLL|      payload      |
 *      //--+--------+-------------------+
 *           byte 7
 *
 * For simplicity we use a 63 bit len even when Redis is compiled with a 32
 * bit target, the overhead for strings > 32k is small.
 *
 * So the pointer returned by arStringNew() always points to the start of the
 * header, and the string data begins immediately after the 2-byte or 8-byte
 * header. */
void *arStringNew(const char *s, size_t len) {
    /* Length is stored in 63 bits; reject >= 2^63 to avoid
     * hypothetical header corruption. On 32 bit builds this is guaranteed
     * by size_t itself, so don't compile an always-true assertion. */
#if SIZE_MAX > UINT32_MAX
    serverAssert(len < ((size_t)1 << 63));
#endif
    size_t hdr_size = arStringHeaderSize(len);
    uint8_t *ptr = zmalloc(hdr_size + len);

    if (hdr_size == 2) {
        /* Short header: MSB=1, 15-bit length */
        ptr[0] = 0x80 | ((len >> 8) & 0x7F);
        ptr[1] = len & 0xFF;
    } else {
        /* Long header: MSB=0, 63-bit length in big-endian */
        for (int i = 7; i >= 0; i--) {
            ptr[7 - i] = (len >> (i * 8)) & 0xFF;
        }
    }

    memcpy(ptr + hdr_size, s, len);
    return ptr;
}

/* Free arString pointer */
void arStringFree(void *ptr) {
    zfree(ptr);
}

/* Duplicate an arString */
void *arStringDup(void *ptr) {
    size_t len = arStringLen(ptr);
    size_t hdr_size = arStringHeaderSize(len);
    size_t total = hdr_size + len;
    void *dup = zmalloc(total);
    memcpy(dup, ptr, total);
    return dup;
}

/* Free arString if value is pointer-tagged, otherwise nothing to
 * free, the info is encoded in the pointer itself. */
void arFreePtr(void *v) {
    if (arIsPtr(v)) {
        arStringFree(v);
    }
}

/* ----------------------------------------------------------------------------
 * Slice allocation and management
 * -------------------------------------------------------------------------- */

/* Create a new dense slice with given rel_idx (index relative to slice base) */
arSlice *arSliceDenseNew(redisArray *ar, uint32_t rel_idx, uint32_t slice_size) {
    uint32_t winsize = AR_SLICE_MIN_ALLOC;
    uint32_t offset = rel_idx;

    /* Adjust offset if the initial window would extend past the slice
     * boundary. For example, with slice size 4096 (the default), creating
     * the slice around relative index 4093 needs the window shifted left. */
    if (offset + winsize > slice_size) {
        offset = slice_size - winsize;
    }

    arSlice *s = arAllocAndTrack(ar, arDenseAllocSize(winsize));
    s->encoding = AR_SLICE_DENSE;
    s->count = 0;
    s->layout.dense.offset = offset;
    s->layout.dense.winsize = winsize;
    s->layout.dense.max_idx = 0;
    s->layout.dense.items = (void **)(s + 1);  /* Payload starts after struct */
    memset(s->layout.dense.items, 0, winsize * sizeof(void *));
    return s;
}

/* Sparse slices are a single allocation: the arSlice struct followed by
 * a payload containing offsets[] and values[]. This function computes
 * where these arrays live in the payload and sets the pointers accordingly.
 * Must be called after zmalloc or memcpy, since copied slices have stale
 * pointers that still reference the source allocation's memory. The values
 * array requires pointer alignment, hence the padding after offsets[]. */
void arSparseSetupPointers(arSlice *s) {
    char *p = (char *)(s + 1);
    size_t offsets_size = s->layout.sparse.cap * sizeof(uint16_t);
    size_t padding = (sizeof(void *) - (offsets_size % sizeof(void *))) % sizeof(void *);
    s->layout.sparse.offsets = (uint16_t *)p;
    s->layout.sparse.values = (void **)(p + offsets_size + padding);
}

/* Create a new sparse slice */
arSlice *arSliceSparseNew(redisArray *ar) {
    uint32_t cap = (ArraySparseKMax < 4) ? ArraySparseKMax : 4;
    arSlice *s = arAllocAndTrack(ar, arSparseAllocSize(cap));
    s->encoding = AR_SLICE_SPARSE;
    s->count = 0;
    s->layout.sparse.cap = cap;
    arSparseSetupPointers(s);
    return s;
}

/* Free a slice (including all arString values inside).
 * When ar is non-NULL, deducts the memory from ar->alloc_size.
 * Pass NULL for ar when the entire array is being destroyed (arFree). */
void arSliceFree(redisArray *ar, arSlice *s) {
    if (!s) return;

    if (s->encoding == AR_SLICE_DENSE) {
        for (uint32_t i = 0; i < s->layout.dense.winsize; i++) {
            arTrackValueOut(ar, s->layout.dense.items[i]);
            arFreePtr(s->layout.dense.items[i]);
        }
    } else {
        void **values = s->layout.sparse.values;
        for (uint32_t i = 0; i < s->count; i++) {
            arTrackValueOut(ar, values[i]);
            arFreePtr(values[i]);
        }
    }
    arFreeAndTrack(ar, s);
}

/* Grow dense slice to accommodate rel_idx (right growth) */
arSlice *arSliceDenseGrowRight(redisArray *ar, arSlice *s, uint32_t rel_idx, uint32_t slice_size) {
    uint32_t new_winsize = s->layout.dense.winsize;

    /* Double until rel_idx fits */
    while (rel_idx >= s->layout.dense.offset + new_winsize && new_winsize < slice_size) {
        new_winsize <<= 1;
    }

    uint32_t new_offset = s->layout.dense.offset;
    if (new_winsize >= slice_size) {
        new_winsize = slice_size;
        new_offset = 0;
    } else if (new_offset + new_winsize > slice_size) {
        /* Window would exceed slice boundary, adjust offset */
        new_offset = slice_size - new_winsize;
    }

    /* Fast path: when offset does not move, we can use realloc() to grow
     * the dense allocation without relocating existing items ourselves. */
    if (new_offset == s->layout.dense.offset) {
        uint32_t old_winsize = s->layout.dense.winsize;
        arSlice *ns = arReallocAndTrack(ar, s, arDenseAllocSize(new_winsize));
        ns->layout.dense.winsize = new_winsize;
        ns->layout.dense.items = (void **)(ns + 1);

        /* New tail must be explicitly zeroed for arIsEmpty() semantics. */
        memset(ns->layout.dense.items + old_winsize, 0,
               (new_winsize - old_winsize) * sizeof(void *));
        return ns;
    }

    /* Data copy path: offset moved, so we allocate a new slice and copy. */
    arSlice *ns = arAllocAndTrack(ar, arDenseAllocSize(new_winsize));
    ns->encoding = AR_SLICE_DENSE;
    ns->count = s->count;
    ns->layout.dense.offset = new_offset;
    ns->layout.dense.winsize = new_winsize;
    ns->layout.dense.max_idx = s->layout.dense.max_idx;
    ns->layout.dense.items = (void **)(ns + 1);

    /* Zero-fill first to ensure arIsEmpty() works for new slots, then
     * copy old data */
    memset(ns->layout.dense.items, 0, new_winsize * sizeof(void *));
    uint32_t shift = s->layout.dense.offset - new_offset;
    serverAssert(shift + s->layout.dense.winsize <= new_winsize);
    memcpy(ns->layout.dense.items + shift, s->layout.dense.items, s->layout.dense.winsize * sizeof(void *));

    arFreeAndTrack(ar, s);
    return ns;
}

/* Grow dense slice to accommodate rel_idx (left growth with slack).
 * Note that in this case no realloc() optimization is possible, still
 * we can grow on the left more than needed (next power of two) so if
 * there is a right-to-left access pattern we can cope. */
arSlice *arSliceDenseGrowLeft(redisArray *ar, arSlice *s, uint32_t rel_idx, uint32_t slice_size) {
    uint32_t old_end = s->layout.dense.offset + s->layout.dense.winsize;
    uint32_t need = old_end - rel_idx;

    /* Find next power of two that fits */
    uint32_t new_winsize = nearestNextPowerOf2(need);
    if (new_winsize < AR_SLICE_MIN_ALLOC) new_winsize = AR_SLICE_MIN_ALLOC;
    if (new_winsize > slice_size) new_winsize = slice_size;

    /* Position the window so that the old data is right-aligned (leaving
     * slack on left) */
    int32_t new_offset = (int32_t)old_end - (int32_t)new_winsize;
    if (new_offset < 0) new_offset = 0;
    if (new_winsize == slice_size) new_offset = 0;

    arSlice *ns = arAllocAndTrack(ar, arDenseAllocSize(new_winsize));
    ns->encoding = AR_SLICE_DENSE;
    ns->count = s->count;
    ns->layout.dense.offset = (uint32_t)new_offset;
    ns->layout.dense.winsize = new_winsize;
    ns->layout.dense.max_idx = s->layout.dense.max_idx;
    ns->layout.dense.items = (void **)(ns + 1);

    /* Zero-fill for arIsEmpty() semantics, then copy old data right-aligned */
    memset(ns->layout.dense.items, 0, new_winsize * sizeof(void *));
    uint32_t shift = s->layout.dense.offset - ns->layout.dense.offset;
    serverAssert(shift + s->layout.dense.winsize <= new_winsize);
    memcpy(ns->layout.dense.items + shift, s->layout.dense.items, s->layout.dense.winsize * sizeof(void *));

    arFreeAndTrack(ar, s);
    return ns;
}

/* Grow dense slice if rel_idx is outside the current window. Returns a new
 * slice, or the old pointer if the current slice can already accommodate the
 * index. */
arSlice *arSliceDenseGrowIfNeeded(redisArray *ar, arSlice *s, uint32_t rel_idx, uint32_t slice_size) {
    if (rel_idx >= s->layout.dense.offset + s->layout.dense.winsize) {
        return arSliceDenseGrowRight(ar, s, rel_idx, slice_size);
    } else if (rel_idx < s->layout.dense.offset) {
        return arSliceDenseGrowLeft(ar, s, rel_idx, slice_size);
    }
    return s;
}

/* Binary search in sparse slice.
 * Returns index where rel_idx is or should be (the two cases
 * can be distinguished via 'found'). */
uint32_t arSparseFindPos(arSlice *s, uint16_t rel_idx, int *found) {
    uint16_t *offsets = s->layout.sparse.offsets;
    uint32_t lo = 0, hi = s->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (offsets[mid] < rel_idx) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *found = (lo < s->count && offsets[lo] == rel_idx);
    return lo;
}

/* Promote sparse slice to dense. */
arSlice *arSparsePromote(redisArray *ar, arSlice *s, uint32_t slice_size) {
    if (s->count == 0) {
        arFreeAndTrack(ar, s);
        return arSliceDenseNew(ar, 0, slice_size);
    }

    uint16_t *offsets = s->layout.sparse.offsets;
    void **values = s->layout.sparse.values;

    uint32_t min_off = offsets[0];
    uint32_t max_off = offsets[s->count - 1];
    uint32_t need = max_off - min_off + 1;

    uint32_t winsize = nearestNextPowerOf2(need);
    if (winsize < AR_SLICE_MIN_ALLOC) winsize = AR_SLICE_MIN_ALLOC;

    uint32_t offset = min_off;
    if (winsize >= slice_size) {
        winsize = slice_size;
        offset = 0;
    } else if (offset + winsize > slice_size) {
        /* Window would exceed slice boundary, adjust offset */
        offset = slice_size - winsize;
    }

    arSlice *d = arAllocAndTrack(ar, arDenseAllocSize(winsize));
    d->encoding = AR_SLICE_DENSE;
    d->count = s->count;
    d->layout.dense.offset = offset;
    d->layout.dense.winsize = winsize;
    d->layout.dense.max_idx = max_off;
    d->layout.dense.items = (void **)(d + 1);

    /* Set the entries in the sparse representation into the
     * new dense slice. */
    memset(d->layout.dense.items, 0, winsize * sizeof(void *));
    for (uint32_t i = 0; i < s->count; i++) {
        serverAssert(offsets[i] >= offset);
        serverAssert(offsets[i] - offset < winsize);
        d->layout.dense.items[offsets[i] - offset] = values[i];
    }

    arFreeAndTrack(ar, s);
    return d;
}

/* Demote the provided dense slice to a sparse slice, if beneficial.
 * The function returns the dense slice given in input if not demoted,
 * otherwise the newly created sparse slice containing the same elements
 * is returned, in this case, as a side effect, the dense slice in
 * input is freed. */
arSlice *arDenseMaybeDemote(redisArray *ar, arSlice *d) {
    if (ArraySparseKMax == 0) return d; // Sparse is disabled by config.
    if (d->count > ArraySparseKMin) return d; // Yet not at demotion level.
    if (d->count > ArraySparseKMax) return d; // Just config sanity check.
    if (d->layout.dense.winsize == AR_SLICE_MIN_ALLOC) return d; // Already small.

    /* Only demote if it actually saves memory. We require the dense slice
     * to be significantly larger than sparse would be (at least 25% bigger),
     * and large enough in absolute terms (4x kmin) to be worth the trouble. */
    size_t dense_bytes = arDenseAllocSize(d->layout.dense.winsize);
    size_t sparse_bytes = arSparseAllocSize(ArraySparseKMin);
    if (d->layout.dense.winsize < 4 * ArraySparseKMin) return d;
    if (dense_bytes < sparse_bytes * 5 / 4) return d;

    /* Demote it. */
    arSlice *s = arAllocAndTrack(ar, arSparseAllocSize(ArraySparseKMin));
    s->encoding = AR_SLICE_SPARSE;
    s->count = 0;
    s->layout.sparse.cap = ArraySparseKMin;
    arSparseSetupPointers(s);

    /* Copy every entry from dense to sparse. */
    uint16_t *offsets = s->layout.sparse.offsets;
    void **values = s->layout.sparse.values;
    for (uint32_t i = 0; i < d->layout.dense.winsize && s->count < d->count; i++) {
        if (!arIsEmpty(d->layout.dense.items[i])) {
            offsets[s->count] = d->layout.dense.offset + i;
            values[s->count] = d->layout.dense.items[i];
            s->count++;
        }
    }

    arFreeAndTrack(ar, d);
    return s;
}

/* Update max_idx after deletion in dense slice. This is O(winsize) in the worst
 * case, but we only scan when we deleted the current max, which is rare. */
void arDenseUpdateMaxIdx(arSlice *d, uint32_t deleted_off) {
    /* Note that if the slice is left without elements, it will get
     * deallocated so there is nothing to set. */
    if (d->count == 0 || deleted_off < d->layout.dense.max_idx) return;

    /* Scan backward from old max to find new max. */
    for (int pos = d->layout.dense.max_idx - d->layout.dense.offset; pos >= 0; pos--) {
        if (!arIsEmpty(d->layout.dense.items[pos])) {
            d->layout.dense.max_idx = d->layout.dense.offset + pos;
            return;
        }
    }
}

/* ----------------------------------------------------------------------------
 * Directory management (flat mode and superdir mode)
 *
 * Why two modes:
 *
 * - Flat mode (ar->superdir == NULL): ar->dir is indexed by slice_id
 *   (ar->dir[slice_id] -> arSlice*). This is very fast and compact while
 *   slice IDs stay relatively low.
 *
 * - Superdir mode (ar->superdir != NULL): there are two levels of indirection.
 *   Metadata (that is, pointers to actual array slices) is split into sorted
 *   entries by block_id; each block is a fixed table of 2048 slice pointers.
 *   That table uses about 8 KB on 32-bit builds and 16 KB on 64-bit builds.
 *   Blocks are allocated on demand. Basically this means that what was, in
 *   flat mode, a contiguous array of slice pointers (called the directory),
 *   in superdir mode becomes a sparse array of directory pieces.
 *
 * The superdir avoids catastrophic metadata growth for sparse/high indices.
 * A flat directory must be sized up to the highest slice_id, even if almost
 * all entries are NULL. With very large index jumps, that would waste huge
 * memory. Superdir keeps metadata proportional to the number of populated
 * blocks/slices instead of the largest slice_id ever seen.
 *
 * Promotion trigger:
 * - When slice_id >= AR_SUPER_BLOCK_SLOTS (2048), flat mode is promoted.
 * - Practical meaning: slice_id is idx / slice_size.
 *   With default slice_size=4096, threshold slice_id=2048 corresponds to
 *   idx >= 2048*4096 = 8,388,608 (first index that needs block_id 1).
 *
 * Hint: here what we gain is not just efficiency. Also there are no security
 * concerns with setting a very high index. No problem with a corrupted RDB
 * file containing a very high index, and no need to configure a maximum index
 * allowable in an array. Thanks to this design the array type of Redis is
 * a more useful and safe type.
 * -------------------------------------------------------------------------- */

/* Binary search for block_id in superdir.
 * Returns index where found or should be inserted. */
uint32_t arSuperDirFind(redisArray *ar, uint64_t block_id, int *found) {
    uint32_t lo = 0, hi = ar->sdir_len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ar->superdir[mid].block_id < block_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *found = (lo < ar->sdir_len && ar->superdir[lo].block_id == block_id);
    return lo;
}

/* Get slice pointer from superdir mode. Returns NULL if not found. */
arSlice **arSuperDirGetSlot(redisArray *ar, uint64_t slice_id) {
    uint64_t block_id = slice_id / AR_SUPER_BLOCK_SLOTS;
    uint32_t block_off = slice_id % AR_SUPER_BLOCK_SLOTS;

    int found;
    uint32_t pos = arSuperDirFind(ar, block_id, &found);
    if (!found) return NULL;

    return ar->superdir[pos].slots + block_off;
}

/* Ensure block exists in superdir, creating if needed. Returns slot pointer. */
arSlice **arSuperDirEnsureSlot(redisArray *ar, uint64_t slice_id) {
    uint64_t block_id = slice_id / AR_SUPER_BLOCK_SLOTS;
    uint32_t block_off = slice_id % AR_SUPER_BLOCK_SLOTS;

    int found;
    uint32_t pos = arSuperDirFind(ar, block_id, &found);

    if (!found) {
        /* Need to insert new block */
        if (ar->sdir_len >= ar->sdir_cap) {
            /* Grow superdir array */
            uint32_t new_cap = ar->sdir_cap ? ar->sdir_cap * 2 : 4;
            ar->superdir = arReallocAndTrack(ar, ar->superdir, new_cap * sizeof(arSDirEntry));
            ar->sdir_cap = new_cap;
        }

        /* Shift entries to make room */
        if (pos < ar->sdir_len) {
            memmove(ar->superdir + pos + 1, ar->superdir + pos,
                    (ar->sdir_len - pos) * sizeof(arSDirEntry));
        }

        /* Initialize new entry */
        ar->superdir[pos].block_id = block_id;
        ar->superdir[pos].count = 0;
        ar->superdir[pos].slots = arCallocAndTrack(ar, AR_SUPER_BLOCK_SLOTS * sizeof(arSlice *));
        ar->sdir_len++;
    }

    return ar->superdir[pos].slots + block_off;
}

/* Look up the superdir block that contains slice_id.
 * Returns a pointer to that arSDirEntry, or NULL if the block was never
 * allocated (no slices currently exist in that block). */
arSDirEntry *arSuperDirGetEntry(redisArray *ar, uint64_t slice_id) {
    uint64_t block_id = slice_id / AR_SUPER_BLOCK_SLOTS;
    int found;
    uint32_t pos = arSuperDirFind(ar, block_id, &found);
    return found ? ar->superdir + pos : NULL;
}

/* Remove one block entry from superdir at index pos.
 * We assume 'pos' is valid and the block is logically empty (count == 0).
 * Frees the slice-pointer table, compacts remaining entries (keeping order by
 * block_id), and decrements ar->sdir_len. */
void arSuperDirRemoveBlock(redisArray *ar, uint32_t pos) {
    arFreeAndTrack(ar, ar->superdir[pos].slots);
    if (pos < ar->sdir_len - 1) {
        memmove(ar->superdir + pos, ar->superdir + pos + 1,
                (ar->sdir_len - pos - 1) * sizeof(arSDirEntry));
    }
    ar->sdir_len--;
}

/* Promote from flat directory to superdir mode. Flat mode only ever uses
 * slice_id < AR_SUPER_BLOCK_SLOTS, so all existing slices belong to block 0. */
void arPromoteToSuperDir(redisArray *ar) {
    ar->sdir_cap = 4;
    ar->sdir_len = 0;
    ar->superdir = arAllocAndTrack(ar, ar->sdir_cap * sizeof(arSDirEntry));

    /* Copy existing flat dir content into block 0 */
    if (ar->dir_alloc > 0) {
        ar->superdir[0].block_id = 0;
        ar->superdir[0].slots = arCallocAndTrack(ar, AR_SUPER_BLOCK_SLOTS * sizeof(arSlice *));
        ar->superdir[0].count = 0;
        ar->sdir_len = 1;

        /* Copy flat dir pointers to block 0, counting non-NULL */
        for (uint64_t i = 0; i < ar->dir_alloc; i++) {
            ar->superdir[0].slots[i] = ar->dir[i];
            if (ar->dir[i]) ar->superdir[0].count++;
        }
    }

    /* Free old flat directory */
    if (ar->dir) arFreeAndTrack(ar, ar->dir);
    ar->dir = NULL;
    ar->dir_alloc = 0;
}

/* Grow directory to accommodate slice_id (handles both modes, dense and
 * superdir mode). */
void arDirGrow(redisArray *ar, uint64_t slice_id) {
    /* Check if promotion to superdir is needed */
    if (ar->superdir == NULL && slice_id >= AR_SUPER_BLOCK_SLOTS) {
        arPromoteToSuperDir(ar);
    }

    if (ar->superdir) {
        /* Superdir allocates blocks on-demand in arSetSlice(), so we don't
         * allocate a 2048-pointer block for ranges that end up empty. */
        return;
    }

    /* Flat mode: grow directory if needed */
    if (slice_id < ar->dir_alloc) return;

    uint64_t new_alloc = ar->dir_alloc ? ar->dir_alloc : 1;

    /* Grow geometrically and stop at the first power-of-two size
     * that can index slice_id. Note that thanks to superdir mode the
     * size of this table of pointers is bound. */
    while (new_alloc <= slice_id) {
        new_alloc <<= 1;
    }

    arSlice **new_dir = arReallocAndTrack(ar, ar->dir, new_alloc * sizeof(arSlice *));

    /* Zero-fill new slots */
    memset(new_dir + ar->dir_alloc, 0, (new_alloc - ar->dir_alloc) * sizeof(arSlice *));
    ar->dir = new_dir;
    ar->dir_alloc = new_alloc;
}

/* Maybe shrink directory after freeing a slice (flat mode only).
 * Since dir_alloc is always a power of two, we can only shrink by halving.
 * So shrinking only happens when dir_highest_used < dir_alloc/2. The 90%
 * check is just a quick early-out to skip the loop in the common case. */
void arDirMaybeShrink(redisArray *ar) {
    if (ar->superdir) return;  /* Superdir mode: blocks freed individually */
    if (ar->count == 0) return;  /* Will be deleted anyway */
    if (ar->dir_highest_used >= ar->dir_alloc * 9 / 10) return;

    /* Find smallest power of two > dir_highest_used */
    uint64_t new_alloc = 1;
    while (new_alloc <= ar->dir_highest_used) new_alloc <<= 1;
    if (new_alloc >= ar->dir_alloc) return;

    ar->dir = arReallocAndTrack(ar, ar->dir, new_alloc * sizeof(arSlice *));
    ar->dir_alloc = new_alloc;
}

/* Update dir_highest_used after freeing a slice.
 * To always know the highest directory index used is useful
 * for a number of reasons:
 * 1. arLen() is O(1) this way.
 * 2. We can start reverse scans from the rightmost populated directory entry.
 * 3. We can shrink the directory (in flat mode) if needed, since we know
 *    the usage. */
void arDirUpdateHighest(redisArray *ar, uint64_t freed_id) {
    if (ar->count == 0) return;
    if (freed_id < ar->dir_highest_used) return;

    if (ar->superdir) {
        /* Superdir mode: scan backwards through blocks */
        for (int32_t bi = ar->sdir_len - 1; bi >= 0; bi--) {
            arSDirEntry *e = ar->superdir + bi;
            if (e->count == 0) continue;
            /* Scan backwards through this block's slots */
            for (int32_t si = AR_SUPER_BLOCK_SLOTS - 1; si >= 0; si--) {
                if (e->slots[si] != NULL) {
                    ar->dir_highest_used = e->block_id * AR_SUPER_BLOCK_SLOTS + si;
                    return;
                }
            }
        }
        ar->dir_highest_used = 0;
    } else {
        /* Flat mode: scan backward for next non-NULL slice */
        for (int64_t i = (int64_t)freed_id - 1; i >= 0; i--) {
            if (ar->dir[i] != NULL) {
                ar->dir_highest_used = i;
                return;
            }
        }
        ar->dir_highest_used = 0;
    }
}

/* Get slice pointer by slice_id (which is the logical array-index divided by
 * the elements-per-slice), handling both flat and superdir modes. If no slice
 * was already allocated for such slice_id, NULL is returned. */
arSlice *arGetSlice(redisArray *ar, uint64_t slice_id) {
    if (ar->superdir) {
        arSlice **slot = arSuperDirGetSlot(ar, slice_id);
        return slot ? *slot : NULL;
    } else {
        if (slice_id >= ar->dir_alloc) return NULL;
        return ar->dir[slice_id];
    }
}

/* Set slice pointer in the directory. In superdir mode, setting to NULL
 * decrements the block's slice count and frees the block if it becomes empty.
 * Setting to non-NULL allocates the block if needed. */
void arSetSlice(redisArray *ar, uint64_t slice_id, arSlice *s) {
    if (ar->superdir) {
        uint64_t block_id = slice_id / AR_SUPER_BLOCK_SLOTS;
        uint32_t block_off = slice_id % AR_SUPER_BLOCK_SLOTS;

        int found;
        uint32_t pos = arSuperDirFind(ar, block_id, &found);

        if (s == NULL) {
            /* Setting to NULL: decrement block count, maybe remove block */
            if (!found) return;  /* Block doesn't exist, nothing to do */
            arSDirEntry *entry = ar->superdir + pos;
            if (entry->slots[block_off] != NULL) {
                entry->slots[block_off] = NULL;
                entry->count--;
                ar->num_slices--;
                /* Remove empty block */
                if (entry->count == 0) {
                    arSuperDirRemoveBlock(ar, pos);
                }
            }
        } else {
            /* Setting to non-NULL: ensure block exists */
            arSlice **slot = arSuperDirEnsureSlot(ar, slice_id);
            arSDirEntry *entry = arSuperDirGetEntry(ar, slice_id);
            if (*slot == NULL) {
                entry->count++;
                ar->num_slices++;
            }
            *slot = s;
        }
    } else {
        if (s == NULL && ar->dir[slice_id] != NULL) ar->num_slices--;
        else if (s != NULL && ar->dir[slice_id] == NULL) ar->num_slices++;
        ar->dir[slice_id] = s;
    }
}

/* ----------------------------------------------------------------------------
 * Value encoding
 * -------------------------------------------------------------------------- */

/* Try to encode string as immediate integer */
int arTryEncodeInt(const char *s, size_t len, void **out) {
    long long ll;
    if (string2ll(s, len, &ll) && arIntFits(ll)) {
        *out = arFromInt(ll);
        return 1;
    }
    return 0;
}

/* Try to encode string as immediate float.
 *
 * The local immediate float encoding clears the low 2 bits of the underlying
 * floating-point payload to make room for the tag. On 64-bit builds we do it
 * on the IEEE-754 double bits directly. On 32-bit builds we first quantize to
 * float, then clear the low 2 bits of the float payload. We only encode if the
 * later string representation matches the original input exactly.
 *
 * There's a subtlety with whole-number floats: d2string formats 1.0 as "1"
 * (without decimal point), so "1.0" wouldn't match and would be stored as
 * a heap string. We fix this by appending ".0" when d2string produces an
 * integer-looking result and comparing again.
 *
 * Note: pure integers like "1" are handled by arTryEncodeInt first, so values
 * reaching here that look like integers after d2string likely had ".0". */
int arTryEncodeFloat(const char *s, size_t len, void **out) {
    /* Fast filter to discard things that obviously can't pass the later
     * round-trip test:
     *
     *  1. Can have optional leading '-'.
     *  2. Can be composed only by digits plus one mandatory '.'.
     *
     * This skips expensive float parsing for obvious non-candidates. */
    size_t i = 0;
    int dot_seen = 0;

    if (len == 0) return 0;
    if (s[0] == '-') {
        if (len == 1) return 0;
        i = 1;
    }
    for (; i < len; i++) {
        char c = s[i];
        if (c == '.') {
            if (dot_seen) return 0;
            dot_seen = 1;
        } else if (c < '0' || c > '9') {
            return 0;
        }
    }
    if (!dot_seen) return 0;

    /* Expensive round-trip path: convert to double. */
    double d;
    if (!string2d(s, len, &d)) return 0;
    if (isnan(d) || isinf(d)) return 0;

    uint64_t bits_trunc;
    double d_trunc;
#if UINTPTR_MAX == UINT64_MAX
    /* Truncate the double payload directly on 64-bit builds. */
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    bits_trunc = bits & ~AR_TAG_MASK;
    memcpy(&d_trunc, &bits_trunc, sizeof(d_trunc));
#else
    /* 32-bit builds inline floats, not doubles. Quantize first, then clear
     * the low 2 bits of the float payload. */
    float f = (float)d;
    if (!isfinite(f)) return 0; // May happen after casting.
    uint32_t bits32;
    uint32_t bits32_trunc;
    float f_trunc;

    memcpy(&bits32, &f, sizeof(bits32));
    bits32_trunc = bits32 & ~(uint32_t)AR_TAG_MASK;
    memcpy(&f_trunc, &bits32_trunc, sizeof(f_trunc));
    bits_trunc = bits32_trunc;
    d_trunc = (double)f_trunc; // Reduced precision float here.
#endif

    /* Verify round-trip */
    char buf[AR_INLINE_BUFSIZE];
    int buflen = d2string(buf, sizeof(buf) - 2, d_trunc);
    if ((size_t)buflen == len && memcmp(buf, s, len) == 0) {
        *out = arFromFloatBits(bits_trunc);
        return 1;
    }

    /* Also try the ".0" form. d2string(1.0) returns "1", but when floats are
     * later converted back to strings we restore ".0" for integer-looking
     * values, so inputs like "1.0" can still round-trip exactly. */
    buf[buflen] = '.';
    buf[buflen + 1] = '0';
    buf[buflen + 2] = '\0';
    buflen += 2;
    if ((size_t)buflen == len && memcmp(buf, s, len) == 0) {
        *out = arFromFloatBits(bits_trunc);
        return 1;
    }

    return 0;
}

/* Format a float in the canonical string form exposed by arrays.
 * buf must be at least AR_INLINE_BUFSIZE bytes. We use d2string() for the
 * shortest round-trippable representation, then restore ".0" for
 * integer-looking finite values so decoded/replied floats match the logical
 * form expected by array persistence and encoding checks. */
int arFormatFloat(double d, char *buf, size_t bufsize) {
    serverAssert(bufsize >= AR_INLINE_BUFSIZE);
    int len = d2string(buf, bufsize - 2, d);
    if (isfinite(d) && !memchr(buf, '.', len) && !memchr(buf, 'e', len) &&
        !memchr(buf, 'E', len)) {
        serverAssert((size_t)len + 2 < bufsize);
        buf[len++] = '.';
        buf[len++] = '0';
        buf[len] = '\0';
    }
    return len;
}

/* Encode a string into a tagged value */
void *arEncode(const char *s, size_t len) {
    void *v;

    /* Try integer first */
    if (arTryEncodeInt(s, len, &v)) {
        return v;
    }

    /* Try float */
    if (arTryEncodeFloat(s, len, &v)) {
        return v;
    }

    /* Try small string (architecture-dependent inline limit). */
    if (len <= AR_SMALLSTR_MAXLEN) {
        return arFromSmallStr(s, (int)len);
    }

    /* Fall back to arString (8+ bytes) */
    return arStringNew(s, len);
}

void *arValueFromRdbInt(int64_t ival) {
    if (arIntFits(ival)) return arFromInt(ival);

    /* If the integer does not fit (i.e. loading into a 32 bit instance
     * what was stored in the RDB by a 64 bit instance), we promote it
     * to a plain string. */
    char buf[32];
    int len = ll2string(buf, sizeof(buf), ival);
    return arStringNew(buf, len);
}

void *arValueFromRdbFloat(double d) {
#if UINTPTR_MAX == UINT64_MAX
    /* On 64-bit, doubles are inlined directly (low 2 bits cleared).
     * No string round-trip needed: the RDB double already has clean
     * low bits (from the saving side's arToDouble). */
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return arFromFloatBits(bits);
#endif

    /* Loading on a 32 bit system is more complicated to do efficiently.
     *
     * RDB always stores array floats as doubles. On 32-bit systems we can
     * only inline a float payload with the low 2 bits stolen for the tag.
     * Simulate that exact quantization path and keep the value encoded only
     * if it survives unchanged. */
    uint32_t bits32;
    uint32_t bits32_trunc;
    float f_trunc;
    double d_trunc;

    /* Narrow to float first, then clear the low 2 payload bits that are
     * reserved for the tagged-pointer type. */
    float f = (float)d;
    memcpy(&bits32, &f, sizeof(bits32));
    bits32_trunc = bits32 & ~(uint32_t)AR_TAG_MASK;
    memcpy(&f_trunc, &bits32_trunc, sizeof(f_trunc));
    d_trunc = (double)f_trunc;

    /* Bitwise comparison keeps signed zero distinct and tells us whether
     * the 64-bit RDB value is exactly representable by the local 30-bit
     * inline-float format. */
    uint64_t bits64;
    uint64_t bits64_trunc;
    memcpy(&bits64, &d, sizeof(bits64));
    memcpy(&bits64_trunc, &d_trunc, sizeof(bits64_trunc));
    if (bits64 == bits64_trunc) return arFromFloatBits(bits32_trunc);

    /* Otherwise materialize the canonical string form for this float. */
    char buf[AR_INLINE_BUFSIZE];
    int len = arFormatFloat(d, buf, sizeof(buf));
    return arStringNew(buf, len);
}

void *arValueFromRdbSmallStr(const char *s, size_t len) {
    if (len <= AR_SMALLSTR_MAXLEN) return arFromSmallStr(s, (int)len);
    return arStringNew(s, len);
}

/* Decode a tagged value into raw bytes.
 * For inline values, buf must point to at least AR_INLINE_BUFSIZE bytes and
 * the returned pointer will be buf. For arString values, the returned pointer
 * aliases the string payload directly. Returns NULL if value is empty.
 *
 * This is a helper function used for AOF rewriting, AROP string "MATCH"
 * and DEBUG DIGEST. */
const char *arDecode(void *v, char *buf, size_t bufsize, size_t *outlen) {
    serverAssert(bufsize >= AR_INLINE_BUFSIZE);
    if (arIsEmpty(v)) {
        if (outlen) *outlen = 0;
        return NULL;
    }

    if (arIsInt(v)) {
        int64_t ival = arToInt(v);
        int len = ll2string(buf, 32, ival);
        if (outlen) *outlen = len;
        return buf;
    }

    if (arIsFloat(v)) {
        double d = arToDouble(v);
        int len = arFormatFloat(d, buf, bufsize);
        if (outlen) *outlen = len;
        return buf;
    }

    if (arIsSmallStr(v)) {
        int len = arSmallStrLen(v);
        if (outlen) *outlen = len;
        arToSmallStr(v, buf);
        return buf;
    }

    /* arString pointer */
    size_t len = arStringLen(v);
    if (outlen) *outlen = len;
    return arStringData(v);
}


/* ----------------------------------------------------------------------------
 * Array lifecycle
 * -------------------------------------------------------------------------- */

/* Create a new empty array */
redisArray *arNew(void) {
    redisArray *ar = zmalloc(sizeof(redisArray));
    ar->count = 0;
    ar->insert_idx = AR_INSERT_IDX_NONE;
    ar->dir_alloc = 0;
    ar->dir_highest_used = 0;
    ar->num_slices = 0;
    ar->alloc_size = zmalloc_size(ar);
    ar->slice_size = ArraySliceSize;  /* Use current config value */
    ar->sdir_len = 0;
    ar->sdir_cap = 0;
    ar->dir = NULL;
    ar->superdir = NULL;
    return ar;
}

/* Free an array and all its contents */
void arFree(redisArray *ar) {
    if (!ar) return;

    if (ar->superdir) {
        /* Superdir mode: free all blocks and their slices */
        for (uint32_t i = 0; i < ar->sdir_len; i++) {
            arSDirEntry *e = ar->superdir + i;
            for (uint32_t j = 0; j < AR_SUPER_BLOCK_SLOTS; j++) {
                if (e->slots[j]) arSliceFree(NULL, e->slots[j]);
            }
            zfree(e->slots);
        }
        zfree(ar->superdir);
    } else {
        /* Flat mode */
        for (uint64_t i = 0; i < ar->dir_alloc; i++) {
            if (ar->dir[i]) {
                arSliceFree(NULL, ar->dir[i]);
            }
        }
        zfree(ar->dir);
    }
    zfree(ar);
}

/* Dismiss a single slice's memory back to the OS. */
static void arSliceDismiss(arSlice *s, int dismiss_values) {
    if (s->encoding == AR_SLICE_DENSE) {
        if (dismiss_values) {
            void **items = s->layout.dense.items;
            for (uint32_t i = 0; i < s->layout.dense.winsize; i++) {
                if (arIsPtr(items[i]))
                    dismissMemory(items[i], arStringLen(items[i]));
            }
        }
        dismissMemory(s, arDenseAllocSize(s->layout.dense.winsize));
    } else {
        if (dismiss_values) {
            void **values = s->layout.sparse.values;
            for (uint32_t i = 0; i < s->count; i++) {
                if (arIsPtr(values[i]))
                    dismissMemory(values[i], arStringLen(values[i]));
            }
        }
        dismissMemory(s, arSparseAllocSize(s->layout.sparse.cap));
    }
}

/* See dismissObject(). Always dismiss the directory and slices; per-value
 * dismissal only when the average element size makes it worthwhile. */
void arDismiss(redisArray *ar, size_t size_hint) {
    if (!ar) return;
    uint64_t count = ar->count;
    int dismiss_values = (count != 0 && size_hint / count >= server.page_size);

    if (ar->superdir) {
        for (uint32_t bi = 0; bi < ar->sdir_len; bi++) {
            arSDirEntry *e = ar->superdir + bi;
            for (uint32_t si = 0; si < AR_SUPER_BLOCK_SLOTS; si++) {
                if (e->slots[si] == NULL) continue;
                arSliceDismiss(e->slots[si], dismiss_values);
            }
            dismissMemory(e->slots, AR_SUPER_BLOCK_SLOTS * sizeof(arSlice *));
        }
        dismissMemory(ar->superdir, ar->sdir_cap * sizeof(arSDirEntry));
    } else if (ar->dir) {
        for (uint64_t i = 0; i < ar->dir_alloc; i++) {
            if (ar->dir[i] == NULL) continue;
            arSliceDismiss(ar->dir[i], dismiss_values);
        }
        dismissMemory(ar->dir, ar->dir_alloc * sizeof(arSlice *));
    }
}

/* arDup() helper to duplicate a single slice into the duplicated array.
 * This function is responsible of tracking allocations in dup_ar
 * (hence the name of the parameter), since it has the knowledge of
 * the array slice that it is duplicating.
 *
 * The dear reader of this code may wonder why we don't just duplicate the
 * array and its slices without tracking memory, and then copy the memory
 * field of the array at the end. The problem is that the array does not
 * track the logical allocated memory, but the actual memory usage reported
 * by the allocator: there is no guarantee that the allocations of the copy
 * perfectly match the ones of the original array. */
arSlice *arSliceDup(redisArray *dup_ar, arSlice *s) {
    if (s->encoding == AR_SLICE_DENSE) {
        size_t sz = arDenseAllocSize(s->layout.dense.winsize);
        arSlice *nd = arAllocAndTrack(dup_ar, sz);
        memcpy(nd, s, sizeof(arSlice));
        nd->layout.dense.items = (void **)(nd + 1);
        memcpy(nd->layout.dense.items, s->layout.dense.items,
               s->layout.dense.winsize * sizeof(void *));

        /* Duplicate arString pointers */
        for (uint32_t j = 0; j < s->layout.dense.winsize; j++) {
            if (arIsPtr(nd->layout.dense.items[j])) {
                nd->layout.dense.items[j] = arStringDup(nd->layout.dense.items[j]);
                arTrackValueIn(dup_ar, nd->layout.dense.items[j]);
            }
        }
        return nd;
    } else {
        size_t sz = arSparseAllocSize(s->layout.sparse.cap);
        arSlice *nsp = arAllocAndTrack(dup_ar, sz);
        memcpy(nsp, s, sizeof(arSlice));
        arSparseSetupPointers(nsp);
        memcpy(nsp->layout.sparse.offsets, s->layout.sparse.offsets,
               s->layout.sparse.cap * sizeof(uint16_t));
        memcpy(nsp->layout.sparse.values, s->layout.sparse.values,
               s->layout.sparse.cap * sizeof(void *));

        /* Duplicate arString pointers */
        void **values = nsp->layout.sparse.values;
        for (uint32_t j = 0; j < s->count; j++) {
            if (arIsPtr(values[j])) {
                values[j] = arStringDup(values[j]);
                arTrackValueIn(dup_ar, values[j]);
            }
        }
        return nsp;
    }
}

/* Duplicate an array (deep copy) */
redisArray *arDup(redisArray *ar) {
    redisArray *dup = zmalloc(sizeof(redisArray));
    dup->count = ar->count;
    dup->insert_idx = ar->insert_idx;
    dup->dir_alloc = ar->dir_alloc;
    dup->dir_highest_used = ar->dir_highest_used;
    dup->num_slices = ar->num_slices;
    dup->alloc_size = zmalloc_size(dup);
    dup->slice_size = ar->slice_size;
    dup->sdir_len = ar->sdir_len;
    dup->sdir_cap = ar->sdir_cap;

    if (ar->superdir) {
        /* Superdir mode */
        dup->dir = NULL;
        dup->superdir = arAllocAndTrack(dup, ar->sdir_cap * sizeof(arSDirEntry));

        for (uint32_t i = 0; i < ar->sdir_len; i++) {
            arSDirEntry *src = ar->superdir + i;
            arSDirEntry *dst = dup->superdir + i;

            dst->block_id = src->block_id;
            dst->count = src->count;
            dst->slots = arCallocAndTrack(dup, AR_SUPER_BLOCK_SLOTS * sizeof(arSlice *));

            for (uint32_t j = 0; j < AR_SUPER_BLOCK_SLOTS; j++) {
                if (src->slots[j]) {
                    dst->slots[j] = arSliceDup(dup, src->slots[j]);
                }
            }
        }
    } else if (ar->dir_alloc > 0) {
        /* Flat mode */
        dup->superdir = NULL;
        dup->dir = arAllocAndTrack(dup, ar->dir_alloc * sizeof(arSlice *));
        memset(dup->dir, 0, ar->dir_alloc * sizeof(arSlice *));

        for (uint64_t i = 0; i < ar->dir_alloc; i++) {
            if (ar->dir[i]) {
                dup->dir[i] = arSliceDup(dup, ar->dir[i]);
            }
        }
    } else {
        dup->dir = NULL;
        dup->superdir = NULL;
    }

    return dup;
}

/* ----------------------------------------------------------------------------
 * Core operations
 * -------------------------------------------------------------------------- */

/* Get value at index (returns NULL for empty/missing) */
void *arGet(redisArray *ar, uint64_t idx) {
    uint64_t slice_id = arSliceId(idx, ar->slice_size);
    uint32_t rel_idx = arSliceOff(idx, ar->slice_size);

    arSlice *s = arGetSlice(ar, slice_id);
    if (s == NULL) return NULL; // No slice at all for this index.

    if (s->encoding == AR_SLICE_DENSE) {
        if (rel_idx < s->layout.dense.offset ||
            rel_idx >= s->layout.dense.offset + s->layout.dense.winsize)
        {
            // The slice window does not include this index.
            return NULL;
        }
        return s->layout.dense.items[rel_idx - s->layout.dense.offset];
    } else {
        int found;
        uint32_t pos = arSparseFindPos(s, (uint16_t)rel_idx, &found);
        if (found) {
            void **values = s->layout.sparse.values;
            return values[pos];
        }
        return NULL;
    }
}

/* Set value at index. Caller must ensure idx != UINT64_MAX.
 * v must not be NULL (empty) - use arDel() to delete elements. */
void arSet(redisArray *ar, uint64_t idx, void *v) {
    serverAssert(v != NULL);  /* Use arDel for deletion, not arSet(v=NULL) */
    /* UINT64_MAX can't be used for a couple of reasons: for once,
     * the array len is the max index set + 1, so we could not represent
     * that; also it is a sentinel for last set index still not being set. */
    serverAssert(idx != UINT64_MAX);
    uint64_t slice_id = arSliceId(idx, ar->slice_size);
    uint32_t rel_idx = arSliceOff(idx, ar->slice_size);

    /* Ensure directory capacity (may trigger promotion to superdir) */
    arDirGrow(ar, slice_id);

    /* Get current slice */
    arSlice *s = arGetSlice(ar, slice_id);

    /* Create slice if missing */
    if (s == NULL) {
        if (ArraySparseKMax > 0) {
            s = arSliceSparseNew(ar);
        } else {
            s = arSliceDenseNew(ar, rel_idx, ar->slice_size);
        }
        arSetSlice(ar, slice_id, s);
    }

    if (s->encoding == AR_SLICE_DENSE) {
        /* Grow the slice window if needed */
        s = arSliceDenseGrowIfNeeded(ar, s, rel_idx, ar->slice_size);
        arSetSlice(ar, slice_id, s); // In case it changed.

        uint32_t pos = rel_idx - s->layout.dense.offset;
        void *old = s->layout.dense.items[pos];

        if (arIsEmpty(old)) {
            s->count++;
            ar->count++;
        } else {
            /* Replace existing value. */
            arTrackValueOut(ar, old);
            arFreePtr(old);
        }

        arTrackValueIn(ar, v);
        s->layout.dense.items[pos] = v;

        /* Update max_idx */
        if (rel_idx > s->layout.dense.max_idx) {
            s->layout.dense.max_idx = rel_idx;
        }
    } else {
        int found;
        uint32_t pos = arSparseFindPos(s, (uint16_t)rel_idx, &found);
        uint16_t *offsets = s->layout.sparse.offsets;
        void **values = s->layout.sparse.values;

        if (found) {
            /* Replace existing */
            arTrackValueOut(ar, values[pos]);
            arFreePtr(values[pos]);
            arTrackValueIn(ar, v);
            values[pos] = v;
        } else {
            /* Insert new */
            if (s->count >= ArraySparseKMax) {
                /* Promote to dense */
                arSlice *d = arSparsePromote(ar, s, ar->slice_size);

                /* Grow window if needed */
                d = arSliceDenseGrowIfNeeded(ar, d, rel_idx, ar->slice_size);
                arSetSlice(ar, slice_id, d);

                uint32_t dpos = rel_idx - d->layout.dense.offset;
                arTrackValueIn(ar, v);
                d->layout.dense.items[dpos] = v;
                d->count++;
                ar->count++;
                if (rel_idx > d->layout.dense.max_idx) d->layout.dense.max_idx = rel_idx;
            } else {
                /* Insert in sparse */
                if (s->count >= s->layout.sparse.cap) {
                    /* Grow capacity, we grow 2x but note that there is no
                     * point in growing more than kmax, so we clamp to kmax. */
                    uint32_t new_cap = s->layout.sparse.cap * 2;
                    if (new_cap > ArraySparseKMax) new_cap = ArraySparseKMax;
                    arSlice *ns = arAllocAndTrack(ar, arSparseAllocSize(new_cap));
                    ns->encoding = AR_SLICE_SPARSE;
                    ns->count = s->count;
                    ns->layout.sparse.cap = new_cap;
                    arSparseSetupPointers(ns);

                    /* Copy old data to new slice */
                    uint16_t *old_offsets = s->layout.sparse.offsets;
                    void **old_values = s->layout.sparse.values;
                    uint16_t *new_offsets = ns->layout.sparse.offsets;
                    void **new_values = ns->layout.sparse.values;
                    memcpy(new_offsets,old_offsets,s->count * sizeof(uint16_t));
                    memcpy(new_values,old_values,s->count * sizeof(void *));

                    arFreeAndTrack(ar, s);
                    s = ns;
                    arSetSlice(ar, slice_id, s);
                    offsets = new_offsets;
                    values = new_values;
                }

                /* Shift and insert in place */
                memmove(offsets + pos + 1, offsets + pos,
                        (s->count - pos) * sizeof(uint16_t));
                memmove(values + pos + 1, values + pos,
                        (s->count - pos) * sizeof(void *));
                offsets[pos] = (uint16_t)rel_idx;
                arTrackValueIn(ar, v);
                values[pos] = v;
                s->count++;
                ar->count++;
            }
        }
    }

    /* Update dir_highest_used. The count==1 check handles when we just added
     * the first element to an empty array. */
    if (slice_id > ar->dir_highest_used || ar->count == 1) {
        ar->dir_highest_used = slice_id;
    }
}

/* Delete value at index. Returns 1 if deleted, 0 if was already empty. */
int arDel(redisArray *ar, uint64_t idx) {
    uint64_t slice_id = arSliceId(idx, ar->slice_size);
    uint32_t rel_idx = arSliceOff(idx, ar->slice_size);

    arSlice *s = arGetSlice(ar, slice_id);
    if (s == NULL) return 0;

    if (s->encoding == AR_SLICE_DENSE) {
        if (rel_idx < s->layout.dense.offset || rel_idx >= s->layout.dense.offset + s->layout.dense.winsize) {
            return 0;
        }

        uint32_t pos = rel_idx - s->layout.dense.offset;
        void *old = s->layout.dense.items[pos];
        if (arIsEmpty(old)) return 0;

        arTrackValueOut(ar, old);
        arFreePtr(old);
        s->layout.dense.items[pos] = NULL;
        s->count--;
        ar->count--;

        /* Update max_idx if we deleted the max */
        arDenseUpdateMaxIdx(s, rel_idx);
        if (s->count != 0) {
            /* Maybe demote to sparse. */
            arSetSlice(ar, slice_id, arDenseMaybeDemote(ar, s));
            return 1;
        }
    } else {
        int found;
        uint32_t pos = arSparseFindPos(s, (uint16_t)rel_idx, &found);
        if (!found) return 0;

        uint16_t *offsets = s->layout.sparse.offsets;
        void **values = s->layout.sparse.values;

        arTrackValueOut(ar, values[pos]);
        arFreePtr(values[pos]);
        memmove(offsets + pos, offsets + pos + 1,
                (s->count - pos - 1) * sizeof(uint16_t));
        memmove(values + pos, values + pos + 1,
                (s->count - pos - 1) * sizeof(void *));
        s->count--;
        ar->count--;
    }

    /* Delete the slice if now it is empty. */
    if (s->count == 0) {
        arSliceFree(ar, s);
        /* Note that in superdir mode arSetSlice() will also free
         * empty blocks. */
        arSetSlice(ar, slice_id, NULL);
        arDirUpdateHighest(ar, slice_id);
        arDirMaybeShrink(ar);
    }
    return 1;
}

/* ============================================================================
 * GENERALIZED RANGE DELETE - arDeleteRange
 * ============================================================================
 *
 * This function provides O(N) range deletion where N is the number of stored
 * elements, NOT the numeric range length. It achieves this by:
 *
 * 1. Deleting whole fully-covered slices in the middle range.
 * 2. In superdir mode, visiting only overlapping blocks and covered slice
 *    slots within them, instead of scanning the numeric slice-id span.
 * 3. Only doing element-level deletion inside the two boundary slices.
 *
 * This is used by ARDELRANGE directly and by arTruncate as a special case.
 * -------------------------------------------------------------------------- */

/* Helper: delete elements within a single slice in offset range
 * [del_lo..del_hi]. Returns number of elements deleted. Handles both dense
 * and sparse slices.
 *
 * Dense slices delete slot-by-slot inside the window. Sparse slices identify
 * the contiguous offset span to delete, free those values, then compact the
 * tail once.
 *
 * If the slice becomes empty, it is freed and the slot is cleared. */
uint64_t arDeleteSliceRange(redisArray *ar, uint64_t slice_id,
                                   uint32_t del_lo, uint32_t del_hi) {
    arSlice *s = arGetSlice(ar, slice_id);
    if (!s) return 0;

    uint64_t deleted = 0;

    if (s->encoding == AR_SLICE_DENSE) {
        /* Dense: intersect deletion range with allocated window */
        uint32_t win_lo = s->layout.dense.offset;
        uint32_t win_hi = s->layout.dense.offset + s->layout.dense.winsize - 1;

        /* Clamp to window */
        uint32_t eff_lo = (del_lo > win_lo) ? del_lo : win_lo;
        uint32_t eff_hi = (del_hi < win_hi) ? del_hi : win_hi;

        if (eff_lo <= eff_hi) {
            /* Clear every populated slot in the effective dense range. */
            for (uint32_t off = eff_lo; off <= eff_hi; off++) {
                uint32_t pos = off - s->layout.dense.offset;
                if (!arIsEmpty(s->layout.dense.items[pos])) {
                    arTrackValueOut(ar, s->layout.dense.items[pos]);
                    arFreePtr(s->layout.dense.items[pos]);
                    s->layout.dense.items[pos] = NULL;
                    s->count--;
                    ar->count--;
                    deleted++;
                }
            }

            /* Update max_idx if affected */
            if (s->count > 0 && s->layout.dense.max_idx >= eff_lo) {
                /* Scan backwards to find new max */
                s->layout.dense.max_idx = s->layout.dense.offset;
                for (int32_t i = (int32_t)win_hi; i >= (int32_t)win_lo; i--) {
                    if (!arIsEmpty(s->layout.dense.items[i - s->layout.dense.offset])) {
                        s->layout.dense.max_idx = i;
                        break;
                    }
                }
            }
        }

        /* Delete slice if empty, or demote it to sparse if we are
         * below the threshold. */
        if (s->count == 0) {
            arSliceFree(ar, s);
            arSetSlice(ar, slice_id, NULL);
        } else {
            arSetSlice(ar, slice_id, arDenseMaybeDemote(ar, s));
        }
    } else {
        /* Sparse: deleted elements form a contiguous span in the sorted
         * offsets/values arrays. Find that span, free the values in it,
         * then compact the tail once. */
        uint16_t *offsets = s->layout.sparse.offsets;
        void **values = s->layout.sparse.values;

        int found;
        uint32_t first = arSparseFindPos(s, (uint16_t)del_lo, &found);
        uint32_t last = arSparseFindPos(s, (uint16_t)del_hi, &found);
        if (found) last++;

        /* Free all values in the contiguous sparse span to delete. */
        for (uint32_t i = first; i < last; i++) {
            arTrackValueOut(ar, values[i]);
            arFreePtr(values[i]);
        }

        /* Shift the surviving tail left to close the deleted gap. */
        if (first < last) {
            uint32_t tail = s->count - last;
            if (tail > 0) {
                memmove(offsets + first, offsets + last,
                        tail * sizeof(uint16_t));
                memmove(values + first, values + last,
                        tail * sizeof(void *));
            }

            deleted = last - first;
            s->count -= deleted;
            ar->count -= deleted;
        }

        if (s->count == 0) {
            arSliceFree(ar, s);
            arSetSlice(ar, slice_id, NULL);
        }
    }

    return deleted;
}

/* Main range delete function: delete all elements in [lo..hi].
 * Returns number of elements deleted.
 *
 * Algorithm:
 * 1. Compute slice boundaries
 * 2. Handle boundary slices with element-level deletion
 * 3. Delete full slices/blocks in between (O(1) per slice)
 * 4. Update metadata (dir_highest_used, shrink directories)
 *
 * Complexity: O(S + N) where S = slices touched, N = boundary elements.
 * Note that just looping with arGetSlice() and removing the in-the-middle
 * slices one after the other would be much simpler but would have completely
 * different complexity properties, in case of big span of empty indexes. */
uint64_t arDeleteRange(redisArray *ar, uint64_t lo, uint64_t hi) {
    if (ar->count == 0 || lo > hi) return 0;

    uint32_t slice_size = ar->slice_size;
    uint64_t lo_slice = arSliceId(lo, slice_size);
    uint64_t hi_slice = arSliceId(hi, slice_size);
    uint32_t lo_off = arSliceOff(lo, slice_size);
    uint32_t hi_off = arSliceOff(hi, slice_size);

    uint64_t deleted = 0;
    int touched_highest = 0;  /* Did we touch dir_highest_used? */

    if (lo_slice == hi_slice) {
        /* Range is within a single slice: element-level delete only */
        deleted = arDeleteSliceRange(ar, lo_slice, lo_off, hi_off);
        if (lo_slice >= ar->dir_highest_used) touched_highest = 1;
    } else {
        /* Multiple slices: handle boundaries and full slices separately */

        /* 1. Delete within lo_slice: [lo_off .. slice_size-1] */
        deleted += arDeleteSliceRange(ar, lo_slice, lo_off, slice_size - 1);

        /* 2. Delete within hi_slice: [0 .. hi_off] */
        deleted += arDeleteSliceRange(ar, hi_slice, 0, hi_off);
        if (hi_slice >= ar->dir_highest_used) touched_highest = 1;

        /* 3. Delete full slices in between [lo_slice+1 .. hi_slice-1] */
        if (lo_slice + 1 <= hi_slice - 1) {
            if (ar->superdir) {
                /* Superdir mode: identify only the block entries that can
                 * contain slices in the middle range, then delete the covered
                 * slot interval inside each of those blocks. Iterate from high
                 * to low so removing an empty block does not invalidate the
                 * yet-to-be-visited entries. */
                uint64_t mid_lo = lo_slice + 1;
                uint64_t mid_hi = hi_slice - 1;
                uint64_t lo_block = mid_lo / AR_SUPER_BLOCK_SLOTS;
                uint64_t hi_block = mid_hi / AR_SUPER_BLOCK_SLOTS;

                /* arSuperDirFind() is a lower-bound search on block_id.
                 * start is the first entry whose block_id is >= lo_block.
                 * end is the first entry whose block_id is > hi_block, so the
                 * blocks to visit are exactly [start, end). */
                int found;
                uint32_t start = arSuperDirFind(ar, lo_block, &found);
                uint32_t end = arSuperDirFind(ar, hi_block, &found);
                if (found) end++; /* Convert matching index to past-the-end. */

                /* Iterate backward because deleting the last slice in a block
                 * removes that block entry and compacts the superdir array. */
                for (int32_t bi = (int32_t)end - 1; bi >= (int32_t)start; bi--) {
                    arSDirEntry *e = ar->superdir + bi;
                    uint64_t block_base = e->block_id * AR_SUPER_BLOCK_SLOTS;
                    uint64_t block_end = block_base + AR_SUPER_BLOCK_SLOTS - 1;

                    /* Convert the global middle slice range to the local slot
                     * interval covered inside this block. */
                    uint32_t first_si = (mid_lo > block_base) ?
                        (uint32_t)(mid_lo - block_base) : 0;
                    uint32_t last_si = (mid_hi < block_end) ?
                        (uint32_t)(mid_hi - block_base) : AR_SUPER_BLOCK_SLOTS - 1;

                    /* Delete each covered slice slot. The block itself, if it
                     * becomes empty, is removed after this local scan. */
                    for (uint32_t si = first_si; si <= last_si; si++) {
                        if (e->slots[si]) {
                            uint64_t slice_id = block_base + si;
                            deleted += e->slots[si]->count;
                            ar->count -= e->slots[si]->count;
                            arSliceFree(ar, e->slots[si]);
                            e->slots[si] = NULL;
                            e->count--;
                            ar->num_slices--;
                            if (slice_id >= ar->dir_highest_used)
                                touched_highest = 1;
                        }
                    }

                    /* Remove the superdir block if empty. */
                    if (e->count == 0) {
                        arSuperDirRemoveBlock(ar, bi);
                    }
                }
            } else {
                /* Flat mode: delete full slices in middle range */
                uint64_t end = hi_slice - 1;
                if (end >= ar->dir_alloc) end = ar->dir_alloc - 1;

                for (uint64_t sid = lo_slice + 1; sid <= end; sid++) {
                    if (ar->dir[sid]) {
                        deleted += ar->dir[sid]->count;
                        ar->count -= ar->dir[sid]->count;
                        arSliceFree(ar, ar->dir[sid]);
                        ar->dir[sid] = NULL;
                        ar->num_slices--;
                        if (sid >= ar->dir_highest_used) touched_highest = 1;
                    }
                }
            }
        }
    }

    /* Update dir_highest_used if we touched or deleted high slices */
    if (touched_highest && ar->count > 0) {
        ar->dir_highest_used = 0;
        if (ar->superdir) {
            for (int32_t bi = ar->sdir_len - 1; bi >= 0; bi--) {
                arSDirEntry *e = ar->superdir + bi;
                if (e->count == 0) continue;
                for (int32_t si = AR_SUPER_BLOCK_SLOTS - 1; si >= 0; si--) {
                    if (e->slots[si]) {
                        ar->dir_highest_used = e->block_id * AR_SUPER_BLOCK_SLOTS + si;
                        goto found_highest;
                    }
                }
            }
        } else {
            for (int64_t i = (int64_t)ar->dir_alloc - 1; i >= 0; i--) {
                if (ar->dir[i]) {
                    ar->dir_highest_used = i;
                    goto found_highest;
                }
            }
        }
    }
found_highest:

    if (ar->count == 0) {
        ar->dir_highest_used = 0;
    }

    arDirMaybeShrink(ar);
    return deleted;
}

/* Truncate array: delete all elements with index >= limit.
 * Used by ARRING to implement ring buffer wrap-around.
 *
 * This is implemented as a special case of arDeleteRange. limit==0 means
 * "delete everything". */
void arTruncate(redisArray *ar, uint64_t limit) {
    if (ar->count == 0) return;

    uint64_t len = arLen(ar);
    if (limit >= len) return;  /* Nothing to delete */

    arDeleteRange(ar, limit, len - 1);
}

/* ----------------------------------------------------------------------------
 * Properties
 * -------------------------------------------------------------------------- */

/* Get count of non-empty elements */
uint64_t arCount(redisArray *ar) {
    return ar->count;
}

/* Get logical length (max index + 1) */
uint64_t arLen(redisArray *ar) {
    if (ar->count == 0) return 0;

    arSlice *s = arGetSlice(ar, ar->dir_highest_used);
    if (s == NULL) return 0;  /* Defensive: if count>0 but slice missing, corrupted state */
    uint32_t local_max = arSliceMaxIdx(s);
    return arMakeIdx(ar->dir_highest_used, local_max, ar->slice_size) + 1;
}

/* ----------------------------------------------------------------------------
 * Range set optimization
 * -------------------------------------------------------------------------- */

/* Pre-promote sparse slices to dense if a range set would overflow them.
 *
 * When ARSET writes many elements to a sparse slice, each insertion
 * requires a sorted insert with memmove. If the slice eventually exceeds
 * kmax elements, it gets promoted to dense anyway - wasting all that work.
 *
 * This function checks each slice touched by [lo, hi] and promotes it to
 * dense upfront if the final element count would exceed kmax. Slices that
 * will stay within kmax remain sparse. This way, bulk writes either go
 * into sparse (if small) or dense (if large), but never do expensive
 * sparse insertions followed by promotion. */
void arMayPromoteToDenseForRangeSet(redisArray *ar, uint64_t lo, uint64_t hi) {
    if (ArraySparseKMax == 0) return;  /* Sparse disabled, nothing to do */

    uint64_t slice_lo = arSliceId(lo, ar->slice_size);
    uint64_t slice_hi = arSliceId(hi, ar->slice_size);

    /* Ensure directory can hold all slices we might touch */
    arDirGrow(ar, slice_hi);

    for (uint64_t sid = slice_lo; sid <= slice_hi; sid++) {
        /* Compute offset range within this slice */
        uint64_t range_start = (sid == slice_lo) ? lo : (sid << arSliceBits(ar->slice_size));
        uint64_t range_end = (sid == slice_hi) ? hi : ((sid + 1) << arSliceBits(ar->slice_size)) - 1;
        uint32_t start_off = arSliceOff(range_start, ar->slice_size);
        uint32_t end_off = arSliceOff(range_end, ar->slice_size);
        uint32_t range_size = end_off - start_off + 1;

        arSlice *s = arGetSlice(ar, sid);

        if (s == NULL) {
            /* No slice yet - create dense directly if range exceeds kmax */
            if (range_size > ArraySparseKMax) {
                arSetSlice(ar, sid, arSliceDenseNew(ar, start_off, ar->slice_size));
            }
            continue;
        }

        if (s->encoding == AR_SLICE_DENSE) continue;  /* Already dense */

        /* Sparse slice - check if we need to promote */
        if (range_size > ArraySparseKMax) {
            /* Range alone exceeds kmax, must promote */
            arSetSlice(ar, sid, arSparsePromote(ar, s, ar->slice_size));
            continue;
        }

        /* Count existing elements in [start_off, end_off] via linear scan.
         * Sparse slices have at most kmax elements, so this is O(kmax). */
        uint16_t *offsets = s->layout.sparse.offsets;
        uint32_t existing = 0;
        for (uint32_t i = 0; i < s->count; i++) {
            if (offsets[i] >= start_off && offsets[i] <= end_off) {
                existing++;
            }
        }

        /* New elements = range_size - existing (slots we'll fill that are empty) */
        uint32_t new_elements = range_size - existing;
        if (s->count + new_elements > ArraySparseKMax) {
            arSetSlice(ar, sid, arSparsePromote(ar, s, ar->slice_size));
        }
    }
}

/* ----------------------------------------------------------------------------
 * Defragmentation
 * -------------------------------------------------------------------------- */

/* Defrag one slice, fix the slice pointers that point inside its allocation
 * and defrag the heap strings as well.
 *
 * If work is not NULL, also account for the slice scan performed here:
 * dense slices add winsize, while sparse slices add count. We update the
 * active defrag scanned statistic at the same time, so callers do not need
 * to duplicate that logic. */
static arSlice *arDefragSlice(arSlice *s, unsigned long *work,
                              void *(*defragfn)(void *)) {
    /* 1. Try to defrag the slice itself. If the pointer changed,
     *    we need to also change the structure pointers pointing inside
     *    the allocation (that now has a different base address). */
    arSlice *new_s = defragfn(s);
    if (new_s) {
        s = new_s;
        if (s->encoding == AR_SLICE_DENSE)
            s->layout.dense.items = (void **)(s + 1);
        else
            arSparseSetupPointers(s);
    }

    /* Defrag the arString() items. All the other items are
     * encoded in the pointer value itself and need no handling. */
    if (s->encoding == AR_SLICE_DENSE) {
        for (uint32_t j = 0; j < s->layout.dense.winsize; j++) {
            if (!arIsPtr(s->layout.dense.items[j])) continue;
            void *new_ptr = defragfn(s->layout.dense.items[j]);
            if (new_ptr) s->layout.dense.items[j] = new_ptr;
        }
        if (work) {
            *work += s->layout.dense.winsize;
            server.stat_active_defrag_scanned += s->layout.dense.winsize;
        }
    } else {
        void **values = s->layout.sparse.values;
        for (uint32_t j = 0; j < s->count; j++) {
            if (!arIsPtr(values[j])) continue;
            void *new_ptr = defragfn(values[j]);
            if (new_ptr) values[j] = new_ptr;
        }
        if (work) {
            *work += s->count;
            server.stat_active_defrag_scanned += s->count;
        }
    }
    return s;
}

/* Defrag the array header and the top-level directory object that points to
 * slices. This is the cheap metadata pass done before we walk the slices
 * themselves. */
static redisArray *arDefragTopLevel(redisArray *ar, void *(*defragfn)(void *)) {
    redisArray *new_ar = defragfn(ar);
    if (new_ar) ar = new_ar;

    if (ar->superdir) {
        arSDirEntry *new_sdir = defragfn(ar->superdir);
        if (new_sdir) ar->superdir = new_sdir;
    } else if (ar->dir) {
        arSlice **new_dir = defragfn(ar->dir);
        if (new_dir) ar->dir = new_dir;
    }
    return ar;
}

/* Encode the next superdir scan position as a single cursor.
 * Cursor 0 means "start from the beginning" and also "finished".
 *
 * On 64-bit builds we encode block_id and slot, so resume is stable even if
 * blocks before the current one are inserted or removed between defrag steps.
 *
 * On 32-bit builds the generic defrag cursor type is only unsigned long, so
 * it cannot always hold a full 64-bit block_id. In that case we fall back to
 * the positional (block-index, slot) encoding. */
static inline unsigned long arDefragSuperdirCursor(redisArray *ar, uint32_t bi, uint32_t si) {
    serverAssert(si < AR_SUPER_BLOCK_SLOTS);
#if ULONG_MAX >= UINT64_MAX
    uint64_t block_id = ar->superdir[bi].block_id;
    serverAssert(block_id <= (ULONG_MAX - 1) / AR_SUPER_BLOCK_SLOTS);
    return ((unsigned long)block_id * AR_SUPER_BLOCK_SLOTS + si) + 1;
#else
    UNUSED(ar);
    return ((unsigned long)bi * AR_SUPER_BLOCK_SLOTS + si) + 1;
#endif
}

/* Decode the next superdir scan position stored in the incremental defrag
 * cursor. */
static void arDefragDecodeSuperdirCursor(redisArray *ar, unsigned long cursor,
                                         uint32_t *bi, uint32_t *si) {
    serverAssert(cursor > 0);
    unsigned long pos = cursor - 1;
#if ULONG_MAX >= UINT64_MAX
    /* Flat-mode cursors are also encoded as "slot + 1". After promotion to
     * superdir, those old cursors still decode correctly here as block_id 0
     * with the same slot index, because flat mode only ever covers block 0
     * and arPromoteToSuperDir() copies the flat directory into block 0. */
    uint64_t block_id = pos / AR_SUPER_BLOCK_SLOTS;
    int found;

    *si = pos % AR_SUPER_BLOCK_SLOTS;
    *bi = arSuperDirFind(ar, block_id, &found);
    if (!found) *si = 0;
#else
    UNUSED(ar);
    *bi = pos / AR_SUPER_BLOCK_SLOTS;
    *si = pos % AR_SUPER_BLOCK_SLOTS;
#endif
}

/* Defrag an array that is small enough that we can handle it
 * in a single pass. */
redisArray *arDefrag(redisArray *ar, void *(*defragfn)(void *)) {
    ar = arDefragTopLevel(ar, defragfn);

    if (ar->superdir) {
        /* Defrag each block slots array, then each slice referenced by it. */
        for (uint32_t bi = 0; bi < ar->sdir_len; bi++) {
            arSDirEntry *e = ar->superdir + bi;
            arSlice **new_slots = defragfn(e->slots);
            if (new_slots) e->slots = new_slots;

            for (uint32_t si = 0; si < AR_SUPER_BLOCK_SLOTS; si++) {
                if (e->slots[si] == NULL) continue;
                e->slots[si] = arDefragSlice(e->slots[si], NULL, defragfn);
            }
        }
    } else if (ar->dir) {
        /* Defrag each slice referenced by the flat directory. */
        for (uint64_t i = 0; i < ar->dir_alloc; i++) {
            if (ar->dir[i] == NULL) continue;
            ar->dir[i] = arDefragSlice(ar->dir[i], NULL, defragfn);
        }
    }

    return ar;
}

/* Incremental defrag step for arrays. Cursor 0 means "start from the
 * beginning" and also "no more work".
 *
 * Work is counted explicitly in order to keep one call roughly aligned with
 * active_defrag_max_scan_fields:
 *
 * 1. Visiting one flat directory entry costs 1.
 * 2. In superdir mode, visiting one top-level block entry costs 1, and
 *    visiting one slot inside that block costs another 1.
 * 3. Defragmenting a slice then adds the cost of scanning that slice:
 *    sparse slices add s->count, while dense slices add winsize.
 *
 * Slices are still defragmented as whole units. So a dense slice may cause one
 * call to overshoot the configured budget, but we still stop immediately after
 * that slice in order to resume from the next cursor position later. */
unsigned long arDefragIncremental(redisArray **arref, unsigned long cursor,
                                  void *(*defragfn)(void *))
{
    redisArray *ar = *arref;
    unsigned long work = 0;
    unsigned long maxwork = server.active_defrag_max_scan_fields;
    if (ar == NULL) return 0;

    if (cursor == 0) {
        ar = arDefragTopLevel(ar, defragfn);
        *arref = ar;
    }

    if (ar->superdir) {
        uint32_t bi = 0, si = 0;
        if (cursor != 0) arDefragDecodeSuperdirCursor(ar, cursor, &bi, &si);

        for (; bi < ar->sdir_len; bi++, si = 0) {
            arSDirEntry *e = ar->superdir + bi;
            /* Defrag the block slots array once when we enter the block from
             * its first slot. If we later resume in the middle of the same
             * block, the slots array was already handled. */
            if (si == 0) {
                arSlice **new_slots = defragfn(e->slots);
                if (new_slots) e->slots = new_slots;
                work++;
                server.stat_active_defrag_scanned++;
            }

            for (; si < AR_SUPER_BLOCK_SLOTS; si++) {
                arSlice *s = e->slots[si];
                work++;
                server.stat_active_defrag_scanned++;

                if (s == NULL) {
                    if (work > maxwork) {
                        si++;
                        if (si == AR_SUPER_BLOCK_SLOTS) {
                            bi++;
                            si = 0;
                        }
                        if (bi >= ar->sdir_len) return 0;
                        return arDefragSuperdirCursor(ar, bi, si);
                    }
                    continue;
                }

                e->slots[si] = arDefragSlice(s, &work, defragfn);

                if (work > maxwork) {
                    si++;
                    if (si == AR_SUPER_BLOCK_SLOTS) {
                        bi++;
                        si = 0;
                    }
                    if (bi >= ar->sdir_len) return 0;
                    return arDefragSuperdirCursor(ar, bi, si);
                }
            }
        }
        return 0;
    }

    if (ar->dir == NULL) return 0;

    uint64_t i = (cursor == 0) ? 0 : cursor - 1;
    for (; i < ar->dir_alloc; i++) {
        arSlice *s = ar->dir[i];
        work++;
        server.stat_active_defrag_scanned++;

        if (s == NULL) {
            if (work > maxwork) {
                i++;
                if (i >= ar->dir_alloc) return 0;
                return i + 1;
            }
            continue;
        }

        ar->dir[i] = arDefragSlice(s, &work, defragfn);

        if (work > maxwork) {
            i++;
            if (i >= ar->dir_alloc) return 0;
            return i + 1;
        }
    }
    return 0;
}
