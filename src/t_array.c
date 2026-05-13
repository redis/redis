/*
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Redis Array commands implementation.
 * Originally authored by: Salvatore Sanfilippo.
 *
 * The core sparse array data structure is in sparsearray.c/sparsearray.h.
 * This file contains Redis command handlers and Redis-specific operations.
 */

#include "server.h"
#include "../deps/tre/local_includes/tre.h"
#include <math.h>

/******************************************************************************
 *
 * ARRAY COMMANDS AND HIGHER LEVEL LOGIC
 *
 * This section contains all the Redis commands for the Array type, as well
 * as the type operations used by COPY and other server-level functionality.
 *
 *****************************************************************************/

/* ----------------------------------------------------------------------------
 * Array type operations for COPY command
 * -------------------------------------------------------------------------- */

robj *arrayTypeDup(robj *o) {
    redisArray *ar = o->ptr;
    redisArray *dup = arDup(ar);
    robj *newobj = createObject(OBJ_ARRAY, dup);
    newobj->encoding = OBJ_ENCODING_SLICED_ARRAY;
    return newobj;
}

/* ----------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

#define ARGETRANGE_MAX_ITEMS 1000000

/* Lookup array object for write, create it if missing, or reply with
 * WRONGTYPE and return NULL if the key holds a different type. */
robj *lookupArrayForWriteOrReply(client *c, robj *key) {
    robj *o = lookupKeyWrite(c->db, key);
    if (o == NULL) {
        o = createArrayObject();
        dbAdd(c->db, key, &o);
    } else if (checkType(c, o, OBJ_ARRAY)) {
        return NULL;
    }
    return o;
}

/* Reply with an array value. This helper is needed because we used
 * tagged pointers for inlining values like floats, integers, small
 * strings directly inside the pointer. Big memory saves, but more
 * work needed when there is to reply to the client. */
void addReplyArrayValue(client *c, void *v) {
    if (arIsEmpty(v)) {
        addReplyNull(c);
        return;
    }

    char buf[AR_INLINE_BUFSIZE];
    size_t len;
    const char *data = arDecode(v, buf, sizeof(buf), &len);
    addReplyBulkCBuffer(c, data, len);
}

/* Parse array index from object. Accepts 0 to 2^64-2 by default.
 * If allow_max is true, also accepts UINT64_MAX. This is used by ARSEEK
 * because ARSEEK UINT64_MAX sets insert_idx to UINT64_MAX-1, which is
 * a valid terminal state (next ARINSERT would overflow).
 * Returns C_OK/C_ERR. Does NOT send error reply - caller decides. */
int getArrayIndexFromObject(robj *o, uint64_t *idx, int allow_max) {
    unsigned long long ull;
    if (o->encoding == OBJ_ENCODING_INT) {
        if ((long)o->ptr < 0) return C_ERR;
        ull = (unsigned long long)(long)o->ptr;
    } else {
        if (!string2ull(o->ptr, &ull)) return C_ERR;
    }
    if (ull == UINT64_MAX && !allow_max) return C_ERR;
    *idx = ull;
    return C_OK;
}

/* Parse an array index argument and reply with an error on failure. */
int arrayParseIndexOrReply(client *c, robj *arg, uint64_t *idx) {
    if (getArrayIndexFromObject(arg, idx, 0) != C_OK) {
        addReplyError(c, "invalid array index");
        return C_ERR;
    }
    return C_OK;
}

/* ----------------------------------------------------------------------------
 * ARGET / ARMGET
 * -------------------------------------------------------------------------- */

/* ARGET key idx
 *
 * Returns the value at idx in O(1).
 * Missing keys and holes both reply with NULL. */
void argetCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (o && checkType(c, o, OBJ_ARRAY)) return;

    uint64_t idx;
    if (arrayParseIndexOrReply(c, c->argv[2], &idx) != C_OK) return;

    void *v = o ? arGet(o->ptr, idx) : NULL;
    addReplyArrayValue(c, v);
}

/* ARMGET key idx [idx ...]
 *
 * Returns the values at the requested indices in O(N), where N is the number
 * of indices. Missing keys and holes reply with NULLs. All indices are
 * validated before the reply starts, so malformed input fails atomically. */
void armgetCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (o != NULL && checkType(c, o, OBJ_ARRAY)) return;

    /* Pre-validate all indices so malformed input fails the whole command,
     * like the other array commands. */
    for (int i = 2; i < c->argc; i++) {
        uint64_t idx;
        if (arrayParseIndexOrReply(c, c->argv[i], &idx) != C_OK) return;
    }

    addReplyArrayLen(c, c->argc - 2);

    for (int i = 2; i < c->argc; i++) {
        if (o == NULL) {
            /* Non existing keys are semantically equivalent
             * to non existing indexes of existing arrays. */
            addReplyNull(c);
            continue;
        }

        uint64_t idx = 0;
        getArrayIndexFromObject(c->argv[i], &idx, 0);  /* Already validated. */

        redisArray *ar = o->ptr;
        void *v = arGet(ar, idx);
        addReplyArrayValue(c, v);
    }
}

/* ----------------------------------------------------------------------------
 * ARSET / ARMSET
 * -------------------------------------------------------------------------- */

/* ARSET key <index> <value> [value ...]
 *
 * Sets one or more contiguous values in O(N), where N is the number of
 * values. Creates the array if needed and returns the number of previously
 * empty slots that were filled. */
 void arsetCommand(client *c) {
    uint64_t start_idx;
    if (arrayParseIndexOrReply(c, c->argv[2], &start_idx) != C_OK) return;

    int num_values = c->argc - 3;

    /* Pre-validate: check for overflow and forbidden max index. */
    uint64_t last_idx = start_idx + (uint64_t)num_values - 1;
    if (last_idx < start_idx || last_idx == UINT64_MAX) {
        addReplyError(c, "array index overflow");
        return;
    }

    robj *o = lookupArrayForWriteOrReply(c, c->argv[1]);
    if (o == NULL) return;

    redisArray *ar = o->ptr;
    uint64_t old_count = arCount(ar);
    size_t old_alloc = 0;
    if (server.memory_tracking_enabled) old_alloc = kvobjAllocSize(o);

    /* Pre-promote sparse slices only for true bulk sets. A single-element
     * write does not benefit from the extra range-analysis pass. */
    if (num_values > 1)
        arMayPromoteToDenseForRangeSet(ar, start_idx, last_idx);

    /* Write all values starting at start_idx */
    uint64_t idx = start_idx;
    for (int i = 3; i < c->argc; i++) {
        sds val = c->argv[i]->ptr;
        void *v = arEncode(val, sdslen(val));
        arSet(ar, idx, v);
        idx++;
    }

    long long set_count = arCount(ar) - old_count;
    updateKeysizesHist(c->db, OBJ_ARRAY, old_count, arCount(ar));
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, old_alloc, kvobjAllocSize(o));
    keyModified(c, c->db, c->argv[1], o, 1);
    notifyKeyspaceEvent(NOTIFY_ARRAY, "arset", c->argv[1], c->db->id);
    server.dirty += num_values;
    addReplyLongLong(c, set_count);
}

/* ARMSET key idx value [idx value ...]
 *
 * Sets multiple scattered index/value pairs in O(N), where N is the number of
 * pairs. Creates the array if needed, returns the number of newly filled
 * slots, and validates all indices before mutating. */
void armsetCommand(client *c) {
    if ((c->argc - 2) % 2 != 0) {
        addReplyErrorArity(c);
        return;
    }

    /* Validate all indices first */
    for (int i = 2; i < c->argc; i += 2) {
        uint64_t idx;
        if (arrayParseIndexOrReply(c, c->argv[i], &idx) != C_OK) return;
    }

    robj *o = lookupArrayForWriteOrReply(c, c->argv[1]);
    if (o == NULL) return;

    redisArray *ar = o->ptr;
    uint64_t old_count = arCount(ar);
    size_t old_alloc = 0;
    if (server.memory_tracking_enabled) old_alloc = kvobjAllocSize(o);

    for (int i = 2; i < c->argc; i += 2) {
        uint64_t idx = 0;
        getArrayIndexFromObject(c->argv[i], &idx, 0);  /* Already validated */

        sds val = c->argv[i + 1]->ptr;
        void *v = arEncode(val, sdslen(val));
        arSet(ar, idx, v);
    }

    int num_pairs = (c->argc - 2) / 2;
    long long set_count = arCount(ar) - old_count;
    updateKeysizesHist(c->db, OBJ_ARRAY, old_count, arCount(ar));
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, old_alloc, kvobjAllocSize(o));
    keyModified(c, c->db, c->argv[1], o, 1);
    notifyKeyspaceEvent(NOTIFY_ARRAY, "armset", c->argv[1], c->db->id);
    server.dirty += num_pairs;
    addReplyLongLong(c, set_count);
}

/* ----------------------------------------------------------------------------
 * ARDEL / ARDELRANGE
 * -------------------------------------------------------------------------- */

/* ARDEL key idx [idx ...]
 *
 * Deletes the specified indices in O(N), where N is the number of indices.
 * All indices are validated first, and if the array becomes empty the key
 * itself is deleted. The number of deleted (existing) items is returned. */
void ardelCommand(client *c) {
    /* Pre-validate all indices before mutating, to report syntax errors
     * even if the key doesn't exist. */
    for (int i = 2; i < c->argc; i++) {
        uint64_t idx;
        if (arrayParseIndexOrReply(c, c->argv[i], &idx) != C_OK) return;
    }

    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (o == NULL) {
        addReplyLongLong(c, 0);
        return;
    }
    if (checkType(c, o, OBJ_ARRAY)) return;

    redisArray *ar = o->ptr;
    uint64_t old_count = arCount(ar);
    size_t old_alloc = 0;
    if (server.memory_tracking_enabled) old_alloc = kvobjAllocSize(o);
    long long deleted = 0;

    for (int i = 2; i < c->argc; i++) {
        uint64_t idx = 0;
        getArrayIndexFromObject(c->argv[i], &idx, 0);  /* Already validated */
        deleted += arDel(ar, idx);
    }

    int keyremoved = (arCount(ar) == 0);
    if (server.memory_tracking_enabled && deleted > 0 && keyremoved)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, old_alloc, kvobjAllocSize(o));
    if (deleted > 0) {
        if (keyremoved)
            dbDeleteSkipKeysizesUpdate(c->db, c->argv[1]);
        updateKeysizesHist(c->db, OBJ_ARRAY,
                           old_count, keyremoved ? -1 : (int64_t)arCount(ar));
        if (server.memory_tracking_enabled && !keyremoved)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, old_alloc, kvobjAllocSize(o));
        keyModified(c, c->db, c->argv[1], keyremoved ? NULL : o, 1);
        notifyKeyspaceEvent(NOTIFY_ARRAY, "ardel", c->argv[1], c->db->id);
        if (keyremoved)
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c, deleted);
}

/* ARDELRANGE key start end [start end ...]
 *
 * Deletes one or more ranges. Complexity is proportional to the existing
 * elements / slices touched, not to the numeric span of the requested ranges,
 * so huge ranges do not block the server forever.
 *
 * Each pair may be given in either order. All ranges are validated up front,
 * and an empty resulting array deletes the key. */
void ardelrangeCommand(client *c) {
    if ((c->argc - 2) % 2 != 0) {
        addReplyErrorArity(c);
        return;
    }

    /* Pre-validate all ranges before mutating, to avoid partial updates
     * if a later range has invalid syntax. */
    for (int i = 2; i < c->argc; i += 2) {
        uint64_t start, end;
        if (arrayParseIndexOrReply(c, c->argv[i], &start) != C_OK) return;
        if (arrayParseIndexOrReply(c, c->argv[i + 1], &end) != C_OK) return;
    }

    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (o == NULL) {
        addReplyLongLong(c, 0);
        return;
    }
    if (checkType(c, o, OBJ_ARRAY)) return;

    redisArray *ar = o->ptr;
    uint64_t old_count = arCount(ar);
    size_t old_alloc = 0;
    if (server.memory_tracking_enabled) old_alloc = kvobjAllocSize(o);
    uint64_t total_deleted = 0;

    /* Process each range using the generalized arDeleteRange */
    for (int i = 2; i < c->argc; i += 2) {
        uint64_t start = 0, end = 0;
        getArrayIndexFromObject(c->argv[i], &start, 0); /* Already validated */
        getArrayIndexFromObject(c->argv[i + 1], &end, 0);

        uint64_t lo = (start <= end) ? start : end;
        uint64_t hi = (start <= end) ? end : start;

        total_deleted += arDeleteRange(ar, lo, hi);
    }

    int keyremoved = (arCount(ar) == 0);
    if (server.memory_tracking_enabled && total_deleted > 0 && keyremoved)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, old_alloc, kvobjAllocSize(o));
    if (total_deleted > 0) {
        if (keyremoved)
            dbDeleteSkipKeysizesUpdate(c->db, c->argv[1]);
        updateKeysizesHist(c->db, OBJ_ARRAY,
                           old_count, keyremoved ? -1 : (int64_t)arCount(ar));
        if (server.memory_tracking_enabled && !keyremoved)
            updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, old_alloc, kvobjAllocSize(o));
        keyModified(c, c->db, c->argv[1], keyremoved ? NULL : o, 1);
        notifyKeyspaceEvent(NOTIFY_ARRAY, "ardelrange", c->argv[1], c->db->id);
        if (keyremoved)
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty += total_deleted;
    }
    addReplyUnsignedLongLong(c, total_deleted);
}

/* ----------------------------------------------------------------------------
 * ARLEN / ARCOUNT
 * -------------------------------------------------------------------------- */

/* ARLEN key
 *
 * Returns max-index-plus-one in O(1).
 * Missing keys reply with 0. */
void arlenCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c, c->argv[1], shared.czero);
    if (o == NULL || checkType(c, o, OBJ_ARRAY)) return;

    redisArray *ar = o->ptr;
    addReplyUnsignedLongLong(c, arLen(ar));
}

/* ARCOUNT key
 *
 * Returns the number of non-empty elements in O(1).
 * Missing keys reply with 0. */
void arcountCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c, c->argv[1], shared.czero);
    if (o == NULL || checkType(c, o, OBJ_ARRAY)) return;

    redisArray *ar = o->ptr;
    addReplyUnsignedLongLong(c, arCount(ar));
}

/* ----------------------------------------------------------------------------
 * ARGETRANGE
 * -------------------------------------------------------------------------- */

/* ARGETRANGE key start end
 *
 * Returns every position in the requested range in O(N), where N is the range
 * length. Holes are returned as NULLs, and a missing key behaves like an all-
 * NULL array. If start > end the reply order is reversed.
 *
 * To avoid giant synthetic NULL replies, the range length is hard-limited,
 * otherwise the command, with a wrong range, could make the server totally
 * unusable. The max range is 1 million elements and is fixed, constituting
 * the user-facing semantic of the command. */
void argetrangeCommand(client *c) {
    uint64_t start, end;
    if (arrayParseIndexOrReply(c, c->argv[2], &start) != C_OK) return;
    if (arrayParseIndexOrReply(c, c->argv[3], &end) != C_OK) return;

    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (o != NULL && checkType(c, o, OBJ_ARRAY)) return;

    int reverse = start > end;
    uint64_t lo = reverse ? end : start;
    uint64_t hi = reverse ? start : end;
    uint64_t len = hi - lo + 1;

    /* ARGETRANGE is a special command: it can trigger a huge reply blocking
     * the server (basically forever) even if there is no actual data. This
     * is unlike an SMEMBERS against a very big key: somebody added so many
     * elements inside, before asking for a huge amount of elements. But, in the
     * case of ARGETRANGE, you can just trigger a huge amount of NULLs to be
     * sent to the client. In ARSCAN this was optimized to be O(N) with the
     * actual populated elements, but in this case it can't be done because
     * of the semantic of the command, and the Redis protocol inability to reply
     * with run-length ranges (a, b, c, 1293455 NULLs, d, e).
     *
     * Because of all that, we put an hard limit in the range size, and this
     * limit must be part of the Redis culture, so it should not be tuned in
     * any way: 1 million items, with an hard error if the range is bigger than
     * that, not just a silent trimming at this length, that would cause hard
     * to track bugs. */
    if (len > ARGETRANGE_MAX_ITEMS) {
        addReplyErrorFormat(c, "range exceeds maximum of %u items",
            ARGETRANGE_MAX_ITEMS);
        return;
    }

    addReplyArrayLen(c, len);
    if (o == NULL) {
        for (uint64_t i = 0; i < len; i++) addReplyNull(c);
        return;
    }

    redisArray *ar = o->ptr;
    if (reverse) {
        for (uint64_t idx = hi; ; idx--) {
            void *v = arGet(ar, idx);
            addReplyArrayValue(c, v);
            if (idx == lo) break;
        }
    } else {
        for (uint64_t idx = lo; idx <= hi; idx++) {
            void *v = arGet(ar, idx);
            addReplyArrayValue(c, v);
        }
    }
}

/* ----------------------------------------------------------------------------
 * ARSCAN
 * -------------------------------------------------------------------------- */

/* Iterate populated elements in [start..end].
 *
 * This iterator is read-only and not mutation-stable: between Init() and the
 * final Next() that returns 0, the caller must not write to the array. Any
 * write may free or relocate the current slice, making the iterator state
 * stale. The goal of this abstraction was to capture repeated code in the
 * implementation of ARSCAN, ARGREP, AROP.
 *
 * The struct lives on the caller stack, so setup and iteration stay allocation
 * free and command-local. */
typedef struct {
    redisArray *ar;
    uint64_t lo;              /* Normalized inclusive range start. */
    uint64_t hi;              /* Normalized inclusive range end. */
    uint64_t lo_slice;        /* First slice touched by the range. */
    uint64_t hi_slice;        /* Last slice touched by the range. */
    uint32_t slice_size;      /* Cached slice size. */
    int reverse;              /* Iterate from high to low. */
    int32_t step;             /* +1 forward, -1 backward. */
    int done;                 /* No more elements to return. */
    int top_done;             /* No more slices to inspect after current. */

    uint64_t slice_id;        /* Next flat-directory slice to inspect. */
    int32_t sdir_index;       /* Next superdir entry to inspect. */
    int32_t slot_index;       /* Next slot inside the current superdir entry. */

    arSlice *slice;           /* Slice currently being scanned. */
    uint64_t slice_base;      /* Logical index of slice offset 0. */
    uint32_t off_lo;          /* First in-range offset for current slice. */
    uint32_t off_hi;          /* Last in-range offset for current slice. */
    int dense;                /* Current slice is dense. */
    void **dense_items;       /* Dense items window. */
    int32_t dense_off;        /* Current dense logical offset. */
    int32_t dense_item_pos;   /* Current dense window index. */
    int32_t dense_item_end;   /* Final dense window index. */
    uint16_t *sparse_offsets; /* Sparse offsets array. */
    void **sparse_values;     /* Sparse values array. */
    int32_t sparse_count;     /* Sparse entry count. */
    int32_t sparse_pos;       /* Current sparse entry position. */
    int slice_ready;          /* Current slice scan state is initialized. */
} arScanIter;

#define AR_SCAN_ITER_SLOT_UNSET INT32_MIN

/* Keep the per-element iterator hot path inline in the command loops.
 * It helps a lot with certain targets, up to ~30-50% speed regression
 * without forcing the inlining. */
#if defined(__GNUC__) || defined(__clang__)
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define ALWAYS_INLINE inline
#endif

/* Initialize a populated-elements iterator. Empty arrays and empty clipped
 * ranges are turned into a done iterator here so the first Next() is a single
 * branch. */
static void arScanIterInit(redisArray *ar, uint64_t start, uint64_t end,
                           arScanIter *it)
{
    memset(it, 0, sizeof(*it));
    it->ar = ar;

    if (ar == NULL || arCount(ar) == 0) {
        it->done = 1;
        it->top_done = 1;
        return;
    }

    /* Note that a few things here could be taken
     * from the array itself, as they are immutable,
     * but after introducing this abstraction a small
     * but measurable speed regression suggested to
     * micro-optimize for this hot path and have
     * iterator-side copies of often used stuff. */
    it->reverse = start > end;
    it->step = it->reverse ? -1 : 1;
    it->lo = it->reverse ? end : start;
    it->hi = it->reverse ? start : end;
    it->slice_size = ar->slice_size;
    it->lo_slice = it->lo / it->slice_size;
    it->hi_slice = it->hi / it->slice_size;
    it->slot_index = AR_SCAN_ITER_SLOT_UNSET;

    /* No intersection between the range and the array span. */
    if (it->lo_slice > ar->dir_highest_used) {
        it->done = 1;
        it->top_done = 1;
        return;
    }

    /* Clip the high end to the actual array span. */
    if (it->hi_slice > ar->dir_highest_used) {
        it->hi_slice = ar->dir_highest_used;
        it->hi = arMakeIdx(it->hi_slice, it->slice_size - 1, it->slice_size);
    }

    /* Clipping made the range empty? */
    if (it->lo_slice > it->hi_slice) {
        it->done = 1;
        it->top_done = 1;
        return;
    }

    if (ar->superdir) {
        int found;

        /* Start from the first superdir block that can intersect the range. */
        uint64_t block_id = (it->reverse ? it->hi_slice : it->lo_slice) /
                            AR_SUPER_BLOCK_SLOTS;
        uint32_t pos = arSuperDirFind(ar, block_id, &found);

        if (it->reverse) it->sdir_index = found ? (int32_t)pos : (int32_t)pos - 1;
        else it->sdir_index = (int32_t)pos;

        /* No superdir block intersects the clipped range. */
        if (it->sdir_index < 0 || it->sdir_index >= (int32_t)ar->sdir_len) {
            it->done = 1;
            it->top_done = 1;
        }
    } else {
        /* Flat directory iteration starts directly from the first in-range slice. */
        it->slice_id = it->reverse ? it->hi_slice : it->lo_slice;
    }
}

/* Prepare the current slice-local scan state. Returns 1 if the slice may
 * yield at least one populated element in range, otherwise 0.
 * The function is used by arScanIterLoadNextSlice() each time a new
 * slice should be iterated. When a new slice is selected by
 * arScanIterLoadNextSlice(), then this function is called to setup the
 * iteration needed by arScanIterNext(). */
static ALWAYS_INLINE int arScanIterPrepareSlice(arScanIter *it,
                                                arSlice *s, uint64_t slice_id)
{
    uint64_t slice_base = slice_id * it->slice_size;
    /* Restrict the scan to the part of this slice touched by the query. */
    uint32_t off_lo = (slice_id == it->lo_slice) ?
        arSliceOff(it->lo, it->slice_size) : 0;
    uint32_t off_hi = (slice_id == it->hi_slice) ?
        arSliceOff(it->hi, it->slice_size) : it->slice_size - 1;

    if (s->encoding == AR_SLICE_DENSE) {
        uint32_t win_lo = s->layout.dense.offset;
        uint32_t win_hi = s->layout.dense.offset + s->layout.dense.winsize - 1;

        /* Dense slices may only have a smaller populated window allocated. */
        if (off_lo < win_lo) off_lo = win_lo;
        if (off_hi > win_hi) off_hi = win_hi;

        /* No intersection between the range and the dense window. */
        if (off_lo > off_hi) return 0;

        it->dense = 1;
        it->dense_items = s->layout.dense.items;
        it->dense_off = it->reverse ? (int32_t)off_hi : (int32_t)off_lo;
        it->dense_item_pos = it->dense_off - (int32_t)win_lo;
        it->dense_item_end = (it->reverse ? (int32_t)off_lo :
                              (int32_t)off_hi) - (int32_t)win_lo;
    } else {
        int found;
        uint32_t pos;
        uint16_t *offsets = s->layout.sparse.offsets;

        it->dense = 0;
        it->sparse_offsets = offsets;
        it->sparse_values = s->layout.sparse.values;
        it->sparse_count = (int32_t)s->count;
        if (it->reverse) {
            /* Start from the last sparse entry that can still be in range. */
            pos = arSparseFindPos(s, (uint16_t)off_hi, &found);
            it->sparse_pos = found ? (int32_t)pos : (int32_t)pos - 1;

            /* No sparse entry falls inside the requested offsets. */
            if (it->sparse_pos < 0 || offsets[it->sparse_pos] < off_lo)
                return 0;
        } else {
            /* Start from the first sparse entry that can still be in range. */
            pos = arSparseFindPos(s, (uint16_t)off_lo, &found);
            it->sparse_pos = (int32_t)pos;

            /* No sparse entry falls inside the requested offsets. */
            if (it->sparse_pos >= (int32_t)s->count ||
                offsets[it->sparse_pos] > off_hi) return 0;
        }
    }

    it->slice = s;
    it->slice_base = slice_base;
    it->off_lo = off_lo;
    it->off_hi = off_hi;
    it->slice_ready = 1;
    return 1;
}

/* Advance top-level directory state until a non-NULL slice in range is ready
 * for local scanning, or return 0 if the iterator is exhausted. */
static ALWAYS_INLINE int arScanIterLoadNextSlice(arScanIter *it) {
    redisArray *ar = it->ar;

    if (ar->superdir) {
        while (!it->top_done) {
            /* No more superdir blocks to inspect. */
            if (it->sdir_index < 0 || it->sdir_index >= (int32_t)ar->sdir_len) {
                it->top_done = 1;
                break;
            }

            arSDirEntry *e = ar->superdir + it->sdir_index;
            uint64_t block_base = e->block_id * AR_SUPER_BLOCK_SLOTS;
            uint64_t block_end = block_base + AR_SUPER_BLOCK_SLOTS - 1;
            int32_t block_slot_lo = (block_base < it->lo_slice) ?
                (int32_t)(it->lo_slice - block_base) : 0;
            int32_t block_slot_hi = (block_end > it->hi_slice) ?
                (int32_t)(it->hi_slice - block_base) : AR_SUPER_BLOCK_SLOTS - 1;

            /* This block starts after the requested range. */
            if (block_base > it->hi_slice) {
                it->top_done = 1;
                break;
            }

            /* This block ends before the requested range. */
            if (block_end < it->lo_slice) {
                if (it->reverse) it->top_done = 1;
                else it->sdir_index++;
                it->slot_index = AR_SCAN_ITER_SLOT_UNSET;
                continue;
            }

            if (it->reverse) {
                /* slot_index uses a sentinel outside the valid 0..2047 range
                 * so reverse scans can consume slot 0 and then fall below the
                 * block without looking like a fresh block entry. */
                if (it->slot_index == AR_SCAN_ITER_SLOT_UNSET)
                    it->slot_index = block_slot_hi;

                while (it->slot_index >= block_slot_lo) {
                    int32_t si = it->slot_index--;
                    arSlice *s = e->slots[si];
                    if (s && arScanIterPrepareSlice(it, s, block_base + si))
                        return 1;
                }

                /* This block had no more matching slices, move to the previous block. */
                it->sdir_index--;
                it->slot_index = AR_SCAN_ITER_SLOT_UNSET;
            } else {
                /* slot_index uses a sentinel outside the valid 0..2047 range
                 * so an exhausted block does not look like a fresh entry. */
                if (it->slot_index == AR_SCAN_ITER_SLOT_UNSET)
                    it->slot_index = block_slot_lo;

                while (it->slot_index <= block_slot_hi) {
                    int32_t si = it->slot_index++;
                    arSlice *s = e->slots[si];
                    if (s && arScanIterPrepareSlice(it, s, block_base + si))
                        return 1;
                }

                /* This block had no more matching slices, move to the next block. */
                it->sdir_index++;
                it->slot_index = AR_SCAN_ITER_SLOT_UNSET;
            }
        }
    } else {
        while (!it->top_done) {
            uint64_t slice_id = it->slice_id;
            arSlice *s = ar->dir[slice_id];

            /* Advance the top-level cursor before possibly returning this slice. */
            if (it->reverse) {
                if (slice_id == it->lo_slice) it->top_done = 1;
                else it->slice_id = slice_id - 1;
            } else {
                if (slice_id == it->hi_slice) it->top_done = 1;
                else it->slice_id = slice_id + 1;
            }

            if (s && arScanIterPrepareSlice(it, s, slice_id))
                return 1;
        }
    }

    return 0;
}

/* Return the next populated element in range, or 0 when done. */
static ALWAYS_INLINE int arScanIterNext(arScanIter *it,
                                        uint64_t *idx, void **value)
{
    /* The iterator was already fully consumed. */
    if (it->done) return 0;

    while (1) {
        if (it->slice_ready) {
            /* Drain the current slice before asking for another one. */
            if (it->dense) {
                while ((it->step > 0 && it->dense_item_pos <= it->dense_item_end) ||
                       (it->step < 0 && it->dense_item_pos >= it->dense_item_end)) {
                    uint32_t off = (uint32_t)it->dense_off;
                    void *v = it->dense_items[it->dense_item_pos];
                    it->dense_off += it->step;
                    it->dense_item_pos += it->step;

                    /* Dense windows may contain holes. */
                    if (arIsEmpty(v)) continue;

                    if (idx) *idx = it->slice_base + off;
                    *value = v;
                    return 1;
                }
            } else {
                while (it->sparse_pos >= 0 && it->sparse_pos < it->sparse_count) {
                    int32_t pos = it->sparse_pos;
                    uint32_t off = it->sparse_offsets[pos];

                    /* Sparse entries are sorted, so leaving the window ends this slice. */
                    if (off < it->off_lo || off > it->off_hi) break;

                    it->sparse_pos += it->step;
                    if (idx) *idx = it->slice_base + off;
                    *value = it->sparse_values[pos];
                    return 1;
                }
            }

            /* The current slice has no more in-range populated elements. */
            it->slice = NULL;
            it->slice_ready = 0;
        }

        /* No more in-range slices are available. */
        if (!arScanIterLoadNextSlice(it)) {
            it->done = 1;
            return 0;
        }
    }
}

/* ARSCAN key start end [LIMIT count]
 *
 * Returns only existing elements as flat index/value pairs.
 *
 * Complexity is O(P), where P is visited positions in touched slices
 * (dense scanned slots + sparse entries), with worst-case O(|end-start|+1)
 * and typical case close to O(N), where N is the number of existing
 * elements in range. This means that huge ranges are safe and will not
 * block the server with a work bound to the span length.
 *
 * Unlike ARGETRANGE, holes are skipped rather than returned as NULLs.
 * LIMIT caps the number of returned pairs. */
void arscanCommand(client *c) {
    uint64_t start, end;
    if (arrayParseIndexOrReply(c, c->argv[2], &start) != C_OK) return;
    if (arrayParseIndexOrReply(c, c->argv[3], &end) != C_OK) return;

    /* Parse optional LIMIT */
    uint64_t remaining = UINT64_MAX;
    if (c->argc == 6) {
        if (strcasecmp(c->argv[4]->ptr, "LIMIT") != 0) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
        long long ll;
        if (getLongLongFromObjectOrReply(c, c->argv[5], &ll, NULL) != C_OK)
            return;
        if (ll <= 0) {
            addReplyError(c, "LIMIT must be positive");
            return;
        }
        remaining = (uint64_t)ll;
    } else if (c->argc != 4) {
        addReplyErrorArity(c);
        return;
    }

    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (o != NULL && checkType(c, o, OBJ_ARRAY)) return;

    if (o == NULL) {
        addReplyArrayLen(c, 0);
        return;
    }

    redisArray *ar = o->ptr;
    void *replylen = addReplyDeferredLen(c);
    uint64_t count = 0;
    arScanIter it;
    uint64_t idx;
    void *v;

    arScanIterInit(ar, start, end, &it);
    while (remaining && arScanIterNext(&it, &idx, &v)) {
        /* Reply with nested [idx, value] pairs. */
        addReplyArrayLen(c, 2);
        addReplyUnsignedLongLong(c, idx);
        addReplyArrayValue(c, v);
        count++;
        remaining--;
    }

    setDeferredArrayLen(c, replylen, count);
}

/* ============================================================================
 * ARGREP
 * ============================================================================
 *
 * Search existing array elements in a range using textual predicates.
 * Like ARSCAN, the work is bound by the visited slices, not by the raw
 * numeric span alone: dense slices scan the touched dense window, while
 * sparse slices only scan stored entries inside the covered offsets.
 * -------------------------------------------------------------------------- */

#define ARGREP_PRED_EXACT 1
#define ARGREP_PRED_MATCH 2
#define ARGREP_PRED_GLOB  3
#define ARGREP_PRED_RE    4

#define ARGREP_MAX_PREDICATES 250
#define ARGREP_MAX_RE_LEN 2048

#define ARGREP_COMBINE_OR  1
#define ARGREP_COMBINE_AND 2

#define ARGREP_BOUND_INDEX 1
#define ARGREP_BOUND_START 2
#define ARGREP_BOUND_END   3

typedef struct {
    int type;               /* EXACT, MATCH, GLOB, or RE. */
    sds pattern;            /* Pattern argument exactly as given by the user. */
    regex_t regex;          /* Compiled regex for RE predicates. */
    int regex_compiled;     /* Whether regex must be freed. */
} arGrepPredicate;

typedef struct {
    int type;               /* Numeric index, logical start, or logical end. */
    uint64_t index;         /* Used only for numeric bounds. */
} arGrepBound;

typedef struct {
    arGrepPredicate *preds; /* All predicates to apply to each element. */
    int num_preds;          /* Number of predicates stored in preds[]. */
    int combine;            /* OR by default, AND if requested. */
    int withvalues;         /* Reply with [idx value ...] instead of [idx ...]. */
    int nocase;             /* Apply case-insensitive matching globally. */
} arGrepPlan;

/* Lowercase only ASCII letters. This keeps MATCH/EXACT deterministic and
 * locale-independent even on arbitrary binary payloads. */
static inline unsigned char arGrepLowerAscii(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* Compare two byte strings, optionally ignoring ASCII case. */
int arGrepBytesEqual(const char *a, size_t alen, const char *b, size_t blen,
                     int nocase) {
    if (alen != blen) return 0;
    if (!nocase) return memcmp(a, b, alen) == 0;

    for (size_t i = 0; i < alen; i++) {
        if (arGrepLowerAscii((unsigned char)a[i]) !=
            arGrepLowerAscii((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

/* Find a needle inside a byte string, optionally ignoring ASCII case. */
int arGrepBytesContains(const char *haystack, size_t haystack_len,
                        const char *needle, size_t needle_len, int nocase) {
    if (needle_len == 0) return 1;
    if (needle_len > haystack_len) return 0;

    size_t last = haystack_len - needle_len;
    for (size_t i = 0; i <= last; i++) {
        if (arGrepBytesEqual(haystack + i, needle_len, needle, needle_len,
                             nocase)) {
            return 1;
        }
    }
    return 0;
}

/* Return the predicate type for a keyword, or 0 if it is not one. */
int arGrepPredicateType(const char *token) {
    if (!strcasecmp(token, "EXACT")) return ARGREP_PRED_EXACT;
    if (!strcasecmp(token, "MATCH")) return ARGREP_PRED_MATCH;
    if (!strcasecmp(token, "GLOB")) return ARGREP_PRED_GLOB;
    if (!strcasecmp(token, "RE")) return ARGREP_PRED_RE;
    return 0;
}

/* Free any compiled regex state created while parsing ARGREP. */
void arGrepFreePlan(arGrepPlan *plan) {
    if (plan->preds == NULL) return;

    for (int i = 0; i < plan->num_preds; i++) {
        if (plan->preds[i].regex_compiled)
            tre_regfree(&plan->preds[i].regex);
    }
    zfree(plan->preds);
    plan->preds = NULL;
}

/* Parse a bound argument. ARGREP accepts the special tokens "-" and "+"
 * in addition to normal array indexes. */
int arGrepParseBoundOrReply(client *c, robj *arg, arGrepBound *bound) {
    if (arg->encoding != OBJ_ENCODING_INT) {
        sds token = arg->ptr;
        if (sdslen(token) == 1 && token[0] == '-') {
            bound->type = ARGREP_BOUND_START;
            bound->index = 0;
            return C_OK;
        }
        if (sdslen(token) == 1 && token[0] == '+') {
            bound->type = ARGREP_BOUND_END;
            bound->index = 0;
            return C_OK;
        }
    }

    if (getArrayIndexFromObject(arg, &bound->index, 0) != C_OK) {
        addReplyError(c, "invalid array index");
        return C_ERR;
    }
    bound->type = ARGREP_BOUND_INDEX;
    return C_OK;
}

/* Resolve a parsed bound against the current array length. */
uint64_t arGrepResolveBound(arGrepBound *bound, uint64_t max_index) {
    if (bound->type == ARGREP_BOUND_START) return 0;
    if (bound->type == ARGREP_BOUND_END) return max_index;
    return bound->index;
}

/* Compile all RE predicates after the whole command is parsed, so NOCASE is
 * already known and affects every regex consistently. */
int arGrepCompileRegexesOrReply(client *c, arGrepPlan *plan) {
    for (int i = 0; i < plan->num_preds; i++) {
        arGrepPredicate *pred = &plan->preds[i];
        if (pred->type != ARGREP_PRED_RE) continue;

        if (sdslen(pred->pattern) == 0) {
            addReplyError(c, "regular expression is empty");
            return C_ERR;
        }

        int cflags = REG_EXTENDED | REG_NOSUB | REG_USEBYTES;
        if (plan->nocase) cflags |= REG_ICASE;

        int err = tre_regncompb(&pred->regex, pred->pattern,
                                sdslen(pred->pattern), cflags);
        if (err != REG_OK) {
            char errbuf[256];
            tre_regerror(err, &pred->regex, errbuf, sizeof(errbuf));
            addReplyErrorFormat(c, "invalid regular expression: %s", errbuf);
            return C_ERR;
        }
        pred->regex_compiled = 1;

        if (tre_have_backrefs(&pred->regex)) {
            addReplyError(c, "regular expression backreferences are not supported");
            return C_ERR;
        }
    }
    return C_OK;
}

/* Parse predicates and global modifiers in a single pass. This makes the
 * command more user-friendly because predicates and options can be mixed
 * freely. If the same global option appears multiple times, the last one
 * wins. */
int arGrepParsePlanOrReply(client *c, arGrepPlan *plan, uint64_t *limit) {
    memset(plan, 0, sizeof(*plan));
    plan->combine = ARGREP_COMBINE_OR;
    *limit = UINT64_MAX;

    int max_preds = c->argc - 4;
    plan->preds = zcalloc(sizeof(*plan->preds) * max_preds);

    for (int arg = 4; arg < c->argc; ) {
        sds token = c->argv[arg]->ptr;
        int type = arGrepPredicateType(token);

        if (type != 0) {
            if (arg + 1 >= c->argc) {
                addReplyErrorObject(c, shared.syntaxerr);
                return C_ERR;
            }
            if (plan->num_preds >= ARGREP_MAX_PREDICATES) {
                addReplyErrorFormat(c, "too many predicates, maximum is %d",
                                    ARGREP_MAX_PREDICATES);
                return C_ERR;
            }

            arGrepPredicate *pred = &plan->preds[plan->num_preds++];
            pred->type = type;
            pred->pattern = c->argv[arg + 1]->ptr;
            if (type == ARGREP_PRED_RE &&
                sdslen(pred->pattern) > ARGREP_MAX_RE_LEN) {
                addReplyErrorFormat(c,
                    "regular expression is too long, maximum is %d bytes",
                    ARGREP_MAX_RE_LEN);
                return C_ERR;
            }
            arg += 2;
            continue;
        }

        if (!strcasecmp(token, "LIMIT")) {
            if (arg + 1 >= c->argc) {
                addReplyErrorObject(c, shared.syntaxerr);
                return C_ERR;
            }

            long long ll;
            if (getLongLongFromObjectOrReply(c, c->argv[arg + 1], &ll, NULL)
                != C_OK) {
                return C_ERR;
            }
            if (ll <= 0) {
                addReplyError(c, "LIMIT must be positive");
                return C_ERR;
            }

            *limit = (uint64_t)ll;
            arg += 2;
            continue;
        }

        if (!strcasecmp(token, "WITHVALUES")) {
            plan->withvalues = 1;
            arg++;
            continue;
        }

        if (!strcasecmp(token, "NOCASE")) {
            plan->nocase = 1;
            arg++;
            continue;
        }

        if (!strcasecmp(token, "AND") || !strcasecmp(token, "OR")) {
            plan->combine = !strcasecmp(token, "AND") ?
                ARGREP_COMBINE_AND : ARGREP_COMBINE_OR;
            arg++;
            continue;
        }

        addReplyErrorObject(c, shared.syntaxerr);
        return C_ERR;
    }

    if (plan->num_preds == 0) {
        addReplyErrorObject(c, shared.syntaxerr);
        return C_ERR;
    }

    return arGrepCompileRegexesOrReply(c, plan);
}

/* Match one predicate against the decoded element bytes. */
int arGrepMatchPredicate(arGrepPredicate *pred, const char *data, size_t len,
                         int nocase) {
    size_t pattern_len = sdslen(pred->pattern);

    switch (pred->type) {
    case ARGREP_PRED_EXACT:
        return arGrepBytesEqual(data, len, pred->pattern, pattern_len, nocase);
    case ARGREP_PRED_MATCH:
        return arGrepBytesContains(data, len, pred->pattern, pattern_len,
                                   nocase);
    case ARGREP_PRED_GLOB:
        return stringmatchlen(pred->pattern, pattern_len, data, len, nocase);
    case ARGREP_PRED_RE:
        return tre_regnexecb(&pred->regex, data, len, 0, NULL, 0) == REG_OK;
    default:
        serverPanic("Unknown ARGREP predicate type");
    }
}

/* Decode one array value and apply all the predicates to it. */
int arGrepValueMatches(arGrepPlan *plan, void *v) {
    char buf[AR_INLINE_BUFSIZE];
    size_t len;
    const char *data = arDecode(v, buf, sizeof(buf), &len);

    if (plan->combine == ARGREP_COMBINE_AND) {
        for (int i = 0; i < plan->num_preds; i++) {
            if (!arGrepMatchPredicate(&plan->preds[i], data, len,
                                      plan->nocase)) {
                return 0;
            }
        }
        return 1;
    }

    for (int i = 0; i < plan->num_preds; i++) {
        if (arGrepMatchPredicate(&plan->preds[i], data, len, plan->nocase))
            return 1;
    }
    return 0;
}

/* ARGREP key start end
 *        (EXACT string | MATCH string | GLOB pattern | RE pattern) ...
 *        [AND | OR] [LIMIT count] [WITHVALUES] [NOCASE]
 *
 * Search existing elements in a range and return matching indexes.
 *
 * Complexity is O(P * C), where P is the number of visited positions in the
 * touched slices and C is the cost of evaluating the active predicates.
 * Dense slices scan the touched dense window, sparse slices only visit stored
 * entries, and LIMIT stops as soon as enough matches were emitted.
 *
 * "-" and "+" mean the logical start and end of the array. WITHVALUES changes
 * the reply from [idx ...] to [idx value ...]. */
void argrepCommand(client *c) {
    arGrepBound start_bound, end_bound;
    if (arGrepParseBoundOrReply(c, c->argv[2], &start_bound) != C_OK) return;
    if (arGrepParseBoundOrReply(c, c->argv[3], &end_bound) != C_OK) return;

    arGrepPlan plan;
    uint64_t remaining;
    if (arGrepParsePlanOrReply(c, &plan, &remaining) != C_OK) {
        arGrepFreePlan(&plan);
        return;
    }

    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (o != NULL && checkType(c, o, OBJ_ARRAY)) {
        arGrepFreePlan(&plan);
        return;
    }
    if (o == NULL) {
        arGrepFreePlan(&plan);
        addReplyArrayLen(c, 0);
        return;
    }

    redisArray *ar = o->ptr;
    uint64_t ar_len = arLen(ar);
    if (ar_len == 0 || arCount(ar) == 0) {
        arGrepFreePlan(&plan);
        addReplyArrayLen(c, 0);
        return;
    }

    void *replylen = addReplyDeferredLen(c);
    uint64_t count = 0;
    uint64_t max_index = ar_len - 1;
    uint64_t start = arGrepResolveBound(&start_bound, max_index);
    uint64_t end = arGrepResolveBound(&end_bound, max_index);
    arScanIter it;
    uint64_t idx;
    void *v;

    arScanIterInit(ar, start, end, &it);
    while (remaining && arScanIterNext(&it, &idx, &v)) {
        if (!arGrepValueMatches(&plan, v)) continue;
        /* With WITHVALUES, reply nested [idx, value] pairs. */
        if (plan.withvalues) addReplyArrayLen(c, 2);
        addReplyUnsignedLongLong(c, idx);
        if (plan.withvalues) addReplyArrayValue(c, v);
        count++;
        remaining--;
    }

    setDeferredArrayLen(c, replylen, count);
    arGrepFreePlan(&plan);
}

/* ============================================================================
 * AROP
 * ============================================================================
 *
 * Aggregate operations over a range. Uses O(N) iteration where N is the
 * number of stored elements. Dense slices scan the window intersection
 * (bounded by dense.winsize, kept small by demotion when density drops).
 * -------------------------------------------------------------------------- */

/* Operation types for AROP */
#define AROP_SUM   1  /* Sum of numeric elements in range. */
#define AROP_MIN   2  /* Minimum numeric element in range. */
#define AROP_MAX   3  /* Maximum numeric element in range. */
#define AROP_AND   4  /* Bitwise AND of integer elements in range. */
#define AROP_OR    5  /* Bitwise OR of integer elements in range. */
#define AROP_XOR   6  /* Bitwise XOR of integer elements in range. */
#define AROP_MATCH 7  /* Count elements equal to a target string. */
#define AROP_USED  8  /* Count of non-empty (used) slots in range. */

/* Accumulator state for AROP */
typedef struct {
    int op;                    /* Selected AROP operation. */
    sds match_val;             /* MATCH target string. */
    long double sum_acc;       /* Running SUM accumulator. */
    long double minmax_acc;    /* Running MIN or MAX accumulator. */
    int64_t bitwise_acc;       /* Running AND/OR/XOR accumulator. */
    long long match_count;     /* Number of MATCH hits. */
    long long used_count;      /* Number of non-empty elements seen. */
    int has_numeric;           /* Saw at least one numeric value. */
    int has_int;               /* Saw at least one bitwise-usable integer. */
} arOpAcc;

/* Process a single value for AROP aggregation, aggregating it
 * into the structure arOpAcc 'acc'. This helper is used
 * directly by the AROP command implementation while scanning
 * populated elements in the requested range. */
static inline void arOpAccumulate(arOpAcc *acc, void *v) {
    if (acc->op == AROP_USED) {
        acc->used_count++;
        return;
    }

    if (acc->op == AROP_MATCH) {
        size_t vlen;
        char vbuf[AR_INLINE_BUFSIZE];
        const char *data = arDecode(v, vbuf, sizeof(vbuf), &vlen);
        if (vlen == sdslen(acc->match_val) &&
            memcmp(data, acc->match_val, vlen) == 0) {
            acc->match_count++;
        }
        return;
    }

    /* Numeric operations */
    long double num;
    int is_int = 0;
    int64_t ival = 0;

    if (arIsInt(v)) {
        ival = arToInt(v);
        num = (long double)ival;
        is_int = 1;
    } else if (arIsFloat(v)) {
        num = (long double)arToDouble(v);
    } else {
        const char *data;
        size_t vlen;
        char smallbuf[8];

        if (arIsSmallStr(v)) {
            vlen = arToSmallStr(v, smallbuf);
            data = smallbuf;
        } else {
            data = arStringData(v);
            vlen = arStringLen(v);
        }

        long long ll;
        if (string2ll(data, vlen, &ll)) {
            ival = ll;
            num = (long double)ll;
            is_int = 1;
        } else {
            long double ld;
            if (string2ld(data, vlen, &ld)) {
                num = ld;
            } else {
                return;
            }
        }
    }

    if (acc->op == AROP_AND || acc->op == AROP_OR || acc->op == AROP_XOR) {
        if (!is_int) {
            /* If it is a float, we only take the integer part. */
            if (isnan(num)) return;
            if (num < (long double)INT64_MIN || num > (long double)INT64_MAX)
                return;
            ival = (int64_t)num;  /* Truncate toward zero. */
        }
        if (!acc->has_int) {
            acc->bitwise_acc = ival;
            acc->has_int = 1;
        } else {
            if (acc->op == AROP_AND) acc->bitwise_acc &= ival;
            else if (acc->op == AROP_OR) acc->bitwise_acc |= ival;
            else acc->bitwise_acc ^= ival;
        }
    } else {
        if (!acc->has_numeric) {
            /* Handle the first element seen for SUM, MIN, MAX. */
            acc->sum_acc = num;
            acc->minmax_acc = num;
            acc->has_numeric = 1;
        } else {
            if (acc->op == AROP_SUM)
                acc->sum_acc += num;
            else if (acc->op == AROP_MIN && num < acc->minmax_acc)
                acc->minmax_acc = num;
            else if (acc->op == AROP_MAX && num > acc->minmax_acc)
                acc->minmax_acc = num;
        }
    }
}

/* AROP key start end OP [arg]
 *
 * Aggregates over existing elements in the requested range, the
 * aggregation performed depends in the "op" argument.
 *
 * Complexity is O(P), where P is visited positions in touched slices
 * (dense scanned slots + sparse entries), with worst-case O(|end-start|+1)
 * and typical case close to O(N), where N is the number of existing
 * elements in range.
 *
 * MATCH and USED count hits. SUM/MIN/MAX ignore values that are not numeric.
 * AND/OR/XOR truncate floats toward zero and ignore values that, after the
 * truncation, cannot be represented as int64_t. */
void aropCommand(client *c) {
    uint64_t start, end;
    if (arrayParseIndexOrReply(c, c->argv[2], &start) != C_OK) return;
    if (arrayParseIndexOrReply(c, c->argv[3], &end) != C_OK) return;

    const char *opstr = c->argv[4]->ptr;
    int op = 0;
    if (!strcasecmp(opstr, "SUM")) op = AROP_SUM;
    else if (!strcasecmp(opstr, "MIN")) op = AROP_MIN;
    else if (!strcasecmp(opstr, "MAX")) op = AROP_MAX;
    else if (!strcasecmp(opstr, "AND")) op = AROP_AND;
    else if (!strcasecmp(opstr, "OR")) op = AROP_OR;
    else if (!strcasecmp(opstr, "XOR")) op = AROP_XOR;
    else if (!strcasecmp(opstr, "MATCH")) op = AROP_MATCH;
    else if (!strcasecmp(opstr, "USED")) op = AROP_USED;
    else {
        addReplyError(c, "unknown operation");
        return;
    }

    sds match_val = NULL;
    if (op == AROP_MATCH) {
        if (c->argc != 6) {
            addReplyError(c, "MATCH requires a value argument");
            return;
        }
        match_val = c->argv[5]->ptr;
    } else if (c->argc != 5) {
        addReplyErrorArity(c);
        return;
    }

    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (o == NULL) {
        if (op == AROP_MATCH || op == AROP_USED) {
            addReplyLongLong(c, 0);
        } else {
            addReplyNull(c);
        }
        return;
    }
    if (checkType(c, o, OBJ_ARRAY)) return;

    redisArray *ar = o->ptr;
    arOpAcc acc = {
        .op = op, .match_val = match_val,
        .sum_acc = 0, .minmax_acc = 0, .bitwise_acc = 0,
        .match_count = 0, .used_count = 0,
        .has_numeric = 0, .has_int = 0
    };
    arScanIter it;
    void *v;

    /* All current AROP operations are order-independent, so iterating the
     * user-provided direction is fine here. */
    arScanIterInit(ar, start, end, &it);
    while (arScanIterNext(&it, NULL, &v))
        arOpAccumulate(&acc, v);

    /* Reply */
    if (op == AROP_MATCH) {
        addReplyLongLong(c, acc.match_count);
    } else if (op == AROP_USED) {
        addReplyLongLong(c, acc.used_count);
    } else if (op == AROP_AND || op == AROP_OR || op == AROP_XOR) {
        if (!acc.has_int) addReplyNull(c);
        else addReplyLongLong(c, acc.bitwise_acc);
    } else {
        if (!acc.has_numeric) {
            addReplyNull(c);
        } else {
            long double result = (op == AROP_SUM) ? acc.sum_acc : acc.minmax_acc;
            char buf[MAX_LONG_DOUBLE_CHARS + 1];
            int len = ld2string(buf, sizeof(buf), result, LD_STR_AUTO);
            addReplyBulkCBuffer(c, buf, len);
        }
    }
}

/* ----------------------------------------------------------------------------
 * The ring buffer family of commands:
 *
 * ARINSERT / ARNEXT / ARSEEK / ARLASTITEMS
 * -------------------------------------------------------------------------- */

/* ARINSERT key value [value ...]
 *
 * Appends one or more values at the private insert cursor in O(N), where N is
 * the number of values. The whole batch fails on index overflow.
 *
 * The cursor is then advanced to the last written index, which is also
 * returned as the command return value, and can be inspected later
 * with ARNEXT. */
void arinsertCommand(client *c) {
    robj *o = lookupArrayForWriteOrReply(c, c->argv[1]);
    if (o == NULL) return;

    redisArray *ar = o->ptr;
    uint64_t old_count = arCount(ar);
    size_t old_alloc = 0;
    if (server.memory_tracking_enabled) old_alloc = kvobjAllocSize(o);
    int num_values = c->argc - 2;

    /* Pre-validate: compute start cursor and check entire batch fits */
    uint64_t start_cursor;
    if (ar->insert_idx == AR_INSERT_IDX_NONE) {
        start_cursor = 0;
    } else {
        if (ar->insert_idx >= UINT64_MAX - 1) {
            addReplyError(c, "insert index overflow");
            return;
        }
        start_cursor = ar->insert_idx + 1;
    }

    /* Check last cursor won't overflow or reach forbidden index. */
    uint64_t last_cursor = start_cursor + (uint64_t)num_values - 1;
    if (last_cursor < start_cursor || last_cursor == UINT64_MAX) {
        addReplyError(c, "insert index overflow");
        return;
    }

    /* Pre-promote sparse slices only for true bulk inserts. A single-element
     * insert does not benefit from the extra range-analysis pass. */
    if (num_values > 1)
        arMayPromoteToDenseForRangeSet(ar, start_cursor, last_cursor);

    /* Apply all values */
    uint64_t cursor = start_cursor;
    for (int i = 0; i < num_values; i++) {
        sds val = c->argv[2 + i]->ptr;
        void *v = arEncode(val, sdslen(val));
        arSet(ar, cursor, v);
        cursor++;
    }
    ar->insert_idx = last_cursor;

    updateKeysizesHist(c->db, OBJ_ARRAY, old_count, arCount(ar));
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, old_alloc, kvobjAllocSize(o));
    keyModified(c, c->db, c->argv[1], o, 1);
    notifyKeyspaceEvent(NOTIFY_ARRAY, "arinsert", c->argv[1], c->db->id);
    server.dirty += num_values;

    addReplyUnsignedLongLong(c, ar->insert_idx);
}

/* Duplicate one array value exactly. Immediate values can be copied as tagged
 * words, while heap strings are re-encoded from their logical string form.
 * This could be regarded as costly, but capturing values out of the existing
 * array would break the sparsearray API isolation. */
static void *arRingDupValue(void *v) {
    if (v == NULL || !arIsPtr(v)) return v;
    return arEncode(arStringData(v), arStringLen(v));
}

/* Return the next slot that ARRING would write to before modulo reduction. */
static uint64_t arRingNextCursor(redisArray *ar) {
    return (ar->insert_idx == AR_INSERT_IDX_NONE) ? 0 : ar->insert_idx + 1;
}

/* Decide if ARRING needs to rebuild the retained logical ring positions before
 * writing new values.
 *
 * We rebuild in only two cases:
 *
 * 1. Shrink: new size is smaller than the current inferred ring span.
 * 2. Grow after wrap: the ring had already wrapped inside the old span, so
 *    without a rebuild the next write would overwrite old low indexes instead
 *    of using the newly added capacity.
 *
 * An explicit ARSEEK 0 is treated differently on grow: it is a direct cursor
 * override saying "write next at index 0", so we honor it instead of forcing
 * a grow-after-wrap repack first.
 *
 * keep_span is the maximum number of logical positions that may be retained. */
static int arRingNeedsRework(redisArray *ar, uint64_t ring_size,
                             uint64_t *old_span, uint64_t *keep_span) {
    *old_span = arLen(ar);
    *keep_span = 0;

    if (*old_span == 0) return 0;

    if (ring_size < *old_span) {
        *keep_span = ring_size;
        return 1;
    }
    if (ring_size == *old_span) {
        return 0;
    }
    if (ar->insert_idx == AR_INSERT_IDX_NONE) {
        return 0;
    }
    if (arRingNextCursor(ar) < *old_span) {
        *keep_span = *old_span;
        return 1;
    }
    return 0;
}

/* Rebuild the retained logical ring positions into a fresh compact array.
 *
 * We walk backward from the current anchor and keep at most keep_span items,
 * but stop as soon as the first NULL is encountered. This makes resize keep
 * the latest contiguous tail of existing items instead of crossing holes.
 *
 * The retained items are replayed in chronological order, oldest to newest,
 * so after the rebuild:
 *
 * - index 0 holds the oldest retained position
 * - index retained_count-1 holds the newest retained position
 * - insert_idx points to retained_count-1, ready for the next ARRING write
 *
 * We use two passes: one backward pass to count the contiguous retained tail,
 * then one forward replay pass into the new array. This avoids any temporary
 * retained-items buffer. */
static redisArray *arRingRework(redisArray *ar, uint64_t old_span,
                                uint64_t keep_span) {
    serverAssert(old_span > 0);
    serverAssert(keep_span > 0);
    serverAssert(keep_span <= old_span);

    redisArray *new_ar = arNew();

    /* The rebuild operates on the inferred ring window [0..old_span-1]. If
     * insert_idx is outside that window because of ARSEEK, fold it back into
     * the current inferred span with modulo. If ARSEEK 0 was used and we are
     * shrinking, anchor the walk at the current tail, just like ARLASTITEMS.
     * Grow does not reach this path because arRingNeedsRework() skips grow
     * rework when insert_idx is AR_INSERT_IDX_NONE. */
    uint64_t anchor_idx = (ar->insert_idx == AR_INSERT_IDX_NONE) ?
        (old_span - 1) : (ar->insert_idx % old_span);

    uint64_t retained_count = 0;
    uint64_t src_idx = anchor_idx;

    while (retained_count < keep_span) {
        void *v = arGet(ar, src_idx);
        if (v == NULL) break; /* This makes any mix of ARSET/SEEK/RING calls
                               * always bound to populatede items, not logical
                               * array span. */

        retained_count++;
        src_idx = (src_idx == 0) ? old_span - 1 : src_idx - 1;
    }

    /* src_idx now points to the position just before the oldest retained
     * item, so advance once to start replaying oldest -> newest. */
    src_idx++;
    if (src_idx == old_span) src_idx = 0;

    for (uint64_t dst_idx = 0; dst_idx < retained_count; dst_idx++) {
        void *v = arGet(ar, src_idx);
        serverAssert(v != NULL);
        arSet(new_ar, dst_idx, arRingDupValue(v));

        src_idx++;
        if (src_idx == old_span) src_idx = 0;
    }
    if (retained_count != 0) new_ar->insert_idx = retained_count - 1;
    return new_ar;
}

/* ARRING key size value [value ...]
 *
 * Writes values into a logical ring buffer. May rework the array if
 * the logical size changes across calls, so that the up to size
 * items are retained in the correct logical position.
 *
 * Complexity is O(M) normally, where M is the number of inserted values,
 * and O(N+M) on resize, where N is the maximum of the old and new ring size.
 * The rebuild stops at the first NULL, so holes cut the retained tail.
 *
 * ARSEEK 0 is still honored as a direct cursor override on grow.
 *
 * Returns the last written slot. */
void arringCommand(client *c) {
    long long ll;
    if (getLongLongFromObjectOrReply(c,c->argv[2],&ll,"invalid size") != C_OK)
        return;
    if (ll <= 0) {
        addReplyError(c, "size must be positive");
        return;
    }
    uint64_t ring_size = (uint64_t)ll;

    robj *o = lookupArrayForWriteOrReply(c, c->argv[1]);
    if (o == NULL) return;

    redisArray *ar = o->ptr;
    uint64_t old_count = arCount(ar);
    size_t old_alloc = 0;
    if (server.memory_tracking_enabled) old_alloc = kvobjAllocSize(o);
    int num_values = c->argc - 3;
    uint64_t cursor = 0;

    /* If the requested size changes the logical ring shape, rebuild once
     * before the hot insertion loop. This makes the command, when the user
     * updates the window, no longer O(M), but O(N+M), however note that this
     * is absolutely needed for high level sane semantics. Users will resize
     * ring buffers, and they want to retain the latest items in a logically
     * correct way. */
    uint64_t old_span, keep_span;
    if (arRingNeedsRework(ar, ring_size, &old_span, &keep_span)) {
        redisArray *new_ar = arRingRework(ar, old_span, keep_span);
        arFree(ar);
        o->ptr = ar = new_ar;
    }

    /* Set the new items, modulo ring size. */
    for (int i = 0; i < num_values; i++) {
        /* Compute the next write position, then wrap it into the requested
         * ring size if needed. By this point any needed resize/rework was
         * already handled above. */
        cursor = arRingNextCursor(ar);
        if (cursor >= ring_size) cursor = cursor % ring_size;

        /* Set the value */
        sds val = c->argv[3 + i]->ptr;
        void *v = arEncode(val, sdslen(val));
        arSet(ar, cursor, v);
        ar->insert_idx = cursor;
    }

    updateKeysizesHist(c->db, OBJ_ARRAY, old_count, arCount(ar));
    if (server.memory_tracking_enabled)
        updateSlotAllocSize(c->db, getKeySlot(c->argv[1]->ptr), o, old_alloc, kvobjAllocSize(o));
    keyModified(c, c->db, c->argv[1], o, 1);
    notifyKeyspaceEvent(NOTIFY_ARRAY, "arring", c->argv[1], c->db->id);
    server.dirty += num_values;

    addReplyUnsignedLongLong(c, cursor);
}

/* ARNEXT key
 *
 * Returns in O(1) the next index that ARINSERT / ARRING would use.
 *
 * Missing keys and the pre-insert state reply with 0. If the cursor is in the
 * terminal state where the next append would overflow, the reply is NULL. */
void arnextCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (o == NULL) {
        addReplyLongLong(c, 0);
        return;
    }
    if (checkType(c, o, OBJ_ARRAY)) return;

    redisArray *ar = o->ptr;
    if (ar->insert_idx == AR_INSERT_IDX_NONE) {
        addReplyLongLong(c, 0);
    } else if (ar->insert_idx == UINT64_MAX - 1) {
        addReplyNull(c);  /* Terminal: index space exhausted */
    } else {
        addReplyUnsignedLongLong(c, ar->insert_idx + 1);
    }
}

/* ARSEEK key idx
 *
 * Sets in O(1) the next index used by ARINSERT and ARRING.
 *
 * Returns 1 if the cursor was updated and 0 if the key does not exist.
 * idx 0 resets the insert state to "next write goes to 0": in this case
 * successive ARRING calls are guaranteed to don't rework the array in chase
 * of logical size change. */
void arseekCommand(client *c) {
    uint64_t idx;
    /* Allow UINT64_MAX because ARSEEK UINT64_MAX sets insert_idx to
     * UINT64_MAX-1, which is a valid terminal state (next ARINSERT
     * would overflow and fail). This is needed for AOF persistence. */
    if (getArrayIndexFromObject(c->argv[2], &idx, 1) != C_OK) {
        addReplyError(c, "invalid array index");
        return;
    }

    /* There aren't many good options for non existing keys: both creating
     * an empty array or failing with "no such key" does not align very
     * well with the Redis commands usual semantics. However we need to signal
     * back that we ignored the index set if the key is not there, so zero
     * is returned. */
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (o == NULL) {
        addReplyLongLong(c, 0);
        return;
    }
    if (checkType(c, o, OBJ_ARRAY)) return;

    redisArray *ar = o->ptr;

    /* Set insert_idx so next ARINSERT writes to idx */
    if (idx == 0) {
        ar->insert_idx = AR_INSERT_IDX_NONE;
    } else {
        ar->insert_idx = idx - 1;
    }

    keyModified(c, c->db, c->argv[1], o, 1);
    notifyKeyspaceEvent(NOTIFY_ARRAY, "arseek", c->argv[1], c->db->id);
    server.dirty++;
    addReplyLongLong(c, 1);
}

/* ARLASTITEMS key count [REV]
 *
 * Returns the most recent positions from the current insert anchor in O(N),
 * where N is the requested count. REV flips the reply order.
 *
 * This command may return NULLs because it walks positions, not only existing
 * items. If ARSEEK 0 was used, the current array tail is used as the anchor. */
void arlastitemsCommand(client *c) {
    long long count;
    if (getLongLongFromObjectOrReply(c, c->argv[2], &count,
        "invalid COUNT") != C_OK) return;

    /* For count <= 0, nothing to return, just an empty array. */
    if (count <= 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    /* Parse REV if provided. */
    int rev = 0;
    if (c->argc == 4) {
        if (strcasecmp(c->argv[3]->ptr, "REV") == 0) {
            rev = 1;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    } else if (c->argc != 3) {
        addReplyErrorArity(c);
        return;
    }

    /* No key? Empty reply. */
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (o == NULL) {
        addReplyArrayLen(c, 0);
        return;
    }
    if (checkType(c, o, OBJ_ARRAY)) return;

    redisArray *ar = o->ptr;
    uint64_t ar_len = arLen(ar);
    uint64_t effective_count =
        (uint64_t)count > ar->count ? ar->count : (uint64_t)count;

    /* Should never happen in practice, because we checked the COUNT before
     * and the array should not be empty to be still a Redis key, so this
     * is mostly a safety net. */
    if (effective_count == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    /* Collect items walking backward from insert_idx. If ARSEEK 0 was used,
     * insert_idx is AR_INSERT_IDX_NONE: in that case use the max set index as
     * the anchor so ARLASTITEMS still reports the tail of the current array.
     *
     * Note that we use an array to collect the items: in the no-REV case
     * otherwise a double scan would be needed. */
    void **collected = zmalloc(effective_count * sizeof(void *));
    uint64_t anchor_idx =
        (ar->insert_idx == AR_INSERT_IDX_NONE) ? ar_len - 1 : ar->insert_idx;
    uint64_t current_idx = anchor_idx;
    uint64_t steps = 0;

    while(steps < effective_count) {
        collected[steps] = arGet(ar, current_idx);
        steps++;

        /* Decrement with wrap */
        if (current_idx == 0) {
            current_idx = ar_len - 1;
        } else {
            current_idx--;
        }
    }

    /* Emit the protocol with the collected items. */
    addReplyArrayLen(c, steps);
    if (rev) {
        /* Return in reverse chronological order (newest first) */
        for (uint64_t i = 0; i < steps; i++)
            addReplyArrayValue(c, collected[i]);
    } else {
        /* Return in chronological order (oldest first) */
        for (int64_t i = steps - 1; i >= 0; i--)
            addReplyArrayValue(c, collected[i]);
    }
    zfree(collected);
}

/* ----------------------------------------------------------------------------
 * ARINFO
 * -------------------------------------------------------------------------- */

/* ARINFO key [FULL]
 *
 * Returns metadata about the array in O(1), or O(N) with FULL where N is the
 * number of slices. Unlike ARLEN and ARCOUNT, a missing key is an error.
 * FULL adds per-encoding slice statistics by scanning the directory. */
void arinfoCommand(client *c) {
    int full = 0;

    if (c->argc > 2) {
        if (c->argc == 3 && !strcasecmp(c->argv[2]->ptr, "full")) {
            full = 1;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (o == NULL) {
        addReplyError(c, "no such key");
        return;
    }
    if (checkType(c, o, OBJ_ARRAY)) return;

    redisArray *ar = o->ptr;

    /* Per-encoding stats (only computed for FULL) */
    uint64_t num_dense = 0;
    uint64_t num_sparse = 0;
    uint64_t dense_total_winsize = 0;
    uint64_t dense_total_count = 0;
    uint64_t sparse_total_cap = 0;

    if (full) {
        if (ar->superdir) {
            for (uint32_t bi = 0; bi < ar->sdir_len; bi++) {
                arSDirEntry *e = ar->superdir + bi;
                for (uint32_t si = 0; si < AR_SUPER_BLOCK_SLOTS; si++) {
                    arSlice *s = e->slots[si];
                    if (!s) continue;
                    if (s->encoding == AR_SLICE_DENSE) {
                        num_dense++;
                        dense_total_winsize += s->layout.dense.winsize;
                        dense_total_count += s->count;
                    } else {
                        num_sparse++;
                        sparse_total_cap += s->layout.sparse.cap;
                    }
                }
            }
        } else {
            for (uint64_t i = 0; i < ar->dir_alloc; i++) {
                arSlice *s = ar->dir[i];
                if (!s) continue;
                if (s->encoding == AR_SLICE_DENSE) {
                    num_dense++;
                    dense_total_winsize += s->layout.dense.winsize;
                    dense_total_count += s->count;
                } else {
                    num_sparse++;
                    sparse_total_cap += s->layout.sparse.cap;
                }
            }
        }
    }

    if (full) {
        addReplyMapLen(c, 12);
    } else {
        addReplyMapLen(c, 7);
    }

    addReplyBulkCString(c, "count");
    addReplyUnsignedLongLong(c, ar->count);

    addReplyBulkCString(c, "len");
    addReplyUnsignedLongLong(c, arLen(ar));

    addReplyBulkCString(c, "next-insert-index");
    if (ar->insert_idx == AR_INSERT_IDX_NONE ||
        ar->insert_idx == UINT64_MAX - 1) {
        addReplyLongLong(c, 0);
    } else {
        addReplyUnsignedLongLong(c, ar->insert_idx + 1);
    }

    addReplyBulkCString(c, "slices");
    addReplyLongLong(c, ar->num_slices);

    addReplyBulkCString(c, "directory-size");
    if (ar->superdir) {
        /* Superdir mode: report allocated capacity */
        addReplyLongLong(c, ar->sdir_cap);
    } else {
        addReplyLongLong(c, ar->dir_alloc);
    }

    addReplyBulkCString(c, "super-dir-entries");
    addReplyLongLong(c, ar->superdir ? ar->sdir_len : 0);

    addReplyBulkCString(c, "slice-size");
    addReplyLongLong(c, ar->slice_size);

    if (full) {
        addReplyBulkCString(c, "dense-slices");
        addReplyLongLong(c, num_dense);

        addReplyBulkCString(c, "sparse-slices");
        addReplyLongLong(c, num_sparse);

        addReplyBulkCString(c, "avg-dense-size");
        if (num_dense > 0) {
            addReplyDouble(c, (double)dense_total_winsize / num_dense);
        } else {
            addReplyDouble(c, 0);
        }

        addReplyBulkCString(c, "avg-dense-fill");
        if (dense_total_winsize > 0) {
            addReplyDouble(c, (double)dense_total_count / dense_total_winsize);
        } else {
            addReplyDouble(c, 0);
        }

        addReplyBulkCString(c, "avg-sparse-size");
        if (num_sparse > 0) {
            addReplyDouble(c, (double)sparse_total_cap / num_sparse);
        } else {
            addReplyDouble(c, 0);
        }
    }
}
