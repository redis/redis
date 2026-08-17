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

/* Fanout of the tree. Nodes are allowed to temporarily hold one extra slot
 * (hence the "+1" sized arrays) before they are split. */
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

/* Allocate an element with the member SDS embedded in the same allocation
 * (single block: zbtElem header + sds header + data). The member is copied
 * from 'buf', which does not have to be an sds: callers holding plain bytes
 * (listpack entries, integer members) can build an element without first
 * materializing a temporary sds. */
zbtElem *zbtCreateElemBuf(double score, const char *buf, size_t len) {
    char sds_type = sdsReqType(len);
    size_t sds_hdr_len = sdsHdrSize(sds_type);
    size_t hdr = sizeof(zbtElem);
    size_t sds_buf_size = sds_hdr_len + len + 1;
    size_t total = hdr + sds_buf_size;

    zbtElem *e = zmalloc(total);
    e->score = score;
    size_t sds_offset = hdr + sds_hdr_len;
    e->sdsoffset = (uint16_t)sds_offset;

    char *dst = (char *)e + hdr;
    sds emb = sdsnewplacement(dst, sds_buf_size, sds_type, buf, len);
    serverAssert(emb == (sds)((char *)e + sds_offset));
    return e;
}

/* Same as zbtCreateElemBuf(), for callers that already hold an sds. The caller
 * keeps ownership of 'ele' (it is copied). */
zbtElem *zbtCreateElem(double score, sds ele) {
    return zbtCreateElemBuf(score, ele, sdslen(ele));
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
    if (score < e->score) return -1;
    if (score > e->score) return 1;
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

/* Minimum element of a subtree rooted at 'n' (assumes non-empty). */
static zbtElem *zbtNodeMin(zbtNode *n) {
    while (!n->isleaf) n = ((zbtInner *)n)->child[0];
    return ((zbtLeaf *)n)->elems[0];
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

/* Recompute csize/sep for every ancestor of 'n' up to the root. Used after
 * an insertion, deletion or in-place element replacement changed a subtree. */
static void zbtUpdateToRoot(zbtree *t, zbtNode *n) {
    UNUSED(t);
    while (n->parent) {
        zbtInner *p = (zbtInner *)n->parent;
        int idx = zbtChildIdx(p, n);
        p->csize[idx] = zbtSubtreeSize(n);
        p->sep[idx] = zbtNodeMin(n);
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

static void zbtSplitLeaf(zbtree *t, zbtLeaf *lf) {
    zbtLeaf *r = zbtNewLeaf(t);
    int total = (int)lf->n.count; /* == ZBT_LEAF_MAX + 1 */
    int keep = total / 2;
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
    double score = e->score;
    sds ele = zbtGetEle(e);
    zbtLeaf *lf = zbtFindLeaf(t, score, ele);
    int found;
    int idx = zbtLeafSearch(lf, score, ele, &found);
    serverAssert(!found);

    memmove(&lf->elems[idx + 1], &lf->elems[idx],
            ((int)lf->n.count - idx) * sizeof(zbtElem *));
    lf->elems[idx] = e;
    lf->n.count++;
    t->length++;
    t->alloc_size += zmalloc_usable_size(e);

    if (lf->n.count > ZBT_LEAF_MAX)
        zbtSplitLeaf(t, lf);
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

static void zbtRebalanceInner(zbtree *t, zbtInner *in) {
    zbtInner *p = (zbtInner *)in->n.parent;
    int idx = zbtChildIdx(p, (zbtNode *)in);

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

static void zbtRebalanceLeaf(zbtree *t, zbtLeaf *lf) {
    zbtInner *p = (zbtInner *)lf->n.parent;
    int idx = zbtChildIdx(p, (zbtNode *)lf);

    /* Borrow from left sibling. */
    if (idx > 0) {
        zbtLeaf *L = (zbtLeaf *)p->child[idx - 1];
        if (L->n.count > ZBT_LEAF_MIN) {
            memmove(&lf->elems[1], &lf->elems[0], lf->n.count * sizeof(zbtElem *));
            lf->elems[0] = L->elems[L->n.count - 1];
            L->n.count--;
            lf->n.count++;
            p->csize[idx - 1] = L->n.count; p->sep[idx - 1] = zbtNodeMin((zbtNode *)L);
            p->csize[idx] = lf->n.count;    p->sep[idx] = zbtNodeMin((zbtNode *)lf);
            zbtUpdateToRoot(t, (zbtNode *)p);
            return;
        }
    }
    /* Borrow from right sibling. */
    if (idx < (int)p->n.count - 1) {
        zbtLeaf *R = (zbtLeaf *)p->child[idx + 1];
        if (R->n.count > ZBT_LEAF_MIN) {
            lf->elems[lf->n.count] = R->elems[0];
            memmove(&R->elems[0], &R->elems[1], (R->n.count - 1) * sizeof(zbtElem *));
            R->n.count--;
            lf->n.count++;
            p->csize[idx] = lf->n.count;     p->sep[idx] = zbtNodeMin((zbtNode *)lf);
            p->csize[idx + 1] = R->n.count;  p->sep[idx + 1] = zbtNodeMin((zbtNode *)R);
            zbtUpdateToRoot(t, (zbtNode *)p);
            return;
        }
    }

    /* Merge with a sibling. */
    zbtLeaf *a, *b;
    int ai;
    if (idx > 0) { a = (zbtLeaf *)p->child[idx - 1]; b = lf; ai = idx - 1; }
    else { a = lf; b = (zbtLeaf *)p->child[idx + 1]; ai = idx; }
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
    double score = e->score;
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

/* Move an existing element to reflect a new score. The element object is
 * reused so the ZSET dict entry does not need updating. */
void zbtUpdateScore(zbtree *t, zbtElem *e, double newscore) {
    /* Remove and reinsert. We must remove from the dict? No: the dict maps
     * member -> elem and the member is unchanged, so only the tree position
     * changes. Detach the element object, reinsert it with the new score. */
    double score = e->score;
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

    e->score = newscore;
    zbtInsertElem(t, e);
}

/*-----------------------------------------------------------------------------
 * Rank and rank-based access
 *----------------------------------------------------------------------------*/

/* 1-based rank of element 'e'. */
unsigned long zbtRankByElem(zbtree *t, zbtElem *e) {
    double score = e->score;
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
    if (!zbtSeek(t, e->score, zbtGetEle(e), &it)) return NULL;
    return zbtIterNext(&it);
}

zbtElem *zbtPrev(zbtree *t, zbtElem *e) {
    zbtIter it;
    if (!zbtSeek(t, e->score, zbtGetEle(e), &it)) return NULL;
    return zbtIterPrev(&it);
}

/*-----------------------------------------------------------------------------
 * Range queries
 *----------------------------------------------------------------------------*/

/* Count the elements at the start of the sorted order for which before()
 * returns true. 'before' must be monotonic in tree order (true for a prefix,
 * then false). */
typedef int (*zbtBeforeFn)(const zbtElem *e, void *arg);

static unsigned long zbtCountBefore(zbtree *t, zbtBeforeFn before, void *arg) {
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
    for (uint32_t i = 0; i < lf->n.count; i++) {
        if (before(lf->elems[i], arg)) cnt++;
        else break;
    }
    return cnt;
}

/* Predicates for score ranges. */
static int beforeScoreLt(const zbtElem *e, void *arg) {
    return e->score < *(double *)arg;
}
static int beforeScoreLe(const zbtElem *e, void *arg) {
    return e->score <= *(double *)arg;
}

/* Predicates for lex ranges. */
static int beforeNotGteMin(const zbtElem *e, void *arg) {
    return !zslLexValueGteMin(zbtGetEle((zbtElem *)e), (zlexrangespec *)arg);
}
static int beforeLteMax(const zbtElem *e, void *arg) {
    return zslLexValueLteMax(zbtGetEle((zbtElem *)e), (zlexrangespec *)arg);
}

/* Shared implementation once the [firstRank, lastRank] window of the range
 * is known. Mirrors the skiplist zslNthIn*Range semantics: n >= 0 counts
 * forward from the first in-range element, n < 0 counts back from the last. */
static zbtElem *zbtNthGeneric(zbtree *t, long n, unsigned long *out_rank,
                              zbtIter *it, unsigned long firstRank,
                              unsigned long lastRank) {
    if (firstRank == 0 || firstRank > lastRank) return NULL;
    long target;
    if (n >= 0) target = (long)firstRank + n;
    else target = (long)lastRank + 1 + n;
    if (target < (long)firstRank || target > (long)lastRank) return NULL;
    if (out_rank) *out_rank = (unsigned long)target;
    return zbtElemByRank(t, (unsigned long)target, it);
}

zbtElem *zbtNthInRange(zbtree *t, zrangespec *range, long n,
                       unsigned long *out_rank, zbtIter *it) {
    if (t->length == 0) return NULL;
    double minv = range->min, maxv = range->max;
    /* Elements strictly before the range start. */
    unsigned long before = range->minex ?
        zbtCountBefore(t, beforeScoreLe, &minv) :
        zbtCountBefore(t, beforeScoreLt, &minv);
    /* Elements up to and including the range end. */
    unsigned long upto = range->maxex ?
        zbtCountBefore(t, beforeScoreLt, &maxv) :
        zbtCountBefore(t, beforeScoreLe, &maxv);
    return zbtNthGeneric(t, n, out_rank, it, before + 1, upto);
}

zbtElem *zbtNthInLexRange(zbtree *t, zlexrangespec *range, long n,
                          unsigned long *out_rank, zbtIter *it) {
    if (t->length == 0) return NULL;
    unsigned long before = zbtCountBefore(t, beforeNotGteMin, range);
    unsigned long upto = zbtCountBefore(t, beforeLteMax, range);
    return zbtNthGeneric(t, n, out_rank, it, before + 1, upto);
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
    zbtLeaf *lf = zbtFindLeaf(t, newe->score, zbtGetEle(newe));
    int found;
    int idx = zbtLeafSearch(lf, newe->score, zbtGetEle(newe), &found);
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
            t->defrag_resume_score = e0->score;
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

static unsigned long zbtVerifyNode(zbtree *t, zbtNode *n, int depth,
                                   int *leafdepth) {
    if (n->isleaf) {
        zbtLeaf *lf = (zbtLeaf *)n;
        if (n->parent) serverAssert(n->count >= ZBT_LEAF_MIN);
        serverAssert(n->count <= ZBT_LEAF_MAX);
        if (*leafdepth == -1) *leafdepth = depth;
        else serverAssert(*leafdepth == depth); /* all leaves same depth */
        for (uint32_t i = 1; i < n->count; i++) {
            zbtElem *a = lf->elems[i - 1], *b = lf->elems[i];
            serverAssert(zbtCompare(a->score, zbtGetEle(a), b) < 0);
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
        serverAssert(zbtNodeMin(in->child[i]) == in->sep[i]);
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
                serverAssert(zbtCompare(prev_elem->score, zbtGetEle(prev_elem), cur) < 0);
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
            if (prev) assert(zbtCompare(prev->score, zbtGetEle(prev), e) < 0);
            prev = e;
            c++;
            e = zbtIterNext(&it);
        }
        test_cond("Forward scan sorted and complete", c == (unsigned long)N);
    }

    /* Delete half in random order. */
    for (int i = 0; i < N; i += 2) {
        zbtElem *e = elements[i].elem;
        assert(zbtGetRank(t, e->score, zbtGetEle(e)) != 0);
        zbtDeleteElem(t, e);
        elements[i].deleted = 1;
        if (i % 101 == 0) zbtDebugVerify(t);
    }
    zbtDebugVerify(t);
    test_cond("Delete half", t->length == (unsigned long)N / 2);

    /* Update scores of the survivors. */
    for (int i = 1; i < N; i += 2) {
        zbtElem *e = elements[i].elem;
        zbtUpdateScore(t, e, (double)(rand() % 300));
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
    for (int trial = 0; trial < 3; trial++) {
        /* Cover boundary sizes around leaf/inner fan-out multiples. */
        static const int sizes[] = {1, ZBT_LEAF_MAX, ZBT_LEAF_MAX + 1,
                                    ZBT_LEAF_MAX * ZBT_INNER_MAX + 3, 5000};
        int M = sizes[trial % (int)(sizeof(sizes) / sizeof(sizes[0]))];
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
        serverAssert(zbtElemByRank(bt, 1, NULL)->score == 0);
        serverAssert(zbtElemByRank(bt, M, NULL)->score == (double)(M - 1));

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
                serverAssert(zbtCompare(prev->score, zbtGetEle(prev), e) < 0);
            prev = e;
            c++;
            e = zbtIterNext(&it);
        }
        serverAssert(c == (unsigned long)M);
        zbtFree(bt);
        test_cond("Incremental node defrag", 1);
    }

    return 0;
}
#endif
