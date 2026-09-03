/* zbtree.c -- Order-statistic B+ tree used as the large-encoding backend of
 * Redis sorted sets (ZSETs), replacing the previous skiplist.
 *
 * Design goals (see the ZSET encoding notes in server.h):
 *   - Elements are ordered by (score, member) exactly like the old skiplist.
 *   - Each member is a single heap object (zbtElem) with an embedded SDS, so
 *     the ZSET dict can keep mapping member -> zbtElem* for O(1) score lookup.
 *   - Leaves are packed arrays of zbtElem pointers, doubly linked so range
 *     scans walk contiguous memory instead of chasing skiplist pointers.
 *   - Internal nodes carry per-child subtree sizes, giving O(log N) rank and
 *     rank-based access (ZRANK / ZRANGE by index / ZREMRANGEBYRANK).
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "server.h"
#include <math.h>

/* Fanout of the tree. Nodes are allowed to temporarily hold one extra slot
 * (hence the "+1" sized arrays) before they are split.
 *
 * Every node holds at least its MIN, with three exceptions: the root, and the
 * head and tail leaves, which zbtSplitLeaf() deliberately starts under-filled
 * so that sorted insertion does not strand every leaf at half occupancy. */

/* Which end of the leaf the element that overflowed it landed on. Sorted
 * insertion never returns to the side the split leaves behind, so an even
 * split would strand that side half full for the rest of its life. */
#define ZBT_SPLIT_EVEN    0
#define ZBT_SPLIT_APPEND  1  /* went in at the end of the tail leaf */
#define ZBT_SPLIT_PREPEND 2  /* went in at the front of the head leaf */
/* Leaf fanout. A smaller fanout (35, the largest that still fits the
 * jemalloc 320-byte size class) was tried and measured: it saved ~0.2% of
 * total memory on a 1M-element zset, but cost ~4% throughput on range scans
 * (more, smaller leaves means more leaf-to-leaf pointer chases per scan).
 * Prefetching the next leaf and a few upcoming elements in
 * zbtIterNext()/zbtIterPrev() recovered that loss, but adding the same
 * prefetching back here at fanout 64 only moved throughput by ~1% at best
 * (fewer, bigger leaves already mean fewer boundary crossings to hide the
 * latency of) - not worth the extra branches in a hot path, so left out. */
#define ZBT_LEAF_MAX   64
#define ZBT_LEAF_MIN   (ZBT_LEAF_MAX/2)
#define ZBT_INNER_MAX  64
#define ZBT_INNER_MIN  (ZBT_INNER_MAX/2)

/* Common node header. Both leaf and inner nodes start with it so that a
 * zbtNode* can be inspected polymorphically. */
struct zbtNode {
    struct zbtNode *parent;
    uint32_t count;   /* used slots: elems (leaf) or children (inner) */
    uint32_t isleaf;
};

typedef struct zbtLeaf {
    zbtNode n;
    struct zbtLeaf *prev, *next;         /* sibling leaves (sorted order) */
    zbtElem *elems[ZBT_LEAF_MAX + 1];
} zbtLeaf;


typedef struct zbtInner {
    zbtNode n;
    struct zbtNode *child[ZBT_INNER_MAX + 1];
    unsigned long csize[ZBT_INNER_MAX + 1]; /* subtree element count of child[i] */
    zbtElem *sep[ZBT_INNER_MAX + 1];        /* minimum element of child[i] */
} zbtInner;

/*-----------------------------------------------------------------------------
 * Element allocation
 *----------------------------------------------------------------------------*/

/* moff is bounded by the header plus the widest score plus the largest sds
 * header, so a single byte is always enough to hold it. */
static_assert(sizeof(zbtElem) + 8 + sizeof(struct sdshdr64) <= UINT8_MAX,
              "zbtElem member offset must fit in a byte");

static size_t zbtScoreEncSize(uint8_t enc) {
    switch (enc) {
    case ZBT_SCORE_I8:  return 1;
    case ZBT_SCORE_I16: return 2;
    case ZBT_SCORE_I24: return 3;
    case ZBT_SCORE_I32: return 4;
    case ZBT_SCORE_I48: return 6;
    default:            return 8;
    }
}

/* Pick the narrowest integer encoding that round-trips 'd', or a raw double.
 * Not on the compare hot path; zbtGetScore is. */
static void zbtScoreEncode(double d, uint8_t *enc, unsigned char *buf) {
    long long ll;

    /* double2ll(-0.0) succeeds with 0, but ZSCORE must still reply -0. */
    if (d == 0 && signbit(d)) {
        *enc = ZBT_SCORE_DBL;
        memcpy(buf, &d, sizeof(d));
        return;
    }
    if (!double2ll(d, &ll)) {
        *enc = ZBT_SCORE_DBL;
        memcpy(buf, &d, sizeof(d));
        return;
    }
    if (ll >= INT8_MIN && ll <= INT8_MAX) {
        *enc = ZBT_SCORE_I8;
        buf[0] = (unsigned char)(int8_t)ll;
    } else if (ll >= INT16_MIN && ll <= INT16_MAX) {
        int16_t v = (int16_t)ll;
        *enc = ZBT_SCORE_I16;
        memcpy(buf, &v, 2);
    } else if (ll >= -(1LL << 23) && ll <= ((1LL << 23) - 1)) {
        unsigned long long u = (unsigned long long)ll;
        *enc = ZBT_SCORE_I24;
        buf[0] = (unsigned char)u;
        buf[1] = (unsigned char)(u >> 8);
        buf[2] = (unsigned char)(u >> 16);
    } else if (ll >= INT32_MIN && ll <= INT32_MAX) {
        int32_t v = (int32_t)ll;
        *enc = ZBT_SCORE_I32;
        memcpy(buf, &v, 4);
    } else if (ll >= -(1LL << 47) && ll <= ((1LL << 47) - 1)) {
        unsigned long long u = (unsigned long long)ll;
        *enc = ZBT_SCORE_I48;
        buf[0] = (unsigned char)u;
        buf[1] = (unsigned char)(u >> 8);
        buf[2] = (unsigned char)(u >> 16);
        buf[3] = (unsigned char)(u >> 24);
        buf[4] = (unsigned char)(u >> 32);
        buf[5] = (unsigned char)(u >> 40);
    } else {
        *enc = ZBT_SCORE_DBL;
        memcpy(buf, &d, sizeof(d));
    }
}

/* Allocate an element with the member SDS embedded in the same allocation
 * (single block: zbtElem header + score bytes + sds header + data). The
 * member is copied from 'buf', which does not have to be an sds: callers
 * holding plain bytes (listpack entries, integer members) can build an
 * element without first materializing a temporary sds. When 'wide' is set
 * the score is stored as a raw double so a later in-place write cannot
 * overflow the allocation. */
static zbtElem *zbtCreateElemBufGen(double score, const char *buf, size_t len,
                                    int wide)
{
    uint8_t enc;
    unsigned char sbuf[8];
    if (wide) {
        enc = ZBT_SCORE_DBL;
        memcpy(sbuf, &score, sizeof(score));
    } else {
        zbtScoreEncode(score, &enc, sbuf);
    }
    size_t score_sz = zbtScoreEncSize(enc);
    char sds_type = sdsReqType(len);
    size_t sds_hdr_len = sdsHdrSize(sds_type);
    size_t hdr = sizeof(zbtElem) + score_sz;
    size_t sds_buf_size = sds_hdr_len + len + 1;
    size_t total = hdr + sds_buf_size;

    zbtElem *e = zmalloc(total);
    e->enc = enc;
    memcpy(e->data, sbuf, score_sz);
    size_t sds_offset = hdr + sds_hdr_len;
    zbtSetOffset(e, (uint8_t)sds_offset);

    char *dst = (char *)e + hdr;
    sds emb = sdsnewplacement(dst, sds_buf_size, sds_type, buf, len);
    serverAssert(emb == (sds)((char *)e + sds_offset));
    return e;
}

/* Allocate an element with the member SDS embedded in the same allocation
 * (single block: zbtElem header + score bytes + sds header + data). The
 * member is copied from 'buf', which does not have to be an sds: callers
 * holding plain bytes (listpack entries, integer members) can build an
 * element without first materializing a temporary sds. */
zbtElem *zbtCreateElemBuf(double score, const char *buf, size_t len) {
    return zbtCreateElemBufGen(score, buf, len, 0);
}

/* Same as zbtCreateElemBuf(), for callers that already hold an sds. The caller
 * keeps ownership of 'ele' (it is copied). */
zbtElem *zbtCreateElem(double score, sds ele) {
    return zbtCreateElemBuf(score, ele, sdslen(ele));
}

/* Like zbtCreateElem(), but the score is forced to ZBT_SCORE_DBL so a later
 * in-place write of any double (ZUNIONSTORE aggregation) cannot overflow. */
zbtElem *zbtCreateElemWide(double score, sds ele) {
    return zbtCreateElemBufGen(score, ele, sdslen(ele), 1);
}

/* Free a detached element that is not owned by any tree. Used by callers that
 * allocate an element with zbtCreateElem() but fail before ownership is
 * transferred to a tree (e.g. duplicate detection on RDB load). */
void zbtFreeElem(zbtElem *e) {
    zfree(e);  /* embedded sds is part of the allocation, no separate free */
}

/* Compare {score, ele} with element 'e'. Returns 1 (bigger), 0 (equal),
 * -1 (smaller). NULL is treated as +infinity. Ordering: score, then member. */
int zbtCompare(double score, sds ele, const zbtElem *e) {
    if (e == NULL) return -1;
    double escore = zbtGetScore(e);
    if (score < escore) return -1;
    if (score > escore) return 1;
    return sdscmp(ele, zbtGetEle(e));
}

/* dict keyFromStoredKey callback: recover the member SDS from a stored
 * zbtElem*. */
const void *zbtGetEleForDict(const void *elem) {
    return zbtGetEle((const zbtElem *)elem);
}

/*-----------------------------------------------------------------------------
 * Node allocation / tree lifecycle
 *----------------------------------------------------------------------------*/

static zbtLeaf *zbtNewLeaf(zbtree *t) {
    size_t usable;
    zbtLeaf *lf = zmalloc_usable(sizeof(*lf), &usable);
    lf->n.parent = NULL;
    lf->n.count = 0;
    lf->n.isleaf = 1;
    lf->prev = lf->next = NULL;
    t->alloc_size += usable;
    return lf;
}

static zbtInner *zbtNewInner(zbtree *t) {
    size_t usable;
    zbtInner *in = zmalloc_usable(sizeof(*in), &usable);
    in->n.parent = NULL;
    in->n.count = 0;
    in->n.isleaf = 0;
    t->alloc_size += usable;
    return in;
}

static void zbtFreeNodeShallow(zbtree *t, zbtNode *n) {
    size_t usable;
    zfree_usable(n, &usable);
    t->alloc_size -= usable;
}

zbtree *zbtCreate(void) {
    size_t usable;
    zbtree *t = zmalloc_usable(sizeof(*t), &usable);
    t->length = 0;
    t->alloc_size = usable;
    t->root = NULL;
    t->defrag_resume = NULL;
    t->defrag_resume_score = 0;
    zbtLeaf *lf = zbtNewLeaf(t);
    t->root = (zbtNode *)lf;
    t->head = t->tail = (zbtNode *)lf;
    return t;
}

static void zbtFreeSubtree(zbtree *t, zbtNode *n) {
    if (n->isleaf) {
        zbtLeaf *lf = (zbtLeaf *)n;
        for (uint32_t i = 0; i < n->count; i++) {
            t->alloc_size -= zmalloc_usable_size(lf->elems[i]);
            zbtFreeElem(lf->elems[i]);
        }
    } else {
        zbtInner *in = (zbtInner *)n;
        for (uint32_t i = 0; i < n->count; i++)
            zbtFreeSubtree(t, in->child[i]);
    }
    zbtFreeNodeShallow(t, n);
}

void zbtFree(zbtree *t) {
    zbtFreeSubtree(t, t->root);
    if (t->defrag_resume) sdsfree(t->defrag_resume);
    zfree(t);
}

size_t zbtAllocSize(const zbtree *t) { return t->alloc_size; }

/*-----------------------------------------------------------------------------
 * Navigation helpers
 *----------------------------------------------------------------------------*/

/* Minimum element of a subtree rooted at 'n' (assumes non-empty). An inner node
 * records the minimum of every child in sep[], so sep[0] is already the minimum
 * of the whole subtree and there is no need to descend to the leftmost leaf.
 * Callers must therefore keep sep[] consistent bottom-up, which is what
 * zbtUpdateToRoot() and the split/merge paths do. */
static zbtElem *zbtNodeMin(zbtNode *n) {
    if (n->isleaf) return ((zbtLeaf *)n)->elems[0];
    return ((zbtInner *)n)->sep[0];
}

/* Number of elements contained in the subtree rooted at 'n'. */
static unsigned long zbtSubtreeSize(zbtNode *n) {
    if (n->isleaf) return n->count;
    zbtInner *in = (zbtInner *)n;
    unsigned long s = 0;
    for (uint32_t i = 0; i < in->n.count; i++) s += in->csize[i];
    return s;
}

/* Index of child 'c' inside inner node 'p'. */
static int zbtChildIdx(zbtInner *p, zbtNode *c) {
    for (uint32_t i = 0; i < p->n.count; i++)
        if (p->child[i] == c) return (int)i;
    serverPanic("zbtree: child not found in parent");
}

/* Choose the child of inner node 'in' whose key range contains (score,ele). */
static int zbtInnerChildIdx(zbtInner *in, double score, sds ele) {
    int i = (int)in->n.count - 1;
    while (i > 0 && zbtCompare(score, ele, in->sep[i]) < 0) i--;
    return i;
}

/* Descend from the root to the leaf that would contain (score,ele). */
static zbtLeaf *zbtFindLeaf(zbtree *t, double score, sds ele) {
    zbtNode *n = t->root;
    while (!n->isleaf) {
        zbtInner *in = (zbtInner *)n;
        n = in->child[zbtInnerChildIdx(in, score, ele)];
    }
    return (zbtLeaf *)n;
}

/* Locate (score,ele) inside a leaf. Sets *found and returns the index where
 * the element is (if found) or where it should be inserted. */
static int zbtLeafSearch(zbtLeaf *lf, double score, sds ele, int *found) {
    uint32_t i;
    for (i = 0; i < lf->n.count; i++) {
        int c = zbtCompare(score, ele, lf->elems[i]);
        if (c == 0) { *found = 1; return (int)i; }
        if (c < 0) { *found = 0; return (int)i; }
    }
    *found = 0;
    return (int)lf->n.count;
}

/* Refresh csize/sep for every ancestor of 'n' up to the root. Used after an
 * insertion, deletion or in-place element replacement changed a subtree.
 *
 * The caller has already made 'n' itself consistent, and 'n' is the only child
 * whose recorded size can be stale. It is stale by a fixed amount, and every
 * ancestor's total is off by that same amount, so the subtree is sized once at
 * the bottom and the difference is propagated upwards rather than re-summing
 * csize[] at every level. The walk stops at the first level that turns out to
 * be unchanged, because then no ancestor above it can change either: that makes
 * sibling borrows and defrag replacements O(1) instead of O(height). */
static void zbtUpdateToRoot(zbtree *t, zbtNode *n) {
    UNUSED(t);
    long delta = 0;
    int have_delta = 0;

    while (n->parent) {
        zbtInner *p = (zbtInner *)n->parent;
        int idx = zbtChildIdx(p, n);
        unsigned long oldsize = p->csize[idx];
        unsigned long newsize;

        if (have_delta) {
            newsize = (unsigned long)((long)oldsize + delta);
        } else {
            /* O(1) when the walk starts at a leaf, which is the common case;
             * only the split/merge paths start at an inner node. */
            newsize = zbtSubtreeSize(n);
            delta = (long)newsize - (long)oldsize;
            have_delta = 1;
        }

        zbtElem *newsep = zbtNodeMin(n);
        if (newsize == oldsize && newsep == p->sep[idx]) return;
        p->csize[idx] = newsize;
        p->sep[idx] = newsep;
        n = (zbtNode *)p;
    }
}

/*-----------------------------------------------------------------------------
 * Insertion
 *----------------------------------------------------------------------------*/

static void zbtSplitInner(zbtree *t, zbtInner *in);

/* Insert 'right' as a new child immediately after 'left' in their parent.
 * If 'p' is NULL, 'left' is the current root and a new root is created. */
static void zbtInsertChild(zbtree *t, zbtInner *p, zbtNode *left, zbtNode *right) {
    if (p == NULL) {
        zbtInner *root = zbtNewInner(t);
        root->n.count = 2;
        root->child[0] = left;  left->parent = (zbtNode *)root;
        root->child[1] = right; right->parent = (zbtNode *)root;
        root->csize[0] = zbtSubtreeSize(left);  root->sep[0] = zbtNodeMin(left);
        root->csize[1] = zbtSubtreeSize(right); root->sep[1] = zbtNodeMin(right);
        t->root = (zbtNode *)root;
        return;
    }

    int li = zbtChildIdx(p, left);
    int at = li + 1;
    int tail = (int)p->n.count - at;
    memmove(&p->child[at + 1], &p->child[at], tail * sizeof(zbtNode *));
    memmove(&p->csize[at + 1], &p->csize[at], tail * sizeof(unsigned long));
    memmove(&p->sep[at + 1], &p->sep[at], tail * sizeof(zbtElem *));
    p->child[at] = right;
    right->parent = (zbtNode *)p;
    p->n.count++;

    p->csize[li] = zbtSubtreeSize(left);  p->sep[li] = zbtNodeMin(left);
    p->csize[at] = zbtSubtreeSize(right); p->sep[at] = zbtNodeMin(right);

    if (p->n.count > ZBT_INNER_MAX)
        zbtSplitInner(t, p);
    else
        zbtUpdateToRoot(t, (zbtNode *)p);
}

static void zbtSplitInner(zbtree *t, zbtInner *in) {
    zbtInner *r = zbtNewInner(t);
    int total = (int)in->n.count; /* == ZBT_INNER_MAX + 1 */
    int keep = total / 2;
    int move = total - keep;
    memcpy(r->child, &in->child[keep], move * sizeof(zbtNode *));
    memcpy(r->csize, &in->csize[keep], move * sizeof(unsigned long));
    memcpy(r->sep, &in->sep[keep], move * sizeof(zbtElem *));
    r->n.count = move;
    in->n.count = keep;
    for (int i = 0; i < move; i++) r->child[i]->parent = (zbtNode *)r;
    zbtInsertChild(t, (zbtInner *)in->n.parent, (zbtNode *)in, (zbtNode *)r);
}

/* Split the overfull leaf 'lf' in two, biased by where the overflowing element
 * landed (see the ZBT_SPLIT_* constants).
 *
 * An even split is right in the general case but wrong for sorted insertion:
 * inserts never come back to the side left behind, so it stays half full for
 * the rest of its life and a sorted bulk ZADD needs twice the leaves its
 * contents call for. Peeling off just the overflowing element instead keeps the
 * other side full. Ascending insertion leaves the left side full and starts a
 * fresh tail; descending insertion is the mirror image, keeping one element in
 * the head leaf and handing the full load to the new right leaf.
 *
 * Since ZBT_LEAF_MIN is ZBT_LEAF_MAX/2, no skew is possible without letting the
 * under-filled side sit below the minimum -- which is why the head and tail
 * leaves are exempt from it (see zbtVerifyNode()). Further inserts fill them
 * again, and the delete paths need no special case: a full neighbour can always
 * cover a short end leaf's deficit, and rebalancing only merges when the pair
 * totals below 2 * ZBT_LEAF_MIN, so it can never overfill a leaf. */
static void zbtSplitLeaf(zbtree *t, zbtLeaf *lf, int bias) {
    zbtLeaf *r = zbtNewLeaf(t);
    int total = (int)lf->n.count; /* == ZBT_LEAF_MAX + 1 */
    int keep = total / 2;
    if (bias == ZBT_SPLIT_APPEND) keep = total - 1;
    else if (bias == ZBT_SPLIT_PREPEND) keep = 1;
    int move = total - keep;
    memcpy(r->elems, &lf->elems[keep], move * sizeof(zbtElem *));
    r->n.count = move;
    lf->n.count = keep;

    r->next = lf->next;
    r->prev = lf;
    if (lf->next) lf->next->prev = r;
    else t->tail = (zbtNode *)r;
    lf->next = r;

    zbtInsertChild(t, (zbtInner *)lf->n.parent, (zbtNode *)lf, (zbtNode *)r);
}

/* Insert an already-allocated element. The caller must guarantee the member
 * is not already present. Ownership of 'e' transfers to the tree. */
void zbtInsertElem(zbtree *t, zbtElem *e) {
    double score = zbtGetScore(e);
    sds ele = zbtGetEle(e);
    zbtLeaf *lf = zbtFindLeaf(t, score, ele);
    int found;
    int idx = zbtLeafSearch(lf, score, ele, &found);
    serverAssert(!found);

    /* Landing at either end of an end leaf means the tree is growing in sorted
     * order; zbtSplitLeaf() then keeps the other side of the split full. Both
     * cannot hold at once: that would need idx to be 0 and lf->n.count at the
     * same time, and an empty leaf never overflows. */
    int bias = ZBT_SPLIT_EVEN;
    if (lf->next == NULL && idx == (int)lf->n.count) bias = ZBT_SPLIT_APPEND;
    else if (lf->prev == NULL && idx == 0) bias = ZBT_SPLIT_PREPEND;

    memmove(&lf->elems[idx + 1], &lf->elems[idx],
            ((int)lf->n.count - idx) * sizeof(zbtElem *));
    lf->elems[idx] = e;
    lf->n.count++;
    t->length++;
    t->alloc_size += zmalloc_usable_size(e);

    if (lf->n.count > ZBT_LEAF_MAX)
        zbtSplitLeaf(t, lf, bias);
    else
        zbtUpdateToRoot(t, (zbtNode *)lf);
}

zbtElem *zbtInsert(zbtree *t, double score, sds ele) {
    zbtElem *e = zbtCreateElem(score, ele);
    zbtInsertElem(t, e);
    return e;
}

/* Build a packed, balanced tree over 'elems[0..n)' in O(n). The elements must
 * already be strictly ascending by (score, member) and ownership of each one
 * transfers to the tree. 't' must be freshly created and empty. This is much
 * cheaper than n independent zbtInsert() calls (used by RDB load, COPY and
 * listpack->tree conversion, where the source order is already known). */
void zbtBuildFromSorted(zbtree *t, zbtElem **elems, unsigned long n) {
    if (n == 0) return;
    serverAssert(t->length == 0);

    /* Discard the placeholder empty root leaf created by zbtCreate(). */
    zbtFreeNodeShallow(t, t->root);
    t->root = t->head = t->tail = NULL;

    /* Build the leaf level. Distribute elements as evenly as possible so that
     * every non-root leaf holds at least ZBT_LEAF_MIN elements (required by the
     * structural invariants). With nleaves = ceil(n / ZBT_LEAF_MAX) the even
     * split guarantees this for more than one leaf. */
    unsigned long nleaves = (n + ZBT_LEAF_MAX - 1) / ZBT_LEAF_MAX;
    unsigned long base = n / nleaves;
    unsigned long rem = n % nleaves;

    zbtNode **level = zmalloc(sizeof(zbtNode *) * nleaves);
    unsigned long pos = 0;
    zbtLeaf *prev = NULL;
    for (unsigned long i = 0; i < nleaves; i++) {
        zbtLeaf *lf = zbtNewLeaf(t);
        unsigned long cnt = base + (i < rem ? 1 : 0);
        memcpy(lf->elems, &elems[pos], cnt * sizeof(zbtElem *));
        lf->n.count = (uint32_t)cnt;
        pos += cnt;
        lf->prev = prev;
        if (prev) prev->next = lf;
        else t->head = (zbtNode *)lf;
        prev = lf;
        level[i] = (zbtNode *)lf;
    }
    prev->next = NULL;
    t->tail = (zbtNode *)prev;

    /* Build inner levels bottom-up until a single root remains. */
    unsigned long count = nleaves;
    while (count > 1) {
        unsigned long nparents = (count + ZBT_INNER_MAX - 1) / ZBT_INNER_MAX;
        unsigned long pbase = count / nparents;
        unsigned long prem = count % nparents;
        zbtNode **parents = zmalloc(sizeof(zbtNode *) * nparents);
        unsigned long ci = 0;
        for (unsigned long p = 0; p < nparents; p++) {
            zbtInner *in = zbtNewInner(t);
            unsigned long nch = pbase + (p < prem ? 1 : 0);
            in->n.count = (uint32_t)nch;
            for (unsigned long k = 0; k < nch; k++) {
                zbtNode *c = level[ci++];
                in->child[k] = c;
                in->csize[k] = zbtSubtreeSize(c);
                in->sep[k] = zbtNodeMin(c);
                c->parent = (zbtNode *)in;
            }
            parents[p] = (zbtNode *)in;
        }
        zfree(level);
        level = parents;
        count = nparents;
    }

    t->root = level[0];
    t->root->parent = NULL;
    zfree(level);

    t->length = n;
    for (unsigned long i = 0; i < n; i++)
        t->alloc_size += zmalloc_usable_size(elems[i]);
}

/*-----------------------------------------------------------------------------
 * Deletion
 *----------------------------------------------------------------------------*/

static void zbtRebalanceInner(zbtree *t, zbtInner *in);

/* Remove child at position 'pos' from inner node 'p' (does not free it). */
static void zbtRemoveChild(zbtInner *p, int pos) {
    int tail = (int)p->n.count - pos - 1;
    memmove(&p->child[pos], &p->child[pos + 1], tail * sizeof(zbtNode *));
    memmove(&p->csize[pos], &p->csize[pos + 1], tail * sizeof(unsigned long));
    memmove(&p->sep[pos], &p->sep[pos + 1], tail * sizeof(zbtElem *));
    p->n.count--;
}

/* Called when inner node 'p' became the single-child root, or a subtree
 * shrank: fix up parent slots or collapse the root as needed. */
static void zbtFixupInnerAfterShrink(zbtree *t, zbtInner *p, int slot) {
    p->csize[slot] = zbtSubtreeSize(p->child[slot]);
    p->sep[slot] = zbtNodeMin(p->child[slot]);
    if (p->n.parent && p->n.count < ZBT_INNER_MIN) {
        zbtRebalanceInner(t, p);
    } else if (!p->n.parent && p->n.count == 1) {
        zbtNode *c = p->child[0];
        c->parent = NULL;
        t->root = c;
        zbtFreeNodeShallow(t, (zbtNode *)p);
    } else {
        zbtUpdateToRoot(t, (zbtNode *)p);
    }
}

/* Inner-node counterpart of zbtRebalanceLeaf(). A single borrowed child is
 * enough here, unlike for leaves: every caller reaches this through one
 * zbtRemoveChild() in zbtFixupInnerAfterShrink(), so 'in' is always short by
 * exactly one child. */
static void zbtRebalanceInner(zbtree *t, zbtInner *in) {
    zbtInner *p = (zbtInner *)in->n.parent;
    int idx = zbtChildIdx(p, (zbtNode *)in);
    serverAssert(in->n.count == ZBT_INNER_MIN - 1);

    /* Borrow from left sibling. */
    if (idx > 0) {
        zbtInner *L = (zbtInner *)p->child[idx - 1];
        if (L->n.count > ZBT_INNER_MIN) {
            memmove(&in->child[1], &in->child[0], in->n.count * sizeof(zbtNode *));
            memmove(&in->csize[1], &in->csize[0], in->n.count * sizeof(unsigned long));
            memmove(&in->sep[1], &in->sep[0], in->n.count * sizeof(zbtElem *));
            int last = (int)L->n.count - 1;
            in->child[0] = L->child[last];
            in->csize[0] = L->csize[last];
            in->sep[0] = L->sep[last];
            in->child[0]->parent = (zbtNode *)in;
            in->n.count++;
            L->n.count--;
            p->csize[idx - 1] = zbtSubtreeSize((zbtNode *)L); p->sep[idx - 1] = zbtNodeMin((zbtNode *)L);
            p->csize[idx] = zbtSubtreeSize((zbtNode *)in);   p->sep[idx] = zbtNodeMin((zbtNode *)in);
            zbtUpdateToRoot(t, (zbtNode *)p);
            return;
        }
    }
    /* Borrow from right sibling. */
    if (idx < (int)p->n.count - 1) {
        zbtInner *R = (zbtInner *)p->child[idx + 1];
        if (R->n.count > ZBT_INNER_MIN) {
            in->child[in->n.count] = R->child[0];
            in->csize[in->n.count] = R->csize[0];
            in->sep[in->n.count] = R->sep[0];
            in->child[in->n.count]->parent = (zbtNode *)in;
            in->n.count++;
            zbtRemoveChild(R, 0);
            p->csize[idx] = zbtSubtreeSize((zbtNode *)in);   p->sep[idx] = zbtNodeMin((zbtNode *)in);
            p->csize[idx + 1] = zbtSubtreeSize((zbtNode *)R); p->sep[idx + 1] = zbtNodeMin((zbtNode *)R);
            zbtUpdateToRoot(t, (zbtNode *)p);
            return;
        }
    }

    /* Merge with a sibling. */
    zbtInner *a, *b;
    int ai;
    if (idx > 0) { a = (zbtInner *)p->child[idx - 1]; b = in; ai = idx - 1; }
    else { a = in; b = (zbtInner *)p->child[idx + 1]; ai = idx; }
    serverAssert(a->n.count + b->n.count <= ZBT_INNER_MAX);
    for (uint32_t i = 0; i < b->n.count; i++) {
        a->child[a->n.count] = b->child[i];
        a->csize[a->n.count] = b->csize[i];
        a->sep[a->n.count] = b->sep[i];
        b->child[i]->parent = (zbtNode *)a;
        a->n.count++;
    }
    zbtRemoveChild(p, ai + 1);
    zbtFreeNodeShallow(t, (zbtNode *)b);
    zbtFixupInnerAfterShrink(t, p, ai);
}

/* Restore the minimum-occupancy invariant for a leaf that just dropped below
 * ZBT_LEAF_MIN.
 *
 * The amount moved tracks how short the leaf is. The range-delete path removes
 * a whole leaf slice per step, so 'lf' can arrive here short by many elements,
 * or empty; shifting a single element would leave it below the minimum. Queries
 * stay correct either way (they read count/csize, which stay consistent), but
 * the occupancy invariant zbtVerifyNode() checks would not hold, and nothing
 * would bound how empty leaves can get under repeated ZREMRANGEBY* traffic. */
static void zbtRebalanceLeaf(zbtree *t, zbtLeaf *lf) {
    zbtInner *p = (zbtInner *)lf->n.parent;
    int idx = zbtChildIdx(p, (zbtNode *)lf);

    /* Move exactly the deficit, no more: an element-at-a-time delete then costs
     * the same single-element shift it always did, while a slice delete pulls
     * across however many it takes. A sibling can cover the deficit precisely
     * when the pair holds 2 * ZBT_LEAF_MIN between them. */
    int deficit = ZBT_LEAF_MIN - (int)lf->n.count;
    serverAssert(deficit > 0);

    /* Take the tail of the left sibling onto the front of 'lf'. */
    if (idx > 0) {
        zbtLeaf *L = (zbtLeaf *)p->child[idx - 1];
        if ((int)L->n.count - deficit >= ZBT_LEAF_MIN) {
            zbtElem **src = &L->elems[(int)L->n.count - deficit];
            memmove(&lf->elems[deficit], &lf->elems[0],
                    lf->n.count * sizeof(zbtElem *));
            /* Single-element deletes are the common case and land here with a
             * deficit of one; keep that a plain store rather than a call. */
            if (deficit == 1) lf->elems[0] = *src;
            else memcpy(&lf->elems[0], src, deficit * sizeof(zbtElem *));
            L->n.count -= (uint32_t)deficit;
            lf->n.count += (uint32_t)deficit;
            p->csize[idx - 1] = L->n.count; p->sep[idx - 1] = zbtNodeMin((zbtNode *)L);
            p->csize[idx] = lf->n.count;    p->sep[idx] = zbtNodeMin((zbtNode *)lf);
            zbtUpdateToRoot(t, (zbtNode *)p);
            return;
        }
    }
    /* Take the head of the right sibling onto the end of 'lf'. */
    if (idx < (int)p->n.count - 1) {
        zbtLeaf *R = (zbtLeaf *)p->child[idx + 1];
        if ((int)R->n.count - deficit >= ZBT_LEAF_MIN) {
            if (deficit == 1) lf->elems[lf->n.count] = R->elems[0];
            else memcpy(&lf->elems[lf->n.count], &R->elems[0],
                        deficit * sizeof(zbtElem *));
            memmove(&R->elems[0], &R->elems[deficit],
                    ((int)R->n.count - deficit) * sizeof(zbtElem *));
            lf->n.count += (uint32_t)deficit;
            R->n.count -= (uint32_t)deficit;
            p->csize[idx] = lf->n.count;     p->sep[idx] = zbtNodeMin((zbtNode *)lf);
            p->csize[idx + 1] = R->n.count;  p->sep[idx + 1] = zbtNodeMin((zbtNode *)R);
            zbtUpdateToRoot(t, (zbtNode *)p);
            return;
        }
    }

    /* Merge with a sibling. Neither pair could spare enough to lift 'lf' to
     * the minimum, so both pairs total below 2 * ZBT_LEAF_MIN and the survivor
     * fits in a single leaf. */
    zbtLeaf *a, *b;
    int ai;
    if (idx > 0) { a = (zbtLeaf *)p->child[idx - 1]; b = lf; ai = idx - 1; }
    else { a = lf; b = (zbtLeaf *)p->child[idx + 1]; ai = idx; }
    serverAssert(a->n.count + b->n.count <= ZBT_LEAF_MAX);
    memcpy(&a->elems[a->n.count], b->elems, b->n.count * sizeof(zbtElem *));
    a->n.count += b->n.count;
    a->next = b->next;
    if (b->next) b->next->prev = a;
    else t->tail = (zbtNode *)a;
    zbtRemoveChild(p, ai + 1);
    zbtFreeNodeShallow(t, (zbtNode *)b);
    zbtFixupInnerAfterShrink(t, p, ai);
}

/* Remove element 'e' from the tree and free it. The caller is responsible for
 * removing it from the ZSET dict first (the dict has no key destructor). */
void zbtDeleteElem(zbtree *t, zbtElem *e) {
    double score = zbtGetScore(e);
    sds ele = zbtGetEle(e);
    zbtLeaf *lf = zbtFindLeaf(t, score, ele);
    int found;
    int idx = zbtLeafSearch(lf, score, ele, &found);
    serverAssert(found && lf->elems[idx] == e);

    memmove(&lf->elems[idx], &lf->elems[idx + 1],
            ((int)lf->n.count - idx - 1) * sizeof(zbtElem *));
    lf->n.count--;
    t->length--;
    t->alloc_size -= zmalloc_usable_size(e);
    zbtFreeElem(e);

    if (lf->n.parent && lf->n.count < ZBT_LEAF_MIN)
        zbtRebalanceLeaf(t, lf);
    else
        zbtUpdateToRoot(t, (zbtNode *)lf);
}

/* Move an existing element to reflect a new score. Returns the (possibly
 * reallocated) element: a width change allocates a new object, and the
 * caller must rewire the ZSET dict key when the pointer changes. */
zbtElem *zbtUpdateScore(zbtree *t, zbtElem *e, double newscore) {
    /* Detach, rewrite the score, reinsert. The dict maps member -> elem and
     * the member is unchanged, so only the tree position (and possibly the
     * allocation) changes. */
    double score = zbtGetScore(e);
    sds ele = zbtGetEle(e);
    zbtLeaf *lf = zbtFindLeaf(t, score, ele);
    int found;
    int idx = zbtLeafSearch(lf, score, ele, &found);
    serverAssert(found && lf->elems[idx] == e);

    memmove(&lf->elems[idx], &lf->elems[idx + 1],
            ((int)lf->n.count - idx - 1) * sizeof(zbtElem *));
    lf->n.count--;
    t->length--;
    t->alloc_size -= zmalloc_usable_size(e);
    if (lf->n.parent && lf->n.count < ZBT_LEAF_MIN)
        zbtRebalanceLeaf(t, lf);
    else
        zbtUpdateToRoot(t, (zbtNode *)lf);

    uint8_t newenc;
    unsigned char newbuf[8];
    zbtScoreEncode(newscore, &newenc, newbuf);
    if (zbtScoreEncSize(newenc) == zbtScoreEncSize(e->enc)) {
        e->enc = newenc;
        memcpy(e->data, newbuf, zbtScoreEncSize(newenc));
        zbtInsertElem(t, e);
        return e;
    }

    zbtElem *ne = zbtCreateElem(newscore, ele);
    zbtFreeElem(e);
    zbtInsertElem(t, ne);
    return ne;
}

/*-----------------------------------------------------------------------------
 * Rank and rank-based access
 *----------------------------------------------------------------------------*/

/* 1-based rank of element 'e'. */
unsigned long zbtRankByElem(zbtree *t, zbtElem *e) {
    double score = zbtGetScore(e);
    sds ele = zbtGetEle(e);
    unsigned long rank = 0;
    zbtNode *n = t->root;
    while (!n->isleaf) {
        zbtInner *in = (zbtInner *)n;
        int ci = zbtInnerChildIdx(in, score, ele);
        for (int i = 0; i < ci; i++) rank += in->csize[i];
        n = in->child[ci];
    }
    zbtLeaf *lf = (zbtLeaf *)n;
    int found;
    int idx = zbtLeafSearch(lf, score, ele, &found);
    serverAssert(found);
    return rank + (unsigned long)idx + 1;
}

/* 1-based rank of (score,ele), or 0 when the element does not exist. */
unsigned long zbtGetRank(zbtree *t, double score, sds ele) {
    unsigned long rank = 0;
    zbtNode *n = t->root;
    while (!n->isleaf) {
        zbtInner *in = (zbtInner *)n;
        int ci = zbtInnerChildIdx(in, score, ele);
        for (int i = 0; i < ci; i++) rank += in->csize[i];
        n = in->child[ci];
    }
    zbtLeaf *lf = (zbtLeaf *)n;
    int found;
    int idx = zbtLeafSearch(lf, score, ele, &found);
    if (!found) return 0;
    return rank + (unsigned long)idx + 1;
}

/* Return the element at the given 1-based rank, or NULL if out of range.
 * When 'it' is not NULL it is positioned at the returned element. */
zbtElem *zbtElemByRank(zbtree *t, unsigned long rank, zbtIter *it) {
    if (rank < 1 || rank > t->length) return NULL;
    unsigned long r = rank - 1; /* 0-based */
    zbtNode *n = t->root;
    while (!n->isleaf) {
        zbtInner *in = (zbtInner *)n;
        uint32_t i = 0;
        while (i < in->n.count && r >= in->csize[i]) { r -= in->csize[i]; i++; }
        n = in->child[i];
    }
    zbtLeaf *lf = (zbtLeaf *)n;
    if (it) { it->leaf = (zbtNode *)lf; it->idx = (int)r; }
    return lf->elems[r];
}

/*-----------------------------------------------------------------------------
 * Iteration
 *----------------------------------------------------------------------------*/

zbtElem *zbtFirst(zbtree *t, zbtIter *it) {
    if (t->length == 0) return NULL;
    zbtLeaf *lf = (zbtLeaf *)t->head;
    if (it) { it->leaf = (zbtNode *)lf; it->idx = 0; }
    return lf->elems[0];
}

zbtElem *zbtLast(zbtree *t, zbtIter *it) {
    if (t->length == 0) return NULL;
    zbtLeaf *lf = (zbtLeaf *)t->tail;
    if (it) { it->leaf = (zbtNode *)lf; it->idx = (int)lf->n.count - 1; }
    return lf->elems[lf->n.count - 1];
}

zbtElem *zbtIterNext(zbtIter *it) {
    if (!it->leaf) return NULL;
    zbtLeaf *lf = (zbtLeaf *)it->leaf;
    it->idx++;
    if (it->idx >= (int)lf->n.count) {
        it->leaf = (zbtNode *)lf->next;
        it->idx = 0;
        if (!it->leaf) return NULL;
        lf = (zbtLeaf *)it->leaf;
        if (lf->n.count == 0) return NULL;
    }
    return lf->elems[it->idx];
}

zbtElem *zbtIterPrev(zbtIter *it) {
    if (!it->leaf) return NULL;
    zbtLeaf *lf = (zbtLeaf *)it->leaf;
    it->idx--;
    if (it->idx < 0) {
        it->leaf = (zbtNode *)lf->prev;
        if (!it->leaf) return NULL;
        lf = (zbtLeaf *)it->leaf;
        it->idx = (int)lf->n.count - 1;
        if (it->idx < 0) return NULL;
    }
    return lf->elems[it->idx];
}

/* Position 'it' exactly on the element matching (score,ele). Returns 1 if
 * found. */
static int zbtSeek(zbtree *t, double score, sds ele, zbtIter *it) {
    zbtLeaf *lf = zbtFindLeaf(t, score, ele);
    int found;
    int idx = zbtLeafSearch(lf, score, ele, &found);
    it->leaf = (zbtNode *)lf;
    it->idx = idx;
    return found;
}

/* Element-based next/prev (O(log N)). Used where holding an iterator is
 * inconvenient (e.g. the module API cursor). */
zbtElem *zbtNext(zbtree *t, zbtElem *e) {
    zbtIter it;
    if (!zbtSeek(t, zbtGetScore(e), zbtGetEle(e), &it)) return NULL;
    return zbtIterNext(&it);
}

zbtElem *zbtPrev(zbtree *t, zbtElem *e) {
    zbtIter it;
    if (!zbtSeek(t, zbtGetScore(e), zbtGetEle(e), &it)) return NULL;
    return zbtIterPrev(&it);
}

/*-----------------------------------------------------------------------------
 * Range queries
 *----------------------------------------------------------------------------*/

/* A predicate that is monotonic in tree order: true for a prefix of the
 * sorted order, then false. */
typedef int (*zbtBeforeFn)(const zbtElem *e, void *arg);

/* Where such a predicate flips: 'count' elements satisfy it, and 'leaf'/'idx'
 * hold the position of the first element that does not ('idx' reaches
 * leaf->n.count when every element of the leaf satisfies it).
 *
 * The descent only skips a child once the minimum of the following one
 * satisfies the predicate, and that minimum is inherited all the way down, so
 * a non-empty prefix always ends inside the leaf the descent lands on: count
 * > 0 implies idx > 0, and the last satisfying element sits at idx - 1 of
 * that same leaf. Both ends of a range are therefore one descent away. */
typedef struct zbtBoundary {
    unsigned long count;
    zbtLeaf *leaf;
    int idx;
} zbtBoundary;

static void zbtFindBoundary(zbtree *t, zbtBeforeFn before, void *arg,
                            zbtBoundary *b) {
    unsigned long cnt = 0;
    zbtNode *n = t->root;
    while (!n->isleaf) {
        zbtInner *in = (zbtInner *)n;
        int i = 0;
        while (i < (int)in->n.count - 1 && before(in->sep[i + 1], arg)) {
            cnt += in->csize[i];
            i++;
        }
        n = in->child[i];
    }
    zbtLeaf *lf = (zbtLeaf *)n;
    uint32_t i = 0;
    while (i < lf->n.count && before(lf->elems[i], arg)) i++;
    b->count = cnt + i;
    b->leaf = lf;
    b->idx = (int)i;
}

/* Count the elements at the start of the sorted order for which before()
 * returns true. */
static unsigned long zbtCountBefore(zbtree *t, zbtBeforeFn before, void *arg) {
    zbtBoundary b;
    zbtFindBoundary(t, before, arg, &b);
    return b.count;
}

/* Last element of the prefix (rank b->count), or NULL when it is empty. */
static zbtElem *zbtBoundaryLast(zbtBoundary *b, zbtIter *it) {
    if (b->count == 0) return NULL;
    debugServerAssert(b->idx > 0);
    if (it) { it->leaf = (zbtNode *)b->leaf; it->idx = b->idx - 1; }
    return b->leaf->elems[b->idx - 1];
}

/* First element past the prefix (rank b->count + 1), or NULL when the prefix
 * covers the whole tree. */
static zbtElem *zbtBoundaryNext(zbtBoundary *b, zbtIter *it) {
    zbtLeaf *lf = b->leaf;
    int idx = b->idx;
    while (lf && idx >= (int)lf->n.count) { lf = lf->next; idx = 0; }
    if (!lf) return NULL;
    if (it) { it->leaf = (zbtNode *)lf; it->idx = idx; }
    return lf->elems[idx];
}

/* Predicates for score ranges. */
static int beforeScoreLt(const zbtElem *e, void *arg) {
    return zbtGetScore(e) < *(double *)arg;
}
static int beforeScoreLe(const zbtElem *e, void *arg) {
    return zbtGetScore(e) <= *(double *)arg;
}

/* Locate the first element satisfying the lower score bound. Searching from
 * the left keeps the common low-minimum case short. The skipped subtree sizes
 * give the element's rank without a second descent. */
static zbtElem *zbtFirstInRange(zbtree *t, zrangespec *range,
                                unsigned long *out_rank, zbtIter *it) {
    unsigned long before = 0;
    zbtNode *n = t->root;
    while (!n->isleaf) {
        zbtInner *in = (zbtInner *)n;
        int i = 0;
        while (i < (int)in->n.count - 1 &&
               !zslValueGteMin(zbtGetScore(in->sep[i + 1]), range))
        {
            before += in->csize[i];
            i++;
        }
        n = in->child[i];
    }

    zbtLeaf *lf = (zbtLeaf *)n;
    int idx = 0;
    while (idx < (int)lf->n.count &&
           !zslValueGteMin(zbtGetScore(lf->elems[idx]), range))
    {
        before++;
        idx++;
    }
    if (idx == (int)lf->n.count) {
        lf = lf->next;
        idx = 0;
    }
    if (lf == NULL) return NULL;

    zbtElem *e = lf->elems[idx];
    if (!zslValueLteMax(zbtGetScore(e), range)) return NULL;
    if (out_rank) *out_rank = before + 1;
    if (it) { it->leaf = (zbtNode *)lf; it->idx = idx; }
    return e;
}

/* Locate the last element satisfying the upper score bound. Searching from
 * the right is the reverse-range counterpart of zbtFirstInRange(): whole
 * subtrees beyond max are subtracted from the absolute rank, and the result
 * is already positioned for backward leaf iteration. */
static zbtElem *zbtLastInRange(zbtree *t, zrangespec *range,
                               unsigned long *out_rank, zbtIter *it) {
    unsigned long rank = t->length;
    zbtNode *n = t->root;
    while (!n->isleaf) {
        zbtInner *in = (zbtInner *)n;
        int i = (int)in->n.count - 1;
        while (i > 0 &&
               !zslValueLteMax(zbtGetScore(in->sep[i]), range))
        {
            rank -= in->csize[i];
            i--;
        }
        n = in->child[i];
    }

    zbtLeaf *lf = (zbtLeaf *)n;
    int idx = (int)lf->n.count - 1;
    while (idx >= 0 &&
           !zslValueLteMax(zbtGetScore(lf->elems[idx]), range))
    {
        rank--;
        idx--;
    }
    if (idx < 0) return NULL;

    zbtElem *e = lf->elems[idx];
    if (!zslValueGteMin(zbtGetScore(e), range)) return NULL;
    if (out_rank) *out_rank = rank;
    if (it) { it->leaf = (zbtNode *)lf; it->idx = idx; }
    return e;
}

/* Predicates for lex ranges. */
static int beforeNotGteMin(const zbtElem *e, void *arg) {
    return !zslLexValueGteMin(zbtGetEle((zbtElem *)e), (zlexrangespec *)arg);
}
static int beforeLteMax(const zbtElem *e, void *arg) {
    return zslLexValueLteMax(zbtGetEle((zbtElem *)e), (zlexrangespec *)arg);
}

/* Offsets up to this many elements are reached by stepping along the leaf
 * chain, which stays cheaper than the root-to-leaf descent zbtElemByRank()
 * needs to jump straight to a rank. Matches the search window the skiplist
 * used before the tree replaced it. */
#define ZBT_RANGE_WALK_MAX 10

/* Shared implementation of the Nth-in-range lookups. 'before_lo' selects the
 * elements that precede the range, 'before_hi' those up to and including its
 * end. Mirrors the skiplist zslNthIn*Range semantics: n >= 0 counts forward
 * from the first in-range element, n < 0 counts back from the last.
 *
 * Only the end the walk starts from is located, by a single boundary descent.
 * Everything past that end already clears its side of the range, so one
 * predicate check on the element landed on decides whether the offset has
 * carried the result out through the opposite side. */
static zbtElem *zbtNthGeneric(zbtree *t, long n, unsigned long *out_rank,
                              zbtIter *it,
                              zbtBeforeFn before_lo, void *lo_arg,
                              zbtBeforeFn before_hi, void *hi_arg) {
    zbtBoundary b;
    zbtIter pos;
    zbtElem *e;
    unsigned long rank;

    if (n >= 0) {
        zbtFindBoundary(t, before_lo, lo_arg, &b);
        e = zbtBoundaryNext(&b, &pos);
        if (e == NULL) return NULL;

        unsigned long steps = (unsigned long)n;
        if (steps >= t->length - b.count) return NULL;
        rank = b.count + 1 + steps;
        if (n > 0) {
            if (n <= ZBT_RANGE_WALK_MAX) {
                for (long i = 0; i < n; i++) e = zbtIterNext(&pos);
            } else {
                e = zbtElemByRank(t, rank, &pos);
            }
            if (e == NULL) return NULL;
        }
        if (!before_hi(e, hi_arg)) return NULL;
    } else {
        zbtFindBoundary(t, before_hi, hi_arg, &b);
        e = zbtBoundaryLast(&b, &pos);
        if (e == NULL) return NULL;

        /* Add before negating so LONG_MIN remains representable. */
        unsigned long steps = (unsigned long)(-(n + 1));
        if (steps >= b.count) return NULL;
        rank = b.count - steps;
        if (steps > 0) {
            if (steps <= ZBT_RANGE_WALK_MAX) {
                for (unsigned long i = 0; i < steps; i++) e = zbtIterPrev(&pos);
            } else {
                e = zbtElemByRank(t, rank, &pos);
            }
            if (e == NULL) return NULL;
        }
        if (before_lo(e, lo_arg)) return NULL;
    }

    if (out_rank) *out_rank = rank;
    if (it) *it = pos;
    return e;
}

zbtElem *zbtNthInRange(zbtree *t, zrangespec *range, long n,
                       unsigned long *out_rank, zbtIter *it) {
    if (t->length == 0) return NULL;

    zbtIter pos;
    zbtElem *e;
    unsigned long rank;

    if (n >= 0) {
        e = zbtFirstInRange(t, range, &rank, &pos);
        if (e == NULL) return NULL;

        unsigned long steps = (unsigned long)n;
        if (steps > t->length - rank) return NULL;
        rank += steps;
        if (steps > 0) {
            if (steps <= ZBT_RANGE_WALK_MAX) {
                for (unsigned long i = 0; i < steps; i++)
                    e = zbtIterNext(&pos);
            } else {
                e = zbtElemByRank(t, rank, &pos);
            }
            if (e == NULL || !zslValueLteMax(zbtGetScore(e), range))
                return NULL;
        }
    } else {
        /* Add before negating so LONG_MIN remains representable. */
        unsigned long steps = (unsigned long)(-(n + 1));
        if (steps <= ZBT_RANGE_WALK_MAX) {
            e = zbtLastInRange(t, range, &rank, &pos);
            if (e == NULL || steps >= rank) return NULL;
            for (unsigned long i = 0; i < steps; i++)
                e = zbtIterPrev(&pos);
            rank -= steps;
            if (steps > 0 && !zslValueGteMin(zbtGetScore(e), range))
                return NULL;
        } else {
            /* For a rank jump, only the upper endpoint's rank is needed.
             * Counting from the left matches zbtElemByRank()'s traversal and
             * avoids positioning an iterator that would be discarded. */
            double maxv = range->max;
            rank = range->maxex ?
                zbtCountBefore(t, beforeScoreLt, &maxv) :
                zbtCountBefore(t, beforeScoreLe, &maxv);
            if (steps >= rank) return NULL;
            rank -= steps;
            e = zbtElemByRank(t, rank, &pos);
            if (e == NULL || !zslValueGteMin(zbtGetScore(e), range))
                return NULL;
        }
    }

    if (out_rank) *out_rank = rank;
    if (it) *it = pos;
    return e;
}

zbtElem *zbtNthInLexRange(zbtree *t, zlexrangespec *range, long n,
                          unsigned long *out_rank, zbtIter *it) {
    if (t->length == 0) return NULL;
    return zbtNthGeneric(t, n, out_rank, it,
                         beforeNotGteMin, range, beforeLteMax, range);
}

/*-----------------------------------------------------------------------------
 * Range deletion (also removes the members from the ZSET dict)
 *----------------------------------------------------------------------------*/

/* Delete every element whose 1-based rank falls in [first, last] (inclusive),
 * removing each member from the companion dict 'd' as well.
 *
 * Instead of locating and rebalancing once per element (O(K log N)), this
 * removes a whole leaf slice per structural pass: at most one O(log N) rank
 * lookup and one rebalance per touched leaf, giving O(K + (K/leaf) * log N). */
static unsigned long zbtDeleteRankRange(zbtree *t, unsigned long first,
                                        unsigned long last, dict *d) {
    unsigned long removed = 0;
    if (last > t->length) last = t->length;

    while (first <= last) {
        zbtIter it;
        zbtElem *e = zbtElemByRank(t, first, &it);
        if (!e) break;
        zbtLeaf *lf = (zbtLeaf *)it.leaf;
        int idx = it.idx;

        /* Delete the contiguous in-range slice contained in this leaf. */
        int avail = (int)lf->n.count - idx;
        long want = (long)(last - first + 1);
        int take = (want < avail) ? (int)want : avail;

        for (int k = 0; k < take; k++) {
            zbtElem *el = lf->elems[idx + k];
            dictDelete(d, zbtGetEle(el));
            t->alloc_size -= zmalloc_usable_size(el);
            zbtFreeElem(el);
        }
        memmove(&lf->elems[idx], &lf->elems[idx + take],
                ((int)lf->n.count - idx - take) * sizeof(zbtElem *));
        lf->n.count -= take;
        t->length -= take;
        removed += take;
        /* We removed 'take' elements starting at rank 'first'; the next
         * survivor now occupies rank 'first', so keep 'first' and shrink the
         * remaining window from the top. */
        last -= take;

        if (lf->n.parent && lf->n.count < ZBT_LEAF_MIN)
            zbtRebalanceLeaf(t, lf);
        else
            zbtUpdateToRoot(t, (zbtNode *)lf);
    }
    return removed;
}

unsigned long zbtDeleteRangeByScore(zbtree *t, zrangespec *range, dict *d) {
    if (t->length == 0) return 0;
    double minv = range->min, maxv = range->max;
    unsigned long before = range->minex ?
        zbtCountBefore(t, beforeScoreLe, &minv) :
        zbtCountBefore(t, beforeScoreLt, &minv);
    unsigned long upto = range->maxex ?
        zbtCountBefore(t, beforeScoreLt, &maxv) :
        zbtCountBefore(t, beforeScoreLe, &maxv);
    if (before >= upto) return 0;
    return zbtDeleteRankRange(t, before + 1, upto, d);
}

unsigned long zbtDeleteRangeByLex(zbtree *t, zlexrangespec *range, dict *d) {
    if (t->length == 0) return 0;
    unsigned long before = zbtCountBefore(t, beforeNotGteMin, range);
    unsigned long upto = zbtCountBefore(t, beforeLteMax, range);
    if (before >= upto) return 0;
    return zbtDeleteRankRange(t, before + 1, upto, d);
}

/* Delete elements whose 1-based rank is in [start, end] (inclusive). */
unsigned long zbtDeleteRangeByRank(zbtree *t, unsigned int start,
                                   unsigned int end, dict *d) {
    if (t->length == 0 || start > end) return 0;
    return zbtDeleteRankRange(t, start, end, d);
}

/*-----------------------------------------------------------------------------
 * Active defragmentation support
 *----------------------------------------------------------------------------*/

/* Replace element 'olde' with the (content-identical) relocated 'newe' in its
 * leaf slot and fix any separator pointers that referenced it. */
void zbtReplaceElem(zbtree *t, zbtElem *olde, zbtElem *newe) {
    double score = zbtGetScore(newe);
    zbtLeaf *lf = zbtFindLeaf(t, score, zbtGetEle(newe));
    int found;
    int idx = zbtLeafSearch(lf, score, zbtGetEle(newe), &found);
    serverAssert(found && lf->elems[idx] == olde);
    lf->elems[idx] = newe;
    zbtUpdateToRoot(t, (zbtNode *)lf);
}

static zbtNode *zbtDefragNode(zbtNode *n, void *(*fn)(void *)) {
    zbtNode *nn = fn(n);
    if (nn) n = nn;
    if (!n->isleaf) {
        zbtInner *in = (zbtInner *)n;
        for (uint32_t i = 0; i < in->n.count; i++) {
            zbtNode *c = zbtDefragNode(in->child[i], fn);
            in->child[i] = c;
            c->parent = n;
        }
    }
    return n;
}

static void zbtCollectLeaves(zbtree *t, zbtNode *n, zbtLeaf **prev) {
    if (n->isleaf) {
        zbtLeaf *lf = (zbtLeaf *)n;
        lf->prev = *prev;
        if (*prev) (*prev)->next = lf;
        else t->head = (zbtNode *)lf;
        *prev = lf;
    } else {
        zbtInner *in = (zbtInner *)n;
        for (uint32_t i = 0; i < in->n.count; i++)
            zbtCollectLeaves(t, in->child[i], prev);
    }
}

/* Relocate every tree node using the provided defrag allocator, then rebuild
 * the leaf sibling chain and head/tail pointers. Element objects are handled
 * separately by the caller (via the ZSET dict scan + zbtReplaceElem). */
void zbtDefragNodes(zbtree *t, void *(*fn)(void *)) {
    t->root = zbtDefragNode(t->root, fn);
    t->root->parent = NULL;
    zbtLeaf *prev = NULL;
    zbtCollectLeaves(t, t->root, &prev);
    if (prev) prev->next = NULL;
    t->tail = (zbtNode *)prev;
}

/* Relocate a single leaf (if the allocator decides to move it) and repair all
 * external references: the parent child slot (or the root), the sibling links
 * and the head/tail pointers. Returns the current (possibly new) leaf. */
static zbtLeaf *zbtDefragRelocLeaf(zbtree *t, zbtLeaf *lf, void *(*fn)(void *)) {
    zbtLeaf *nl = fn(lf);
    if (!nl) return lf;
    if (nl->n.parent) {
        zbtInner *p = (zbtInner *)nl->n.parent;
        p->child[zbtChildIdx(p, (zbtNode *)lf)] = (zbtNode *)nl;
    } else {
        t->root = (zbtNode *)nl;
    }
    if (nl->prev) nl->prev->next = nl; else t->head = (zbtNode *)nl;
    if (nl->next) nl->next->prev = nl; else t->tail = (zbtNode *)nl;
    return nl;
}

/* Relocate a single inner node (if moved) and repair the grandparent child
 * slot (or the root) and every child's parent back-pointer. Returns the
 * current (possibly new) inner node. */
static zbtInner *zbtDefragRelocInner(zbtree *t, zbtInner *in, void *(*fn)(void *)) {
    zbtInner *ni = fn(in);
    if (!ni) return in;
    if (ni->n.parent) {
        zbtInner *p = (zbtInner *)ni->n.parent;
        p->child[zbtChildIdx(p, (zbtNode *)in)] = (zbtNode *)ni;
    } else {
        t->root = (zbtNode *)ni;
    }
    for (uint32_t i = 0; i < ni->n.count; i++)
        ni->child[i]->parent = (zbtNode *)ni;
    return ni;
}

/* Incremental variant of zbtDefragNodes(): relocate up to 'budget' tree nodes,
 * walking the leaves left to right and relocating each inner node right after
 * its last child is processed (post-order). The resume position is stored in
 * the tree as the (score, member) key of the first element of the next leaf to
 * process, so it survives element inserts/deletes/rebalances happening between
 * calls. Returns 1 if more work remains (bookmark saved) or 0 when the whole
 * tree has been relocated. Unlike zbtDefragNodes(), the leaf sibling chain and
 * head/tail are kept consistent after every single relocation, so the tree is
 * safe to query/modify between steps. */
int zbtDefragNodesIncremental(zbtree *t, void *(*fn)(void *), unsigned int budget) {
    zbtLeaf *lf;
    if (t->defrag_resume)
        lf = zbtFindLeaf(t, t->defrag_resume_score, t->defrag_resume);
    else
        lf = (zbtLeaf *)t->head;

    unsigned int work = 0;
    while (lf) {
        zbtLeaf *next = lf->next; /* value stays valid across relocation */
        lf = zbtDefragRelocLeaf(t, lf, fn);
        work++;

        /* Post-order: relocate ancestors whose last child we just completed. */
        zbtNode *c = (zbtNode *)lf;
        while (c->parent) {
            zbtInner *p = (zbtInner *)c->parent;
            if (p->child[p->n.count - 1] != c) break;
            c = (zbtNode *)zbtDefragRelocInner(t, p, fn);
            work++;
        }

        if (!next) {
            /* Processed the last leaf; the cascade above relocated the root. */
            if (t->defrag_resume) { sdsfree(t->defrag_resume); t->defrag_resume = NULL; }
            return 0;
        }
        if (work >= budget) {
            zbtElem *e0 = next->elems[0];
            sds m = sdsdup(zbtGetEle(e0));
            if (t->defrag_resume) sdsfree(t->defrag_resume);
            t->defrag_resume = m;
            t->defrag_resume_score = zbtGetScore(e0);
            return 1;
        }
        lf = next;
    }

    /* Empty tree (single empty root leaf) or nothing left. */
    if (t->defrag_resume) { sdsfree(t->defrag_resume); t->defrag_resume = NULL; }
    return 0;
}

/*-----------------------------------------------------------------------------
 * Debugging / test verification
 *----------------------------------------------------------------------------*/

#ifdef REDIS_TEST
#include <assert.h>
#include "testhelp.h"

/* Subtree minimum obtained by descending to the leftmost leaf. zbtNodeMin()
 * trusts sep[0] instead, so the verifier needs this independent version to
 * actually check the separator invariant against the tree contents. */
static zbtElem *zbtNodeMinDescend(zbtNode *n) {
    while (!n->isleaf) n = ((zbtInner *)n)->child[0];
    return ((zbtLeaf *)n)->elems[0];
}

static unsigned long zbtVerifyNode(zbtree *t, zbtNode *n, int depth,
                                   int *leafdepth) {
    if (n->isleaf) {
        zbtLeaf *lf = (zbtLeaf *)n;
        /* The head and tail leaves are exempt: zbtSplitLeaf() starts one of
         * them with a single element so sorted insertion can leave the leaf on
         * the other side of the split full. */
        if (n->parent && n != t->tail && n != t->head)
            serverAssert(n->count >= ZBT_LEAF_MIN);
        if (n->parent) serverAssert(n->count >= 1);
        serverAssert(n->count <= ZBT_LEAF_MAX);
        if (*leafdepth == -1) *leafdepth = depth;
        else serverAssert(*leafdepth == depth); /* all leaves same depth */
        for (uint32_t i = 1; i < n->count; i++) {
            zbtElem *a = lf->elems[i - 1], *b = lf->elems[i];
            serverAssert(zbtCompare(zbtGetScore(a), zbtGetEle(a), b) < 0);
        }
        return n->count;
    }
    zbtInner *in = (zbtInner *)n;
    if (n->parent) serverAssert(n->count >= ZBT_INNER_MIN);
    serverAssert(n->count >= 2 || !n->parent);
    serverAssert(n->count <= ZBT_INNER_MAX);
    unsigned long total = 0;
    for (uint32_t i = 0; i < n->count; i++) {
        serverAssert(in->child[i]->parent == n);
        serverAssert(zbtNodeMinDescend(in->child[i]) == in->sep[i]);
        unsigned long cs = zbtVerifyNode(t, in->child[i], depth + 1, leafdepth);
        serverAssert(cs == in->csize[i]);
        total += cs;
    }
    return total;
}

/* Panics if any structural invariant is violated. */
void zbtDebugVerify(zbtree *t) {
    int leafdepth = -1;
    unsigned long total = zbtVerifyNode(t, t->root, 0, &leafdepth);
    serverAssert(total == t->length);

    /* Verify the leaf chain matches in-order traversal and head/tail. */
    unsigned long chain = 0;
    zbtLeaf *lf = (zbtLeaf *)t->head;
    zbtLeaf *plf = NULL;
    zbtElem *prev_elem = NULL;
    while (lf) {
        serverAssert(lf->prev == plf);
        for (uint32_t i = 0; i < lf->n.count; i++) {
            if (prev_elem) {
                zbtElem *cur = lf->elems[i];
                serverAssert(zbtCompare(zbtGetScore(prev_elem), zbtGetEle(prev_elem), cur) < 0);
            }
            prev_elem = lf->elems[i];
            chain++;
        }
        plf = lf;
        lf = lf->next;
    }
    serverAssert(chain == t->length);
    serverAssert(t->tail == (zbtNode *)plf || (t->length == 0));
}

/* Test allocator that always relocates the block, to exercise the pointer
 * fix-ups in the incremental node-defrag path. */
static void *zbtTestReloc(void *ptr) {
    size_t sz = zmalloc_usable_size(ptr);
    void *n = zmalloc(sz);
    memcpy(n, ptr, sz);
    zfree(ptr);
    return n;
}

/* Reference zbtNthInRange(), by linear scan: the elements of 'range' in tree
 * order, indexed the same way (n >= 0 from the first, n < 0 from the last).
 * Deliberately ignores the tree structure so it cannot share a bug with the
 * boundary descent it checks. */
static zbtElem *zbtRefNthInRange(zbtree *t, zrangespec *range, long n,
                                 unsigned long *out_rank) {
    unsigned long first = 0, last = 0, rank = 0;
    zbtIter it;

    for (zbtElem *e = zbtFirst(t, &it); e; e = zbtIterNext(&it)) {
        double s = zbtGetScore(e);
        rank++;
        if (!zslValueGteMin(s, range) || !zslValueLteMax(s, range)) continue;
        if (first == 0) first = rank;
        last = rank;
    }
    if (first == 0) return NULL;

    unsigned long target;
    if (n >= 0) {
        unsigned long steps = (unsigned long)n;
        if (steps > last - first) return NULL;
        target = first + steps;
    } else {
        unsigned long steps = (unsigned long)(-(n + 1));
        if (steps > last - first) return NULL;
        target = last - steps;
    }
    if (out_rank) *out_rank = target;
    return zbtElemByRank(t, target, NULL);
}

/* Compare zbtNthInRange() against the linear reference for one (range, n),
 * including the rank it reports and the position of the iterator it left
 * behind. */
static void zbtCheckNthInRange(zbtree *t, zrangespec *range, long n) {
    unsigned long got_rank = 0, want_rank = 0;
    zbtIter it;
    zbtElem *got = zbtNthInRange(t, range, n, &got_rank, &it);
    zbtElem *want = zbtRefNthInRange(t, range, n, &want_rank);

    serverAssert(got == want);
    if (want == NULL) return;
    serverAssert(got_rank == want_rank);
    serverAssert(zbtElemByRank(t, got_rank, NULL) == got);

    /* The iterator has to be usable for the scan the callers run from here,
     * in either direction. */
    zbtIter fwd = it, bwd = it;
    serverAssert(zbtIterNext(&fwd) == zbtElemByRank(t, got_rank + 1, NULL));
    if (got_rank > 1)
        serverAssert(zbtIterPrev(&bwd) == zbtElemByRank(t, got_rank - 1, NULL));
    else
        serverAssert(zbtIterPrev(&bwd) == NULL);
}

int zbtreeTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    printf("Testing B+ tree operations with structure verification\n");

    const int N = 2000;
    zbtree *t = zbtCreate();

    typedef struct {
        double score;
        sds ele;
        zbtElem *elem;
        int deleted;
    } Inserted;

    Inserted *elements = zmalloc(sizeof(Inserted) * N);
    srand(12345);

    for (int i = 0; i < N; i++) {
        double score = (double)(rand() % 137);
        char buf[32];
        snprintf(buf, sizeof(buf), "elem:%d", i);
        sds ele = sdsnew(buf);
        zbtElem *e = zbtInsert(t, score, ele);
        elements[i].score = score;
        elements[i].ele = ele;
        elements[i].elem = e;
        elements[i].deleted = 0;

        if (i % 97 == 0) zbtDebugVerify(t);

        unsigned long rank = zbtGetRank(t, score, ele);
        assert(rank != 0);
        assert(zbtElemByRank(t, rank, NULL) == e);
        assert(zbtRankByElem(t, e) == rank);
    }
    zbtDebugVerify(t);
    test_cond("Insert N elements", t->length == (unsigned long)N);

    /* Full in-order scan is sorted and matches length. */
    {
        zbtIter it;
        zbtElem *e = zbtFirst(t, &it);
        unsigned long c = 0;
        zbtElem *prev = NULL;
        while (e) {
            if (prev) assert(zbtCompare(zbtGetScore(prev), zbtGetEle(prev), e) < 0);
            prev = e;
            c++;
            e = zbtIterNext(&it);
        }
        test_cond("Forward scan sorted and complete", c == (unsigned long)N);
    }

    /* Delete half in random order. */
    for (int i = 0; i < N; i += 2) {
        zbtElem *e = elements[i].elem;
        assert(zbtGetRank(t, zbtGetScore(e), zbtGetEle(e)) != 0);
        zbtDeleteElem(t, e);
        elements[i].deleted = 1;
        if (i % 101 == 0) zbtDebugVerify(t);
    }
    zbtDebugVerify(t);
    test_cond("Delete half", t->length == (unsigned long)N / 2);

    /* Update scores of the survivors. */
    for (int i = 1; i < N; i += 2) {
        zbtElem *e = elements[i].elem;
        elements[i].elem = zbtUpdateScore(t, e, (double)(rand() % 300));
    }
    zbtDebugVerify(t);

    /* Delete the rest. */
    for (int i = 1; i < N; i += 2) {
        zbtDeleteElem(t, elements[i].elem);
        if (i % 103 == 0) zbtDebugVerify(t);
    }
    zbtDebugVerify(t);
    test_cond("Delete rest", t->length == 0);

    for (int i = 0; i < N; i++) sdsfree(elements[i].ele);
    zfree(elements);
    zbtFree(t);

    /* --- Bottom-up bulk build and batched range deletion --- */
    /* Cover boundary sizes around leaf/inner fan-out multiples. Every entry
     * must run: the multi-leaf sizes are the only ones that exercise range
     * deletes spanning whole leaves. */
    static const int sizes[] = {1, ZBT_LEAF_MAX, ZBT_LEAF_MAX + 1,
                                ZBT_LEAF_MAX * ZBT_INNER_MAX + 3, 5000};
    for (int trial = 0; trial < (int)(sizeof(sizes) / sizeof(sizes[0])); trial++) {
        int M = sizes[trial];
        dict *d = dictCreate(&zsetDictType);
        zbtElem **arr = zmalloc(sizeof(zbtElem *) * M);
        for (int i = 0; i < M; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "bm:%08d", i);
            sds s = sdsnew(buf);
            arr[i] = zbtCreateElem((double)i, s);
            sdsfree(s);
        }
        zbtree *bt = zbtCreate();
        zbtBuildFromSorted(bt, arr, M);
        for (int i = 0; i < M; i++)
            serverAssert(dictAdd(d, arr[i], NULL) == DICT_OK);
        zfree(arr);
        zbtDebugVerify(bt);
        serverAssert(bt->length == (unsigned long)M);
        serverAssert(zbtGetScore(zbtElemByRank(bt, 1, NULL)) == 0);
        serverAssert(zbtGetScore(zbtElemByRank(bt, M, NULL)) == (double)(M - 1));

        if (M >= 10) {
            /* Remove a middle window and confirm dict/tree stay in sync. */
            unsigned long lo = M / 4 + 1, hi = M / 2;
            unsigned long want = hi - lo + 1;
            unsigned long got = zbtDeleteRangeByRank(bt, lo, hi, d);
            zbtDebugVerify(bt);
            serverAssert(got == want);
            serverAssert(bt->length == (unsigned long)M - want);
            serverAssert(dictSize(d) == bt->length);
            /* Score suffix removal. */
            zrangespec rs = {.min = (double)(M * 3 / 4), .max = 1.0 / 0.0,
                             .minex = 0, .maxex = 0};
            zbtDeleteRangeByScore(bt, &rs, d);
            zbtDebugVerify(bt);
            serverAssert(dictSize(d) == bt->length);
        }
        /* Remove everything that is left. */
        zbtDeleteRangeByRank(bt, 1, bt->length, d);
        zbtDebugVerify(bt);
        serverAssert(bt->length == 0 && dictSize(d) == 0);
        dictRelease(d);
        zbtFree(bt);
    }
    test_cond("Bulk build + range delete", 1);

    /* --- Random-window range deletion, checking occupancy --- */
    {
        const int M = 20000;
        dict *d = dictCreate(&zsetDictType);
        zbtElem **arr = zmalloc(sizeof(zbtElem *) * M);
        for (int i = 0; i < M; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "fz:%08d", i);
            sds sd = sdsnew(buf);
            arr[i] = zbtCreateElem((double)i, sd);
            sdsfree(sd);
        }
        zbtree *bt = zbtCreate();
        zbtBuildFromSorted(bt, arr, M);
        for (int i = 0; i < M; i++)
            serverAssert(dictAdd(d, arr[i], NULL) == DICT_OK);
        zfree(arr);

        /* Delete windows wide enough to empty whole leaves, from positions
         * that do not line up with leaf boundaries. zbtDebugVerify() asserts
         * the minimum-occupancy invariant, which a single-element borrow
         * cannot maintain against slice-at-a-time deletion. */
        unsigned long seed = 12345;
        while (bt->length > 200) {
            seed = seed * 1103515245 + 12345;
            unsigned long span = 1 + (seed >> 16) % (ZBT_LEAF_MAX * 2);
            seed = seed * 1103515245 + 12345;
            unsigned long lo = 1 + (seed >> 16) % bt->length;
            unsigned long hi = lo + span;
            if (hi > bt->length) hi = bt->length;
            unsigned long before = bt->length;
            unsigned long got = zbtDeleteRangeByRank(bt, lo, hi, d);
            zbtDebugVerify(bt);
            serverAssert(got == hi - lo + 1);
            serverAssert(bt->length == before - got);
            serverAssert(dictSize(d) == bt->length);
            /* Ranks stay dense and ordered after every window removal. */
            serverAssert(zbtRankByElem(bt, zbtElemByRank(bt, 1, NULL)) == 1);
            serverAssert(zbtRankByElem(bt, zbtElemByRank(bt, bt->length, NULL))
                         == bt->length);
        }
        zbtDeleteRangeByRank(bt, 1, bt->length, d);
        serverAssert(bt->length == 0 && dictSize(d) == 0);
        dictRelease(d);
        zbtFree(bt);
    }
    test_cond("Random-window range delete keeps occupancy", 1);

    /* --- Sorted insertion packs leaves, in both directions --- */
    for (int desc = 0; desc < 2; desc++) {
        const int M = 20000;
        zbtree *at = zbtCreate();
        for (int k = 0; k < M; k++) {
            int i = desc ? M - 1 - k : k;   /* descending inserts prepend */
            char buf[32];
            snprintf(buf, sizeof(buf), "as:%08d", i);
            sds sd = sdsnew(buf);
            zbtInsert(at, (double)i, sd);
            sdsfree(sd);
        }
        zbtDebugVerify(at);
        serverAssert(at->length == (unsigned long)M);

        /* Every leaf but the under-filled end one should be full, so the leaf
         * count should match what a bottom-up bulk build of the same elements
         * would use. An even split would need roughly twice as many. */
        unsigned long leaves = 0, full = 0;
        for (zbtLeaf *lf = (zbtLeaf *)at->head; lf; lf = lf->next) {
            leaves++;
            if (lf->n.count == ZBT_LEAF_MAX) full++;
        }
        unsigned long ideal = (M + ZBT_LEAF_MAX - 1) / ZBT_LEAF_MAX;
        serverAssert(leaves == ideal);
        serverAssert(full == leaves - 1);
        /* The short leaf must be the end the inserts were arriving at. */
        zbtLeaf *shortlf = desc ? (zbtLeaf *)at->head : (zbtLeaf *)at->tail;
        serverAssert(shortlf->n.count < ZBT_LEAF_MAX);

        /* Deleting from a packed tree must still hold the invariant, including
         * when the short end leaf itself is the one that underflows. */
        for (int k = 0; k < 3 * ZBT_LEAF_MAX; k++) {
            unsigned long rank = desc ? 1 : at->length;
            zbtElem *e = zbtElemByRank(at, rank, NULL);
            zbtDeleteElem(at, e);
            zbtDebugVerify(at);
        }
        serverAssert(at->length == (unsigned long)M - 3 * ZBT_LEAF_MAX);
        zbtFree(at);
    }
    test_cond("Sorted insert packs leaves both directions", 1);

    /* --- Incremental node defragmentation --- */
    {
        const int M = 4000;
        zbtElem **arr = zmalloc(sizeof(zbtElem *) * M);
        for (int i = 0; i < M; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "dm:%08d", i);
            sds s = sdsnew(buf);
            arr[i] = zbtCreateElem((double)i, s);
            sdsfree(s);
        }
        zbtree *bt = zbtCreate();
        zbtBuildFromSorted(bt, arr, M);
        zfree(arr);
        zbtDebugVerify(bt);

        int steps = 0;
        while (zbtDefragNodesIncremental(bt, zbtTestReloc, 16)) {
            zbtDebugVerify(bt); /* tree must stay valid after every slice */
            serverAssert(++steps < 100000);
        }
        zbtDebugVerify(bt);

        zbtIter it;
        zbtElem *e = zbtFirst(bt, &it);
        unsigned long c = 0;
        zbtElem *prev = NULL;
        while (e) {
            if (prev)
                serverAssert(zbtCompare(zbtGetScore(prev), zbtGetEle(prev), e) < 0);
            prev = e;
            c++;
            e = zbtIterNext(&it);
        }
        serverAssert(c == (unsigned long)M);
        zbtFree(bt);
        test_cond("Incremental node defrag", 1);
    }

    /* Compact score encoding: round-trip every width and cross boundaries
     * in both directions, including inf and -0.0. */
    {
        zbtree *bt = zbtCreate();
        struct {
            double score;
            const char *name;
            uint8_t enc;
        } cases[] = {
            {0, "z0", ZBT_SCORE_I8},
            {127, "i8hi", ZBT_SCORE_I8},
            {-128, "i8lo", ZBT_SCORE_I8},
            {128, "i16", ZBT_SCORE_I16},
            {-129, "i16n", ZBT_SCORE_I16},
            {32767, "i16hi", ZBT_SCORE_I16},
            {32768, "i24", ZBT_SCORE_I24},
            {8388607, "i24hi", ZBT_SCORE_I24},
            {8388608, "i32", ZBT_SCORE_I32},
            {2147483647.0, "i32hi", ZBT_SCORE_I32},
            {2147483648.0, "i48", ZBT_SCORE_I48},
            {(double)((1LL << 47) - 1), "i48hi", ZBT_SCORE_I48},
            {(double)(1LL << 47), "dbl48", ZBT_SCORE_DBL},
            {1.5, "frac", ZBT_SCORE_DBL},
            {INFINITY, "inf", ZBT_SCORE_DBL},
            {-INFINITY, "ninf", ZBT_SCORE_DBL},
            {-0.0, "nzero", ZBT_SCORE_DBL},
        };
        int ncases = (int)(sizeof(cases) / sizeof(cases[0]));
        zbtElem **elems = zmalloc(sizeof(zbtElem *) * ncases);
        for (int i = 0; i < ncases; i++) {
            sds s = sdsnew(cases[i].name);
            elems[i] = zbtInsert(bt, cases[i].score, s);
            sdsfree(s);
            serverAssert(elems[i]->enc == cases[i].enc);
            double got = zbtGetScore(elems[i]);
            if (cases[i].enc == ZBT_SCORE_DBL && cases[i].score == 0) {
                serverAssert(got == 0 && signbit(got));
            } else if (isinf(cases[i].score)) {
                serverAssert(isinf(got) && !!signbit(got) == !!signbit(cases[i].score));
            } else {
                serverAssert(got == cases[i].score);
            }
        }
        zbtDebugVerify(bt);

        /* Width changes in both directions. */
        elems[1] = zbtUpdateScore(bt, elems[1], 128);           /* 127 I8 -> 128 I16 */
        serverAssert(elems[1]->enc == ZBT_SCORE_I16 && zbtGetScore(elems[1]) == 128);
        elems[1] = zbtUpdateScore(bt, elems[1], 127);           /* back I16 -> I8 */
        serverAssert(elems[1]->enc == ZBT_SCORE_I8 && zbtGetScore(elems[1]) == 127);

        zbtElem *one;
        {
            sds s = sdsnew("one");
            one = zbtInsert(bt, 1, s);
            sdsfree(s);
        }
        one = zbtUpdateScore(bt, one, 1.5);                     /* 1 I8 -> 1.5 DBL */
        serverAssert(one->enc == ZBT_SCORE_DBL && zbtGetScore(one) == 1.5);
        one = zbtUpdateScore(bt, one, 1);                       /* 1.5 DBL -> 1 I8 */
        serverAssert(one->enc == ZBT_SCORE_I8 && zbtGetScore(one) == 1);

        zbtElem *big;
        {
            sds s = sdsnew("i32x");
            big = zbtInsert(bt, 2147483647.0, s);
            sdsfree(s);
        }
        serverAssert(big->enc == ZBT_SCORE_I32);
        big = zbtUpdateScore(bt, big, (double)(1LL << 47));     /* I32 -> DBL via 2^47 */
        serverAssert(big->enc == ZBT_SCORE_DBL && zbtGetScore(big) == (double)(1LL << 47));
        big = zbtUpdateScore(bt, big, 2147483648.0);            /* DBL -> I48 */
        serverAssert(big->enc == ZBT_SCORE_I48 && zbtGetScore(big) == 2147483648.0);

        zbtDebugVerify(bt);
        zfree(elems);
        zbtFree(bt);
        test_cond("Compact score encoding width boundaries", 1);
    }

    /* Range endpoint lookups, against a linear reference. Both directions of
     * Z[REV]RANGEBYSCORE start here, and the offset decides whether the
     * element is reached by walking the leaf chain or by a rank jump. */
    {
        /* Equal-score runs wider than a leaf, so a boundary lands inside a
         * run instead of at a leaf edge, with infinities at both ends. */
        static const struct { double score; int count; } runs[] = {
            {-INFINITY, 1},
            {0, 100},
            {10, 300},          /* several leaves of one score */
            {10.5, 1},
            {20, 100},
            {INFINITY, 1},
        };
        int nruns = (int)(sizeof(runs) / sizeof(runs[0]));
        int total = 0;
        for (int r = 0; r < nruns; r++) total += runs[r].count;

        zbtElem **arr = zmalloc(sizeof(zbtElem *) * total);
        int at = 0;
        for (int r = 0; r < nruns; r++) {
            for (int i = 0; i < runs[r].count; i++) {
                char buf[32];
                snprintf(buf, sizeof(buf), "r%d:%05d", r, i);
                sds s = sdsnew(buf);
                arr[at++] = zbtCreateElem(runs[r].score, s);
                sdsfree(s);
            }
        }
        zbtree *bt = zbtCreate();
        zbtBuildFromSorted(bt, arr, total);
        zfree(arr);
        zbtDebugVerify(bt);

        static const zrangespec ranges[] = {
            {-INFINITY, INFINITY, 0, 0},   /* everything */
            {-INFINITY, INFINITY, 1, 1},   /* everything but the infinities */
            {10, 10, 0, 0},                /* the multi-leaf run, exactly */
            {10, 10, 1, 0},                /* empty: excluded from below */
            {10, 10, 0, 1},                /* empty: excluded from above */
            {0, 20, 0, 0},
            {0, 20, 1, 1},
            {-INFINITY, 10, 0, 0},
            {10, INFINITY, 0, 0},
            {-INFINITY, -INFINITY, 0, 0},  /* single element at the head */
            {INFINITY, INFINITY, 0, 0},    /* single element at the tail */
            {10.5, 10.5, 0, 0},            /* single element mid-tree */
            {20, 0, 0, 0},                 /* inverted */
            {100, 200, 0, 0},              /* past the tail */
            {-200, -100, 0, 0},            /* before the head */
        };
        /* Offsets around ZBT_RANGE_WALK_MAX (leaf walk vs rank jump), around
         * the leaf fanout, and past both ends of every range. */
        static const long offsets[] = {
            0, 1, 2, 9, 10, 11, 12, 63, 64, 65, 99, 100, 299, 300, 301,
            (long)ZBT_LEAF_MAX * ZBT_INNER_MAX, 100000,
            -1, -2, -9, -10, -11, -12, -64, -100, -300, -301, -100000,
            LONG_MIN, LONG_MAX,
        };
        int nranges = (int)(sizeof(ranges) / sizeof(ranges[0]));
        int noffsets = (int)(sizeof(offsets) / sizeof(offsets[0]));
        for (int i = 0; i < nranges; i++) {
            zrangespec rs = ranges[i];
            for (int j = 0; j < noffsets; j++)
                zbtCheckNthInRange(bt, &rs, offsets[j]);
        }
        zbtFree(bt);

        /* Degenerate trees: nothing to descend into, and a single element
         * that is both ends of every range covering it. */
        zbtree *empty = zbtCreate();
        zrangespec all = {-INFINITY, INFINITY, 0, 0};
        serverAssert(zbtNthInRange(empty, &all, 0, NULL, NULL) == NULL);
        serverAssert(zbtNthInRange(empty, &all, -1, NULL, NULL) == NULL);
        zbtFree(empty);

        zbtree *one = zbtCreate();
        {
            sds s = sdsnew("only");
            zbtInsert(one, 5, s);
            sdsfree(s);
        }
        for (int j = 0; j < noffsets; j++) {
            zrangespec rs = all;
            zbtCheckNthInRange(one, &rs, offsets[j]);
        }
        zbtFree(one);
        test_cond("Range endpoint lookups match a linear scan", 1);
    }

    return 0;
}
#endif
