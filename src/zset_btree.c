/* zset_btree.c -- memory efficient sorted set implementation.
 *
 * Copyright (c) 2026-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include "zset_btree.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*-----------------------------------------------------------------------------
 * B+ tree implementation of the low level sorted set API
 *----------------------------------------------------------------------------*/

/* A large sorted set needs to answer two different questions efficiently:
 *
 *  1. Given a member, what is its score?
 *  2. Given a score and member, where is it in the sorted order?
 *
 * The traditional Redis encoding uses a hash table for the first question
 * and a skiplist for the second. This file uses a small hash table and one B+
 * tree. Both are made for this exact pair of indexes, so they share
 * information and omit everything the sorted set does not need.
 *
 * THE SCORE TREE
 * ==============
 *
 * The score tree is ordered by (score, member), with the member breaking ties
 * between equal scores. It owns all member data. The leaves are linked in
 * both directions, so a range starts with one tree search and then reads
 * consecutive memory pages.
 *
 * A score leaf is one allocation with the following layout:
 *
 * +--------+---------------+-------------+------+------------+---------+
 * | header | packed scores | u16 offsets | tags | free space | records |
 * +--------+---------------+-------------+------+------------+---------+
 *
 * A leaf holds at most ZBT_SCORE_LEAF_MAX elements and requests at most 4095
 * bytes from the allocator. Each offset locates a member record relative to
 * the leaf. A parallel byte stores the high eight bits of the member hash. A
 * lookup checks these small tags first and normally reads only the requested
 * member.
 *
 * Records grow backwards from the end of the allocation. Scores and offsets
 * grow forwards. Spare space between them lets common inserts modify the leaf
 * without allocating another one. Short members follow a small length prefix
 * inside the record. Members of 320 bytes or more use a separate allocation;
 * their record holds its pointer, length, and hash. This keeps leaves dense
 * and lets a member keep the same allocation when its score changes.
 *
 * Scores are stored without losing precision. An IEEE double is first changed
 * into an unsigned integer having the same order. A leaf stores one base and
 * bit-packed differences from that base. If every difference has the same
 * low zero bits, the leaf records that shift once and omits those bits from
 * every score. Consecutive integer scores benefit especially from this.
 *
 * Inner score pages contain child pointers, the maximum score below each
 * child, and the number of elements below each child. The counts are what
 * make rank searches possible in the same tree. When maximum scores are
 * equal, the maximum member below the child is read from its last leaf only
 * when needed.
 *
 * THE MEMBER INDEX
 * ================
 *
 * The by-name index is an open addressed hash table. Eight entries share one
 * bucket. Every entry needs only a one byte piece of the member hash and a
 * small number identifying its score leaf. The number normally uses two
 * bytes and grows to four before its range is exhausted. An entry does not
 * store the member pointer, score, complete hash, or a separate allocation.
 *
 * A lookup uses the complete hash to select the first bucket, but reads the
 * eight one byte pieces together. Usually none match and the more expensive
 * leaf access is avoided. On a match, the score leaf number leads to a small
 * packed page where the real member is compared. The one byte value is only a
 * quick rejection test, so hash collisions can cost work but cannot produce
 * a false result.
 *
 * Empty slots have a zero tag. Deleted slots retain their tag and use a
 * reserved leaf number, so searches that passed through them still reach
 * later buckets. A small filter for each starting bucket records which tags
 * may have started there. It lets most missing-member lookups stop before
 * walking a nearly full table. The filter may say "perhaps", never "present".
 *
 * The table is resized incrementally. It moves one complete score leaf at a
 * time. Inline members provide their full hashes, while external records keep
 * the hash beside the pointer so a long member is not read again. The active
 * resize has a non-zero number. A leaf records that number after its entries
 * are copied, which tells lookups which of the two tables contains its current
 * entries.
 *
 * SCORE LEAF IDs
 * ==============
 *
 * Hash entries refer to score leaves by a small number, not by a pointer.
 * The score_leaf_by_id array translates the number to the current address.
 * This matters because adding, deleting, compacting, or defragmenting a score
 * leaf can move its allocation. A replacement keeps the old ID, a split keeps
 * it on the left page and gives the right page a new one, and a merge keeps
 * the left page's ID. Only members that move to another leaf need their member
 * record changed.
 *
 * Released IDs form a free list inside score_leaf_by_id itself. A marked
 * pointer stores the next free ID. Real allocations are aligned, so their low
 * bit is zero and can never be confused with a free-list link.
 *
 * PAGE CHANGES AND TEMPORARY POINTERS
 * ===================================
 *
 * A failed ZADD lookup records its empty hash slot, but score-tree work before
 * insertion may advance a resize and move the table. Index changes update
 * member_revision whenever a saved slot address can become stale; the
 * position is used only if its revision still matches.
 *
 * The same rule applies to callers: iterators and member pointers returned by
 * this file belong to the score tree. They are valid only until the next
 * sorted set modification and must not be retained across one.
 *
 * Score leaves split according to both element count and byte size. Small
 * leaves are joined with a neighbor when the result fits. Inner pages are
 * removed when empty and a one-child root is collapsed; otherwise sparse
 * inner pages are left alone. Their high fanout keeps their number small.
 *
 * Long members have one owner: the leaf record that points to them. When a
 * leaf is rebuilt, the new record takes the pointer and clears it in the old
 * record. Freeing the old leaf then releases only members that were deleted.
 * COPY makes independent allocations, and active defragmentation moves each
 * allocation and repairs its one pointer.
 */

/* Page limits. Score leaves have both a count limit and a byte limit because
 * they contain variable length records. Keeping a leaf below 4 KB bounds the
 * bytes moved by an edit and keeps its offsets small. A newly built non-full
 * leaf asks for a little spare room, bounded by the same limit. */
#define ZBT_SCORE_LEAF_MAX 96
#define ZBT_SCORE_LEAF_BYTES 4095
#define ZBT_SCORE_LEAF_RESERVE 128
#define ZBT_SCORE_INNER_MAX 32
#define ZBT_SCORE_LEAF_MERGE (ZBT_SCORE_LEAF_MAX / 2)
/* Member lookup uses an open addressed table with eight slots per bucket. */
#define ZBT_INDEX_BUCKET_ITEMS 8
#define ZBT_INDEX_INITIAL_BUCKETS 4
#define ZBT_INDEX_MAX_LOAD_NUM 31
#define ZBT_INDEX_MAX_LOAD_DEN 32
#define ZBT_INDEX_MIN_LOAD_NUM 1
#define ZBT_INDEX_MIN_LOAD_DEN 8
#define ZBT_INDEX_MAX_FILLED_NUM 63
#define ZBT_INDEX_MAX_FILLED_DEN 64
#define ZBT_INDEX_DELETED_ID UINT32_MAX
#define ZBT_INDEX_WIDE_ID_AT (UINT16_MAX / 2)

/* UINT32_MAX asks for a new leaf ID or marks the absence of a leaf. Use the
 * name that describes the meaning at each call site. */
#define ZBT_NEW_LEAF_ID UINT32_MAX
#define ZBT_NO_LEAF_ID UINT32_MAX
/* Longer members get their own allocation. This keeps score leaves dense and
 * avoids copying large strings whenever a leaf changes shape. */
#define ZBT_EXTERNAL_MEMBER_AT 320

#define ZBT_PACKED_SHORT 254
#define ZBT_PACKED_EXTERNAL 255
#define ZBT_EXTERNAL_HASH_OFFSET 1
#define ZBT_EXTERNAL_LENGTH_OFFSET \
    (ZBT_EXTERNAL_HASH_OFFSET + sizeof(uint32_t))
#define ZBT_EXTERNAL_POINTER_OFFSET \
    (ZBT_EXTERNAL_LENGTH_OFFSET + sizeof(uint64_t))
#define ZBT_EXTERNAL_RECORD_BYTES \
    (ZBT_EXTERNAL_POINTER_OFFSET + sizeof(void *))

/* One ZSCAN cursor position covers this many buckets. This keeps the complete
 * 32 bit table revision while allowing 2^35 buckets to be visited. */
#define ZBT_SCAN_BUCKETS_PER_STEP 8

/* Released score leaf IDs form a list in the leaf table. Allocations are
 * aligned, so the low bit distinguishes a list link from a live pointer. */
#define ZBT_FREE_LEAF_ID(next) \
    ((zbtScoreLeaf *)((((uintptr_t)(next)) << 1) | 1))
#define ZBT_IS_FREE_LEAF_ID(ptr) (((uintptr_t)(ptr) & 1) != 0)
#define ZBT_NEXT_FREE_LEAF_ID(ptr) ((uint32_t)((uintptr_t)(ptr) >> 1))

/* ----------------------------- Page headers ----------------------------- */

typedef struct zbtScoreNode zbtScoreNode;
typedef struct zbtScoreLeaf zbtScoreLeaf;
typedef struct zbtScoreInner zbtScoreInner;

struct zbtScoreNode {
    zbtScoreInner *parent;   /* NULL only for the root. */
    uint64_t subtree;        /* Elements represented by this node. */
    uint16_t parent_index;   /* Our position in parent->child[]. */
    uint8_t count;           /* Elements in a leaf, children in an inner node. */
    uint8_t isleaf;          /* Non-zero for zbtScoreLeaf. */
    /* Only score leaves use these. They live in this header's padding so that
     * a score leaf still costs 56 bytes before its packed arrays. */
    uint8_t score_shift;
    uint8_t has_external;
    uint16_t index_resize;   /* Resize that copied this leaf, zero if none. */
};

/* The data[] array starts with packed scores and offsets. Member records
 * occupy the end and record_start marks their first used byte. */
struct zbtScoreLeaf {
    zbtScoreNode n;
    zbtScoreLeaf *prev;
    zbtScoreLeaf *next;
    uint64_t score_base;
    uint32_t id;
    uint8_t score_bits;
    uint8_t reversed;
    uint16_t record_start;
    unsigned char data[];
};

/* max_score[] and child_count[] describe the child at the same index. */
struct zbtScoreInner {
    zbtScoreNode n;
    zbtScoreNode *child[ZBT_SCORE_INNER_MAX];
    uint64_t child_count[ZBT_SCORE_INNER_MAX];
    double max_score[ZBT_SCORE_INNER_MAX];
};

/* Most sets use 16 bit leaf numbers, making one bucket 32 bytes. The small
 * home_tags summary rejects absent members before a table search. Keeping it
 * beside the bucket also puts both checks in the same cache line. Very large
 * sets switch incrementally to the 48 byte form before 16 bit IDs fill up. */
typedef struct zbtIndexBucket16 {
    uint64_t tags;
    uint64_t home_tags;
    uint16_t id[ZBT_INDEX_BUCKET_ITEMS];
} zbtIndexBucket16;

typedef struct zbtIndexBucket32 {
    uint64_t tags;
    uint64_t home_tags;
    uint32_t id[ZBT_INDEX_BUCKET_ITEMS];
} zbtIndexBucket32;

typedef union zbtIndexBucket {
    zbtIndexBucket16 narrow;
    zbtIndexBucket32 wide;
} zbtIndexBucket;

typedef struct zbtIndexTable {
    zbtIndexBucket *buckets;
    unsigned long size;       /* Number of buckets, always a power of two. */
    unsigned long used;       /* Live entries. */
    unsigned long filled;     /* Live entries plus tombstones. */
    int wide_ids;             /* IDs are 32 rather than 16 bits. */
    uint32_t scan_revision;   /* Identity used by scan cursors. */
} zbtIndexTable;

typedef struct zbtIndexRehash {
    zbtIndexTable table;       /* Table being built. */
    uint32_t next_leaf_id;     /* Where the next copy step resumes. */
    uint16_t resize_id;        /* Non-zero number of this resize. */
} zbtIndexRehash;

struct zbtreeSet {
    zbtScoreNode *score_root;
    zbtScoreLeaf *score_first;
    zbtScoreLeaf *score_last;
    zbtIndexTable member_index;
    zbtIndexRehash *member_rehash;

    /*
     * Hash entries store these small numbers instead of pointers. A score
     * leaf can then move during compaction without changing its member
     * entries. Released numbers form a free list inside this same array.
     */
    zbtScoreLeaf **score_leaf_by_id;
    uint32_t score_leaf_cap;
    uint32_t next_score_leaf_id;
    uint32_t free_score_leaf_id;
    uint32_t member_revision; /* Invalidates saved hash slots. */
    unsigned long length;  /* Number of sorted set members. */
    size_t alloc_size;     /* Usable bytes of all allocations, including us. */
};

/* Temporary view used while a score leaf is being built. An external member
 * may already belong to an old record. The builder moves that ownership to
 * the new record instead of copying the member bytes. */
typedef struct {
    const unsigned char *ptr;
    size_t len;
    uint32_t hash;
    unsigned char *external;
    unsigned char *owner_record;
} zbtBuildElement;

/* ------------------------- Packed score leaves -------------------------- */

/* Turn an IEEE double into an unsigned integer with the same ordering. This
 * lets us subtract nearby scores and still reconstruct every bit exactly.
 * NaN never reaches this encoding: the sorted set command layer rejects it. */
static uint64_t zbtScoreToOrdered(double score) {
    uint64_t bits;
    memcpy(&bits, &score, sizeof(bits));
    if (bits & (UINT64_C(1) << 63)) return ~bits;
    return bits ^ (UINT64_C(1) << 63);
}

/* Inverse of zbtScoreToOrdered(). */
static double zbtOrderedToScore(uint64_t ordered) {
    uint64_t bits;
    double score;
    if (ordered & (UINT64_C(1) << 63))
        bits = ordered ^ (UINT64_C(1) << 63);
    else
        bits = ~ordered;
    memcpy(&score, &bits, sizeof(score));
    return score;
}

/* Return the number of bits needed to store 'value'. */
static unsigned int zbtBitsNeeded(uint64_t value) {
    if (value == 0) return 0;
    return 64 - __builtin_clzll(value);
}

/* Eight spare bytes let the bit readers use one unaligned 64 bit load even
 * for the last score. The final rounding keeps the offset array aligned. */
static size_t zbtScoreLeafScoreBytes(unsigned int count,
                                     unsigned int score_bits)
{
    if (score_bits == 0) return 0;
    size_t bytes = ((size_t)count * score_bits + 7) / 8;
    return (bytes + 8 + 1) & ~(size_t)1;
}

/* Return the offset array that follows the packed scores. */
static inline uint16_t *zbtScoreLeafOffsets(zbtScoreLeaf *leaf) {
    return (uint16_t *)(leaf->data +
        zbtScoreLeafScoreBytes(leaf->n.count, leaf->score_bits));
}

/* Return the hash-tag array that follows the offsets. */
static inline uint8_t *zbtScoreLeafHashTags(zbtScoreLeaf *leaf) {
    return (uint8_t *)(zbtScoreLeafOffsets(leaf) + leaf->n.count);
}

/* Left-edge leaves keep their arrays in reverse physical order. Logical
 * positions remain sorted, but prepending then inserts at the physical end
 * and does not move the existing packed scores or offsets. */
static inline unsigned int zbtScoreLeafPhysicalPos(zbtScoreLeaf *leaf,
                                                   unsigned int pos)
{
    return leaf->reversed ? leaf->n.count - pos - 1 : pos;
}

/* Return the member record offset stored at logical position 'pos'. */
static inline uint16_t zbtScoreLeafOffset(zbtScoreLeaf *leaf,
                                          unsigned int pos)
{
    return zbtScoreLeafOffsets(leaf)[zbtScoreLeafPhysicalPos(leaf, pos)];
}

/* The packed arrays form one bit stream: bit p lives in byte p/8 at bit p%8.
 * A word is therefore loaded and stored in little endian order whatever the
 * host is. Without that, a value written through one byte offset lands on
 * top of a value written through another, because a big endian load places
 * the first byte in the most significant position. These conversions compile
 * away on a little endian build.
 */
static uint64_t zbtReadBits(const unsigned char *data, size_t bitpos,
                            unsigned int width)
{
    size_t byte = bitpos / 8;
    unsigned int shift = bitpos & 7;
    unsigned int lowbits = width < 64 - shift ? width : 64 - shift;
    uint64_t word, value;

    memcpy(&word, data + byte, sizeof(word));
    value = intrev64ifbe(word) >> shift;
    if (width > lowbits)
        value |= (uint64_t)data[byte + 8] << lowbits;
    if (width < 64)
        value &= (UINT64_C(1) << width) - 1;
    return value;
}

/* Replace one field in the packed bit stream. */
static void zbtWriteBits(unsigned char *data, size_t bitpos,
                         unsigned int width, uint64_t value)
{
    size_t byte = bitpos / 8;
    unsigned int shift = bitpos & 7;
    unsigned int lowbits = width < 64 - shift ? width : 64 - shift;
    uint64_t mask = lowbits == 64 ? UINT64_MAX :
                    (UINT64_C(1) << lowbits) - 1;
    uint64_t word;

    memcpy(&word, data + byte, sizeof(word));
    word = intrev64ifbe(word);
    word &= ~(mask << shift);
    word |= (value & mask) << shift;
    word = intrev64ifbe(word);
    memcpy(data + byte, &word, sizeof(word));

    if (width > lowbits) {
        unsigned int highbits = width - lowbits;
        unsigned char highmask = (1U << highbits) - 1;
        data[byte + 8] = (data[byte + 8] & ~highmask) |
                         ((value >> lowbits) & highmask);
    }
}

/* Insert one field in the packed score stream. Copy backwards because source
 * and destination overlap. leaf->n.count is still the old count. */
static void zbtInsertScoreBits(zbtScoreLeaf *leaf, unsigned int pos,
                               uint64_t value)
{
    unsigned int bits = leaf->score_bits;
    if (bits == 0) return;

    size_t insert_at = (size_t)pos * bits;
    size_t remaining = (size_t)(leaf->n.count - pos) * bits;
    while (remaining) {
        unsigned int chunk = remaining > 64 ? 64 : remaining;
        size_t source = insert_at + remaining - chunk;
        uint64_t part = zbtReadBits(leaf->data, source, chunk);
        zbtWriteBits(leaf->data, source + bits, chunk, part);
        remaining -= chunk;
    }
    zbtWriteBits(leaf->data, insert_at, bits, value);
}

/* Delete one field from the packed score stream. Copy forwards because source
 * and destination overlap. The final field is cleared because later writes
 * add bits to a field that is expected to start at zero. */
static void zbtDeleteScoreBits(zbtScoreLeaf *leaf, unsigned int pos) {
    unsigned int bits = leaf->score_bits;
    if (bits == 0) return;

    size_t destination = (size_t)pos * bits;
    size_t remaining = (size_t)(leaf->n.count - pos - 1) * bits;
    size_t moved = 0;
    while (moved < remaining) {
        unsigned int chunk = remaining - moved > 64 ?
                             64 : remaining - moved;
        uint64_t part = zbtReadBits(leaf->data,
                                    destination + bits + moved, chunk);
        zbtWriteBits(leaf->data, destination + moved, chunk, part);
        moved += chunk;
    }
    zbtWriteBits(leaf->data, (size_t)(leaf->n.count - 1) * bits,
                 bits, 0);
}

/* Write one score into a zeroed packed field. The base, shift and width must
 * already have been selected by zbtScoreEncoding(). */
static void zbtScoreLeafWriteScore(zbtScoreLeaf *leaf, unsigned int pos,
                                   double score)
{
    unsigned int bits = leaf->score_bits;
    if (bits == 0) return;

    uint64_t ordered = zbtScoreToOrdered(score);
    uint64_t delta = leaf->reversed ? leaf->score_base - ordered :
                                       ordered - leaf->score_base;
    delta >>= leaf->n.score_shift;
    size_t bitpos = (size_t)zbtScoreLeafPhysicalPos(leaf, pos) * bits;
    size_t byte = bitpos / 8;
    unsigned int shift = bitpos & 7;
    uint64_t word;

    memcpy(&word, leaf->data + byte, sizeof(word));
    word = intrev64ifbe(intrev64ifbe(word) | (delta << shift));
    memcpy(leaf->data + byte, &word, sizeof(word));
    if (shift && bits > 64 - shift)
        leaf->data[byte + 8] |= delta >> (64 - shift);
}

/* Decode the score at logical position 'pos'. */
static double zbtScoreLeafScore(zbtScoreLeaf *leaf, unsigned int pos) {
    unsigned int bits = leaf->score_bits;
    uint64_t delta = 0;

    if (bits != 0) {
        size_t bitpos = (size_t)zbtScoreLeafPhysicalPos(leaf, pos) * bits;
        size_t byte = bitpos / 8;
        unsigned int shift = bitpos & 7;
        memcpy(&delta, leaf->data + byte, sizeof(delta));
        delta = intrev64ifbe(delta) >> shift;
        if (shift && bits > 64 - shift)
            delta |= (uint64_t)leaf->data[byte + 8] << (64 - shift);
        if (bits < 64)
            delta &= (UINT64_C(1) << bits) - 1;
        delta <<= leaf->n.score_shift;
    }
    uint64_t ordered = leaf->reversed ? leaf->score_base - delta :
                                         leaf->score_base + delta;
    return zbtOrderedToScore(ordered);
}

/* Return the raw hash byte stored with a member. */
static inline uint8_t zbtScoreLeafTag(zbtScoreLeaf *leaf, unsigned int pos) {
    return zbtScoreLeafHashTags(leaf)[zbtScoreLeafPhysicalPos(leaf, pos)];
}

/* ---------------------------- Packed members ---------------------------- */

/* Most members live directly in the leaf and have only a one byte length for
 * the common case. Long members live in their own allocation. Their leaf
 * record keeps the length, hash, and pointer needed to find and own them.
 *
 *   short inline member:   length byte, member bytes
 *   longer inline member:  byte 254, uint16 length, member bytes
 *   external member:       byte 255, uint32 hash, uint64 length, pointer
 */
static size_t zbtPackedLengthBytes(size_t len) {
    return len < ZBT_PACKED_SHORT ? 1 : 3;
}

/* Write an inline member into a score leaf. */
static void zbtPackedWriteInline(unsigned char *dst,
                                 const unsigned char *src, size_t len)
{
    serverAssert(len < ZBT_EXTERNAL_MEMBER_AT && len <= UINT16_MAX);
    if (len < ZBT_PACKED_SHORT) {
        dst[0] = len;
        dst++;
    } else {
        uint16_t shortlen = len;
        dst[0] = ZBT_PACKED_SHORT;
        memcpy(dst + 1, &shortlen, sizeof(shortlen));
        dst += 3;
    }
    memcpy(dst, src, len);
}

/* These fields are deliberately unaligned. Access them through memcpy so the
 * records remain tightly packed on every supported CPU. */
static uint32_t zbtExternalHash(const unsigned char *record) {
    uint32_t hash;
    serverAssert(record[0] == ZBT_PACKED_EXTERNAL);
    memcpy(&hash, record + ZBT_EXTERNAL_HASH_OFFSET, sizeof(hash));
    return hash;
}

static size_t zbtExternalLength(const unsigned char *record) {
    uint64_t len;
    serverAssert(record[0] == ZBT_PACKED_EXTERNAL);
    memcpy(&len, record + ZBT_EXTERNAL_LENGTH_OFFSET, sizeof(len));
    serverAssert(len <= SIZE_MAX);
    return (size_t)len;
}

static unsigned char *zbtExternalPointer(const unsigned char *record) {
    unsigned char *ptr;
    serverAssert(record[0] == ZBT_PACKED_EXTERNAL);
    memcpy(&ptr, record + ZBT_EXTERNAL_POINTER_OFFSET, sizeof(ptr));
    return ptr;
}

static void zbtExternalSetPointer(unsigned char *record, unsigned char *ptr) {
    serverAssert(record[0] == ZBT_PACKED_EXTERNAL);
    memcpy(record + ZBT_EXTERNAL_POINTER_OFFSET, &ptr, sizeof(ptr));
}

/* Return a member and store its length in 'len'. */
static const unsigned char *zbtPackedData(const unsigned char *record,
                                          size_t *len)
{
    if (record[0] < ZBT_PACKED_SHORT) {
        *len = record[0];
        return record + 1;
    } else if (record[0] == ZBT_PACKED_SHORT) {
        uint16_t shortlen;
        memcpy(&shortlen, record + 1, sizeof(shortlen));
        *len = shortlen;
        return record + 3;
    } else {
        unsigned char *ptr = zbtExternalPointer(record);
        serverAssert(ptr != NULL);
        *len = zbtExternalLength(record);
        return ptr;
    }
}

/* Return the record at logical position 'pos'. */
static inline unsigned char *zbtScoreLeafRecord(zbtScoreLeaf *leaf,
                                                unsigned int pos)
{
    return (unsigned char *)leaf + zbtScoreLeafOffset(leaf, pos);
}

/* Return the member at logical position 'pos'. */
static inline const unsigned char *zbtScoreLeafElement(zbtScoreLeaf *leaf,
                                                       unsigned int pos,
                                                       size_t *len)
{
    return zbtPackedData(zbtScoreLeafRecord(leaf, pos), len);
}

/* Return the complete member hash. External records keep it because hashing a
 * long member again during a split or table resize would make that small
 * maintenance step depend on the member size. */
static uint32_t zbtScoreLeafHash(zbtScoreLeaf *leaf, unsigned int pos) {
    unsigned char *record = zbtScoreLeafRecord(leaf, pos);
    if (record[0] == ZBT_PACKED_EXTERNAL)
        return zbtExternalHash(record);
    size_t len;
    const unsigned char *ele = zbtPackedData(record, &len);
    return (uint32_t)dictGenHashFunction(ele, len);
}

/* Fill a build entry from a live leaf. The old record remains the owner until
 * a new record adopts its external pointer. */
static void zbtBuildElementFromLeaf(zbtBuildElement *ele,
                                    zbtScoreLeaf *leaf, unsigned int pos)
{
    unsigned char *record = zbtScoreLeafRecord(leaf, pos);
    ele->ptr = zbtPackedData(record, &ele->len);
    if (record[0] == ZBT_PACKED_EXTERNAL) {
        ele->hash = zbtExternalHash(record);
        ele->external = (unsigned char *)ele->ptr;
        ele->owner_record = record;
    } else {
        debugServerAssert(ele->len < ZBT_EXTERNAL_MEMBER_AT);
        ele->hash = 0;
        ele->external = NULL;
        ele->owner_record = NULL;
    }
}

/* Fill a build entry from bytes supplied by the caller. */
static void zbtBuildElementFromBytes(zbtBuildElement *ele,
                                     const unsigned char *ptr, size_t len,
                                     uint32_t hash)
{
    ele->ptr = ptr;
    ele->len = len;
    ele->hash = hash;
    ele->external = NULL;
    ele->owner_record = NULL;
}

/* Compare two members without constructing temporary SDS strings. */
static int zbtCompareElements(const unsigned char *a, size_t alen,
                              const unsigned char *b, size_t blen)
{
    size_t minlen = alen < blen ? alen : blen;
    int cmp = memcmp(a, b, minlen);
    if (cmp != 0) return cmp;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

/* Compare two sorted-set entries by score and then by member. */
static inline int zbtScoreCompare(double score,
                                  const unsigned char *ele, size_t elelen,
                                  double other_score,
                                  const unsigned char *other_ele,
                                  size_t other_elelen)
{
    if (score < other_score) return -1;
    if (score > other_score) return 1;
    return zbtCompareElements(ele, elelen, other_ele, other_elelen);
}

/* ---------------------- Allocation and score leaf IDs ------------------- */

/* Allocate memory and add the allocator's usable size to the exact object
 * accounting returned by MEMORY USAGE. */
static void *zbtAlloc(zbtreeSet *zs, size_t bytes) {
    size_t usable;
    void *ptr = zmalloc_usable(bytes, &usable);
    zs->alloc_size += usable;
    return ptr;
}

/* Cached positions and scan cursors use these numbers to notice that a member
 * table changed. A process-wide counter also distinguishes a deleted set from
 * a new one that happens to reuse the same address. */
static uint32_t zbtIndexNextRevision(void) {
    static uint32_t revision = 0;
    if (++revision == 0) revision++;
    return revision;
}

/* Free one accounted allocation. */
static void zbtFreeAllocation(zbtreeSet *zs, void *ptr) {
    size_t usable;
    zfree_usable(ptr, &usable);
    zs->alloc_size -= usable;
}

/* Return true when a build entry already refers to an external allocation or
 * is long enough that a new one should be made. Existing external records
 * retain their form when a leaf is rebuilt. */
static int zbtBuildElementIsExternal(const zbtBuildElement *ele) {
    return ele->external != NULL || ele->len >= ZBT_EXTERNAL_MEMBER_AT;
}

/* Return the leaf bytes needed by one member record. */
static size_t zbtElementStorageBytes(const zbtBuildElement *ele) {
    if (zbtBuildElementIsExternal(ele)) return ZBT_EXTERNAL_RECORD_BYTES;
    return zbtPackedLengthBytes(ele->len) + ele->len;
}

/* Return the bytes occupied by an existing record without following an
 * external pointer. This is used while deleting the record itself. */
static size_t zbtRecordStorageBytes(const unsigned char *record) {
    if (record[0] == ZBT_PACKED_EXTERNAL)
        return ZBT_EXTERNAL_RECORD_BYTES;
    size_t len;
    zbtPackedData(record, &len);
    return zbtPackedLengthBytes(len) + len;
}

/* Write one build entry into a leaf. Existing external allocations move from
 * the old record to the new one: clearing the old pointer makes ownership
 * visible and lets the normal leaf-free path distinguish moved members from
 * deleted ones. */
static int zbtBuildElementWrite(zbtreeSet *zs, unsigned char *record,
                                zbtBuildElement *ele)
{
    if (!zbtBuildElementIsExternal(ele)) {
        zbtPackedWriteInline(record, ele->ptr, ele->len);
        return 0;
    }

    unsigned char *external = ele->external;
    if (external == NULL) {
        external = zbtAlloc(zs, ele->len);
        memcpy(external, ele->ptr, ele->len);
    }

    uint64_t len = ele->len;
    record[0] = ZBT_PACKED_EXTERNAL;
    memcpy(record + ZBT_EXTERNAL_HASH_OFFSET, &ele->hash, sizeof(ele->hash));
    memcpy(record + ZBT_EXTERNAL_LENGTH_OFFSET, &len, sizeof(len));
    memcpy(record + ZBT_EXTERNAL_POINTER_OFFSET, &external, sizeof(external));

    if (ele->owner_record) {
        debugServerAssert(zbtExternalPointer(ele->owner_record) == external);
        zbtExternalSetPointer(ele->owner_record, NULL);
    }
    ele->external = NULL;
    ele->owner_record = NULL;
    return 1;
}

/* Detach an external member before removing its old record. The caller can
 * then reinsert the same allocation after a score change without copying the
 * member bytes. */
static void zbtBuildElementDetach(zbtBuildElement *ele) {
    if (ele->owner_record == NULL) return;
    debugServerAssert(zbtExternalPointer(ele->owner_record) == ele->external);
    zbtExternalSetPointer(ele->owner_record, NULL);
    ele->owner_record = NULL;
}

/* Free a leaf and every external member it still owns. A record copied into a
 * replacement leaf has a NULL pointer here because the builder cleared it.
 * Records for deleted members remain non-NULL and are released now. */
static void zbtScoreLeafFreeOwned(zbtreeSet *zs, zbtScoreLeaf *leaf) {
    if (leaf->n.has_external) {
        for (unsigned int i = 0; i < leaf->n.count; i++) {
            unsigned char *record = zbtScoreLeafRecord(leaf, i);
            if (record[0] != ZBT_PACKED_EXTERNAL) continue;
            unsigned char *external = zbtExternalPointer(record);
            if (external) zbtFreeAllocation(zs, external);
        }
    }
    zbtFreeAllocation(zs, leaf);
}

/* Give 'leaf' an ID and publish its address in score_leaf_by_id. Passing
 * ZBT_NEW_LEAF_ID first reuses a released ID, then grows the table if all
 * existing IDs are busy. Passing an existing ID replaces its address. */
static void zbtScoreRegisterLeaf(zbtreeSet *zs, zbtScoreLeaf *leaf, uint32_t id) {
    if (id == ZBT_NEW_LEAF_ID) {
        /* Old hash entries remain in place during a resize. Do not reuse a
         * released ID until that old table is gone, or a stale entry could
         * appear to refer to an unrelated new leaf. */
        if (zs->free_score_leaf_id && zs->member_rehash == NULL) {
            id = zs->free_score_leaf_id - 1;
            serverAssert(ZBT_IS_FREE_LEAF_ID(zs->score_leaf_by_id[id]));
            zs->free_score_leaf_id =
                ZBT_NEXT_FREE_LEAF_ID(zs->score_leaf_by_id[id]);
        } else {
            id = zs->next_score_leaf_id++;
            if (id == zs->score_leaf_cap) {
                uint32_t newcap = zs->score_leaf_cap ?
                                  zs->score_leaf_cap * 2 : 16;
                size_t usable, old_usable = 0;
                zs->score_leaf_by_id = zrealloc_usable(
                    zs->score_leaf_by_id,
                    newcap * sizeof(*zs->score_leaf_by_id),
                    &usable, &old_usable);
                memset(zs->score_leaf_by_id + zs->score_leaf_cap, 0,
                       (newcap - zs->score_leaf_cap) *
                       sizeof(*zs->score_leaf_by_id));
                zs->score_leaf_cap = newcap;
                zs->alloc_size += usable - old_usable;
            }
        }
    }
    leaf->id = id;
    zs->score_leaf_by_id[id] = leaf;
}

/* Return an ID to the in-table free list. free_score_leaf_id stores ID+1 so
 * zero can mean that the list is empty. */
static void zbtScoreReleaseLeafId(zbtreeSet *zs, uint32_t id) {
    serverAssert(id < zs->next_score_leaf_id);
    zs->score_leaf_by_id[id] =
        ZBT_FREE_LEAF_ID(zs->free_score_leaf_id);
    zs->free_score_leaf_id = id + 1;
}

/* --------------------------- Score tree pages --------------------------- */

/* Return the final member below a score node. Inner nodes deliberately store
 * only maximum scores; this walk supplies the tie breaker when equal scores
 * make it necessary. */
static const unsigned char *zbtScoreNodeMaxElement(zbtScoreNode *node,
                                                   size_t *len)
{
    while (!node->isleaf) {
        zbtScoreInner *inner = (zbtScoreInner *)node;
        node = inner->child[inner->n.count - 1];
    }
    zbtScoreLeaf *leaf = (zbtScoreLeaf *)node;
    return zbtScoreLeafElement(leaf, leaf->n.count - 1, len);
}

/* Return the greatest score below one score-tree node. */
static double zbtScoreNodeMaxScore(zbtScoreNode *node) {
    if (node->isleaf) {
        zbtScoreLeaf *leaf = (zbtScoreLeaf *)node;
        return zbtScoreLeafScore(leaf, leaf->n.count - 1);
    }
    zbtScoreInner *inner = (zbtScoreInner *)node;
    return inner->max_score[inner->n.count - 1];
}

/* Recompute all derived fields of one score inner page. Besides maximums
 * and counts, this repairs each child's parent pointer and parent position.
 * Call it after the child array has been rearranged. */
static void zbtScoreRefreshNode(zbtScoreInner *inner) {
    uint64_t total = 0;
    for (unsigned int i = 0; i < inner->n.count; i++) {
        zbtScoreNode *child = inner->child[i];
        child->parent = inner;
        child->parent_index = i;
        inner->child_count[i] = child->subtree;
        inner->max_score[i] = zbtScoreNodeMaxScore(child);
        total += child->subtree;
    }
    inner->n.subtree = total;
}

/* Recompute complete inner pages from 'node' up to the root. This is used
 * after a reshape, where several child positions may have changed. */
static void zbtScoreRefreshParents(zbtScoreNode *node) {
    while (node->parent) {
        zbtScoreInner *parent = node->parent;
        zbtScoreRefreshNode(parent);
        node = &parent->n;
    }
}

/* Update only the path fields affected by a local leaf edit. length_change is
 * +1 for insertion, -1 for deletion, and zero for a score change. */
static void zbtScoreUpdatePath(zbtScoreNode *node, long long length_change) {
    while (node->parent) {
        zbtScoreInner *parent = node->parent;
        unsigned int pos = node->parent_index;
        double max = zbtScoreNodeMaxScore(node);
        /* An in place score change that leaves the node maximum alone leaves
         * every page above it alone too. */
        if (length_change == 0 && parent->max_score[pos] == max) return;
        parent->child_count[pos] = node->subtree;
        parent->max_score[pos] = max;
        parent->n.subtree += length_change;
        node = &parent->n;
    }
}

/* Scores that share low bits waste those bits in every packed difference.
 * Integer scores are the common example: consecutive integers of the same
 * magnitude are many ulps apart, so their ordered forms all end in the same
 * long run of zero bits. The leaf records that run once as a shift and stores
 * only the bits above it. Nothing is lost, because a difference is a multiple
 * of 2^shift exactly when every score agrees on those low bits.
 */
static void zbtScoreEncoding(unsigned int count, double *scores, int desc,
                             uint64_t *base, uint8_t *bits, uint8_t *shift)
{
    uint64_t first = zbtScoreToOrdered(scores[0]);
    uint64_t min = first, max = first, differing = 0;
    for (unsigned int i = 1; i < count; i++) {
        uint64_t ordered = zbtScoreToOrdered(scores[i]);
        differing |= ordered ^ first;
        if (ordered < min) min = ordered;
        if (ordered > max) max = ordered;
    }
    *base = desc ? max : min;
    if (differing == 0) {
        *bits = 0;
        *shift = 0;
        return;
    }
    *shift = __builtin_ctzll(differing);
    *bits = zbtBitsNeeded((max - min) >> *shift);
}

/* Return the smallest leaf allocation that holds the supplied arrays. */
static size_t zbtScoreLeafRequestBytes(unsigned int count, double *scores,
                                       zbtBuildElement *eles)
{
    uint64_t base;
    uint8_t bits, shift;
    /* The shift and the number of packed bits do not depend on the base, so
     * an ascending encoding gives the size of a reversed leaf as well. */
    zbtScoreEncoding(count, scores, 0, &base, &bits, &shift);
    size_t bytes = offsetof(zbtScoreLeaf, data) +
                   zbtScoreLeafScoreBytes(count, bits) +
                   count * (sizeof(uint16_t) + sizeof(uint8_t));
    for (unsigned int i = 0; i < count; i++)
        bytes += zbtElementStorageBytes(&eles[i]);
    return bytes;
}

/* Build a complete score leaf from sorted arrays. Inline members are copied.
 * External members are either allocated here or adopted from their old leaf.
 *
 * A normal non-full leaf asks the allocator for some reserve, allowing later
 * inserts to use the same allocation. An edge page asks for the full 4095
 * bytes immediately: monotonic insertion will fill that page, so repeated
 * growth would only copy it needlessly.
 *
 * 'id' may be an existing ID when rebuilding a leaf, or ZBT_NEW_LEAF_ID when
 * creating another one. The function publishes the new address in the ID
 * table but does not link the leaf into the tree or leaf list.
 */
static zbtScoreLeaf *zbtScoreLeafBuild(zbtreeSet *zs, unsigned int count,
                                       double *scores, zbtBuildElement *eles,
                                       uint8_t *tags, uint32_t id,
                                       int edge_page)
{
    uint64_t score_base;
    uint8_t score_bits, score_shift;
    zbtScoreEncoding(count, scores, 0, &score_base, &score_bits, &score_shift);
    size_t request = zbtScoreLeafRequestBytes(count, scores, eles);
    size_t alloc_request = request;
    if (edge_page && request <= ZBT_SCORE_LEAF_BYTES) {
        alloc_request = ZBT_SCORE_LEAF_BYTES;
    } else if (request <= ZBT_SCORE_LEAF_BYTES &&
               count < ZBT_SCORE_LEAF_MAX)
    {
        alloc_request += ZBT_SCORE_LEAF_RESERVE;
        if (alloc_request > ZBT_SCORE_LEAF_BYTES)
            alloc_request = ZBT_SCORE_LEAF_BYTES;
    }
    zbtScoreLeaf *leaf = zbtAlloc(zs, alloc_request);
    uint16_t *offsets;
    uint8_t *hash_tags;
    unsigned char *records;

    serverAssert(count > 0 && count <= ZBT_SCORE_LEAF_MAX);
    serverAssert(request <= ZBT_SCORE_LEAF_BYTES);

    memset(leaf, 0, offsetof(zbtScoreLeaf, data));
    leaf->n.isleaf = 1;
    leaf->n.count = count;
    leaf->n.subtree = count;
    leaf->n.score_shift = score_shift;
    leaf->score_base = score_base;
    leaf->score_bits = score_bits;
    memset(leaf->data, 0, zbtScoreLeafScoreBytes(count, score_bits));
    offsets = zbtScoreLeafOffsets(leaf);
    hash_tags = zbtScoreLeafHashTags(leaf);
    size_t capacity = zmalloc_usable_size(leaf);
    if (capacity > ZBT_SCORE_LEAF_BYTES)
        capacity = ZBT_SCORE_LEAF_BYTES;
    records = (unsigned char *)leaf + capacity;

    for (unsigned int i = 0; i < count; i++) {
        size_t item_bytes = zbtElementStorageBytes(&eles[i]);

        zbtScoreLeafWriteScore(leaf, i, scores[i]);
        debugServerAssert(zbtScoreLeafScore(leaf, i) == scores[i]);
        records -= item_bytes;
        serverAssert((char *)records - (char *)leaf <=
                     ZBT_SCORE_LEAF_BYTES);
        offsets[i] = (uint16_t)((char *)records - (char *)leaf);
        hash_tags[i] = tags[i];
        if (zbtBuildElementWrite(zs, records, &eles[i]))
            leaf->n.has_external = 1;
    }
    leaf->record_start = records - (unsigned char *)leaf;
    zbtScoreRegisterLeaf(zs, leaf, id);
    return leaf;
}

/* Allocate an empty inner score-tree page. */
static zbtScoreInner *zbtScoreInnerCreate(zbtreeSet *zs) {
    zbtScoreInner *inner = zbtAlloc(zs, sizeof(*inner));
    memset(inner, 0, sizeof(*inner));
    return inner;
}

/* Put a rebuilt leaf in every place that named oldleaf: parent, neighboring
 * leaves, first/last pointers, and possibly the root. Its ID table entry was
 * already changed by zbtScoreLeafBuild(). */
static void zbtScoreReplaceLeaf(zbtreeSet *zs, zbtScoreLeaf *oldleaf, zbtScoreLeaf *newleaf) {
    newleaf->n.parent = oldleaf->n.parent;
    newleaf->n.parent_index = oldleaf->n.parent_index;
    newleaf->n.index_resize = oldleaf->n.index_resize;
    newleaf->prev = oldleaf->prev;
    newleaf->next = oldleaf->next;

    if (newleaf->prev) newleaf->prev->next = newleaf;
    else zs->score_first = newleaf;
    if (newleaf->next) newleaf->next->prev = newleaf;
    else zs->score_last = newleaf;

    if (oldleaf->n.parent)
        oldleaf->n.parent->child[oldleaf->n.parent_index] = &newleaf->n;
    else
        zs->score_root = &newleaf->n;
    zbtScoreRefreshParents(&newleaf->n);
    zbtScoreLeafFreeOwned(zs, oldleaf);
}

/* Rebuild a leaf without spare or abandoned record bytes, retaining its ID. */
static zbtScoreLeaf *zbtScoreCompactLeaf(zbtreeSet *zs,
                                         zbtScoreLeaf *oldleaf)
{
    double scores[ZBT_SCORE_LEAF_MAX];
    zbtBuildElement eles[ZBT_SCORE_LEAF_MAX];
    uint8_t tags[ZBT_SCORE_LEAF_MAX];

    for (unsigned int i = 0; i < oldleaf->n.count; i++) {
        scores[i] = zbtScoreLeafScore(oldleaf, i);
        zbtBuildElementFromLeaf(&eles[i], oldleaf, i);
        tags[i] = zbtScoreLeafTag(oldleaf, i);
    }
    zbtScoreLeaf *newleaf = zbtScoreLeafBuild(zs, oldleaf->n.count,
        scores, eles, tags, oldleaf->id, 0);
    zbtScoreReplaceLeaf(zs, oldleaf, newleaf);
    return newleaf;
}

/* Detach a score node from the tree. Empty ancestors are removed recursively,
 * and a root with one child is replaced by that child. The node itself is
 * still owned and freed by the caller. */
static void zbtScoreDetachNode(zbtreeSet *zs, zbtScoreNode *node) {
    zbtScoreInner *parent = node->parent;

    if (parent == NULL) {
        serverAssert(zs->score_root == node);
        zs->score_root = NULL;
        return;
    }

    unsigned int pos = node->parent_index;
    memmove(&parent->child[pos], &parent->child[pos + 1],
            (parent->n.count - pos - 1) * sizeof(parent->child[0]));
    parent->n.count--;

    if (parent->n.count == 0) {
        zbtScoreDetachNode(zs, &parent->n);
        zbtFreeAllocation(zs, parent);
    } else if (parent->n.parent == NULL && parent->n.count == 1) {
        zbtScoreNode *child = parent->child[0];
        child->parent = NULL;
        child->parent_index = 0;
        zs->score_root = child;
        zbtFreeAllocation(zs, parent);
    } else {
        zbtScoreRefreshNode(parent);
        zbtScoreRefreshParents(&parent->n);
    }
}

/* Unlink and free a score leaf, including its ID. Member-index records using
 * that ID must be removed or redirected before another lookup can run. */
static void zbtScoreRemoveLeaf(zbtreeSet *zs, zbtScoreLeaf *leaf) {
    if (zs->member_rehash &&
        zs->member_rehash->next_leaf_id == leaf->id)
    {
        zs->member_rehash->next_leaf_id =
            leaf->next ? leaf->next->id : ZBT_NO_LEAF_ID;
    }
    if (leaf->prev) leaf->prev->next = leaf->next;
    else zs->score_first = leaf->next;
    if (leaf->next) leaf->next->prev = leaf->prev;
    else zs->score_last = leaf->prev;
    zbtScoreDetachNode(zs, &leaf->n);
    zbtScoreReleaseLeafId(zs, leaf->id);
    zbtScoreLeafFreeOwned(zs, leaf);
}

static void zbtScoreInsertSibling(zbtreeSet *zs, zbtScoreNode *left, zbtScoreNode *right);

/* Insert one child into a full inner page, split the resulting children in
 * half, then insert the new right page into the level above. */
static void zbtScoreSplitInner(zbtreeSet *zs, zbtScoreInner *inner,
                               unsigned int insert_at, zbtScoreNode *right)
{
    zbtScoreNode *children[ZBT_SCORE_INNER_MAX + 1];
    unsigned int src = 0;

    for (unsigned int i = 0; i < ZBT_SCORE_INNER_MAX + 1; i++) {
        if (i == insert_at) children[i] = right;
        else children[i] = inner->child[src++];
    }

    unsigned int left_count = (ZBT_SCORE_INNER_MAX + 1) / 2;
    unsigned int right_count = ZBT_SCORE_INNER_MAX + 1 - left_count;
    zbtScoreInner *newright = zbtScoreInnerCreate(zs);
    inner->n.count = left_count;
    newright->n.count = right_count;

    for (unsigned int i = 0; i < left_count; i++)
        inner->child[i] = children[i];
    for (unsigned int i = 0; i < right_count; i++)
        newright->child[i] = children[left_count + i];

    zbtScoreRefreshNode(inner);
    zbtScoreRefreshNode(newright);
    zbtScoreInsertSibling(zs, &inner->n, &newright->n);
}

/* Link 'right' immediately after 'left' at the same tree level. A new root is
 * created when left was the old root; a full parent is split recursively. */
static void zbtScoreInsertSibling(zbtreeSet *zs, zbtScoreNode *left, zbtScoreNode *right) {
    zbtScoreInner *parent = left->parent;

    if (parent == NULL) {
        zbtScoreInner *root = zbtScoreInnerCreate(zs);
        root->n.count = 2;
        root->child[0] = left;
        root->child[1] = right;
        zbtScoreRefreshNode(root);
        zs->score_root = &root->n;
        return;
    }

    unsigned int pos = left->parent_index + 1;
    if (parent->n.count == ZBT_SCORE_INNER_MAX) {
        zbtScoreSplitInner(zs, parent, pos, right);
        return;
    }

    memmove(&parent->child[pos + 1], &parent->child[pos],
            (parent->n.count - pos) * sizeof(parent->child[0]));
    parent->child[pos] = right;
    parent->n.count++;
    zbtScoreRefreshNode(parent);
    zbtScoreRefreshParents(&parent->n);
}

/* Same operation as zbtScoreInsertSibling(), but put 'left' immediately
 * before the existing node 'right'. This is used by descending edge builds. */
static void zbtScoreInsertBefore(zbtreeSet *zs, zbtScoreNode *right,
                                 zbtScoreNode *left)
{
    zbtScoreInner *parent = right->parent;

    if (parent == NULL) {
        zbtScoreInner *root = zbtScoreInnerCreate(zs);
        root->n.count = 2;
        root->child[0] = left;
        root->child[1] = right;
        zbtScoreRefreshNode(root);
        zs->score_root = &root->n;
        return;
    }

    unsigned int pos = right->parent_index;
    if (parent->n.count == ZBT_SCORE_INNER_MAX) {
        zbtScoreSplitInner(zs, parent, pos, left);
        return;
    }

    memmove(&parent->child[pos + 1], &parent->child[pos],
            (parent->n.count - pos) * sizeof(parent->child[0]));
    parent->child[pos] = left;
    parent->n.count++;
    zbtScoreRefreshNode(parent);
    zbtScoreRefreshParents(&parent->n);
}

/* ---------------------- Score tree search and insert -------------------- */

/* Select the first child whose greatest (score, member) is not below the
 * requested pair. max_score[] answers the common case. Only equal scores need
 * the member comparison that walks to the child's final leaf. */
static unsigned int zbtScoreChild(zbtScoreInner *inner, double score,
                                  const unsigned char *ele, size_t elelen)
{
    for (unsigned int i = 0; i < inner->n.count; i++) {
        if (score < inner->max_score[i]) return i;
        if (score == inner->max_score[i]) {
            size_t maxlen;
            const unsigned char *maxele =
                zbtScoreNodeMaxElement(inner->child[i], &maxlen);
            if (zbtCompareElements(ele, elelen, maxele, maxlen) <= 0)
                return i;
        }
    }
    return inner->n.count - 1;
}

/* Find the score leaf where a (score, member) pair belongs. */
static zbtScoreLeaf *zbtScoreFindLeaf(const zbtreeSet *zs, double score,
                                      const unsigned char *ele, size_t elelen)
{
    zbtScoreNode *node = zs->score_root;
    if (node == NULL) return NULL;
    while (!node->isleaf) {
        zbtScoreInner *inner = (zbtScoreInner *)node;
        node = inner->child[zbtScoreChild(inner, score, ele, elelen)];
    }
    return (zbtScoreLeaf *)node;
}

/* Return the first logical position not below (score, member). */
static unsigned int zbtScoreLeafLowerBound(zbtScoreLeaf *leaf, double score,
                                           const unsigned char *ele,
                                           size_t elelen)
{
    unsigned int lo = 0, hi = leaf->n.count;
    while (lo < hi) {
        unsigned int mid = lo + (hi - lo) / 2;
        size_t midlen;
        const unsigned char *midele =
            zbtScoreLeafElement(leaf, mid, &midlen);
        int cmp = zbtScoreCompare(zbtScoreLeafScore(leaf, mid),
                                  midele, midlen, score, ele, elelen);
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Look for an exact member in one score leaf. The byte tags are contiguous,
 * so memchr rejects almost every unrelated member before its packed bytes
 * are touched. */
static int zbtScoreLeafFindMember(zbtScoreLeaf *leaf, uint32_t hash,
                                  const unsigned char *ele, size_t elelen,
                                  double *score,
                                  unsigned int *position)
{
    uint8_t tag = hash >> 24;
    uint16_t *offsets = zbtScoreLeafOffsets(leaf);
    uint8_t *tags = zbtScoreLeafHashTags(leaf);
    uint8_t *next = tags;
    uint8_t *end = tags + leaf->n.count;

    while (next < end) {
        uint8_t *found = memchr(next, tag, end - next);
        if (found == NULL) break;
        unsigned int physical = found - tags;
        uint16_t offset = offsets[physical];
        unsigned char *record = (unsigned char *)leaf + offset;

        if (record[0] == ZBT_PACKED_EXTERNAL &&
            zbtExternalHash(record) != hash)
        {
            next = found + 1;
            continue;
        }

        size_t len;
        const unsigned char *stored = zbtPackedData(record, &len);
        if (len == elelen && memcmp(stored, ele, len) == 0)
        {
            unsigned int logical =
                zbtScoreLeafPhysicalPos(leaf, physical);
            if (score) *score = zbtScoreLeafScore(leaf, logical);
            if (position) *position = logical;
            return 1;
        }
        next = found + 1;
    }
    return 0;
}

/* Choose a score-leaf split that balances bytes, not just element count.
 * Both sides must fit in the 4 KB leaf limit. */
static unsigned int zbtScoreChooseSplit(unsigned int count, double *scores,
                                        zbtBuildElement *eles)
{
    size_t total = 0, prefix = 0;
    unsigned int candidate = count / 2;

    /* Eight bytes is an upper bound for one packed score. Use that bound to
     * find a balanced split without repeatedly decoding every score.
     */
    for (unsigned int i = 0; i < count; i++)
        total += zbtElementStorageBytes(&eles[i]) +
                 sizeof(double) + sizeof(uint16_t) + sizeof(uint8_t);
    for (unsigned int i = 1; i < count; i++) {
        prefix += zbtElementStorageBytes(&eles[i - 1]) +
                  sizeof(double) + sizeof(uint16_t) + sizeof(uint8_t);
        if (prefix * 2 >= total) {
            candidate = i;
            break;
        }
    }

    if (zbtScoreLeafRequestBytes(candidate, scores, eles) <=
            ZBT_SCORE_LEAF_BYTES &&
        zbtScoreLeafRequestBytes(count - candidate, scores + candidate,
                                 eles + candidate) <= ZBT_SCORE_LEAF_BYTES)
        return candidate;

    /* The byte estimate is conservative, but verify every boundary if an
     * unusual mix of member lengths did not fit at the estimated midpoint. */
    unsigned int best = 0;
    size_t best_largest = SIZE_MAX;

    for (unsigned int i = 1; i < count; i++) {
        size_t left = zbtScoreLeafRequestBytes(i, scores, eles);
        size_t right = zbtScoreLeafRequestBytes(count - i, scores + i,
                                                eles + i);
        if (left > ZBT_SCORE_LEAF_BYTES ||
            right > ZBT_SCORE_LEAF_BYTES)
            continue;
        size_t largest = left > right ? left : right;
        if (largest < best_largest) {
            best = i;
            best_largest = largest;
        }
    }
    serverAssert(best != 0);
    return best;
}

/* Insert after the new score encoding has been computed, while retaining the
 * same leaf allocation. Existing records remain where they are; only the new
 * record is added and the packed scores and offsets are rewritten.
 *
 * 'locations' contains the old record offsets in the new logical order.
 * Return zero when the changed arrays and new record no longer fit in the
 * free space, so the caller can rebuild or split the leaf.
 */
static int zbtScoreInsertInPlace(zbtreeSet *zs, zbtScoreLeaf *leaf,
                                 unsigned int count,
                                 double *scores, zbtBuildElement *eles,
                                 uint8_t *tags, uint16_t *locations,
                                 unsigned int inserted)
{
    uint64_t score_base;
    uint8_t score_bits, score_shift;
    zbtScoreEncoding(count, scores, leaf->reversed,
                     &score_base, &score_bits, &score_shift);
    size_t score_bytes = zbtScoreLeafScoreBytes(count, score_bits);
    size_t index_end = offsetof(zbtScoreLeaf, data) + score_bytes +
                       count * (sizeof(uint16_t) + sizeof(uint8_t));
    size_t item_bytes = zbtElementStorageBytes(&eles[inserted]);
    if (index_end + item_bytes > leaf->record_start)
        return 0;

    uint16_t new_offset = leaf->record_start - item_bytes;
    if (zbtBuildElementWrite(zs, (unsigned char *)leaf + new_offset,
                             &eles[inserted]))
        leaf->n.has_external = 1;

    leaf->n.count = count;
    leaf->n.subtree = count;
    leaf->n.score_shift = score_shift;
    leaf->score_base = score_base;
    leaf->score_bits = score_bits;
    leaf->record_start = new_offset;
    memset(leaf->data, 0, score_bytes);

    uint16_t *offsets = zbtScoreLeafOffsets(leaf);
    uint8_t *hash_tags = zbtScoreLeafHashTags(leaf);
    for (unsigned int i = 0; i < count; i++) {
        zbtScoreLeafWriteScore(leaf, i, scores[i]);
        uint16_t offset = i == inserted ? new_offset : locations[i];
        unsigned int physical = zbtScoreLeafPhysicalPos(leaf, i);
        offsets[physical] = offset;
        hash_tags[physical] = tags[i];
        debugServerAssert(zbtScoreLeafScore(leaf, i) == scores[i]);
    }
    zbtScoreUpdatePath(&leaf->n, 1);
    return 1;
}

/* Return the packed representation of a score when the leaf's current base,
 * shift and width can hold it exactly. */
static int zbtScoreLeafPackScore(zbtScoreLeaf *leaf, double score,
                                 uint64_t *packed)
{
    uint64_t ordered = zbtScoreToOrdered(score);
    if ((!leaf->reversed && ordered < leaf->score_base) ||
        (leaf->reversed && ordered > leaf->score_base))
        return 0;
    uint64_t delta = leaf->reversed ? leaf->score_base - ordered :
                                       ordered - leaf->score_base;
    unsigned int shift = leaf->n.score_shift;
    if (delta & ((UINT64_C(1) << shift) - 1)) return 0;
    delta >>= shift;
    if (leaf->score_bits == 0) {
        if (delta != 0) return 0;
    } else if (leaf->score_bits < 64 &&
               delta >= (UINT64_C(1) << leaf->score_bits))
    {
        return 0;
    }
    *packed = delta;
    return 1;
}

/* Fast insertion when the current base, shift and width can encode the new
 * score. Shift the packed score fields and offsets in place instead of
 * decoding and rewriting the whole leaf. Return zero if the score needs a
 * different encoding or the allocation has insufficient free space.
 */
static int zbtScoreInsertFast(zbtreeSet *zs, zbtScoreLeaf *leaf, double score,
                              zbtBuildElement *ele,
                              uint8_t tag, unsigned int pos)
{
    uint64_t delta;
    if (!zbtScoreLeafPackScore(leaf, score, &delta)) return 0;

    unsigned int oldcount = leaf->n.count;
    unsigned int count = oldcount + 1;
    size_t score_bytes =
        zbtScoreLeafScoreBytes(count, leaf->score_bits);
    size_t item_bytes = zbtElementStorageBytes(ele);
    size_t index_end = offsetof(zbtScoreLeaf, data) + score_bytes +
                       count * (sizeof(uint16_t) + sizeof(uint8_t));
    if (index_end + item_bytes > leaf->record_start)
        return 0;

    unsigned int physical = leaf->reversed ? oldcount - pos : pos;

    /* Adding a score can move both arrays that follow the packed bits. Save
     * the tags, shift the bits while the old offsets are still available,
     * then rebuild the offsets and tags at their new addresses. */
    uint16_t *old_offsets = zbtScoreLeafOffsets(leaf);
    uint8_t old_tags[ZBT_SCORE_LEAF_MAX];
    memcpy(old_tags, zbtScoreLeafHashTags(leaf), oldcount);
    zbtInsertScoreBits(leaf, physical, delta);
    uint16_t *offsets = (uint16_t *)(leaf->data + score_bytes);
    memmove(offsets, old_offsets, oldcount * sizeof(uint16_t));
    memmove(offsets + physical + 1, offsets + physical,
            (oldcount - physical) * sizeof(uint16_t));
    uint8_t *hash_tags = (uint8_t *)(offsets + count);
    memcpy(hash_tags, old_tags, physical);
    hash_tags[physical] = tag;
    memcpy(hash_tags + physical + 1, old_tags + physical,
           oldcount - physical);
    uint16_t new_offset = leaf->record_start - item_bytes;
    if (zbtBuildElementWrite(zs, (unsigned char *)leaf + new_offset, ele))
        leaf->n.has_external = 1;

    leaf->n.count = count;
    leaf->n.subtree = count;
    leaf->record_start = new_offset;
    offsets[physical] = new_offset;
    zbtScoreUpdatePath(&leaf->n, 1);
    return 1;
}

static int zbtIndexMove(zbtreeSet *zs, uint32_t hash,
                        uint32_t old_leaf_id, uint32_t new_leaf_id,
                        int target_is_incomplete, int *target_was_copied);

/* Start a new leaf at one end of the tree. The old edge leaf was allocated
 * at full page size while growing; compact it now that it will no longer
 * receive monotonic inserts. The new left edge uses reversed arrays. */
static zbtScoreLeaf *zbtScoreInsertEdgeLeaf(zbtreeSet *zs,
                                            zbtScoreLeaf *oldleaf,
                                            double score,
                                            zbtBuildElement *ele,
                                            uint8_t tag, int prepend)
{
    if (zmalloc_usable_size(oldleaf) >= ZBT_SCORE_LEAF_BYTES)
        oldleaf = zbtScoreCompactLeaf(zs, oldleaf);

    double newscore[1] = {score};
    uint8_t newtag[1] = {tag};
    zbtScoreLeaf *newleaf = zbtScoreLeafBuild(zs, 1, newscore, ele,
                                              newtag, ZBT_NEW_LEAF_ID, 1);
    newleaf->n.index_resize = oldleaf->n.index_resize;
    if (prepend) {
        newleaf->reversed = 1;
        newleaf->prev = oldleaf->prev;
        newleaf->next = oldleaf;
        oldleaf->prev = newleaf;
        zs->score_first = newleaf;
        zbtScoreInsertBefore(zs, &oldleaf->n, &newleaf->n);
    } else {
        newleaf->prev = oldleaf;
        newleaf->next = oldleaf->next;
        oldleaf->next = newleaf;
        zs->score_last = newleaf;
        zbtScoreInsertSibling(zs, &oldleaf->n, &newleaf->n);
    }
    return newleaf;
}

/* Insert a new (score, member) pair into the score tree and return the leaf
 * that received it. The member index is updated separately by the caller.
 *
 * The paths, in increasing order of work, are:
 *
 *  1. Shift packed fields in the current leaf without changing its encoding.
 *  2. Re-encode the arrays while keeping the same allocation.
 *  3. Rebuild the leaf, preserving its stable ID.
 *  4. Split it and repair the member records of elements moved to the right.
 *
 * Monotonic insertion has another path that fills an edge page and then
 * starts a new one, avoiding the half-full leaves produced by normal splits.
 */
static zbtScoreLeaf *zbtScoreInsert(zbtreeSet *zs, double score,
                                    zbtBuildElement *ele)
{
    uint32_t hash = ele->hash;
    uint8_t tag = hash >> 24;
    if (zs->score_root == NULL) {
        double scores[1] = {score};
        uint8_t tags[1] = {tag};
        zbtScoreLeaf *leaf = zbtScoreLeafBuild(zs, 1, scores, ele, tags,
                                               ZBT_NEW_LEAF_ID, 0);
        zs->score_root = &leaf->n;
        zs->score_first = zs->score_last = leaf;
        return leaf;
    }

    zbtScoreLeaf *oldleaf = zbtScoreFindLeaf(zs, score, ele->ptr, ele->len);
    unsigned int oldcount = oldleaf->n.count;
    unsigned int pos = zbtScoreLeafLowerBound(oldleaf, score,
                                              ele->ptr, ele->len);
    int at_left_edge = pos == 0 && oldleaf == zs->score_first;
    int at_right_edge = pos == oldcount && oldleaf == zs->score_last;

    /* Monotonic inserts are common when a sorted set is loaded. Once an edge
     * leaf reaches its element limit, start another full-size leaf instead of
     * splitting it. At the left edge the arrays are kept in reverse physical
     * order, so prepending has the same cheap path.
     */
    if (oldcount == ZBT_SCORE_LEAF_MAX &&
        (at_left_edge || at_right_edge))
    {
        return zbtScoreInsertEdgeLeaf(zs, oldleaf, score, ele, tag,
                                      at_left_edge);
    }

    if (oldcount < ZBT_SCORE_LEAF_MAX &&
        zbtScoreInsertFast(zs, oldleaf, score, ele, tag, pos))
        return oldleaf;

    double scores[ZBT_SCORE_LEAF_MAX + 1];
    zbtBuildElement eles[ZBT_SCORE_LEAF_MAX + 1];
    uint8_t tags[ZBT_SCORE_LEAF_MAX + 1];
    uint16_t locations[ZBT_SCORE_LEAF_MAX + 1];

    for (unsigned int i = 0, src = 0; i < oldcount + 1; i++) {
        if (i == pos) {
            scores[i] = score;
            eles[i] = *ele;
            tags[i] = tag;
            locations[i] = 0;
        } else {
            scores[i] = zbtScoreLeafScore(oldleaf, src);
            zbtBuildElementFromLeaf(&eles[i], oldleaf, src);
            tags[i] = zbtScoreLeafTag(oldleaf, src);
            locations[i] = zbtScoreLeafOffset(oldleaf, src);
            src++;
        }
    }

    size_t request = zbtScoreLeafRequestBytes(oldcount + 1, scores, eles);

    /* Long members can fill the page before its element limit is reached.
     * Preserve a full edge page in that case for the same reason as above. */
    if (request > ZBT_SCORE_LEAF_BYTES &&
        (at_left_edge || at_right_edge))
    {
        return zbtScoreInsertEdgeLeaf(zs, oldleaf, score, ele, tag,
                                      at_left_edge);
    }

    if (oldcount < ZBT_SCORE_LEAF_MAX &&
        request <= ZBT_SCORE_LEAF_BYTES)
    {
        if (zbtScoreInsertInPlace(zs, oldleaf, oldcount + 1, scores, eles,
                                  tags, locations, pos))
            return oldleaf;
        zbtScoreLeaf *newleaf = zbtScoreLeafBuild(zs, oldcount + 1,
                                                  scores, eles, tags,
                                                  oldleaf->id, 0);
        zbtScoreReplaceLeaf(zs, oldleaf, newleaf);
        return newleaf;
    }

    unsigned int left_count =
        zbtScoreChooseSplit(oldcount + 1, scores, eles);
    unsigned int right_count = oldcount + 1 - left_count;
    zbtScoreLeaf *left = zbtScoreLeafBuild(zs, left_count, scores, eles,
                                           tags, oldleaf->id, 0);
    zbtScoreLeaf *right = zbtScoreLeafBuild(zs, right_count,
                                            scores + left_count,
                                            eles + left_count,
                                            tags + left_count,
                                            ZBT_NEW_LEAF_ID, 0);
    left->n.index_resize = right->n.index_resize = oldleaf->n.index_resize;

    left->n.parent = oldleaf->n.parent;
    left->n.parent_index = oldleaf->n.parent_index;
    left->prev = oldleaf->prev;
    left->next = right;
    right->prev = left;
    right->next = oldleaf->next;
    if (left->prev) left->prev->next = left;
    else zs->score_first = left;
    if (right->next) right->next->prev = right;
    else zs->score_last = right;

    if (oldleaf->n.parent)
        oldleaf->n.parent->child[oldleaf->n.parent_index] = &left->n;
    else
        zs->score_root = &left->n;

    /* Existing member-index entries still name the old leaf. The left leaf
     * keeps that ID. Entries for members copied to the right leaf only need
     * their small ID field changed. The hash-table position does not depend
     * on that ID.
     * The newly inserted member has no index entry yet, so skip it here.
     */
    int right_was_copied = 0;
    for (unsigned int i = left_count; i < oldcount + 1; i++) {
        if (i == pos) continue;
        uint32_t moved_hash =
            zbtScoreLeafHash(right, i - left_count);
        serverAssert(zbtIndexMove(zs, moved_hash, oldleaf->id, right->id,
                                  pos >= left_count, &right_was_copied));
    }

    zbtScoreLeafFreeOwned(zs, oldleaf);
    zbtScoreInsertSibling(zs, &left->n, &right->n);
    zbtScoreRefreshParents(&left->n);
    return pos < left_count ? left : right;
}

/* --------------------------- Member index ------------------------------- */

/* Return the number of slots in a member-index table. */
static inline unsigned long zbtIndexSlots(const zbtIndexTable *table) {
    return table->size * ZBT_INDEX_BUCKET_ITEMS;
}

/* Return the byte size of one narrow or wide bucket. */
static inline size_t zbtIndexBucketBytes(const zbtIndexTable *table) {
    return table->wide_ids ? sizeof(zbtIndexBucket32) :
                             sizeof(zbtIndexBucket16);
}

/* Return the bytes occupied by all buckets in a table. */
static inline size_t zbtIndexTableBytes(const zbtIndexTable *table) {
    return table->size * zbtIndexBucketBytes(table);
}

/* Return one bucket from a table whose bucket width is selected at runtime. */
static inline zbtIndexBucket *zbtIndexBucketAt(const zbtIndexTable *table,
                                               unsigned long index)
{
    return (zbtIndexBucket *)((unsigned char *)table->buckets +
           index * zbtIndexBucketBytes(table));
}

/* Return the missing-member filter for one home bucket. */
static inline uint64_t *zbtIndexHomeTagsAt(const zbtIndexTable *table,
                                           unsigned long index)
{
    return &zbtIndexBucketAt(table, index)->narrow.home_tags;
}

/* Read a leaf ID from either bucket form. */
static inline uint32_t zbtIndexGetId(const zbtIndexTable *table,
                                     const zbtIndexBucket *bucket,
                                     unsigned int pos)
{
    if (table->wide_ids) return bucket->wide.id[pos];
    uint16_t id = bucket->narrow.id[pos];
    return id == UINT16_MAX ? ZBT_INDEX_DELETED_ID : id;
}

/* Both bucket forms have tags as their common first field. */
static inline uint64_t zbtIndexTags(const zbtIndexBucket *bucket) {
    return bucket->narrow.tags;
}

/* Store a leaf ID in either bucket form. */
static inline void zbtIndexSetId(const zbtIndexTable *table,
                                 zbtIndexBucket *bucket, unsigned int pos,
                                 uint32_t id)
{
    if (table->wide_ids) {
        bucket->wide.id[pos] = id;
    } else {
        serverAssert(id == ZBT_INDEX_DELETED_ID || id < UINT16_MAX);
        bucket->narrow.id[pos] = id == ZBT_INDEX_DELETED_ID ?
                                 UINT16_MAX : id;
    }
}

/* Fold tag zero into one because zero marks an unused table slot. */
static inline uint8_t zbtIndexTag(uint32_t hash) {
    uint8_t tag = hash >> 24;
    return tag ? tag : 1;
}

/* Three bits summarize one tag. Collisions only cause a normal table search. */
static inline uint64_t zbtIndexTagBits(uint8_t tag) {
    uint64_t mixed = (uint64_t)tag * UINT64_C(0x9e3779b97f4a7c15);
    return (UINT64_C(1) << (tag & 63)) |
           (UINT64_C(1) << (mixed >> 58)) |
           (UINT64_C(1) << ((mixed >> 36) & 63));
}

/* Record one member in the missing-member filter of its home bucket. */
static inline void zbtIndexRecordHomeTag(zbtIndexTable *table, uint32_t hash) {
    unsigned long home = hash & (table->size - 1);
    *zbtIndexHomeTagsAt(table, home) |= zbtIndexTagBits(zbtIndexTag(hash));
}

/* Return zero only when the home bucket proves that a hash is absent. */
static inline int zbtIndexHomeMayContain(const zbtIndexTable *table,
                                         uint32_t hash)
{
    unsigned long home = hash & (table->size - 1);
    uint64_t bits = zbtIndexTagBits(zbtIndexTag(hash));
    return (*zbtIndexHomeTagsAt(table, home) & bits) == bits;
}

/* Return a mask with the high bit set in every byte equal to tag. As with
 * the usual zero-byte test, a bit above a real match may also be set. Every
 * candidate is therefore checked against the stored byte before use. */
static inline uint64_t zbtIndexTagMask(uint64_t tags, uint8_t tag) {
    uint64_t x = tags ^ (UINT64_C(0x0101010101010101) * tag);
    return (x - UINT64_C(0x0101010101010101)) & ~x &
           UINT64_C(0x8080808080808080);
}

/* Return the first matching byte position in a non-zero tag mask. */
static inline unsigned int zbtIndexFirstTag(uint64_t mask) {
    return __builtin_ctzll(mask) >> 3;
}

/* Replace one tag byte without disturbing the other seven. */
static inline void zbtIndexSetTag(zbtIndexBucket *bucket, unsigned int pos,
                                  uint8_t tag)
{
    uint64_t shift = pos * 8;
    bucket->narrow.tags =
        (zbtIndexTags(bucket) & ~(UINT64_C(0xff) << shift)) |
        ((uint64_t)tag << shift);
}

/* Round a requested bucket count up to the table's minimum power of two. */
static unsigned long zbtIndexNextPower(unsigned long size) {
    unsigned long result = ZBT_INDEX_INITIAL_BUCKETS;
    while (result < size) result <<= 1;
    return result;
}

/* Choose enough buckets to keep the requested elements below maximum load. */
static unsigned long zbtIndexBucketsForElements(unsigned long elements) {
    if (elements == 0) return ZBT_INDEX_INITIAL_BUCKETS;
    unsigned long slots =
        (elements * ZBT_INDEX_MAX_LOAD_DEN + ZBT_INDEX_MAX_LOAD_NUM - 1) /
        ZBT_INDEX_MAX_LOAD_NUM;
    return zbtIndexNextPower(
        (slots + ZBT_INDEX_BUCKET_ITEMS - 1) / ZBT_INDEX_BUCKET_ITEMS);
}

/* Allocate and clear a narrow or wide member-index table. */
static void zbtIndexTableInit(zbtreeSet *zs, zbtIndexTable *table,
                              unsigned long buckets, int wide_ids)
{
    memset(table, 0, sizeof(*table));
    table->size = zbtIndexNextPower(buckets);
    table->wide_ids = wide_ids;
    table->scan_revision = zbtIndexNextRevision();
    size_t bytes = zbtIndexTableBytes(table);
    table->buckets = zbtAlloc(zs, bytes);
    memset(table->buckets, 0, bytes);
}

/* Release a member-index table and clear its descriptor. */
static void zbtIndexTableRelease(zbtreeSet *zs, zbtIndexTable *table) {
    if (table->buckets) zbtFreeAllocation(zs, table->buckets);
    memset(table, 0, sizeof(*table));
}

/* Insert an entry without checking for duplicates or changing table size.
 * Reusing a tombstone keeps the slot occupied, so every existing probe path
 * remains valid and filled does not increase. */
static void zbtIndexTableInsertRaw(zbtIndexTable *table, uint32_t hash,
                                   uint32_t id)
{
    uint8_t tag = zbtIndexTag(hash);
    unsigned long mask = table->size - 1;
    unsigned long index = hash & mask;

    for (unsigned long probes = 0; probes < table->size; probes++) {
        zbtIndexBucket *bucket = zbtIndexBucketAt(table, index);
        for (unsigned int pos = 0; pos < ZBT_INDEX_BUCKET_ITEMS; pos++) {
            uint8_t oldtag = zbtIndexTags(bucket) >> (pos * 8);
            uint32_t oldid = zbtIndexGetId(table, bucket, pos);
            if (oldtag == 0 || oldid == ZBT_INDEX_DELETED_ID) {
                if (oldtag == 0) table->filled++;
                zbtIndexSetTag(bucket, pos, tag);
                zbtIndexSetId(table, bucket, pos, id);
                table->used++;
                zbtIndexRecordHomeTag(table, hash);
                return;
            }
        }
        index = (index + 1) & mask;
    }
    serverPanic("B+ tree member index has no free slot");
}

/* Search one table. A matching hash still has to be checked against the
 * member in its score leaf because the stored hash is not a unique key. */
static int zbtIndexTableFind(zbtreeSet *zs, zbtIndexTable *table,
                             uint32_t hash, const unsigned char *ele,
                             size_t elelen, double *score,
                             zbtScoreLeaf **score_leaf,
                             unsigned int *score_pos,
                             zbtIndexBucket **found_bucket,
                             unsigned int *found_pos,
                             zbtIndexBucket **insert_bucket,
                             unsigned int *insert_pos)
{
    if (table->size == 0) return 0;
    if (!zbtIndexHomeMayContain(table, hash)) return 0;
    uint8_t tag = zbtIndexTag(hash);
    unsigned long mask = table->size - 1;
    unsigned long index = hash & mask;
    zbtIndexBucket *first_deleted = NULL;
    unsigned int first_deleted_pos = 0;

    for (unsigned long probes = 0; probes < table->size; probes++) {
        zbtIndexBucket *bucket = zbtIndexBucketAt(table, index);
        uint64_t matches = zbtIndexTagMask(zbtIndexTags(bucket), tag);
        while (matches) {
            unsigned int pos = zbtIndexFirstTag(matches);
            if ((uint8_t)(zbtIndexTags(bucket) >> (pos * 8)) != tag) {
                matches &= matches - 1;
                continue;
            }
            uint32_t id = zbtIndexGetId(table, bucket, pos);
            if (id != ZBT_INDEX_DELETED_ID)
            {
                zbtScoreLeaf *leaf = id < zs->next_score_leaf_id ?
                    zs->score_leaf_by_id[id] : NULL;
                if (leaf == NULL || ZBT_IS_FREE_LEAF_ID(leaf)) {
                    serverAssert(zs->member_rehash &&
                                 table == &zs->member_index);
                    matches &= matches - 1;
                    continue;
                }
                serverAssert(leaf->id == id);
                if (zs->member_rehash) {
                    int in_new = table == &zs->member_rehash->table;
                    int migrated = leaf->n.index_resize ==
                                   zs->member_rehash->resize_id;
                    if (in_new != migrated) {
                        matches &= matches - 1;
                        continue;
                    }
                }
                if (zbtScoreLeafFindMember(leaf, hash, ele, elelen, score,
                                           score_pos))
                {
                    if (score_leaf) *score_leaf = leaf;
                    if (found_bucket) *found_bucket = bucket;
                    if (found_pos) *found_pos = pos;
                    return 1;
                }
            }
            matches &= matches - 1;
        }

        /* Save the first tombstone for a possible insertion. An unused slot,
         * unlike a tombstone, proves that the member is not farther ahead. */
        for (unsigned int pos = 0; pos < ZBT_INDEX_BUCKET_ITEMS; pos++) {
            uint8_t oldtag = zbtIndexTags(bucket) >> (pos * 8);
            if (oldtag == 0) {
                if (insert_bucket) {
                    *insert_bucket = first_deleted ? first_deleted : bucket;
                    *insert_pos = first_deleted ? first_deleted_pos : pos;
                }
                return 0;
            }
            if (first_deleted == NULL &&
                zbtIndexGetId(table, bucket, pos) == ZBT_INDEX_DELETED_ID)
            {
                first_deleted = bucket;
                first_deleted_pos = pos;
            }
        }
        index = (index + 1) & mask;
    }

    if (insert_bucket && first_deleted) {
        *insert_bucket = first_deleted;
        *insert_pos = first_deleted_pos;
    }
    return 0;
}

/* Find an entry whose leaf number has to change. Entries with the same tag
 * and leaf number look alike, but any reachable one is sufficient. With
 * linear probing, two search paths stay together after they meet, so
 * exchanging such entries cannot make either member unreachable. */
static int zbtIndexTableFindReference(zbtIndexTable *table, uint32_t hash,
                                      uint32_t id,
                                      zbtIndexBucket **found_bucket,
                                      unsigned int *found_pos)
{
    if (table->size == 0) return 0;
    if (!zbtIndexHomeMayContain(table, hash)) return 0;
    uint8_t tag = zbtIndexTag(hash);
    unsigned long mask = table->size - 1;
    unsigned long index = hash & mask;

    for (unsigned long probes = 0; probes < table->size; probes++) {
        zbtIndexBucket *bucket = zbtIndexBucketAt(table, index);
        uint64_t matches = zbtIndexTagMask(zbtIndexTags(bucket), tag);
        while (matches) {
            unsigned int pos = zbtIndexFirstTag(matches);
            if ((uint8_t)(zbtIndexTags(bucket) >> (pos * 8)) == tag &&
                zbtIndexGetId(table, bucket, pos) == id)
            {
                *found_bucket = bucket;
                *found_pos = pos;
                return 1;
            }
            matches &= matches - 1;
        }
        if (zbtIndexTagMask(zbtIndexTags(bucket), 0)) return 0;
        index = (index + 1) & mask;
    }
    return 0;
}

/* Start copying the member index into a table sized for 'elements'. */
static int zbtIndexStartResize(zbtreeSet *zs, unsigned long elements,
                               int allow_same_size)
{
    if (zs->member_rehash) return 0;
    unsigned long buckets = zbtIndexBucketsForElements(elements);
    if (buckets == zs->member_index.size && !allow_same_size) return 0;

    zs->member_rehash = zbtAlloc(zs, sizeof(*zs->member_rehash));
    memset(zs->member_rehash, 0, sizeof(*zs->member_rehash));
    int wide_ids = zs->member_index.wide_ids ||
                   zs->next_score_leaf_id >= ZBT_INDEX_WIDE_ID_AT;
    zbtIndexTableInit(zs, &zs->member_rehash->table, buckets, wide_ids);
    serverAssert(zs->score_first != NULL);
    zs->member_rehash->next_leaf_id = zs->score_first->id;
    uint16_t resize_id = zs->score_first->n.index_resize + 1;
    /* Zero is what a newly allocated leaf contains. Never use it for an
     * active resize, including when this counter wraps. */
    if (resize_id == 0) resize_id = 1;
    zs->member_rehash->resize_id = resize_id;
    zs->member_revision = zbtIndexNextRevision();
    return 1;
}

/* Return whether a leaf has already been copied by the active resize. */
static int zbtIndexLeafMigrated(const zbtreeSet *zs,
                                const zbtScoreLeaf *leaf)
{
    return zs->member_rehash &&
           leaf->n.index_resize == zs->member_rehash->resize_id;
}

/* Copy one score leaf into the new table. Its old entries are left untouched
 * and ignored from now on. This is much cheaper than locating and removing
 * every old entry, and the complete old table can be freed at the end. */
static void zbtIndexCopyLeaf(zbtreeSet *zs, zbtScoreLeaf *leaf) {
    zbtIndexRehash *rehash = zs->member_rehash;
    serverAssert(rehash != NULL);
    if (zbtIndexLeafMigrated(zs, leaf)) return;

    for (unsigned int i = 0; i < leaf->n.count; i++) {
        uint32_t hash = zbtScoreLeafHash(leaf, i);
        zbtIndexTableInsertRaw(&rehash->table, hash, leaf->id);
    }
    leaf->n.index_resize = rehash->resize_id;
    zs->member_revision = zbtIndexNextRevision();
}

/* Copy complete score leaves to the new table. The leaf bytes supply the full
 * hashes which the compact member index deliberately does not store. */
static int zbtIndexRehashStep(zbtreeSet *zs, int steps) {
    zbtIndexRehash *rehash = zs->member_rehash;
    if (rehash == NULL) return 0;

    while (steps--) {
        zbtScoreLeaf *leaf = NULL;
        if (rehash->next_leaf_id != ZBT_NO_LEAF_ID &&
            rehash->next_leaf_id < zs->next_score_leaf_id)
        {
            leaf = zs->score_leaf_by_id[rehash->next_leaf_id];
            if (leaf && ZBT_IS_FREE_LEAF_ID(leaf)) leaf = NULL;
        }
        /* A merge can release the saved ID between steps. In that case resume
         * from the first leaf and skip all leaves already copied. */
        if (leaf == NULL) leaf = zs->score_first;
        while (leaf && zbtIndexLeafMigrated(zs, leaf)) leaf = leaf->next;
        if (leaf == NULL) {
            /* Every live leaf is in the destination. Make it current and
             * release the old table in one step. */
            serverAssert(rehash->table.used == zs->length);
            zbtIndexTable old = zs->member_index;
            zs->member_index = rehash->table;
            zbtFreeAllocation(zs, rehash);
            zs->member_rehash = NULL;
            zbtIndexTableRelease(zs, &old);
            zs->member_revision = zbtIndexNextRevision();
            return 0;
        }

        rehash->next_leaf_id =
            leaf->next ? leaf->next->id : ZBT_NO_LEAF_ID;
        zbtIndexCopyLeaf(zs, leaf);
    }
    return 1;
}

/* Grow, widen, or clean the member index before adding more entries. */
static void zbtIndexExpandIfNeeded(zbtreeSet *zs, unsigned long add) {
    if (zs->member_index.size == 0) {
        zbtIndexTableInit(zs, &zs->member_index,
                          zbtIndexBucketsForElements(add), 0);
        zs->member_revision = zbtIndexNextRevision();
        return;
    }
    if (zs->member_rehash) {
        zbtIndexTable *target = &zs->member_rehash->table;
        unsigned long slots = zbtIndexSlots(target);

        /* A tombstone cleanup can use a same-sized destination. If many new
         * members arrive before that copy finishes, the destination itself
         * may need to grow. Finish the current copy before it runs out of
         * room; the normal checks below will then start the larger table. */
        if ((zs->length + add) * ZBT_INDEX_MAX_LOAD_DEN <=
                slots * ZBT_INDEX_MAX_LOAD_NUM &&
            (target->filled + add) * ZBT_INDEX_MAX_FILLED_DEN <
                slots * ZBT_INDEX_MAX_FILLED_NUM)
            return;
        while (zs->member_rehash)
            zbtIndexRehashStep(zs, 64);
    }

    /* A narrow table can represent a little more than twice this many leaf
     * IDs. Starting here leaves one operation per old leaf to finish the
     * upgrade even if every operation also creates a new leaf. */
    if (!zs->member_index.wide_ids &&
        zs->next_score_leaf_id >= ZBT_INDEX_WIDE_ID_AT)
    {
        zbtIndexStartResize(zs, zs->member_index.used + add, 1);
        return;
    }

    unsigned long slots = zbtIndexSlots(&zs->member_index);
    unsigned long live = zs->member_index.used + add;
    unsigned long filled = zs->member_index.filled + add;
    if (live * ZBT_INDEX_MAX_LOAD_DEN >
            slots * ZBT_INDEX_MAX_LOAD_NUM ||
        filled * ZBT_INDEX_MAX_FILLED_DEN >=
            slots * ZBT_INDEX_MAX_FILLED_NUM)
    {
        zbtIndexStartResize(zs, live, 1);
    }
}

/* Start a smaller table when deletions leave the current one mostly empty. */
static void zbtIndexShrinkIfNeeded(zbtreeSet *zs) {
    /* Redis will normally free the whole object after its final member is
     * removed. Release the index here as well because range deletion finishes
     * its internal work before the command layer can do that. There is no
     * score leaf from which an incremental resize could start. */
    if (zs->length == 0) {
        zbtIndexTableRelease(zs, &zs->member_index);
        if (zs->member_rehash) {
            zbtIndexTableRelease(zs, &zs->member_rehash->table);
            zbtFreeAllocation(zs, zs->member_rehash);
            zs->member_rehash = NULL;
        }
        zs->member_revision = zbtIndexNextRevision();
        return;
    }
    if (zs->member_rehash ||
        zs->member_index.size <= ZBT_INDEX_INITIAL_BUCKETS)
        return;
    unsigned long used = zs->member_index.used;
    if (used * ZBT_INDEX_MIN_LOAD_DEN <=
        zbtIndexSlots(&zs->member_index) * ZBT_INDEX_MIN_LOAD_NUM)
    {
        zbtIndexStartResize(zs, used ? used : 1, 0);
    }
}

/* Return whether a saved bucket address belongs to this table. */
static int zbtIndexOwnsBucket(zbtIndexTable *table,
                              zbtIndexBucket *bucket)
{
    if (table->buckets == NULL) return 0;
    uintptr_t address = (uintptr_t)bucket;
    uintptr_t first = (uintptr_t)table->buckets;
    uintptr_t last = first + table->size * zbtIndexBucketBytes(table);
    return address >= first && address < last;
}

/* Look in both tables and optionally return score and insertion positions. */
static int zbtIndexFind(zbtreeSet *zs, uint32_t hash,
                        const unsigned char *ele, size_t elelen,
                        double *score,
                        zbtScoreLeaf **found_score_leaf,
                        unsigned int *found_score_position,
                        zbtIndexBucket **found_bucket,
                        unsigned int *found_bucket_position)
{
    zbtIndexBucket *bucket = NULL, *hint_bucket = NULL;
    unsigned int pos = 0, hint_pos = 0;
    zbtIndexRehashStep(zs, 1);

    if (zs->member_rehash &&
        zbtIndexTableFind(zs, &zs->member_rehash->table, hash, ele, elelen,
                          score,
                          found_score_leaf, found_score_position,
                          &bucket, &pos, &hint_bucket, &hint_pos))
        goto found;

    /* During a resize new entries go to the new table, so an insertion slot
     * found in the old table would not be useful to the caller. */
    if (zbtIndexTableFind(zs, &zs->member_index, hash, ele, elelen, score,
                          found_score_leaf, found_score_position,
                          &bucket, &pos,
                          zs->member_rehash ? NULL : &hint_bucket,
                          zs->member_rehash ? NULL : &hint_pos))
        goto found;

    if (found_bucket) *found_bucket = hint_bucket;
    if (found_bucket_position) *found_bucket_position = hint_pos;
    return 0;

found:
    if (found_bucket) *found_bucket = bucket;
    if (found_bucket_position) *found_bucket_position = pos;
    return 1;
}

/* Find a table entry by hash tag and leaf ID without reading its member. */
static int zbtIndexFindReference(zbtreeSet *zs, uint32_t hash,
                                 uint32_t score_leaf_id,
                                 zbtIndexBucket **found_bucket,
                                 unsigned int *found_pos)
{
    zbtIndexBucket *bucket;
    if (zs->member_rehash &&
        zbtIndexTableFindReference(&zs->member_rehash->table, hash,
                                   score_leaf_id, &bucket, found_pos))
    {
        *found_bucket = bucket;
        return 1;
    }
    if (zbtIndexTableFindReference(&zs->member_index, hash, score_leaf_id,
                                   &bucket, found_pos))
    {
        *found_bucket = bucket;
        return 1;
    }
    return 0;
}

/* Change the leaf ID named by one member entry, also during a resize.
 *
 * A resize may place the source and destination leaves in different tables.
 * If the destination leaf is incomplete, copying it would miss the member
 * currently being inserted, so the caller copies it after finishing the leaf.
 * When several members move to one leaf, target_was_copied records that its
 * complete contents were already copied; later old entries are then removed
 * instead of creating duplicates. */
static int zbtIndexMove(zbtreeSet *zs, uint32_t hash,
                        uint32_t old_leaf_id, uint32_t new_leaf_id,
                        int target_is_incomplete, int *target_was_copied)
{
    zbtIndexBucket *bucket;
    unsigned int pos;
    if (zs->member_rehash == NULL) {
        if (!zbtIndexTableFindReference(&zs->member_index, hash,
                                        old_leaf_id, &bucket, &pos))
            return 0;
        zbtIndexSetId(&zs->member_index, bucket, pos, new_leaf_id);
        return 1;
    }

    serverAssert(new_leaf_id < zs->next_score_leaf_id);
    zbtScoreLeaf *newleaf = zs->score_leaf_by_id[new_leaf_id];
    serverAssert(newleaf && !ZBT_IS_FREE_LEAF_ID(newleaf));
    if (!zbtIndexLeafMigrated(zs, newleaf) && target_is_incomplete) {
        /* A split may put the newly inserted, not-yet-indexed member in the
         * right leaf. Keep its existing entries in the old table for now;
         * zbtIndexInsert() will copy the complete leaf after the split. */
        if (!zbtIndexFindReference(zs, hash, old_leaf_id, &bucket, &pos))
            return 0;
        zbtIndexTable *source = zbtIndexOwnsBucket(&zs->member_index, bucket) ?
                                &zs->member_index :
                                &zs->member_rehash->table;
        zbtIndexSetId(source, bucket, pos, new_leaf_id);
        return 1;
    }
    int copied_target = target_was_copied && *target_was_copied;
    if (!zbtIndexLeafMigrated(zs, newleaf)) {
        zbtIndexCopyLeaf(zs, newleaf);
        copied_target = 1;
        if (target_was_copied) *target_was_copied = 1;
    }

    if (!zbtIndexFindReference(zs, hash, old_leaf_id, &bucket, &pos))
        return 0;

    zbtIndexTable *source = &zs->member_index;
    if (!zbtIndexOwnsBucket(source, bucket)) {
        serverAssert(zbtIndexOwnsBucket(&zs->member_rehash->table, bucket));
        source = &zs->member_rehash->table;
    }
    if (copied_target) {
        zbtIndexSetId(source, bucket, pos, ZBT_INDEX_DELETED_ID);
        source->used--;
    } else if (source == &zs->member_rehash->table) {
        zbtIndexSetId(source, bucket, pos, new_leaf_id);
    } else {
        zbtIndexSetId(source, bucket, pos, ZBT_INDEX_DELETED_ID);
        source->used--;
        zbtIndexTableInsertRaw(&zs->member_rehash->table,
                               hash, new_leaf_id);
    }
    return 1;
}

/* Turn one occupied member-index slot into a tombstone. */
static void zbtIndexDeleteAt(zbtreeSet *zs, zbtIndexBucket *bucket,
                             unsigned int pos)
{
    zbtIndexTable *table = &zs->member_index;
    if (!zbtIndexOwnsBucket(table, bucket)) {
        serverAssert(zs->member_rehash &&
                     zbtIndexOwnsBucket(&zs->member_rehash->table, bucket));
        table = &zs->member_rehash->table;
    }
    serverAssert(zbtIndexGetId(table, bucket, pos) != ZBT_INDEX_DELETED_ID);
    zbtIndexSetId(table, bucket, pos, ZBT_INDEX_DELETED_ID);
    table->used--;
}

/* Add one member, reusing a valid lookup position when possible. */
static void zbtIndexInsert(zbtreeSet *zs, uint32_t hash, uint32_t leaf_id,
                           const zbtreeInsertPosition *position)
{
    if (zs->member_rehash) {
        serverAssert(leaf_id < zs->next_score_leaf_id);
        zbtScoreLeaf *leaf = zs->score_leaf_by_id[leaf_id];
        serverAssert(leaf && !ZBT_IS_FREE_LEAF_ID(leaf));
        if (!zbtIndexLeafMigrated(zs, leaf)) {
            /* The new member is already in the score leaf, so copying the
             * complete leaf inserts its index entry as well. */
            zbtIndexCopyLeaf(zs, leaf);
            zbtIndexRehashStep(zs, 1);
            return;
        }
    }

    zbtIndexTable *table = zs->member_rehash ?
        &zs->member_rehash->table : &zs->member_index;
    zbtIndexBucket *bucket = position ?
        (zbtIndexBucket *)position->bucket : NULL;
    unsigned int pos = position ? position->pos : 0;
    if (position && position->hash == hash &&
        position->revision == zs->member_revision && bucket &&
        zbtIndexOwnsBucket(table, bucket) &&
        pos < ZBT_INDEX_BUCKET_ITEMS)
    {
        uint8_t oldtag = zbtIndexTags(bucket) >> (pos * 8);
        uint32_t oldid = zbtIndexGetId(table, bucket, pos);
        if (oldtag == 0 || oldid == ZBT_INDEX_DELETED_ID) {
            if (oldtag == 0) table->filled++;
            zbtIndexSetTag(bucket, pos, zbtIndexTag(hash));
            zbtIndexSetId(table, bucket, pos, leaf_id);
            table->used++;
            zbtIndexRecordHomeTag(table, hash);
            zbtIndexRehashStep(zs, 1);
            return;
        }
    }

    table = zs->member_rehash ?
        &zs->member_rehash->table : &zs->member_index;
    zbtIndexTableInsertRaw(table, hash, leaf_id);
    zbtIndexRehashStep(zs, 1);
}

/* ------------------------------ Deletion -------------------------------- */

/* Join a small score leaf with a neighbor when count and byte limits allow.
 * The left leaf ID survives. Before releasing the right ID, every member
 * reference to it is redirected to the merged leaf.
 */
static zbtScoreLeaf *zbtScoreMerge(zbtreeSet *zs, zbtScoreLeaf *leaf) {
    zbtScoreLeaf *left = leaf->prev;
    zbtScoreLeaf *right = leaf;
    double scores[ZBT_SCORE_LEAF_MAX];
    zbtBuildElement eles[ZBT_SCORE_LEAF_MAX];
    uint8_t tags[ZBT_SCORE_LEAF_MAX];

    if (left == NULL ||
        left->n.count + right->n.count > ZBT_SCORE_LEAF_MAX)
    {
        left = leaf;
        right = leaf->next;
    }
    if (right == NULL ||
        left->n.count + right->n.count > ZBT_SCORE_LEAF_MAX)
        return leaf;

    unsigned int count = left->n.count + right->n.count;
    for (unsigned int i = 0; i < left->n.count; i++) {
        scores[i] = zbtScoreLeafScore(left, i);
        zbtBuildElementFromLeaf(&eles[i], left, i);
        tags[i] = zbtScoreLeafTag(left, i);
    }
    int merged_was_copied = 0;
    for (unsigned int i = 0; i < right->n.count; i++) {
        unsigned int dst = left->n.count + i;
        scores[dst] = zbtScoreLeafScore(right, i);
        zbtBuildElementFromLeaf(&eles[dst], right, i);
        tags[dst] = zbtScoreLeafTag(right, i);
    }
    if (zbtScoreLeafRequestBytes(count, scores, eles) >
        ZBT_SCORE_LEAF_BYTES)
        return leaf;

    uint32_t left_id = left->id;
    uint32_t right_id = right->id;
    zbtScoreLeaf *merged = zbtScoreLeafBuild(zs, count, scores, eles,
                                              tags, left_id, 0);
    zbtScoreReplaceLeaf(zs, left, merged);

    for (unsigned int i = 0; i < right->n.count; i++) {
        uint32_t hash = zbtScoreLeafHash(right, i);
        serverAssert(zbtIndexMove(zs, hash, right_id, left_id, 0,
                                  &merged_was_copied));
    }
    zbtScoreRemoveLeaf(zs, right);
    return merged;
}

/* Remove one packed score and offset. The existing base, shift and width can
 * still represent every remaining score, so no full re-encoding is needed.
 * compact_records decides whether the member record gap is closed too. */
static void zbtScoreDeleteInPlace(zbtreeSet *zs, zbtScoreLeaf *leaf,
                                  unsigned int pos, int compact_records)
{
    unsigned int oldcount = leaf->n.count;
    unsigned int count = oldcount - 1;

    serverAssert(count != 0 && pos < oldcount);
    unsigned int physical = zbtScoreLeafPhysicalPos(leaf, pos);
    uint16_t *old_offsets = zbtScoreLeafOffsets(leaf);
    uint8_t old_tags[ZBT_SCORE_LEAF_MAX];
    memcpy(old_tags, zbtScoreLeafHashTags(leaf), oldcount);
    uint16_t deleted_offset = old_offsets[physical];
    unsigned char *deleted_record = (unsigned char *)leaf + deleted_offset;
    size_t deleted_bytes = zbtRecordStorageBytes(deleted_record);

    if (deleted_record[0] == ZBT_PACKED_EXTERNAL) {
        unsigned char *external = zbtExternalPointer(deleted_record);
        if (compact_records) {
            serverAssert(external != NULL);
            zbtFreeAllocation(zs, external);
        } else {
            debugServerAssert(external == NULL);
        }
    }

    /* Normal deletes close the record gap immediately. A score update is a
     * delete followed by an insert, so it leaves the old bytes behind and
     * avoids moving every record in the leaf. The next rebuild or compaction
     * removes such gaps.
     */
    if (compact_records) {
        unsigned char *records = (unsigned char *)leaf + leaf->record_start;
        memmove(records + deleted_bytes, records,
                deleted_offset - leaf->record_start);
        leaf->record_start += deleted_bytes;
    } else if (deleted_offset == leaf->record_start) {
        leaf->record_start += deleted_bytes;
    }

    zbtDeleteScoreBits(leaf, physical);
    memmove(old_offsets + physical, old_offsets + physical + 1,
            (count - physical) * sizeof(uint16_t));
    size_t score_bytes = zbtScoreLeafScoreBytes(count, leaf->score_bits);
    uint16_t *offsets = (uint16_t *)(leaf->data + score_bytes);
    memmove(offsets, old_offsets, count * sizeof(uint16_t));
    uint8_t *hash_tags = (uint8_t *)(offsets + count);
    memcpy(hash_tags, old_tags, physical);
    memcpy(hash_tags + physical, old_tags + physical + 1,
           count - physical);

    leaf->n.count = count;
    leaf->n.subtree = count;
    if (compact_records) {
        for (unsigned int i = 0; i < count; i++) {
            if (offsets[i] < deleted_offset) offsets[i] += deleted_bytes;
        }
    }
    zbtScoreUpdatePath(&leaf->n, -1);
}

/* Delete one score-tree element. Score updates disable merging because they
 * are about to reinsert the same member; normal deletions also compact the
 * records and try to join a small leaf. */
static void zbtScoreDeleteAt(zbtreeSet *zs, zbtScoreLeaf *leaf,
                             unsigned int pos, int allow_merge)
{
    if (leaf->n.count == 1) {
        zbtScoreRemoveLeaf(zs, leaf);
        return;
    }

    zbtScoreDeleteInPlace(zs, leaf, pos, allow_merge);
    if (allow_merge && leaf->n.count <= ZBT_SCORE_LEAF_MERGE)
        zbtScoreMerge(zs, leaf);
}

/* Remove a run of consecutive elements from one score leaf. Their member
 * entries are found by hash and leaf number, so no member string has to be
 * copied and no second full lookup is needed.
 *
 * A run longer than one element rebuilds the leaf once instead of shifting
 * its packed scores, offsets and records for every removed element. That is
 * what makes a rank range deletion cost one leaf pass rather than one pass
 * per element.
 */
static void zbtScoreDeleteRun(zbtreeSet *zs, zbtScoreLeaf *score_leaf,
                              unsigned int score_pos, unsigned int n,
                              int update_member_index, int allow_merge)
{
    unsigned int count = score_leaf->n.count;
    serverAssert(n >= 1 && score_pos + n <= count);

    if (update_member_index) {
        for (unsigned int i = 0; i < n; i++) {
            uint32_t hash = zbtScoreLeafHash(score_leaf, score_pos + i);
            zbtIndexBucket *bucket;
            unsigned int bucket_pos;

            serverAssert(zbtIndexFindReference(zs, hash, score_leaf->id,
                                               &bucket, &bucket_pos));
            zbtIndexDeleteAt(zs, bucket, bucket_pos);
        }
    }
    zs->length -= n;

    if (n == 1) {
        if (allow_merge) {
            zbtScoreDeleteAt(zs, score_leaf, score_pos, 1);
        } else if (count == 1) {
            zbtScoreRemoveLeaf(zs, score_leaf);
        } else {
            zbtScoreDeleteInPlace(zs, score_leaf, score_pos, 1);
        }
        return;
    }
    if (n == count) {
        zbtScoreRemoveLeaf(zs, score_leaf);
        return;
    }

    double scores[ZBT_SCORE_LEAF_MAX];
    zbtBuildElement eles[ZBT_SCORE_LEAF_MAX];
    uint8_t tags[ZBT_SCORE_LEAF_MAX];
    unsigned int kept = 0;

    for (unsigned int i = 0; i < count; i++) {
        if (i == score_pos) {
            i += n - 1;
            continue;
        }
        scores[kept] = zbtScoreLeafScore(score_leaf, i);
        zbtBuildElementFromLeaf(&eles[kept], score_leaf, i);
        tags[kept] = zbtScoreLeafTag(score_leaf, i);
        kept++;
    }
    serverAssert(kept == count - n);

    zbtScoreLeaf *newleaf = zbtScoreLeafBuild(zs, kept, scores, eles, tags,
                                              score_leaf->id, 0);
    zbtScoreReplaceLeaf(zs, score_leaf, newleaf);
    if (allow_merge && kept <= ZBT_SCORE_LEAF_MERGE)
        zbtScoreMerge(zs, newleaf);
}

/* ------------------------- Creation and release ------------------------- */

/* Free a tree recursively. Tree height is small because inner pages have high
 * fanout, so recursion depth is bounded by the practical set size. */
static void zbtScoreFreeTree(zbtreeSet *zs, zbtScoreNode *node) {
    if (node->isleaf) {
        zbtScoreLeafFreeOwned(zs, (zbtScoreLeaf *)node);
        return;
    }

    zbtScoreInner *inner = (zbtScoreInner *)node;
    for (unsigned int i = 0; i < inner->n.count; i++)
        zbtScoreFreeTree(zs, inner->child[i]);
    zbtFreeAllocation(zs, node);
}

/* Create an empty B+ tree sorted set. The first score leaf and member-index
 * table are allocated lazily on insertion. */
zbtreeSet *zbtreeCreate(void) {
    size_t usable;
    zbtreeSet *zs = zmalloc_usable(sizeof(*zs), &usable);
    memset(zs, 0, sizeof(*zs));
    zs->member_revision = zbtIndexNextRevision();
    zs->alloc_size = usable;
    return zs;
}

/* Copy a score tree while repairing every pointer between its pages. Leaves
 * are visited in score order, which also rebuilds their linked list and the
 * leaf ID table. Inline records can be copied unchanged. External members
 * need their own allocation so the two sorted sets remain independent. */
static zbtScoreNode *zbtScoreDupTree(zbtreeSet *dst,
                                     const zbtScoreNode *source,
                                     zbtScoreInner *parent,
                                     unsigned int parent_index)
{
    size_t bytes = source->isleaf ? zmalloc_usable_size((void *)source) :
                                  sizeof(zbtScoreInner);
    zbtScoreNode *copy = zbtAlloc(dst, bytes);
    memcpy(copy, source, bytes);
    copy->parent = parent;
    copy->parent_index = parent_index;

    if (copy->isleaf) {
        zbtScoreLeaf *leaf = (zbtScoreLeaf *)copy;
        if (leaf->n.has_external) {
            for (unsigned int i = 0; i < leaf->n.count; i++) {
                unsigned char *record = zbtScoreLeafRecord(leaf, i);
                if (record[0] != ZBT_PACKED_EXTERNAL) continue;
                size_t len = zbtExternalLength(record);
                unsigned char *oldptr = zbtExternalPointer(record);
                serverAssert(oldptr != NULL);
                unsigned char *newptr = zbtAlloc(dst, len);
                memcpy(newptr, oldptr, len);
                zbtExternalSetPointer(record, newptr);
            }
        }
        leaf->prev = dst->score_last;
        leaf->next = NULL;
        if (dst->score_last) dst->score_last->next = leaf;
        else dst->score_first = leaf;
        dst->score_last = leaf;
        dst->score_leaf_by_id[leaf->id] = leaf;
    } else {
        zbtScoreInner *inner = (zbtScoreInner *)copy;
        const zbtScoreInner *source_inner =
            (const zbtScoreInner *)source;
        for (unsigned int i = 0; i < inner->n.count; i++)
            inner->child[i] = zbtScoreDupTree(dst, source_inner->child[i],
                                              inner, i);
    }
    return copy;
}

/* The member tables contain leaf IDs rather than page pointers. Once the
 * score tree has been copied, duplicating a table is just a byte copy. */
static void zbtIndexDupTable(zbtreeSet *dst, zbtIndexTable *copy,
                             const zbtIndexTable *source)
{
    *copy = *source;
    copy->scan_revision = zbtIndexNextRevision();
    if (source->buckets) {
        size_t bytes = zbtIndexTableBytes(source);
        copy->buckets = zbtAlloc(dst, bytes);
        memcpy(copy->buckets, source->buckets, bytes);
    }
}

/* Duplicate an entire set without decoding and reinserting its members. */
zbtreeSet *zbtreeDup(const zbtreeSet *source) {
    zbtreeSet *copy = zbtreeCreate();

    copy->score_leaf_cap = source->score_leaf_cap;
    copy->next_score_leaf_id = source->next_score_leaf_id;
    copy->free_score_leaf_id = source->free_score_leaf_id;
    copy->length = source->length;

    if (source->score_leaf_by_id) {
        size_t bytes = source->score_leaf_cap *
                       sizeof(*source->score_leaf_by_id);
        copy->score_leaf_by_id = zbtAlloc(copy, bytes);
        /* Free-list links contain IDs, not addresses, so they can be copied.
         * Live addresses are replaced while the score tree is duplicated. */
        memcpy(copy->score_leaf_by_id, source->score_leaf_by_id, bytes);
    }
    if (source->score_root)
        copy->score_root = zbtScoreDupTree(copy, source->score_root, NULL, 0);

    zbtIndexDupTable(copy, &copy->member_index, &source->member_index);
    if (source->member_rehash) {
        copy->member_rehash = zbtAlloc(copy, sizeof(*copy->member_rehash));
        *copy->member_rehash = *source->member_rehash;
        zbtIndexDupTable(copy, &copy->member_rehash->table,
                         &source->member_rehash->table);
    }
    copy->member_revision = zbtIndexNextRevision();
    return copy;
}

/* Free the score tree, member index, leaf ID table, and set header. Score
 * leaves also release the long members they own. */
void zbtreeFree(zbtreeSet *zs) {
    size_t self;
    if (zs->score_root) zbtScoreFreeTree(zs, zs->score_root);
    zbtIndexTableRelease(zs, &zs->member_index);
    if (zs->member_rehash) {
        zbtIndexTableRelease(zs, &zs->member_rehash->table);
        zbtFreeAllocation(zs, zs->member_rehash);
    }
    if (zs->score_leaf_by_id) zbtFreeAllocation(zs, zs->score_leaf_by_id);
    zfree_usable(zs, &self);
}

/* Return the number of members. */
unsigned long zbtreeLength(const zbtreeSet *zs) {
    return zs->length;
}

/* Return allocator usable bytes owned by this sorted set. */
size_t zbtreeAllocSize(const zbtreeSet *zs) {
    return zs->alloc_size;
}

/* The fork child no longer needs a sorted set after writing it. Release its
 * page-sized allocations there so later writes in the parent do not copy
 * those pages. Sub-page ranges are skipped by zmadvise_dontneed(). */
void zbtreeDismissMemory(zbtreeSet *zs) {
    if (zs->member_index.buckets)
        dismissMemory(zs->member_index.buckets,
                      zbtIndexTableBytes(&zs->member_index));
    if (zs->member_rehash && zs->member_rehash->table.buckets)
        dismissMemory(zs->member_rehash->table.buckets,
                      zbtIndexTableBytes(&zs->member_rehash->table));
    if (zs->score_leaf_by_id)
        dismissMemory(zs->score_leaf_by_id,
                      zs->score_leaf_cap * sizeof(*zs->score_leaf_by_id));

    zbtScoreLeaf *leaf = zs->score_first;
    while (leaf) {
        zbtScoreLeaf *next = leaf->next;
        if (leaf->n.has_external) {
            for (unsigned int i = 0; i < leaf->n.count; i++) {
                unsigned char *record = zbtScoreLeafRecord(leaf, i);
                if (record[0] == ZBT_PACKED_EXTERNAL)
                    dismissMemory(zbtExternalPointer(record), 0);
            }
        }
        dismissMemory(leaf, 0);
        leaf = next;
    }
}

/* ----------------------- Active defragmentation ------------------------- */

/* Active defragmentation may return a new address for any allocation. Moving
 * a tree page is therefore more than copying bytes: every pointer to the old
 * address must be changed before the old allocation is released.
 *
 * A score leaf is named by its parent, two leaf neighbors, the set's edge
 * pointers, and its stable ID table entry. An inner page is named by its
 * parent and by every child's parent pointer. zbtDefragScoreNode() repairs
 * these references.
 *
 * The incremental scan walks linked leaves. External members are visited one
 * allocation per call. After the last member of the last child below an inner
 * page, the scan moves that page and continues upward while the same condition
 * holds. This visits each page once during an uninterrupted pass without
 * keeping a stack.
 *
 * Writes may happen between time-limited slices. The cursor is a stable leaf
 * ID, never a possibly stale leaf pointer. This is a best-effort scan: a write
 * can cause repeated work, but cannot make the next slice dereference a freed
 * page. Hash-table allocations contain no pointers into themselves and are
 * moved once before this incremental page walk starts.
 */

/* Keep exact allocation accounting correct when the allocator moves an object
 * to a size class with a different usable size. */
static void zbtDefragAdjustSize(zbtreeSet *zs, size_t old_size, void *newptr) {
    size_t new_size = zmalloc_usable_size(newptr);
    if (new_size >= old_size)
        zs->alloc_size += new_size - old_size;
    else
        zs->alloc_size -= old_size - new_size;
}

/* Move an allocation which has no pointers back to its own address. */
static void *zbtDefragPlainAllocation(
    zbtreeSet *zs, void *ptr, zbtreeDefragAllocFunction *defrag_alloc)
{
    size_t old_size = zmalloc_usable_size(ptr);
    void *newptr = defrag_alloc(ptr);
    if (newptr == NULL) return ptr;
    zbtDefragAdjustSize(zs, old_size, newptr);
    return newptr;
}

/* The header, leaf table, and hash-table allocations have no pointers back to
 * their old addresses, so they can move before the page scan starts. */
zbtreeSet *zbtreeDefragStart(zbtreeSet *zs,
                             zbtreeDefragAllocFunction *defrag_alloc)
{
    size_t old_size = zmalloc_usable_size(zs);
    zbtreeSet *newzs = defrag_alloc(zs);
    if (newzs) {
        zs = newzs;
        zbtDefragAdjustSize(zs, old_size, zs);
    }

    if (zs->score_leaf_by_id)
        zs->score_leaf_by_id = zbtDefragPlainAllocation(
            zs, zs->score_leaf_by_id, defrag_alloc);
    if (zs->member_index.buckets)
        zs->member_index.buckets = zbtDefragPlainAllocation(
            zs, zs->member_index.buckets, defrag_alloc);
    if (zs->member_rehash) {
        zs->member_rehash = zbtDefragPlainAllocation(
            zs, zs->member_rehash, defrag_alloc);
        if (zs->member_rehash->table.buckets)
            zs->member_rehash->table.buckets = zbtDefragPlainAllocation(
                zs, zs->member_rehash->table.buckets, defrag_alloc);
    }
    return zs;
}

/* Forget an incremental scan. The next call starts at the first score leaf. */
void zbtreeDefragStateReset(zbtreeDefragState *state) {
    memset(state, 0, sizeof(*state));
}

/* Move one score page after saving all the places that refer to it. Leaves
 * are referenced by their neighbors and leaf ID table. Inner pages are
 * referenced by their parent and their children. */
static zbtScoreNode *zbtDefragScoreNode(
    zbtreeSet *zs, zbtScoreNode *node,
    zbtreeDefragAllocFunction *defrag_alloc,
    zbtreeDefragFreeFunction *defrag_free)
{
    size_t old_size = zmalloc_usable_size(node);
    zbtScoreNode *newnode = defrag_alloc(node);
    if (newnode == NULL) return node;
    serverAssert(newnode != node);
    zbtDefragAdjustSize(zs, old_size, newnode);

    if (newnode->parent)
        newnode->parent->child[newnode->parent_index] = newnode;
    else
        zs->score_root = newnode;

    if (newnode->isleaf) {
        zbtScoreLeaf *leaf = (zbtScoreLeaf *)newnode;
        if (leaf->prev) leaf->prev->next = leaf;
        else zs->score_first = leaf;
        if (leaf->next) leaf->next->prev = leaf;
        else zs->score_last = leaf;
        zs->score_leaf_by_id[leaf->id] = leaf;
    } else {
        zbtScoreInner *inner = (zbtScoreInner *)newnode;
        for (unsigned int i = 0; i < inner->n.count; i++)
            inner->child[i]->parent = inner;
    }

    defrag_free(node);
    return newnode;
}

/* Move one external member and repair its record. Keeping this separate from
 * the leaf move lets the incremental scan stop after one possibly large copy. */
static void zbtDefragExternalMember(
    zbtreeSet *zs, unsigned char *record,
    zbtreeDefragAllocFunction *defrag_alloc,
    zbtreeDefragFreeFunction *defrag_free)
{
    unsigned char *oldptr = zbtExternalPointer(record);
    serverAssert(oldptr != NULL);
    size_t old_size = zmalloc_usable_size(oldptr);
    unsigned char *newptr = defrag_alloc(oldptr);
    if (newptr) {
        zbtDefragAdjustSize(zs, old_size, newptr);
        zbtExternalSetPointer(record, newptr);
        defrag_free(oldptr);
    }
}

/* Move each inner page whose last leaf was reached. This visits every inner
 * page once during an uninterrupted ordered scan. */
static void zbtDefragFinishedLeaf(
    zbtreeSet *zs, zbtScoreLeaf *leaf,
    zbtreeDefragAllocFunction *defrag_alloc,
    zbtreeDefragFreeFunction *defrag_free)
{
    zbtScoreNode *node = &leaf->n;
    while (node->parent) {
        zbtScoreInner *parent = node->parent;
        if (node->parent_index + 1 != parent->n.count) break;
        node = zbtDefragScoreNode(zs, &parent->n,
                                  defrag_alloc, defrag_free);
    }
}

/* Resolve a defrag cursor. Deleted IDs contain a marked free-list link rather
 * than a live leaf and must be treated as missing. */
static zbtScoreLeaf *zbtDefragScoreLeafById(zbtreeSet *zs, uint32_t id) {
    if (id >= zs->next_score_leaf_id) return NULL;
    zbtScoreLeaf *leaf = zs->score_leaf_by_id[id];
    if (leaf == NULL || ZBT_IS_FREE_LEAF_ID(leaf)) return NULL;
    serverAssert(leaf->id == id);
    return leaf;
}

/* Defrag one leaf incrementally. A call moves the leaf page if needed, scans
 * its inline records, and moves at most one external member. After the final
 * member it moves the inner pages which end at this leaf. The next call resumes
 * through a stable leaf ID and a small position, never through a page pointer. */
int zbtreeDefragStep(zbtreeSet *zs, zbtreeDefragState *state,
                     zbtreeDefragAllocFunction *defrag_alloc,
                     zbtreeDefragFreeFunction *defrag_free,
                     unsigned long *scanned)
{
    *scanned = 0;
    if (state->tree != zs) {
        zbtreeDefragStateReset(state);
        state->tree = zs;
        state->score_leaf_id =
            zs->score_first ? zs->score_first->id : ZBT_NO_LEAF_ID;
    }

    while (state->score_leaf_id != ZBT_NO_LEAF_ID) {
        zbtScoreLeaf *leaf =
            zbtDefragScoreLeafById(zs, state->score_leaf_id);
        if (leaf == NULL) {
            /* A write removed the saved leaf between defrag slices. New
             * allocations do not need defrag, so restarting is sufficient. */
            state->score_leaf_id =
                zs->score_first ? zs->score_first->id : ZBT_NO_LEAF_ID;
            state->score_pos = 0;
            state->leaf_moved = 0;
            continue;
        }

        if (!state->leaf_moved) {
            leaf = (zbtScoreLeaf *)zbtDefragScoreNode(
                zs, &leaf->n, defrag_alloc, defrag_free);
            state->leaf_moved = 1;
        }

        while (state->score_pos < leaf->n.count) {
            unsigned int pos = state->score_pos++;
            unsigned char *record = zbtScoreLeafRecord(leaf, pos);
            (*scanned)++;
            if (record[0] != ZBT_PACKED_EXTERNAL) continue;
            zbtDefragExternalMember(zs, record, defrag_alloc, defrag_free);
            (*scanned)++;
            return 0;
        }

        uint32_t next_id =
            leaf->next ? leaf->next->id : ZBT_NO_LEAF_ID;
        zbtDefragFinishedLeaf(zs, leaf, defrag_alloc, defrag_free);
        state->score_leaf_id = next_id;
        state->score_pos = 0;
        state->leaf_moved = 0;
        return 0;
    }
    return 1;
}

/* -------------------------- Lookup and update --------------------------- */

/* Find a member supplied as raw bytes, without making a temporary sds. */
int zbtreeScoreRaw(zbtreeSet *zs, const unsigned char *ele,
                   size_t elelen, double *score)
{
    uint32_t hash = (uint32_t)dictGenHashFunction(ele, elelen);
    return zbtIndexFind(zs, hash, ele, elelen, score,
                        NULL, NULL, NULL, NULL);
}

/* Find an sds member and optionally return its score. */
int zbtreeScore(zbtreeSet *zs, sds ele, double *score) {
    return zbtreeScoreRaw(zs, (unsigned char *)ele, sdslen(ele), score);
}

/* Lookup used by ZADD. In addition to the normal result, save the member hash
 * and insertion position found on a miss. zbtreeInsertNew() can reuse it and
 * avoid a second index lookup. The saved revision makes stale positions
 * harmless if score insertion happened to advance a table resize first.
 */
int zbtreeFindForAdd(zbtreeSet *zs, sds ele, double *score,
                     zbtreeInsertPosition *position)
{
    uint32_t hash = (uint32_t)dictGenHashFunction(ele, sdslen(ele));
    zbtIndexBucket *bucket = NULL;
    zbtScoreLeaf *score_leaf = NULL;
    unsigned int pos = 0;
    unsigned int score_pos = 0;
    int found = zbtIndexFind(zs, hash, (unsigned char *)ele,
                             sdslen(ele), score,
                             &score_leaf, &score_pos, &bucket, &pos);
    position->bucket = bucket;
    position->pos = pos;
    position->score_leaf = score_leaf;
    position->score_pos = score_pos;
    position->hash = hash;
    position->revision = zs->member_revision;
    return found;
}

/* Compute a zero-based rank by adding complete left siblings while walking
 * from a score leaf to the root. */
static unsigned long zbtScoreRank(zbtScoreLeaf *leaf, unsigned int pos) {
    unsigned long rank = pos;
    zbtScoreNode *node = &leaf->n;
    while (node->parent) {
        zbtScoreInner *parent = node->parent;
        for (unsigned int i = 0; i < node->parent_index; i++)
            rank += parent->child_count[i];
        node = &parent->n;
    }
    return rank;
}

/* Return the zero-based rank of a member, or -1 if it does not exist.
 * 'reverse' counts from the greatest element instead. */
long zbtreeRank(zbtreeSet *zs, sds ele, int reverse, double *score) {
    uint32_t hash = (uint32_t)dictGenHashFunction(ele, sdslen(ele));
    zbtScoreLeaf *leaf;
    unsigned int pos;

    if (!zbtIndexFind(zs, hash, (unsigned char *)ele,
                      sdslen(ele), score, &leaf, &pos, NULL, NULL))
        return -1;

    unsigned long rank = zbtScoreRank(leaf, pos);
    return reverse ? zs->length - rank - 1 : rank;
}

/* Insert a member that is known to be absent. Bulk builders pass the bytes
 * straight out of another structure, so no temporary sds is created. */
void zbtreeInsertNewRaw(zbtreeSet *zs, double score, const unsigned char *ele,
                        size_t elelen, const zbtreeInsertPosition *position)
{
    uint32_t hash = position ? position->hash :
        (uint32_t)dictGenHashFunction(ele, elelen);
    debugServerAssert(hash == (uint32_t)dictGenHashFunction(ele, elelen));
    zbtIndexExpandIfNeeded(zs, 1);
    zbtBuildElement element;
    zbtBuildElementFromBytes(&element, ele, elelen, hash);
    zbtScoreLeaf *leaf = zbtScoreInsert(zs, score, &element);
    zs->length++;
    zbtIndexInsert(zs, hash, leaf->id, position);
}

/* Insert an absent SDS member without taking ownership of its bytes. */
void zbtreeInsertNew(zbtreeSet *zs, double score, sds ele,
                     const zbtreeInsertPosition *position)
{
    zbtreeInsertNewRaw(zs, score, (unsigned char *)ele, sdslen(ele),
                       position);
}

/* Delete a member, returning one if it existed and zero otherwise. */
int zbtreeDelete(zbtreeSet *zs, sds ele) {
    uint32_t hash = (uint32_t)dictGenHashFunction(ele, sdslen(ele));
    zbtScoreLeaf *score_leaf;
    zbtIndexBucket *bucket;
    unsigned int score_pos, bucket_pos;

    if (!zbtIndexFind(zs, hash, (unsigned char *)ele, sdslen(ele), NULL,
                      &score_leaf, &score_pos, &bucket, &bucket_pos))
        return 0;
    zbtIndexDeleteAt(zs, bucket, bucket_pos);
    zbtScoreDeleteAt(zs, score_leaf, score_pos, 1);
    zs->length--;
    zbtIndexShrinkIfNeeded(zs);
    return 1;
}

/* Change a score without moving the element when two conditions hold:
 *
 *  1. The new (score, member) pair remains strictly between its neighbors.
 *  2. The leaf's current base, shift and width represent the score exactly.
 *
 * In that common case only one packed field and possibly maximums above it
 * change. Return zero when the caller must remove and reinsert the element.
 */
static int zbtScoreUpdateInPlace(zbtScoreLeaf *leaf, unsigned int pos,
                                 double score, const unsigned char *ele,
                                 size_t elelen)
{
    zbtScoreLeaf *neighbor = pos ? leaf : leaf->prev;
    unsigned int neighbor_pos = pos ? pos - 1 :
        (neighbor ? (unsigned int)neighbor->n.count - 1 : 0);
    if (neighbor) {
        double other_score = zbtScoreLeafScore(neighbor, neighbor_pos);
        if (score < other_score) return 0;
        if (score == other_score) {
            size_t len;
            const unsigned char *other =
                zbtScoreLeafElement(neighbor, neighbor_pos, &len);
            if (zbtCompareElements(ele, elelen, other, len) <= 0) return 0;
        }
    }

    neighbor = pos + 1 < leaf->n.count ? leaf : leaf->next;
    neighbor_pos = pos + 1 < leaf->n.count ? pos + 1 : 0;
    if (neighbor) {
        double other_score = zbtScoreLeafScore(neighbor, neighbor_pos);
        if (score > other_score) return 0;
        if (score == other_score) {
            size_t len;
            const unsigned char *other =
                zbtScoreLeafElement(neighbor, neighbor_pos, &len);
            if (zbtCompareElements(ele, elelen, other, len) >= 0) return 0;
        }
    }

    uint64_t delta;
    if (!zbtScoreLeafPackScore(leaf, score, &delta)) return 0;

    if (leaf->score_bits) {
        unsigned int physical = zbtScoreLeafPhysicalPos(leaf, pos);
        zbtWriteBits(leaf->data, (size_t)physical * leaf->score_bits,
                     leaf->score_bits, delta);
    }
    if (pos + 1 == leaf->n.count) zbtScoreUpdatePath(&leaf->n, 0);
    debugServerAssert(zbtScoreLeafScore(leaf, pos) == score);
    return 1;
}

/* Update an existing member's score. Its hash entry remains in place unless
 * the member lands in a different score leaf. */
void zbtreeUpdateScore(zbtreeSet *zs, sds ele, double score,
                       const zbtreeInsertPosition *position)
{
    uint32_t hash = position->hash;
    zbtScoreLeaf *oldleaf = position->score_leaf;
    unsigned int oldpos = position->score_pos;

    serverAssert(position->revision == zs->member_revision);
    serverAssert(oldleaf != NULL && oldpos < oldleaf->n.count);

    /* Most small score changes preserve the member's position. In that case
     * only its packed score bits and the maxima on the path are changed. */
    if (zbtScoreUpdateInPlace(oldleaf, oldpos, score,
                              (unsigned char *)ele, sdslen(ele)))
        return;

    /* A larger move removes and reinserts the member in the score tree. Its
     * hash entry stays in place; only the score leaf number may need
     * to change.
     */
    zbtBuildElement moved;
    zbtBuildElementFromLeaf(&moved, oldleaf, oldpos);
    if (moved.external) {
        serverAssert(moved.hash == hash);
        zbtBuildElementDetach(&moved);
    } else {
        /* Inline bytes may move or disappear during deletion, so use the
         * command argument until the member has its new record. */
        zbtBuildElementFromBytes(&moved, (unsigned char *)ele,
                                 sdslen(ele), hash);
    }
    uint32_t oldid = oldleaf->id;
    zbtScoreDeleteAt(zs, oldleaf, oldpos, 0);
    zbtScoreLeaf *newleaf = zbtScoreInsert(zs, score, &moved);
    if (oldid != newleaf->id) {
        int target_was_copied = 0;
        serverAssert(zbtIndexMove(zs, hash, oldid, newleaf->id, 0,
                                  &target_was_copied));
    }
}

/* Give surviving score leaves dense IDs and shrink their address table. The
 * member index is rebuilt immediately afterwards, so no old leaf ID survives. */
static void zbtScoreRenumberLeaves(zbtreeSet *zs) {
    uint32_t count = 0;
    for (zbtScoreLeaf *leaf = zs->score_first; leaf; leaf = leaf->next)
        count++;

    zbtScoreLeaf **oldtable = zs->score_leaf_by_id;
    if (count == 0) {
        zs->score_leaf_by_id = NULL;
        zs->score_leaf_cap = 0;
        zs->next_score_leaf_id = 0;
        zs->free_score_leaf_id = 0;
        if (oldtable) zbtFreeAllocation(zs, oldtable);
        return;
    }

    uint32_t cap = 16;
    while (cap < count) cap *= 2;
    zbtScoreLeaf **table = zbtAlloc(zs, cap * sizeof(*table));
    memset(table, 0, cap * sizeof(*table));

    uint32_t id = 0;
    for (zbtScoreLeaf *leaf = zs->score_first; leaf; leaf = leaf->next) {
        leaf->id = id;
        leaf->n.index_resize = 0;
        table[id++] = leaf;
    }
    zs->score_leaf_by_id = table;
    zs->score_leaf_cap = cap;
    zs->next_score_leaf_id = count;
    zs->free_score_leaf_id = 0;
    if (oldtable) zbtFreeAllocation(zs, oldtable);
}

/* Rebuild the hash index from the surviving score leaves. This also removes
 * tombstones and makes every entry refer to the newly dense leaf numbers. */
static void zbtIndexRebuild(zbtreeSet *zs) {
    zbtScoreRenumberLeaves(zs);
    zbtIndexTable oldindex = zs->member_index;
    zbtIndexRehash *oldrehash = zs->member_rehash;
    memset(&zs->member_index, 0, sizeof(zs->member_index));
    zs->member_rehash = NULL;

    if (zs->length)
        zbtIndexTableInit(zs, &zs->member_index,
                          zbtIndexBucketsForElements(zs->length),
                          zs->next_score_leaf_id >= ZBT_INDEX_WIDE_ID_AT);
    unsigned long rebuilt = 0;
    for (zbtScoreLeaf *leaf = zs->score_first; leaf; leaf = leaf->next) {
        for (unsigned int i = 0; i < leaf->n.count; i++) {
            uint32_t hash = zbtScoreLeafHash(leaf, i);
            zbtIndexTableInsertRaw(&zs->member_index, hash, leaf->id);
            rebuilt++;
        }
    }
    serverAssert(rebuilt == zs->length);
    zbtIndexTableRelease(zs, &oldindex);
    if (oldrehash) {
        zbtIndexTableRelease(zs, &oldrehash->table);
        zbtFreeAllocation(zs, oldrehash);
    }
    zs->member_revision = zbtIndexNextRevision();
}

/* Delete the inclusive zero-based rank interval [start,end], returning the
 * number removed. Large deletions rebuild the member index from the survivors;
 * smaller deletions maintain it one score-leaf run at a time. */
unsigned long zbtreeDeleteRangeByRank(zbtreeSet *zs, unsigned long start,
                                      unsigned long end)
{
    if (start >= zs->length || start > end) return 0;
    if (end >= zs->length) end = zs->length - 1;
    unsigned long deleted = end - start + 1;

    if (deleted > 1024 && deleted >= zs->length / 10) {
        unsigned long remaining = deleted;
        while (remaining) {
            zbtreeIterator iter;
            serverAssert(zbtreeIteratorSeekRank(zs, start, &iter));
            zbtScoreLeaf *leaf = iter.leaf;
            unsigned long run = leaf->n.count - iter.pos;
            if (run > remaining) run = remaining;
            /* Nothing reads the old member index before zbtIndexRebuild(), so
             * maintaining entries for partial leaves would be wasted work. */
            zbtScoreDeleteRun(zs, leaf, iter.pos, run, 0, 0);
            remaining -= run;
        }
        zbtIndexRebuild(zs);
        return deleted;
    }

    /* Take as long a run as one leaf holds, so a range that fits in a leaf
     * is one rebuild rather than one shift per element. */
    unsigned long remaining = deleted;
    while (remaining) {
        zbtreeIterator iter;
        serverAssert(zbtreeIteratorSeekRank(zs, start, &iter));
        zbtScoreLeaf *leaf = iter.leaf;
        unsigned long run = leaf->n.count - iter.pos;
        if (run > remaining) run = remaining;
        zbtScoreDeleteRun(zs, leaf, iter.pos, run, 1, 1);
        remaining -= run;
    }
    zbtIndexShrinkIfNeeded(zs);
    return deleted;
}

/* Delete all elements inside a score interval. Convert its two bounds to
 * ranks and use the rank deletion path above. */
unsigned long zbtreeDeleteRangeByScore(zbtreeSet *zs, double min, int minex,
                                       double max, int maxex)
{
    zbtreeIterator first, last;
    unsigned long first_rank, last_rank;
    if (!zbtreeIteratorSeekScore(zs, min, minex, 0, &first, &first_rank))
        return 0;
    if (!zbtreeIteratorSeekScore(zs, max, maxex, 1, &last, &last_rank))
        return 0;
    if (last_rank < first_rank) return 0;
    return zbtreeDeleteRangeByRank(zs, first_rank, last_rank);
}

/* ------------------------------ Iterators ------------------------------- */

/* Iterators are a score leaf plus a logical position. zbtreeIteratorNext()
 * returns the current element and advances, which makes the usual loop:
 *
 *     seek or start;
 *     while (zbtreeIteratorNext(...)) { ... }
 *
 * The returned member pointer belongs to the current score leaf. Both it and
 * the iterator become invalid after any modification of this sorted set.
 */

/* Seek the element having the specified zero-based rank. */
int zbtreeIteratorSeekRank(const zbtreeSet *zs, unsigned long rank,
                           zbtreeIterator *iter)
{
    if (rank >= zs->length) {
        iter->leaf = NULL;
        iter->pos = 0;
        return 0;
    }

    zbtScoreNode *node = zs->score_root;
    while (!node->isleaf) {
        zbtScoreInner *inner = (zbtScoreInner *)node;
        unsigned int pos = 0;
        while (rank >= inner->child_count[pos]) {
            rank -= inner->child_count[pos];
            pos++;
        }
        node = inner->child[pos];
    }
    iter->leaf = node;
    iter->pos = rank;
    return 1;
}

/* Find the first score not below the bound. If after_equal is true, equal
 * scores are skipped too. A binary search avoids many packed score reads. */
static unsigned int zbtScoreLeafScoreBound(zbtScoreLeaf *leaf, double score,
                                           int after_equal)
{
    unsigned int lo = 0, hi = leaf->n.count;
    while (lo < hi) {
        unsigned int mid = lo + (hi - lo) / 2;
        double current = zbtScoreLeafScore(leaf, mid);
        if (current < score || (after_equal && current == score))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Seek a score bound. In forward order, find the first score greater than or
 * equal to the bound (strictly greater when exclusive). In reverse order,
 * find the last score less than or equal to it (strictly less when
 * exclusive). Return zero if no such element exists.
 *
 * If 'rank' is not NULL, also return the zero-based forward rank.
 */
int zbtreeIteratorSeekScore(const zbtreeSet *zs, double score, int exclusive,
                            int reverse, zbtreeIterator *iter,
                            unsigned long *rank)
{
    zbtScoreNode *node = zs->score_root;
    if (node == NULL) {
        iter->leaf = NULL;
        iter->pos = 0;
        return 0;
    }

    while (!node->isleaf) {
        zbtScoreInner *inner = (zbtScoreInner *)node;
        unsigned int pos = 0;
        if (reverse) {
            while (pos < inner->n.count &&
                   (exclusive ? inner->max_score[pos] < score :
                                inner->max_score[pos] <= score))
                pos++;
            if (pos == inner->n.count) pos--;
        } else {
            while (pos + 1 < inner->n.count &&
                   (exclusive ? inner->max_score[pos] <= score :
                                inner->max_score[pos] < score))
                pos++;
        }
        node = inner->child[pos];
    }

    zbtScoreLeaf *leaf = (zbtScoreLeaf *)node;
    if (reverse) {
        unsigned int pos = zbtScoreLeafScoreBound(leaf, score, !exclusive);
        if (pos == 0) {
            leaf = leaf->prev;
            if (leaf == NULL) goto notfound;
            pos = leaf->n.count - 1;
        } else {
            pos--;
        }
        iter->leaf = leaf;
        iter->pos = pos;
    } else {
        unsigned int pos = zbtScoreLeafScoreBound(leaf, score, exclusive);
        if (pos == leaf->n.count) {
            leaf = leaf->next;
            if (leaf == NULL) goto notfound;
            pos = 0;
        }
        iter->leaf = leaf;
        iter->pos = pos;
    }
    if (rank)
        *rank = zbtScoreRank(iter->leaf, iter->pos);
    return 1;

notfound:
    iter->leaf = NULL;
    iter->pos = 0;
    return 0;
}

/* Seek a lexical bound. Redis defines useful lexicographical ranges when all
 * members have the same score. Under that condition the score tree is already
 * ordered by member, so a normal (score, member) search finds the bound.
 * shared.minstring and shared.maxstring represent the two unbounded ends.
 */
int zbtreeIteratorSeekLex(const zbtreeSet *zs, sds ele, int exclusive,
                          int reverse, zbtreeIterator *iter,
                          unsigned long *rank)
{
    if (zs->score_first == NULL) goto notfound;

    if (reverse) {
        if (ele == shared.maxstring) {
            int found = zbtreeIteratorStart(zs, 1, iter);
            if (found && rank) *rank = zs->length - 1;
            return found;
        }
        if (ele == shared.minstring) goto notfound;
    } else {
        if (ele == shared.minstring) {
            int found = zbtreeIteratorStart(zs, 0, iter);
            if (found && rank) *rank = 0;
            return found;
        }
        if (ele == shared.maxstring) goto notfound;
    }

    /* Use the common score to turn the lexical bound into a normal score-tree
     * search instead of walking from one end of the set. */
    double score = zbtScoreLeafScore(zs->score_first, 0);
    zbtScoreLeaf *leaf =
        zbtScoreFindLeaf(zs, score, (unsigned char *)ele, sdslen(ele));
    unsigned int pos =
        zbtScoreLeafLowerBound(leaf, score, (unsigned char *)ele, sdslen(ele));
    int equal = 0;

    if (pos < leaf->n.count) {
        size_t len;
        const unsigned char *found = zbtScoreLeafElement(leaf, pos, &len);
        equal = zbtCompareElements(found, len, (unsigned char *)ele,
                                   sdslen(ele)) == 0;
    }

    if (reverse) {
        if (!equal || exclusive) {
            if (pos != 0) {
                pos--;
            } else {
                leaf = leaf->prev;
                if (leaf == NULL) goto notfound;
                pos = leaf->n.count - 1;
            }
        }
    } else if (equal && exclusive) {
        pos++;
    }

    if (!reverse && pos == leaf->n.count) {
        leaf = leaf->next;
        if (leaf == NULL) goto notfound;
        pos = 0;
    }

    iter->leaf = leaf;
    iter->pos = pos;
    if (rank) *rank = zbtScoreRank(leaf, pos);
    return 1;

notfound:
    iter->leaf = NULL;
    iter->pos = 0;
    return 0;
}

/* Position an iterator at the first element, or the last when reverse is
 * non-zero. Return zero for an empty set. */
int zbtreeIteratorStart(const zbtreeSet *zs, int reverse,
                        zbtreeIterator *iter)
{
    zbtScoreLeaf *leaf = reverse ? zs->score_last : zs->score_first;
    iter->leaf = leaf;
    iter->pos = leaf ? (reverse ? leaf->n.count - 1 : 0) : 0;
    return leaf != NULL;
}

/* Return the current member and score, then advance to the next logical
 * position. The output pointers are optional. Return zero at the end. */
int zbtreeIteratorNext(zbtreeIterator *iter, int reverse,
                       const unsigned char **ele, size_t *len, double *score)
{
    zbtScoreLeaf *leaf = iter->leaf;
    if (leaf == NULL) return 0;

    unsigned int pos = iter->pos;
    if (ele || len) {
        size_t elelen;
        const unsigned char *element =
            zbtScoreLeafElement(leaf, pos, &elelen);
        if (ele) *ele = element;
        if (len) *len = elelen;
    }
    if (score) *score = zbtScoreLeafScore(leaf, pos);

    if (reverse) {
        if (pos != 0) {
            iter->pos--;
        } else {
            leaf = leaf->prev;
            iter->leaf = leaf;
            if (leaf) iter->pos = leaf->n.count - 1;
        }
    } else {
        if (pos + 1 < leaf->n.count) {
            iter->pos++;
        } else {
            leaf = leaf->next;
            iter->leaf = leaf;
            iter->pos = 0;
        }
    }
    return 1;
}

/* Return the rank where a score bound would be inserted. Complete children
 * are counted on the way down, so there is no second walk back to the root. */
static unsigned long zbtScoreRankForBound(const zbtreeSet *zs, double score,
                                          int after_equal)
{
    zbtScoreNode *node = zs->score_root;
    unsigned long rank = 0;
    if (node == NULL) return 0;

    while (!node->isleaf) {
        zbtScoreInner *inner = (zbtScoreInner *)node;
        unsigned int pos = 0;
        while (pos + 1 < inner->n.count &&
               (inner->max_score[pos] < score ||
                (after_equal && inner->max_score[pos] == score)))
        {
            rank += inner->child_count[pos++];
        }
        node = inner->child[pos];
    }

    zbtScoreLeaf *leaf = (zbtScoreLeaf *)node;
    return rank + zbtScoreLeafScoreBound(leaf, score, after_equal);
}

/* Count a score range as the difference between its two insertion ranks.
 * Inclusive infinities are already the first and final ranks and need no
 * tree walk. */
unsigned long zbtreeCountByScore(const zbtreeSet *zs, double min, int minex,
                                 double max, int maxex)
{
    unsigned long first = !minex && isinf(min) && min < 0 ? 0 :
        zbtScoreRankForBound(zs, min, minex);
    unsigned long last = !maxex && isinf(max) && max > 0 ? zs->length :
        zbtScoreRankForBound(zs, max, !maxex);
    return last > first ? last - first : 0;
}

/* Return whether a lookup for hash examines target before an empty bucket ends
 * its search. This is used only when several members in one leaf share a tag
 * and a table slot alone cannot tell which member it represents. */
static int zbtIndexHashReachesBucket(const zbtIndexTable *table, uint32_t hash,
                                     const zbtIndexBucket *target)
{
    unsigned long mask = table->size - 1;
    unsigned long index = hash & mask;

    for (unsigned long probes = 0; probes < table->size; probes++) {
        zbtIndexBucket *bucket = zbtIndexBucketAt(table, index);
        if (bucket == target) return 1;
        if (zbtIndexTagMask(zbtIndexTags(bucket), 0)) return 0;
        index = (index + 1) & mask;
    }
    return 0;
}

/* Return the members represented by one physical table slot. Usually its tag
 * occurs once in the score leaf and the answer is immediate. If tags collide,
 * emit each member from every matching slot on its own search path. Duplicates
 * are allowed by ZSCAN, while this rule cannot move a member behind the cursor
 * merely because another member was inserted or changed score. */
static unsigned long zbtIndexScanSlot(const zbtreeSet *zs,
                                      zbtIndexTable *table,
                                      zbtIndexBucket *bucket,
                                      unsigned int slot_pos,
                                      zbtreeScanFunction *fn,
                                      void *privdata)
{
    uint8_t tag = zbtIndexTags(bucket) >> (slot_pos * 8);
    uint32_t id = zbtIndexGetId(table, bucket, slot_pos);
    if (tag == 0 || id == ZBT_INDEX_DELETED_ID) return 0;
    zbtScoreLeaf *leaf = id < zs->next_score_leaf_id ?
        zs->score_leaf_by_id[id] : NULL;
    if (leaf == NULL || ZBT_IS_FREE_LEAF_ID(leaf)) {
        serverAssert(zs->member_rehash && table == &zs->member_index);
        return 0;
    }
    serverAssert(leaf->id == id);
    if (zs->member_rehash) {
        int in_new = table == &zs->member_rehash->table;
        if (in_new != zbtIndexLeafMigrated(zs, leaf)) return 0;
    }

    uint32_t hashes[ZBT_SCORE_LEAF_MAX];
    unsigned int positions[ZBT_SCORE_LEAF_MAX];
    unsigned int count = 0;
    for (unsigned int pos = 0; pos < leaf->n.count; pos++) {
        uint8_t leaf_tag = zbtScoreLeafTag(leaf, pos);
        /* Index tags fold zero into one because zero marks an empty slot.
         * Leaf tags keep the raw byte, so index tag one covers both values. */
        if (leaf_tag != tag && !(tag == 1 && leaf_tag == 0)) continue;
        uint32_t hash = zbtScoreLeafHash(leaf, pos);
        if (zbtIndexTag(hash) != tag) continue;
        hashes[count] = hash;
        positions[count++] = pos;
    }
    serverAssert(count != 0);

    unsigned long emitted = 0;
    for (unsigned int i = 0; i < count; i++) {
        if (count > 1 &&
            !zbtIndexHashReachesBucket(table, hashes[i], bucket))
            continue;
        size_t len;
        const unsigned char *ele =
            zbtScoreLeafElement(leaf, positions[i], &len);
        fn(privdata, ele, len, zbtScoreLeafScore(leaf, positions[i]));
        emitted++;
    }
    serverAssert(emitted != 0);
    return emitted;
}

/* The low half of the cursor is a group of eight buckets plus one; the high
 * half identifies the current table. A resize keeps the old table in place, so
 * the cursor remains valid while leaves are copied. Installing the new table
 * changes its revision and makes the next call restart. This can return
 * duplicates, as allowed by the SCAN contract. A cursor cannot be translated
 * if a caller explicitly changes the object to another encoding. */
uint64_t zbtreeScan(zbtreeSet *zs, uint64_t cursor,
                    unsigned long count, zbtreeScanFunction *fn,
                    void *privdata)
{
    if (zs->length == 0) return 0;
    uint32_t revision = cursor >> 32;
    uint64_t group = cursor ? (uint32_t)cursor - 1 : 0;
    if (cursor == 0 || revision != zs->member_index.scan_revision) {
        revision = zs->member_index.scan_revision;
        group = 0;
    }

    uint64_t first_buckets = zs->member_index.size;
    uint64_t total_buckets = first_buckets;
    if (zs->member_rehash)
        total_buckets += zs->member_rehash->table.size;
    uint64_t bucket_index = group * ZBT_SCAN_BUCKETS_PER_STEP;
    if (bucket_index >= total_buckets) return 0;
    unsigned long emitted = 0;
    while (bucket_index < total_buckets && emitted < count) {
        uint64_t end = bucket_index + ZBT_SCAN_BUCKETS_PER_STEP;
        if (end > total_buckets) end = total_buckets;

        /* Finish the whole group before saving the cursor. COUNT is a hint,
         * as it is for a normal dictionary scan. */
        while (bucket_index < end) {
            zbtIndexTable *table;
            uint64_t local;
            if (bucket_index < first_buckets) {
                table = &zs->member_index;
                local = bucket_index;
            } else {
                serverAssert(zs->member_rehash != NULL);
                table = &zs->member_rehash->table;
                local = bucket_index - first_buckets;
            }
            zbtIndexBucket *bucket = zbtIndexBucketAt(table, local);
            for (unsigned int pos = 0; pos < ZBT_INDEX_BUCKET_ITEMS; pos++)
                emitted += zbtIndexScanSlot(zs, table, bucket, pos,
                                            fn, privdata);
            bucket_index++;
        }
        group++;
    }
    if (bucket_index >= total_buckets) return 0;
    serverAssert(group < UINT32_MAX);
    return ((uint64_t)revision << 32) | (uint32_t)(group + 1);
}
