/* Rax -- A radix tree implementation.
 *
 * Copyright (c) 2017-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include "rax.h"
#include "redisassert.h"

#ifndef RAX_MALLOC_INCLUDE
#define RAX_MALLOC_INCLUDE "rax_malloc.h"
#endif

#include RAX_MALLOC_INCLUDE

/* -------------------------------- Debugging ------------------------------ */

void raxDebugShowNode(const char *msg, raxNode *n);

/* Turn debugging messages on/off by compiling with RAX_DEBUG_MSG macro on.
 * When RAX_DEBUG_MSG is defined by default Rax operations will emit a lot
 * of debugging info to the standard output, however you can still turn
 * debugging on/off in order to enable it only when you suspect there is an
 * operation causing a bug using the function raxSetDebugMsg(). */
#ifdef RAX_DEBUG_MSG
#define debugf(...)                                                            \
    if (raxDebugMsg) {                                                         \
        printf("%s:%s:%d:\t", __FILE__, __func__, __LINE__);                   \
        printf(__VA_ARGS__);                                                   \
        fflush(stdout);                                                        \
    }

#define debugnode(msg,n) raxDebugShowNode(msg,n)
#else
#define debugf(...)
#define debugnode(msg,n)
#endif

/* By default log debug info if RAX_DEBUG_MSG is defined. */
static int raxDebugMsg = 1;

/* When debug messages are enabled, turn them on/off dynamically. By
 * default they are enabled. Set the state to 0 to disable, and 1 to
 * re-enable. */
void raxSetDebugMsg(int onoff) {
    raxDebugMsg = onoff;
}

/* ------------------------- raxStack functions --------------------------
 * The raxStack is a simple stack of pointers that is capable of switching
 * from using a stack-allocated array to dynamic heap once a given number of
 * items are reached. It is used in order to retain the list of parent nodes
 * while walking the radix tree in order to implement certain operations that
 * need to navigate the tree upward.
 * ------------------------------------------------------------------------- */

/* Initialize the stack. */
static inline void raxStackInit(raxStack *ts) {
    ts->stack = ts->static_items;
    ts->items = 0;
    ts->maxitems = RAX_STACK_STATIC_ITEMS;
    ts->oom = 0;
}

/* Push an item into the stack, returns 1 on success, 0 on out of memory. */
static inline int raxStackPush(raxStack *ts, void *ptr) {
    if (ts->items == ts->maxitems) {
        if (ts->stack == ts->static_items) {
            ts->stack = rax_malloc(sizeof(void*)*ts->maxitems*2);
            if (ts->stack == NULL) {
                ts->stack = ts->static_items;
                ts->oom = 1;
                errno = ENOMEM;
                return 0;
            }
            memcpy(ts->stack,ts->static_items,sizeof(void*)*ts->maxitems);
        } else {
            void **newalloc = rax_realloc(ts->stack,sizeof(void*)*ts->maxitems*2);
            if (newalloc == NULL) {
                ts->oom = 1;
                errno = ENOMEM;
                return 0;
            }
            ts->stack = newalloc;
        }
        ts->maxitems *= 2;
    }
    ts->stack[ts->items] = ptr;
    ts->items++;
    return 1;
}

/* Pop an item from the stack, the function returns NULL if there are no
 * items to pop. */
static inline void *raxStackPop(raxStack *ts) {
    if (ts->items == 0) return NULL;
    ts->items--;
    return ts->stack[ts->items];
}

/* Return the stack item at the top of the stack without actually consuming
 * it. */
static inline void *raxStackPeek(raxStack *ts) {
    if (ts->items == 0) return NULL;
    return ts->stack[ts->items-1];
}

/* Free the stack in case we used heap allocation. */
static inline void raxStackFree(raxStack *ts) {
    if (ts->stack != ts->static_items) rax_free(ts->stack);
}

/* ----------------------------------------------------------------------------
 * Radix tree implementation
 * --------------------------------------------------------------------------*/

/* Return the padding needed in the characters section of a node having size
 * 'nodesize'. The padding is needed to store the child pointers to aligned
 * addresses. Note that we add 4 to the node size because the node has a four
 * bytes header. */
#define raxPadding(nodesize) ((sizeof(void*)-(((nodesize)+4) % sizeof(void*))) & (sizeof(void*)-1))

/* Return the pointer to the last child pointer in a node. For the compressed
 * nodes this is the only child pointer. */
#define raxNodeLastChildPtr(n) ((raxNode**) ( \
    ((char*)(n)) + \
    raxNodeCurrentLength(n) - \
    sizeof(raxNode*) - \
    (((n)->iskey && !(n)->isnull) ? sizeof(void*) : 0) \
))

/* Return the pointer to the first child pointer. */
#define raxNodeFirstChildPtr(n) ((raxNode**) ( \
    (n)->data + \
    (n)->size + \
    raxPadding((n)->size)))

/* Return the current total size of the node. Note that the second line
 * computes the padding after the string of characters, needed in order to
 * save pointers to aligned addresses. */
#define raxNodeCurrentLength(n) ( \
    sizeof(raxNode)+(n)->size+ \
    raxPadding((n)->size)+ \
    ((n)->iscompr ? sizeof(raxNode*) : sizeof(raxNode*)*(n)->size)+ \
    (((n)->iskey && !(n)->isnull)*sizeof(void*)) \
)

/* Allocate a new non compressed node with the specified number of children.
 * If datafield is true, the allocation is made large enough to hold the
 * associated data pointer.
 * Returns the new node pointer. On out of memory NULL is returned. */
raxNode *raxNewNode(rax *rax, size_t children, int datafield) {
    size_t nodesize = sizeof(raxNode)+children+raxPadding(children)+
                      sizeof(raxNode*)*children;
    if (datafield) nodesize += sizeof(void*);
    size_t usable;
    raxNode *node = rax_malloc_usable(nodesize,&usable);
    if (node == NULL) return NULL;
    node->iskey = 0;
    node->isnull = 0;
    node->iscompr = 0;
    node->size = children;
    if (rax->alloc_size) *rax->alloc_size += usable;
    return node;
}

/* Deallocate node */
void raxFreeNode(rax *rax, raxNode *n) {
    size_t usable;
    rax_free_usable(n, &usable);
    if (rax->alloc_size) *rax->alloc_size -= usable;
}

/* Bytes consumed descending one step from `n` toward a child: the whole
 * compressed string for a compressed node, or a single edge byte otherwise. */
static inline int raxStepLenNode(const raxNode *n) {
    return n->iscompr ? (int)n->size : 1;
}

/* Allocate a new rax and return its pointer. On out of memory the function
 * returns NULL. */
rax *raxNew(void) {
    return raxNewEx(0, NULL, 0);
}

/* Common rax constructor
 *  alloc_size - if non-NULL, rax will account for its used memory at this location
 *  keyFixedLen - if > 0, rax will enable the leaf-inlining path and assert every
 *                insert/remove uses exactly `keyFixedLen` bytes. */
rax *raxNewEx(int metaSize, size_t *alloc_size, uint32_t keyFixedLen) {
    size_t usable;
    assert(keyFixedLen <= RAX_NODE_MAX_SIZE);
    rax *rax = rax_malloc_usable(sizeof(*rax) + metaSize, &usable);
    if (rax == NULL) return NULL;
    rax->numele = 0;
    rax->numnodes = 1;
    rax->alloc_size = alloc_size;
    rax->keyFixedLen = keyFixedLen;
    if (rax->alloc_size) *rax->alloc_size += usable;
    rax->head = raxNewNode(rax, 0, 0);
    if (rax->head == NULL) {
        if (rax->alloc_size) *rax->alloc_size -= usable;
        rax_free(rax);
        return NULL;
    } else {
        return rax;
    }
}

/* realloc the node to have 'newsize'. On out of memory NULL is returned. */
raxNode *raxNodeRealloc(rax *rax, raxNode *n, size_t newsize) {
    size_t usable, old_usable;
    raxNode *newn = rax_realloc_usable(n,newsize,&usable,&old_usable);
    if (newn == NULL) return NULL;
    if (rax->alloc_size) {
        *rax->alloc_size -= old_usable;
        *rax->alloc_size += usable;
    }
    return newn;
}

/* realloc the node to make room for auxiliary data in order
 * to store an item in that node. On out of memory NULL is returned. */
raxNode *raxReallocForData(rax *rax, raxNode *n, void *data) {
    if (data == NULL) return n; /* No reallocation needed, setting isnull=1 */
    size_t curlen = raxNodeCurrentLength(n);
    return raxNodeRealloc(rax,n,curlen+sizeof(void*));
}

/* Set the node auxiliary data to the specified pointer. */
static inline void raxSetData(raxNode *n, void *data) {
    n->iskey = 1;
    if (data != NULL) {
        n->isnull = 0;
        void **ndata = (void**)
            ((char*)n+raxNodeCurrentLength(n)-sizeof(void*));
        memcpy(ndata,&data,sizeof(data));
    } else {
        n->isnull = 1;
    }
}

/* Get the node auxiliary data. */
void *raxGetData(raxNode *n) {
    if (n->isnull) return NULL;
    void **ndata =(void**)((char*)n+raxNodeCurrentLength(n)-sizeof(void*));
    void *data;
    memcpy(&data,ndata,sizeof(data));
    return data;
}

/* Add a new child to the node 'n' representing the character 'c' and return
 * its new pointer, as well as the child pointer by reference. Additionally
 * '***parentlink' is populated with the raxNode pointer-to-pointer of where
 * the new child was stored, which is useful for the caller to replace the
 * child pointer if it gets reallocated.
 *
 * On success the new parent node pointer is returned (it may change because
 * of the realloc, so the caller should discard 'n' and use the new value).
 * On out of memory NULL is returned, and the old node is still valid. */
raxNode *raxAddChild(rax *rax, raxNode *n, unsigned char c, raxNode **childptr, raxNode ***parentlink) {
    assert(n->iscompr == 0);

    size_t curlen = raxNodeCurrentLength(n);
    n->size++;
    size_t newlen = raxNodeCurrentLength(n);
    n->size--; /* For now restore the original size. We'll update it only on
                  success at the end. */

    /* Alloc the new child we will link to 'n'. */
    raxNode *child = raxNewNode(rax,0,0);
    if (child == NULL) return NULL;

    /* Make space in the original node. If the current allocation already
     * has enough usable bytes (common with jemalloc size-class rounding),
     * skip the realloc entirely. */
    if (rax_malloc_usable_size(n) < newlen) {
        raxNode *newn = raxNodeRealloc(rax,n,newlen);
        if (newn == NULL) {
            raxFreeNode(rax,child);
            return NULL;
        }
        n = newn;
    }

    /* After the reallocation, we have up to 8/16 (depending on the system
     * pointer size, and the required node padding) bytes at the end, that is,
     * the additional char in the 'data' section, plus one pointer to the new
     * child, plus the padding needed in order to store addresses into aligned
     * locations.
     *
     * So if we start with the following node, having "abde" edges.
     *
     * Note:
     * - We assume 4 bytes pointer for simplicity.
     * - Each space below corresponds to one byte
     *
     * [HDR*][abde][Aptr][Bptr][Dptr][Eptr]|AUXP|
     *
     * After the reallocation we need: 1 byte for the new edge character
     * plus 4 bytes for a new child pointer (assuming 32 bit machine).
     * However after adding 1 byte to the edge char, the header + the edge
     * characters are no longer aligned, so we also need 3 bytes of padding.
     * In total the reallocation will add 1+4+3 bytes = 8 bytes:
     *
     * (Blank bytes are represented by ".")
     *
     * [HDR*][abde][Aptr][Bptr][Dptr][Eptr]|AUXP|[....][....]
     *
     * Let's find where to insert the new child in order to make sure
     * it is inserted in-place lexicographically. Assuming we are adding
     * a child "c" in our case pos will be = 2 after the end of the following
     * loop. */
    int pos;
    if (n->size > 0 && c > n->data[n->size - 1]) {
        pos = n->size;
    } else {
        for (pos = 0; pos < n->size; pos++) {
            if (n->data[pos] > c) break;
        }
    }

    /* Now, if present, move auxiliary data pointer at the end
     * so that we can mess with the other data without overwriting it.
     * We will obtain something like that:
     *
     * [HDR*][abde][Aptr][Bptr][Dptr][Eptr][....][....]|AUXP|
     */
    unsigned char *src, *dst;
    if (n->iskey && !n->isnull) {
        src = ((unsigned char*)n+curlen-sizeof(void*));
        dst = ((unsigned char*)n+newlen-sizeof(void*));
        memmove(dst,src,sizeof(void*));
    }

    /* Compute the "shift", that is, how many bytes we need to move the
     * pointers section forward because of the addition of the new child
     * byte in the string section. Note that if we had no padding, that
     * would be always "1", since we are adding a single byte in the string
     * section of the node (where now there is "abde" basically).
     *
     * However we have padding, so it could be zero, or up to 8.
     *
     * Another way to think at the shift is, how many bytes we need to
     * move child pointers forward *other than* the obvious sizeof(void*)
     * needed for the additional pointer itself. */
    size_t shift = newlen - curlen - sizeof(void*);

    /* We said we are adding a node with edge 'c'. The insertion
     * point is between 'b' and 'd', so the 'pos' variable value is
     * the index of the first child pointer that we need to move forward
     * to make space for our new pointer.
     *
     * To start, move all the child pointers after the insertion point
     * of shift+sizeof(pointer) bytes on the right, to obtain:
     *
     * [HDR*][abde][Aptr][Bptr][....][....][Dptr][Eptr]|AUXP|
     */
    src = n->data+n->size+
          raxPadding(n->size)+
          sizeof(raxNode*)*pos;
    memmove(src+shift+sizeof(raxNode*),src,sizeof(raxNode*)*(n->size-pos));

    /* Move the pointers to the left of the insertion position as well. Often
     * we don't need to do anything if there was already some padding to use. In
     * that case the final destination of the pointers will be the same, however
     * in our example there was no pre-existing padding, so we added one byte
     * plus three bytes of padding. After the next memmove() things will look
     * like that:
     *
     * [HDR*][abde][....][Aptr][Bptr][....][Dptr][Eptr]|AUXP|
     */
    if (shift) {
        src = (unsigned char*) raxNodeFirstChildPtr(n);
        memmove(src+shift,src,sizeof(raxNode*)*pos);
    }

    /* Now make the space for the additional char in the data section,
     * but also move the pointers before the insertion point to the right
     * by shift bytes, in order to obtain the following:
     *
     * [HDR*][ab.d][e...][Aptr][Bptr][....][Dptr][Eptr]|AUXP|
     */
    src = n->data+pos;
    memmove(src+1,src,n->size-pos);

    /* We can now set the character and its child node pointer to get:
     *
     * [HDR*][abcd][e...][Aptr][Bptr][....][Dptr][Eptr]|AUXP|
     * [HDR*][abcd][e...][Aptr][Bptr][Cptr][Dptr][Eptr]|AUXP|
     */
    n->data[pos] = c;
    n->size++;
    src = (unsigned char*) raxNodeFirstChildPtr(n);
    raxNode **childfield = (raxNode**)(src+sizeof(raxNode*)*pos);
    memcpy(childfield,&child,sizeof(child));
    *childptr = child;
    *parentlink = childfield;
    return n;
}

/* ------------------------ FIXED-LENGTH helpers --------------------------- */
/* Layout of nodes leafs is unchanged; only slot interpretation differs. Depth 
 * is tracked implicitly: either by raxLowWalk or raxIterator.
 *
 * The child-pointer slots hold inlined values of the key instead of a pointer 
 * to raxNode dedicated, so the layout is identical to a normal node, except each 
 * child slot [Xptr] becomes the value [Xval] for the key ending in 'X', and there 
 * is no trailing |AUXP| (a leaf parent is never itself a complete key):
 *
 *   iscompr=0:  [HDR*][abde][Aval][Bval][Dval][Eval]   (one slot per edge byte)
 *   iscompr=1:  [HDR*][xyz][val]                       (single slot, leafSlot=0)
 */

/* In a keyFixedLen>0 tree, the node at depth keyFixedLen-1 is a "leaf
 * parent": descending one more step reaches depth keyFixedLen, so its child
 * slots hold inlined value pointers, not raxNode pointers. Given the depth at
 * which a node's children sit, report whether those slots are inlined values.*/
static inline int raxSlotsAreValues(const rax *rax, size_t childDepth) {
    return rax->keyFixedLen && childDepth == (size_t)rax->keyFixedLen;
}

/* Read the value pointer stored in slot `idx` of leaf parent `h`. */
static inline void *raxLeafParentReadSlot(raxNode *parent, int idx) {
    void *v;
    memcpy(&v, raxNodeFirstChildPtr(parent) + idx, sizeof(v));
    return v;
}

/* Write `value` into slot `idx` of leaf parent `h`. */
static inline void raxLeafParentWriteSlot(raxNode *paremt, int idx, void *value) {
    memcpy(raxNodeFirstChildPtr(paremt) + idx, &value, sizeof(value));
}

/* Add a new edge byte `ch` to leaf parent `n` (iscompr=0) and store the
 * given value pointer in the new slot. Returns the (possibly realloc'd)
 * parent node, or NULL on OOM. This is the leaf-parent counterpart of
 * raxAddChild: no child raxNode is allocated.
 *
 * The byte/pointer shuffle is identical to raxAddChild; only the "what
 * goes in the slot" step differs (we store `value` rather than a child
 * pointer). Inserting edge 'c' (pos=2) between 'b' and 'd':
 *
 *   before:  [HDR*][abde][Aval][Bval][Dval][Eval]
 *   after:   [HDR*][abcde][Aval][Bval][Cval][Dval][Eval]
 */
static raxNode *raxAddSlot(rax *rax, raxNode *n, unsigned char ch, void *value)
{
    unsigned char *src;
    /* Unlike raxAddChild, there is no AUXP tail to relocate here: a leaf
     * parent sits at depth keyFixedLen-1, so it is never itself a key. */
    debugAssert(!n->iskey);
    debugAssert(n->iscompr == 0);
    debugAssert(rax->keyFixedLen > 0);

    size_t curlen = raxNodeCurrentLength(n);
    n->size++;
    size_t newlen = raxNodeCurrentLength(n);
    n->size--;
    
    /* realloc the node to make space if needed. */
    if (rax_malloc_usable_size(n) < newlen) {
        raxNode *newn = raxNodeRealloc(rax, n, newlen);
        if (newn == NULL) return NULL;
        n = newn;
    }

    /* Find the position of the new edge byte. */
    int pos;
    if (n->size > 0 && ch > n->data[n->size - 1]) {
        pos = n->size;
    } else {
        for (pos = 0; pos < n->size; pos++) {
            if (n->data[pos] > ch) break;
        }

        /* Slots at/after `pos` are the trailing (size-pos) pointers of the node.
         * With no AUXP tail they end at n+curlen, so move that block to end at
         * n+newlen, opening one slot's worth of gap (plus re-align padding). */
        size_t tail = sizeof(void*) * (n->size - pos);
        memmove((unsigned char*)n + newlen - tail,
                (unsigned char*)n + curlen - tail, tail);
    }

    /* shift = padding word added when the new edge byte crosses an 8-byte
     * boundary; if so, re-align the value slots before `pos` rightward. */
    size_t shift = newlen - curlen - sizeof(void*);
    if (shift) {
        src = (unsigned char*) raxNodeFirstChildPtr(n);
        memmove(src + shift, src, sizeof(void*) * pos);
    }

    /* Open the gap in the edge-byte array for the new edge byte. */
    if (pos < n->size) {
        src = n->data + pos;
        memmove(src + 1, src, n->size - pos);
    }

    n->data[pos] = ch;
    n->size++;
    src = (unsigned char*) raxNodeFirstChildPtr(n);
    void **slot = (void**)(src + sizeof(void*) * pos);
    memcpy(slot, &value, sizeof(value));
    return n;
}

/* Remove the slot at index `idx` from leaf parent `parent` (iscompr=0).
 * Returns the (possibly realloc'd) parent node. The caller is responsible
 * for freeing the value previously stored at the slot (the slot is just
 * void*-sized, the rax never owns the value).
 *
 * This mirrors raxRemoveChild's non-compressed branch; the slots hold
 * values (void*), not raxNode* pointers, and there is no AUXP tail to
 * preserve since a leaf parent is never itself a key. */
static raxNode *raxRemoveSlotAt(rax *rax, raxNode *parent, int idx) {
    debugAssert(!parent->iscompr);
    debugAssert(!parent->iskey);
    debugAssert(idx >= 0 && idx < parent->size);
    debugnode("raxRemoveSlotAt before", parent);

    void **pFirst = (void**) raxNodeFirstChildPtr(parent);
    void **pIdx = pFirst + idx;
    
    /* Move the edge bytes before the deletion point. */
    unsigned char *e = parent->data + idx;
    int taillen = parent->size - idx - 1;
    memmove(e, e + 1, taillen);
    
    /* shift = padding word added when the new edge byte crosses an 8-byte
     * boundary; if so, re-align the value slots before `pos` rightward. */
    size_t shift = ((parent->size + 4) % sizeof(void*)) == 1 ? sizeof(void*) : 0;
    if (shift)
        memmove(((char*)pFirst) - shift, pFirst, idx * sizeof(void*));

    /* Move the remaining value slots at the right position as well. */
    memmove(((char*)pIdx) - shift, pIdx + 1, taillen * sizeof(void*));

    parent->size--;
    raxNode *newn = raxNodeRealloc(rax, parent, raxNodeCurrentLength(parent));
    debugnode("raxRemoveSlotAt after", newn ? newn : parent);
    return newn ? newn : parent;
}

/* On insertion with keyFixedLen>0, if the insert loop ended with a new node 
 * (size==0), then append the suffix bytes of the key as compressed to the node 
 * and store the value pointer in the single slot (no leaf raxNode). 
 *
 *   before:  [HDR iscompr=0 size=0]               (new empty, no children)
 *   after:   [HDR iscompr=1 size=1][xyz][V]   (V inlined in the single slot)
 */
static raxNode *raxCompressNodeWithValue(rax *rax, raxNode *n,
                                         unsigned char *s, size_t len, void *value)
{
    debugAssert(n->size == 0);
    /* Only reached in keyFixedLen mode, fusing a fresh intermediate node at
     * depth < keyFixedLen: it is never a key, so there is no value-ptr tail */
    debugAssert(rax->keyFixedLen > 0);
    debugAssert(!n->iskey);

    /* Layout: [hdr][s[0..len-1]][padding][value_slot] (value in slot, no AUXP) */
    size_t newsize = sizeof(raxNode) + len + raxPadding(len) + sizeof(void*);
    raxNode *newn = raxNodeRealloc(rax, n, newsize);
    if (newn == NULL) return NULL;
    n = newn;

    n->iscompr = 1;
    n->size = len;
    memcpy(n->data, s, len);
    void **slot = (void**) raxNodeLastChildPtr(n);
    memcpy(slot, &value, sizeof(value));
    return n;
}

/* Turn the node 'n', that must be a node without any children, into a
 * compressed node representing a set of nodes linked one after the other
 * and having exactly one child each. The node can be a key or not: this
 * property and the associated value if any will be preserved.
 *
 * The function also returns a child node, since the last node of the
 * compressed chain cannot be part of the chain: it has zero children while
 * we can only compress inner nodes with exactly one child each. */
raxNode *raxCompressNode(rax *rax, raxNode *n, unsigned char *s, size_t len, raxNode **child) {
    assert(n->size == 0 && n->iscompr == 0);
    void *data = NULL; /* Initialized only to avoid warnings. */
    size_t newsize;

    debugf("Compress node: %.*s\n", (int)len,s);

    /* Allocate the child to link to this node. */
    *child = raxNewNode(rax,0,0);
    if (*child == NULL) return NULL;

    /* Make space in the parent node. */
    newsize = sizeof(raxNode)+len+raxPadding(len)+sizeof(raxNode*);
    if (n->iskey) {
        data = raxGetData(n); /* To restore it later. */
        if (!n->isnull) newsize += sizeof(void*);
    }
    raxNode *newn = raxNodeRealloc(rax,n,newsize);
    if (newn == NULL) {
        raxFreeNode(rax, *child);
        return NULL;
    }
    n = newn;

    n->iscompr = 1;
    n->size = len;
    memcpy(n->data,s,len);
    if (n->iskey) raxSetData(n,data);
    raxNode **childfield = raxNodeLastChildPtr(n);
    memcpy(childfield,child,sizeof(*child));
    return n;
}

/* Walk the tree following the key bytes `s[0..len-1]` and populate
 * `link` with the stop state (see raxNodeLink). If `ts` is non-NULL,
 * parent nodes are pushed there for upward cleanup by raxRemove.
 *
 * Returns 1 if `s[0..len-1]` is an exact key in the rax, 0 otherwise.
 * On match, if `value` is non-NULL, *value is set to the stored value
 * (which may itself be NULL when the key was inserted with isnull).
 * On miss, *value is left untouched. `link->i` always carries the
 * number of matched bytes regardless of match/miss. */
static inline int raxLowWalk(rax *rax, unsigned char *s, size_t len,
                             void **value, raxNodeLink *link, raxStack *ts) {
    raxNode *h = rax->head;
    raxNode **parentlink = &rax->head;

    size_t i = 0; /* Position in the string. */
    size_t j = 0; /* Position in the node children (or bytes if compressed).*/

    /* Stop-state defaults, overwritten by the walk as it progresses:
     * splitpos=0 (clean stop at a node boundary) and leafSlot=-1 (the
     * stop is not a fixed-length leaf parent). */
    link->splitpos = 0;
    link->leafSlot = -1;
    while(h->size && i < len) {
        debugnode("Lookup current node",h);
        unsigned char *v = h->data;

        if (h->iscompr) {
            for (j = 0; j < h->size && i < len; j++, i++) {
                if (v[j] != s[i]) break;
            }
            if (j != h->size) break;
        } else {
            /* Children are sorted. Check the last child first: for
             * sequential inserts the match is almost always at the end,
             * and for random keys the extra compare is negligible vs
             * the O(n) scan that follows on miss. */
            if (v[h->size - 1] == s[i]) {
                j = h->size - 1;
            } else if (s[i] > v[h->size - 1]) {
                j = h->size;
                break;
            } else {
                /* Even when h->size is large, linear scan provides good
                 * performances compared to other approaches that are in theory
                 * more sounding, like performing a binary search. */
                for (j = 0; j < h->size; j++) {
                    if (v[j] == s[i]) break;
                }
                if (j == h->size) break;
            }
            i++;
        }

        /* Fixed-length leaf-parent stop. We've matched at `h`. Descending now
         * would land us at depth keyFixedLen (the leaf depth), but the value
         * lives inline in the slot at children+j, not in a child raxNode. So
         * stop here, record the slot, and return a match -- this bypasses the
         * normal post-loop key-node checks, which don't apply. Reached only on
         * a successful byte match (iscompr=0) or full-prefix match (iscompr=1),
         * so the slot is always present. */
        if (raxSlotsAreValues(rax, i)) {
            debugnode("Lookup leaf-parent stop node is", h);
            link->stopnode = h;
            link->parentlink = parentlink;
            link->consumed = i;
            link->splitpos = RAX_LEAF_PARENT_STOP;
            link->leafSlot = h->iscompr ? 0 : (int)j;
            if (value != NULL) *value = raxLeafParentReadSlot(h, link->leafSlot);
            return 1;
        }

        if (ts) raxStackPush(ts,h); /* Save stack of parent nodes. */
        raxNode **children = raxNodeFirstChildPtr(h);
        if (h->iscompr) j = 0; /* Compressed node only child is at index 0. */
        memcpy(&h,children+j,sizeof(h));
        parentlink = children+j;
        j = 0; /* If the new node is non compressed and we do not
                  iterate again (since i == len) set the split
                  position to 0 to signal this node represents
                  the searched key. */
    }
    debugnode("Lookup stop node is",h);
    link->stopnode = h;
    link->parentlink = parentlink;
    link->consumed = i;
    /* if Non-leaf-parent-compressed stop: record the split position in the
     * compressed prefix. otherwise splitpos stays at the default 0 set on entry. */
    if (h->iscompr) link->splitpos = j;

    /* Match: query fully consumed, clean stop at a node boundary (not
     * mid-prefix on a compressed node), and h is a key. */
    if (i != len || (h->iscompr && link->splitpos != 0) || !h->iskey)
        return 0;
    
    if (value != NULL) *value = raxGetData(h);
    return 1;
}

#ifdef DEBUG_ASSERTIONS
/* Re-walk the tree and verify `link` still matches the current state,
 * i.e. no rax mutation happened between raxFindLink() and the current
 * raxInsertAt(). Returns 1 if the link is still valid, 0 otherwise.
 * Used only from a debugAssert(). */
static int raxLinkStillValid(rax *rax, unsigned char *s, size_t len, raxNodeLink *link) {
    raxNodeLink cur;
    raxLowWalk(rax, s, len, NULL, &cur, NULL);
    return cur.stopnode   == link->stopnode &&
           cur.parentlink == link->parentlink &&
           cur.consumed   == link->consumed &&
           cur.splitpos   == link->splitpos &&
           cur.leafSlot   == link->leafSlot;
}
#endif

/* Commit an insert at the position recorded in `link`. The link must
 * have come from an immediately-preceding raxFindLink() on (rax, s, len)
 * with no intervening rax mutation.
 *
 * If the link lands on an existing key, the associated data is
 * overwritten with `data`, the prior value is stored at *old (when old
 * is non-NULL), and 0 is returned. Otherwise the element is inserted
 * and 1 is returned. Callers wanting try-insert semantics (preserve
 * existing) should check raxFindLink's return first and skip this call
 * when it reports 1.
 *
 * On out of memory the function returns 0 and sets errno to ENOMEM;
 * otherwise errno is set to 0. */
int raxInsertAt(rax *rax, unsigned char *s, size_t len, void *data, void **old, raxNodeLink *link) {
    /* If rax has fixed-length keys, the input must match the length. */
    assert(!rax->keyFixedLen || len == rax->keyFixedLen);
    /* The link must reflect the current tree: no rax mutation is allowed
     * between the raxFindLink() that produced it and this commit. */
    debugAssert(raxLinkStillValid(rax, s, len, link));
    
    size_t usable;
    /* Pull walk state from `link`. */
    size_t i = link->consumed;
    int j = link->splitpos; /* Split position. If raxLowWalk() stopped in
                               a compressed node, 'j' is the char index
                               within the compressed node where we
                               stopped; i.e. the position where to split
                               the node for insertion. Only meaningful
                               when h->iscompr. */
    raxNode *h = link->stopnode, **parentlink = link->parentlink;
    size_t dummy, *alloc_size = &dummy;

    if (rax->alloc_size) alloc_size = rax->alloc_size;
    debugf("### Insert %.*s with value %p\n", (int)len, s, data);

    /* Fixed-length leaf-parent commit: raxLowWalk halted at h because
     * descending would land at a leaf slot. The walk only stops here
     * on a successful byte match, so the slot at link->leafSlot always
     * holds the existing value; overwrite it. New-key inlining is
     * handled below via the main insert loop (raxAddSlot / fused
     * raxCompressNodeWithValue). */
    if (link->splitpos == RAX_LEAF_PARENT_STOP) {
        if (old) *old = raxLeafParentReadSlot(h, link->leafSlot);
        raxLeafParentWriteSlot(h, link->leafSlot, data);
        errno = 0;
        return 0; /* Existing key, overwritten. */
    }

    /* If i == len we walked following the whole string. If we are not
     * in the middle of a compressed node, the string is either already
     * inserted or this middle node is currently not a key, but can represent
     * our key. We have just to reallocate the node and make space for the
     * data pointer. */
    if (i == len && (!h->iscompr || j == 0 /* not in the middle if j is 0 */)) {
        debugf("### Insert: node representing key exists\n");
        /* Make space for the value pointer if needed. */
        if (!h->iskey || h->isnull) {
            h = raxReallocForData(rax,h,data);
            if (h) memcpy(parentlink,&h,sizeof(h));
        }
        if (h == NULL) {
            errno = ENOMEM;
            return 0;
        }

        /* Update the existing key if there is already one. */
        if (h->iskey) {
            if (old) *old = raxGetData(h);
            raxSetData(h,data);
            errno = 0;
            return 0; /* Element already exists, overwritten. */
        }

        /* Otherwise set the node as a key. Note that raxSetData()
         * will set h->iskey. */
        raxSetData(h,data);
        rax->numele++;
        return 1; /* Element inserted. */
    }

    /* If the node we stopped at is a compressed node, we need to
     * split it before to continue.
     *
     * Splitting a compressed node have a few possible cases.
     * Imagine that the node 'h' we are currently at is a compressed
     * node containing the string "ANNIBALE" (it means that it represents
     * nodes A -> N -> N -> I -> B -> A -> L -> E with the only child
     * pointer of this node pointing at the 'E' node, because remember that
     * we have characters at the edges of the graph, not inside the nodes
     * themselves.
     *
     * In order to show a real case imagine our node to also point to
     * another compressed node, that finally points at the node without
     * children, representing 'O':
     *
     *     "ANNIBALE" -> "SCO" -> []
     *
     * When inserting we may face the following cases. Note that all the cases
     * require the insertion of a non compressed node with exactly two
     * children, except for the last case which just requires splitting a
     * compressed node.
     *
     * 1) Inserting "ANNIENTARE"
     *
     *               |B| -> "ALE" -> "SCO" -> []
     *     "ANNI" -> |-|
     *               |E| -> (... continue algo ...) "NTARE" -> []
     *
     * 2) Inserting "ANNIBALI"
     *
     *                  |E| -> "SCO" -> []
     *     "ANNIBAL" -> |-|
     *                  |I| -> (... continue algo ...) []
     *
     * 3) Inserting "AGO" (Like case 1, but set iscompr = 0 into original node)
     *
     *            |N| -> "NIBALE" -> "SCO" -> []
     *     |A| -> |-|
     *            |G| -> (... continue algo ...) |O| -> []
     *
     * 4) Inserting "CIAO"
     *
     *     |A| -> "NNIBALE" -> "SCO" -> []
     *     |-|
     *     |C| -> (... continue algo ...) "IAO" -> []
     *
     * 5) Inserting "ANNI"
     *
     *     "ANNI" -> "BALE" -> "SCO" -> []
     *
     * The final algorithm for insertion covering all the above cases is as
     * follows.
     *
     * ============================= ALGO 1 =============================
     *
     * For the above cases 1 to 4, that is, all cases where we stopped in
     * the middle of a compressed node for a character mismatch, do:
     *
     * Let $SPLITPOS be the zero-based index at which, in the
     * compressed node array of characters, we found the mismatching
     * character. For example if the node contains "ANNIBALE" and we add
     * "ANNIENTARE" the $SPLITPOS is 4, that is, the index at which the
     * mismatching character is found.
     *
     * 1. Save the current compressed node $NEXT pointer (the pointer to the
     *    child element, that is always present in compressed nodes).
     *
     * 2. Create "split node" having as child the non common letter
     *    at the compressed node. The other non common letter (at the key)
     *    will be added later as we continue the normal insertion algorithm
     *    at step "6".
     *
     * 3a. IF $SPLITPOS == 0:
     *     Replace the old node with the split node, by copying the auxiliary
     *     data if any. Fix parent's reference. Free old node eventually
     *     (we still need its data for the next steps of the algorithm).
     *
     * 3b. IF $SPLITPOS != 0:
     *     Trim the compressed node (reallocating it as well) in order to
     *     contain $splitpos characters. Change child pointer in order to link
     *     to the split node. If new compressed node len is just 1, set
     *     iscompr to 0 (layout is the same). Fix parent's reference.
     *
     * 4a. IF the postfix len (the length of the remaining string of the
     *     original compressed node after the split character) is non zero,
     *     create a "postfix node". If the postfix node has just one character
     *     set iscompr to 0, otherwise iscompr to 1. Set the postfix node
     *     child pointer to $NEXT.
     *
     * 4b. IF the postfix len is zero, just use $NEXT as postfix pointer.
     *
     * 5. Set child[0] of split node to postfix node.
     *
     * 6. Set the split node as the current node, set current index at child[1]
     *    and continue insertion algorithm as usually.
     *
     * ============================= ALGO 2 =============================
     *
     * For case 5, that is, if we stopped in the middle of a compressed
     * node but no mismatch was found, do:
     *
     * Let $SPLITPOS be the zero-based index at which, in the
     * compressed node array of characters, we stopped iterating because
     * there were no more keys character to match. So in the example of
     * the node "ANNIBALE", adding the string "ANNI", the $SPLITPOS is 4.
     *
     * 1. Save the current compressed node $NEXT pointer (the pointer to the
     *    child element, that is always present in compressed nodes).
     *
     * 2. Create a "postfix node" containing all the characters from $SPLITPOS
     *    to the end. Use $NEXT as the postfix node child pointer.
     *    If the postfix node length is 1, set iscompr to 0.
     *    Set the node as a key with the associated value of the new
     *    inserted key.
     *
     * 3. Trim the current node to contain the first $SPLITPOS characters.
     *    As usually if the new node length is just 1, set iscompr to 0.
     *    Take the iskey / associated value as it was in the original node.
     *    Fix the parent's reference.
     *
     * 4. Set the postfix node as the only child pointer of the trimmed
     *    node created at step 1.
     */

    /* ------------------------- ALGORITHM 1 --------------------------- */
    if (h->iscompr && i != len) {
        debugf("ALGO 1: Stopped at compressed node %.*s (%p)\n",
            h->size, h->data, (void*)h);
        debugf("Still to insert: %.*s\n", (int)(len-i), s+i);
        debugf("Splitting at %d: '%c'\n", j, ((char*)h->data)[j]);
        debugf("Other (key) letter is '%c'\n", s[i]);

        /* 1: Save next pointer. */
        raxNode **childfield = raxNodeLastChildPtr(h);
        raxNode *next;
        memcpy(&next,childfield,sizeof(next));
        debugf("Next is %p\n", (void*)next);
        debugf("iskey %d\n", h->iskey);
        if (h->iskey) {
            debugf("key value is %p\n", raxGetData(h));
        }

        /* Set the length of the additional nodes we will need. */
        size_t trimmedlen = j;
        size_t postfixlen = h->size - j - 1;
        int split_node_is_key = !trimmedlen && h->iskey && !h->isnull;
        size_t nodesize;

        /* 2: Create the split node. Also allocate the other nodes we'll need
         *    ASAP, so that it will be simpler to handle OOM. */
        raxNode *splitnode = raxNewNode(rax, 1, split_node_is_key);
        raxNode *trimmed = NULL;
        raxNode *postfix = NULL;

        if (trimmedlen) {
            nodesize = sizeof(raxNode)+trimmedlen+raxPadding(trimmedlen)+
                       sizeof(raxNode*);
            if (h->iskey && !h->isnull) nodesize += sizeof(void*);
            trimmed = rax_malloc_usable(nodesize, &usable);
            *alloc_size += usable;
        }

        if (postfixlen) {
            nodesize = sizeof(raxNode)+postfixlen+raxPadding(postfixlen)+
                       sizeof(raxNode*);
            postfix = rax_malloc_usable(nodesize, &usable);
            *alloc_size += usable;
        }

        /* OOM? Abort now that the tree is untouched. */
        if (splitnode == NULL ||
            (trimmedlen && trimmed == NULL) ||
            (postfixlen && postfix == NULL))
        {
            raxFreeNode(rax,splitnode);
            raxFreeNode(rax,trimmed);
            raxFreeNode(rax,postfix);
            errno = ENOMEM;
            return 0;
        }
        splitnode->data[0] = h->data[j];

        if (j == 0) {
            /* 3a: Replace the old node with the split node. */
            if (h->iskey) {
                void *ndata = raxGetData(h);
                raxSetData(splitnode,ndata);
            }
            memcpy(parentlink,&splitnode,sizeof(splitnode));
        } else {
            /* 3b: Trim the compressed node. */
            trimmed->size = j;
            memcpy(trimmed->data,h->data,j);
            trimmed->iscompr = j > 1 ? 1 : 0;
            trimmed->iskey = h->iskey;
            trimmed->isnull = h->isnull;
            if (h->iskey && !h->isnull) {
                void *ndata = raxGetData(h);
                raxSetData(trimmed,ndata);
            }
            raxNode **cp = raxNodeLastChildPtr(trimmed);
            memcpy(cp,&splitnode,sizeof(splitnode));
            memcpy(parentlink,&trimmed,sizeof(trimmed));
            parentlink = cp; /* Set parentlink to splitnode parent. */
            rax->numnodes++;
        }

        /* 4: Create the postfix node: what remains of the original
         * compressed node after the split. */
        if (postfixlen) {
            /* 4a: create a postfix node. */
            postfix->iskey = 0;
            postfix->isnull = 0;
            postfix->size = postfixlen;
            postfix->iscompr = postfixlen > 1;
            memcpy(postfix->data,h->data+j+1,postfixlen);
            raxNode **cp = raxNodeLastChildPtr(postfix);
            memcpy(cp,&next,sizeof(next));
            rax->numnodes++;
        } else {
            /* 4b: just use next as postfix node. */
            postfix = next;
        }

        /* 5: Set splitnode first child as the postfix node. */
        raxNode **splitchild = raxNodeLastChildPtr(splitnode);
        memcpy(splitchild,&postfix,sizeof(postfix));

        /* 6. Continue insertion: this will cause the splitnode to
         * get a new child (the non common character at the currently
         * inserted key). */
        raxFreeNode(rax,h);
        h = splitnode;
    } else if (h->iscompr && i == len) {
    /* ------------------------- ALGORITHM 2 --------------------------- */
        debugf("ALGO 2: Stopped at compressed node %.*s (%p) j = %d\n",
            h->size, h->data, (void*)h, j);

        /* Allocate postfix & trimmed nodes ASAP to fail for OOM gracefully. */
        size_t postfixlen = h->size - j;
        size_t nodesize = sizeof(raxNode)+postfixlen+raxPadding(postfixlen)+
                          sizeof(raxNode*);
        if (data != NULL) nodesize += sizeof(void*);
        raxNode *postfix = rax_malloc_usable(nodesize, &usable);
        *alloc_size += usable;

        nodesize = sizeof(raxNode)+j+raxPadding(j)+sizeof(raxNode*);
        if (h->iskey && !h->isnull) nodesize += sizeof(void*);
        raxNode *trimmed = rax_malloc_usable(nodesize, &usable);
        *alloc_size += usable;

        if (postfix == NULL || trimmed == NULL) {
            raxFreeNode(rax,postfix);
            raxFreeNode(rax,trimmed);
            errno = ENOMEM;
            return 0;
        }


        /* 1: Save next pointer. */
        raxNode **childfield = raxNodeLastChildPtr(h);
        raxNode *next;
        memcpy(&next,childfield,sizeof(next));

        /* 2: Create the postfix node. */
        postfix->size = postfixlen;
        postfix->iscompr = postfixlen > 1;
        postfix->iskey = 1;
        postfix->isnull = 0;
        memcpy(postfix->data,h->data+j,postfixlen);
        raxSetData(postfix,data);
        raxNode **cp = raxNodeLastChildPtr(postfix);
        memcpy(cp,&next,sizeof(next));
        rax->numnodes++;

        /* 3: Trim the compressed node. */
        trimmed->size = j;
        trimmed->iscompr = j > 1;
        trimmed->iskey = 0;
        trimmed->isnull = 0;
        memcpy(trimmed->data,h->data,j);
        memcpy(parentlink,&trimmed,sizeof(trimmed));
        if (h->iskey) {
            void *aux = raxGetData(h);
            raxSetData(trimmed,aux);
        }

        /* Fix the trimmed node child pointer to point to
         * the postfix node. */
        cp = raxNodeLastChildPtr(trimmed);
        memcpy(cp,&postfix,sizeof(postfix));

        /* Finish! We don't need to continue with the insertion
         * algorithm for ALGO 2. The key is already inserted. */
        rax->numele++;
        raxFreeNode(rax,h);
        return 1; /* Key inserted. */
    }

    /* We walked the radix tree as far as we could, but still there are left
     * chars in our string. We need to insert the missing nodes.
     *
     * On Fixed-length leaf-inlining, if the next step would reach depth
     * keyFixedLen, store the value in the parent's child slot instead */
    while(i < len) {
        raxNode *child;

        /* If this node is going to have a single child, and there
         * are other characters, so that that would result in a chain
         * of single-childed nodes, turn it into a compressed node. */
        if (h->size == 0 && len-i > 1) {
            debugf("Inserting compressed node\n");
            size_t comprsize = len-i;
            if (comprsize > RAX_NODE_MAX_SIZE)
                comprsize = RAX_NODE_MAX_SIZE;
            if (rax->keyFixedLen) {
                /* Fixed-length: this compressed prefix carries the whole remaining
                 * suffix (comprsize == len-i),  reaches the leaf depth. Fuse it with
                 * the value (no leaf raxNode). */                
                debugAssert(raxSlotsAreValues(rax, i + comprsize));
                raxNode *newh = raxCompressNodeWithValue(rax, h, s+i, comprsize, data);
                if (newh == NULL) goto oom;
                memcpy(parentlink, &newh, sizeof(h));
                rax->numele++;
                return 1;
            }
            raxNode *newh = raxCompressNode(rax,h,s+i,comprsize,&child);
            if (newh == NULL) goto oom;
            h = newh;
            memcpy(parentlink,&h,sizeof(h));
            parentlink = raxNodeLastChildPtr(h);
            i += comprsize;
        } else {
            /* Fixed-length: if this edge-byte step reaches the leaf depth,
             * stamp the edge byte and inline the value (no leaf raxNode). */
            if (raxSlotsAreValues(rax, i + 1)) {
                raxNode *newh = raxAddSlot(rax, h, s[i], data);
                if (newh == NULL) goto oom;
                memcpy(parentlink, &newh, sizeof(h));
                rax->numele++;
                return 1;
            }
            debugf("Inserting normal node\n");
            raxNode **new_parentlink;
            raxNode *newh = raxAddChild(rax,h,s[i],&child,&new_parentlink);
            if (newh == NULL) goto oom;
            h = newh;
            memcpy(parentlink,&h,sizeof(h));
            parentlink = new_parentlink;
            i++;
        }
        rax->numnodes++;
        h = child;
    }
    raxNode *newh = raxReallocForData(rax,h,data);
    if (newh == NULL) goto oom;
    h = newh;
    if (!h->iskey) rax->numele++;
    raxSetData(h,data);
    memcpy(parentlink,&h,sizeof(h));
    return 1; /* Element inserted. */

oom:
    /* This code path handles out of memory after part of the sub-tree was
     * already modified. Set the node as a key, and then remove it. However we
     * do that only if the node is a terminal node, otherwise if the OOM
     * happened reallocating a node in the middle, we don't need to free
     * anything. */
    if (h->size == 0) {
        h->isnull = 1;
        h->iskey = 1;
        rax->numele++; /* Compensate the next remove. */
        assert(raxRemove(rax,s,i,NULL) != 0);
    }
    errno = ENOMEM;
    return 0;
}

/* Walk the rax once and record the stop position in `link`. Returns 1 if
 * `s` is an existing key (and, if `value` is non-NULL, stores the value
 * at *value); 0 otherwise. The link is populated either way so a caller
 * that meant "find or insert" can commit via raxInsertAt() without a
 * second walk.
 *
 * Invalidation contract: `link->h` and `link->parentlink` are interior
 * pointers into the tree. They become stale on ANY intervening rax
 * mutation. Callers MUST commit (or discard) immediately after the
 * find; do not interleave other rax calls on the same tree, do not
 * retain across yield points. */
int raxFindLink(rax *rax, unsigned char *s, size_t len,
                void **value, raxNodeLink *link) {
    debugf("### FindLink: %.*s\n", (int)len, s);
    return raxLowWalk(rax, s, len, value, link, NULL);
}

/* Overwriting insert. One walk via raxFindLink, then commit at the
 * recorded position. Existing element is updated. */
int raxInsert(rax *rax, unsigned char *s, size_t len, void *data, void **old) {
    raxNodeLink link;
    raxFindLink(rax, s, len, NULL, &link);
    return raxInsertAt(rax,s,len,data,old,&link);
}

/* Non-overwriting insert. If an element with the same key exists, the
 * value is not updated and 0 is returned (with *old set, if non-NULL,
 * to the existing value). raxFindLink already tells us whether the key
 * exists, so we skip raxInsertAt's overwrite path entirely in that
 * case. */
int raxTryInsert(rax *rax, unsigned char *s, size_t len, void *data, void **old) {
    raxNodeLink link;
    void *existing;
    if (raxFindLink(rax, s, len, &existing, &link)) {
        if (old) *old = existing;
        errno = 0;
        return 0;
    }
    return raxInsertAt(rax,s,len,data,old,&link);
}

/* Find a key in the rax: return 1 if the item is found, 0 otherwise.
 * If there is an item and 'value' is passed in a non-NULL pointer, the
 * value associated with the item is set at that address. */
int raxFind(rax *rax, unsigned char *s, size_t len, void **value) {
    raxNodeLink link;
    return raxFindLink(rax, s, len, value, &link);
}

/* Return the memory address where the 'parent' node stores the specified
 * 'child' pointer, so that the caller can update the pointer with another
 * one if needed. The function assumes it will find a match, otherwise the
 * operation is an undefined behavior (it will continue scanning the
 * memory without any bound checking). */
raxNode **raxFindParentLink(raxNode *parent, raxNode *child) {
    raxNode **cp = raxNodeFirstChildPtr(parent);
    raxNode *c;
    while(1) {
        memcpy(&c,cp,sizeof(c));
        if (c == child) break;
        cp++;
    }
    return cp;
}

/* Low level child removal from node. The new node pointer (after the child
 * removal) is returned. Note that this function does not fix the pointer
 * of the parent node in its parent, so this task is up to the caller.
 * The function never fails for out of memory. */
raxNode *raxRemoveChild(rax *rax, raxNode *parent, raxNode *child) {
    debugnode("raxRemoveChild before", parent);
    /* If parent is a compressed node (having a single child, as for definition
     * of the data structure), the removal of the child consists into turning
     * it into a normal node without children. */
    if (parent->iscompr) {
        void *data = NULL;
        if (parent->iskey) data = raxGetData(parent);
        parent->isnull = 0;
        parent->iscompr = 0;
        parent->size = 0;
        if (parent->iskey) raxSetData(parent,data);
        debugnode("raxRemoveChild after", parent);
        return parent;
    }

    /* Otherwise we need to scan for the child pointer and memmove()
     * accordingly.
     *
     * 1. To start we seek the first element in both the children
     *    pointers and edge bytes in the node. */
    raxNode **cp = raxNodeFirstChildPtr(parent);
    raxNode **c = cp;
    unsigned char *e = parent->data;

    /* 2. Search the child pointer to remove inside the array of children
     *    pointers. */
    while(1) {
        raxNode *aux;
        memcpy(&aux,c,sizeof(aux));
        if (aux == child) break;
        c++;
        e++;
    }

    /* 3. Remove the edge and the pointer by memmoving the remaining children
     *    pointer and edge bytes one position before. */
    int taillen = parent->size - (e - parent->data) - 1;
    debugf("raxRemoveChild tail len: %d\n", taillen);
    memmove(e,e+1,taillen);

    /* Compute the shift, that is the amount of bytes we should move our
     * child pointers to the left, since the removal of one edge character
     * and the corresponding padding change, may change the layout.
     * We just check if in the old version of the node there was at the
     * end just a single byte and all padding: in that case removing one char
     * will remove a whole sizeof(void*) word. */
    size_t shift = ((parent->size+4) % sizeof(void*)) == 1 ? sizeof(void*) : 0;

    /* Move the children pointers before the deletion point. */
    if (shift)
        memmove(((char*)cp)-shift,cp,(parent->size-taillen-1)*sizeof(raxNode**));

    /* Move the remaining "tail" pointers at the right position as well. */
    size_t valuelen = (parent->iskey && !parent->isnull) ? sizeof(void*) : 0;
    memmove(((char*)c)-shift,c+1,taillen*sizeof(raxNode**)+valuelen);

    /* 4. Update size. */
    parent->size--;

    /* realloc the node according to the theoretical memory usage, to free
     * data if we are over-allocating right now. */
    raxNode *newnode = raxNodeRealloc(rax,parent,raxNodeCurrentLength(parent));
    if (newnode) {
        debugnode("raxRemoveChild after", newnode);
    }
    /* Note: if raxNodeRealloc() fails we just return the old address, which
     * is valid. */
    return newnode ? newnode : parent;
}

/* Remove the specified item. Returns 1 if the item was found and
 * deleted, 0 otherwise. */
int raxRemove(rax *rax, unsigned char *s, size_t len, void **old) {
    debugAssert(!rax->keyFixedLen || len == rax->keyFixedLen);
    raxNodeLink link;
    raxStack ts;

    debugf("### Delete: %.*s\n", (int)len, s);
    raxStackInit(&ts);
    if (!raxLowWalk(rax, s, len, old, &link, &ts)) {
        raxStackFree(&ts);
        return 0;
    }
    raxNode *h = link.stopnode;
    rax->numele--;

    /* Fixed-length leaf-parent slot removal. We stopped at h before entering the
     * leaf slot. link.leafSlot is the inlined value-slot index: 0 when
     * iscompr=1, otherwise the matched edge index. Drop it, then unlink h if it
     * becomes useless and continue the normal upward cleanup. */
    if (link.splitpos == RAX_LEAF_PARENT_STOP) {
        int slot_idx = link.leafSlot;

        /* Case A: iscompr=0 leaf parent with > 1 slots. Shrink h and done. */
        if (!h->iscompr && h->size > 1) {
            raxNode *newh = raxRemoveSlotAt(rax, h, slot_idx);
            if (newh != h) {
                raxNode *parent = raxStackPeek(&ts);
                raxNode **parentlink = parent == NULL
                    ? &rax->head : raxFindParentLink(parent, h);
                memcpy(parentlink, &newh, sizeof(newh));
            }
            raxStackFree(&ts);
            return 1;
        }

        /* Case B: removing this slot empties the leaf parent: always for iscompr=1,
         * or for iscompr=0 when size==1. Free it, then walk upward freeing
         * single-child non-key ancestors, as in the leaf-raxNode cleanup path below. */
        h->isnull = 0;
        h->iscompr = 0;
        h->size = 0;
    }  /* fall-through to standard cleanup */
    
    /* Normal leaf raxNode removal. */
    h->iskey = 0;

    /* If this node has no children, the deletion needs to reclaim the
     * no longer used nodes. This is an iterative process that needs to
     * walk the three upward, deleting all the nodes with just one child
     * that are not keys, until the head of the rax is reached or the first
     * node with more than one child is found. */

    int trycompress = 0; /* Will be set to 1 if we should try to optimize the
                            tree resulting from the deletion. */

    if (h->size == 0) {
        debugf("Key deleted in node without children. Cleanup needed.\n");
        raxNode *child = NULL;
        while(h != rax->head) {
            child = h;
            debugf("Freeing child %p [%.*s] key:%d\n", (void*)child,
                (int)child->size, (char*)child->data, child->iskey);
            raxFreeNode(rax,child);
            rax->numnodes--;
            h = raxStackPop(&ts);
             /* If this node has more than one child, or actually holds
              * a key, stop here. */
            if (h->iskey || (!h->iscompr && h->size != 1)) break;
        }
        if (child) {
            debugf("Unlinking child %p from parent %p\n",
                (void*)child, (void*)h);
            raxNode *new = raxRemoveChild(rax,h,child);
            if (new != h) {
                raxNode *parent = raxStackPeek(&ts);
                raxNode **parentlink;
                if (parent == NULL) {
                    parentlink = &rax->head;
                } else {
                    parentlink = raxFindParentLink(parent,h);
                }
                memcpy(parentlink,&new,sizeof(new));
            }

            /* If after the removal the node has just a single child
             * and is not a key, we need to try to compress it. */
            if (new->size == 1 && new->iskey == 0) {
                trycompress = 1;
                h = new;
            }
        }
    } else if (h->size == 1) {
        /* If the node had just one child, after the removal of the key
         * further compression with adjacent nodes is potentially possible. */
        trycompress = 1;
    }

    /* Don't try node compression if our nodes pointers stack is not
     * complete because of OOM while executing raxLowWalk() */
    if (trycompress && ts.oom) trycompress = 0;

    /* Recompression: if trycompress is true, 'h' points to a radix tree node
     * that changed in a way that could allow to compress nodes in this
     * sub-branch. Compressed nodes represent chains of nodes that are not
     * keys and have a single child, so there are two deletion events that
     * may alter the tree so that further compression is needed:
     *
     * 1) A node with a single child was a key and now no longer is a key.
     * 2) A node with two children now has just one child.
     *
     * We try to navigate upward till there are other nodes that can be
     * compressed, when we reach the upper node which is not a key and has
     * a single child, we scan the chain of children to collect the
     * compressible part of the tree, and replace the current node with the
     * new one, fixing the child pointer to reference the first non
     * compressible node.
     *
     * Example of case "1". A tree stores the keys "FOO" = 1 and
     * "FOOBAR" = 2:
     *
     *
     * "FOO" -> "BAR" -> [] (2)
     *           (1)
     *
     * After the removal of "FOO" the tree can be compressed as:
     *
     * "FOOBAR" -> [] (2)
     *
     *
     * Example of case "2". A tree stores the keys "FOOBAR" = 1 and
     * "FOOTER" = 2:
     *
     *          |B| -> "AR" -> [] (1)
     * "FOO" -> |-|
     *          |T| -> "ER" -> [] (2)
     *
     * After the removal of "FOOTER" the resulting tree is:
     *
     * "FOO" -> |B| -> "AR" -> [] (1)
     *
     * That can be compressed into:
     *
     * "FOOBAR" -> [] (1)
     */
    if (trycompress) {
        debugf("After removing %.*s:\n", (int)len, s);
        debugnode("Compression may be needed",h);
        debugf("Seek start node\n");

        /* Try to reach the upper node that is compressible.
         * At the end of the loop 'h' will point to the first node we
         * can try to compress and 'parent' to its parent. */
        raxNode *parent;
        while(1) {
            parent = raxStackPop(&ts);
            if (!parent || parent->iskey ||
                (!parent->iscompr && parent->size != 1)) break;
            h = parent;
            debugnode("Going up to",h);
        }
        raxNode *start = h; /* Compression starting node. */

        /* Fixed-length: compute start's depth, based on stack, so the scans below 
         * can stop at a leaf parent. its depth is the sum of their edge lengths */
        int dstart = 0; /* depth of the compression start node */
        if (rax->keyFixedLen) {
            for (size_t k = 0; k < ts.items; k++)
                dstart += raxStepLenNode(ts.stack[k]);
            /* If parent exists, add its edge len too (Not part of the stack) */
            if (parent) dstart += raxStepLenNode(parent);
        }

        /* Scan chain of nodes we can compress. */
        size_t comprsize = h->size;
        int nodes = 1;
        int dh = dstart;            /* depth of the current node `h` */
        while(h->size != 0) {
            raxNode **cp = raxNodeLastChildPtr(h);
            dh += raxStepLenNode(h);   /* depth of the child */
            memcpy(&h,cp,sizeof(h));
            /* Stop at a key, a multi-child node, or (fixed-length) a leaf parent */
            if (h->iskey || (!h->iscompr && h->size != 1) ||
                raxSlotsAreValues(rax, dh + raxStepLenNode(h)))
                break;
            /* Stop here if going to the next node would result into
             * a compressed node larger than h->size can hold. */
            if (comprsize + h->size > RAX_NODE_MAX_SIZE) break;
            nodes++;
            comprsize += h->size;
        }
        if (nodes > 1) {
            /* If we can compress, create the new node and populate it. */
            size_t nodesize =
                sizeof(raxNode)+comprsize+raxPadding(comprsize)+sizeof(raxNode*);
            size_t usable;
            raxNode *new = rax_malloc_usable(nodesize, &usable);
            /* An out of memory here just means we cannot optimize this
             * node, but the tree is left in a consistent state. */
            if (new == NULL) {
                raxStackFree(&ts);
                return 1;
            }
            if (rax->alloc_size) *rax->alloc_size += usable;
            new->iskey = 0;
            new->isnull = 0;
            new->iscompr = 1;
            new->size = comprsize;
            rax->numnodes++;

            /* Scan again, this time to populate the new node content and
             * to fix the new node child pointer. At the same time we free
             * all the nodes that we'll no longer use. */
            comprsize = 0;
            h = start;
            dh = dstart;
            while(h->size != 0) {
                memcpy(new->data+comprsize,h->data,h->size);
                comprsize += h->size;
                raxNode **cp = raxNodeLastChildPtr(h);
                dh += raxStepLenNode(h);
                raxNode *tofree = h;
                memcpy(&h,cp,sizeof(h));
                raxFreeNode(rax,tofree);
                rax->numnodes--;
                /* Same stop as the measuring scan above: terminate at a key,
                 * a multi-child node, or a fixed-length leaf parent. */
                if (h->iskey || (!h->iscompr && h->size != 1) ||
                    raxSlotsAreValues(rax, dh + raxStepLenNode(h)))
                    break;
            }
            debugnode("New node",new);

            /* Now 'h' points to the first node that we still need to use,
             * so our new node child pointer will point to it. */
            raxNode **cp = raxNodeLastChildPtr(new);
            memcpy(cp,&h,sizeof(h));

            /* Fix parent link. */
            if (parent) {
                raxNode **parentlink = raxFindParentLink(parent,start);
                memcpy(parentlink,&new,sizeof(new));
            } else {
                rax->head = new;
            }

            debugf("Compressed %d nodes, %d total bytes\n",
                nodes, (int)comprsize);
        }
    }
    raxStackFree(&ts);
    return 1;
}

/* Invoke the appropriate user callback on an inlined value pointer or
 * a "raw" leaf data pointer. Used by both the depth-tracking and the
 * variable-length free walkers. */
static inline void raxFreeInvokeValueCb(void *data,
                                        void (*free_cb)(void *item),
                                        void (*free_cb_withctx)(void *item, void *ctx),
                                        void *ctx) {
    if (data == NULL) return;
    if (free_cb_withctx) free_cb_withctx(data, ctx);
    else if (free_cb) free_cb(data);
}

/* This is the core of raxFree(): performs an iterative depth-first scan
 * of the tree and frees all the nodes found. Uses an explicit heap stack
 * to avoid stack overflow on deep trees. The caller passes exactly one
 * callback variant and the non-NULL one is invoked. */
static void raxFreeNodesWithCallback(rax *rax, raxNode *n,
                                     void (*free_cb)(void *item),
                                     void (*free_cb_withctx)(void *item, void *ctx),
                                     void *ctx)
{
    raxStack stack, depths;
    raxStackInit(&stack);
    raxStackInit(&depths);
    raxStackPush(&stack, n);
    raxStackPush(&depths, (void*)(uintptr_t)0);

    while (stack.items > 0) {
        raxNode *curr = raxStackPop(&stack);
        size_t depth = (size_t)(uintptr_t)raxStackPop(&depths);
        debugnode("free traversing", curr);

        int numchildren = curr->iscompr ? 1 : (int)curr->size;
        raxNode **cp = raxNodeFirstChildPtr(curr);

        size_t child_depth = depth + raxStepLenNode(curr);

        for (int i = 0; i < numchildren; i++) {
            void *slot;
            memcpy(&slot, cp + i, sizeof(slot));
            /* If fixed-length leaf inlining, slots hold values not raxNode*. */
            if (raxSlotsAreValues(rax, child_depth)) {
                raxFreeInvokeValueCb(slot, free_cb, free_cb_withctx, ctx);
            } else {
                raxStackPush(&stack, slot);
                raxStackPush(&depths, (void*)(uintptr_t)child_depth);
            }
        }

        debugnode("free depth-first", curr);
        if (curr->iskey && !curr->isnull)
            raxFreeInvokeValueCb(raxGetData(curr), free_cb, free_cb_withctx, ctx);
        raxFreeNode(rax, curr);
        rax->numnodes--;
    }

    raxStackFree(&depths);
    raxStackFree(&stack);
}

/* Free a whole radix tree, calling the specified callback in order to
 * free the auxiliary data. */
void raxFreeWithCallback(rax *rax, void (*free_callback)(void*)) {
    raxFreeNodesWithCallback(rax, rax->head, free_callback, NULL, NULL);
    assert(rax->numnodes == 0);
    size_t *alloc_size = rax->alloc_size;
    size_t usable;
    rax_free_usable(rax, &usable);
    if (alloc_size) *alloc_size -= usable;
}

/* Free a whole radix tree, calling the specified callback in order to
 * free the auxiliary data. */
void raxFreeWithCbAndContext(rax *rax,
                             void (*free_callback)(void *item, void *ctx), void *ctx) {
    raxFreeNodesWithCallback(rax, rax->head, NULL, free_callback, ctx);
    assert(rax->numnodes == 0);
    size_t *alloc_size = rax->alloc_size;
    size_t usable;
    rax_free_usable(rax, &usable);
    if (alloc_size) *alloc_size -= usable;
}

/* Free a whole radix tree. */
void raxFree(rax *rax) {
    raxFreeWithCallback(rax,NULL);
}

/* ------------------------------- Iterator --------------------------------- */

/* Initialize a Rax iterator. This call should be performed a single time
 * to initialize the iterator, and must be followed by a raxSeek() call,
 * otherwise the raxPrev()/raxNext() functions will just return EOF. */
void raxStart(raxIterator *it, rax *rt) {
    it->flags = RAX_ITER_EOF; /* No crash if the iterator is not seeked. */
    it->rt = rt;
    it->key_len = 0;
    it->key = it->key_static_string;
    it->key_max = RAX_ITER_STATIC_LEN;
    it->data = NULL;
    it->node_cb = NULL;
    it->privdata = NULL;
    it->leaf_slot_idx = -1;
    raxStackInit(&it->stack);
}

/* Update the value associated with the iterator's current position.
 * Lives at either the leaf parent's slot (fixed-length leaf-inlined) or
 * the node's value tail (variable-length / non-leaf-parent). Used by
 * defrag-style callers that have to overwrite the slot value in place
 * after the iterator stopped on a key. */
void raxIteratorSetData(raxIterator *it, void *data) {
    if (it->leaf_slot_idx >= 0) {
        debugAssert(it->rt->keyFixedLen);
        debugAssert(it->node != NULL);
        debugAssert(it->leaf_slot_idx >= 0 && it->leaf_slot_idx < (int)it->node->size);
        raxLeafParentWriteSlot(it->node, it->leaf_slot_idx, data);
        it->data = data;
        return;
    }
    raxSetData(it->node, data);
    it->data = data;
}

/* Append characters at the current key string of the iterator 'it'. This
 * is a low level function used to implement the iterator, not callable by
 * the user. Returns 0 on out of memory, otherwise 1 is returned. */
int raxIteratorAddChars(raxIterator *it, unsigned char *s, size_t len) {
    if (len == 0) return 1;
    if (it->key_max < it->key_len+len) {
        unsigned char *old = (it->key == it->key_static_string) ? NULL :
                                                                  it->key;
        size_t new_max = (it->key_len+len)*2;
        it->key = rax_realloc(old,new_max);
        if (it->key == NULL) {
            it->key = (!old) ? it->key_static_string : old;
            errno = ENOMEM;
            return 0;
        }
        if (old == NULL) memcpy(it->key,it->key_static_string,it->key_len);
        it->key_max = new_max;
    }
    /* Use memmove since there could be an overlap between 's' and
     * it->key when we use the current key in order to re-seek. */
    memmove(it->key+it->key_len,s,len);
    it->key_len += len;
    return 1;
}

/* Remove the specified number of chars from the right of the current
 * iterator key. */
void raxIteratorDelChars(raxIterator *it, size_t count) {
    it->key_len -= count;
}

/* Do an iteration step towards the next element. At the end of the step the
 * iterator key will represent the (new) current key. If it is not possible
 * to step in the specified direction since there are no longer elements, the
 * iterator is flagged with RAX_ITER_EOF.
 *
 * If 'noup' is true the function starts directly scanning for the next
 * lexicographically smaller children, and the current node is already assumed
 * to be the parent of the last key node, so the first operation to go back to
 * the parent will be skipped. This option is used by raxSeek() when
 * implementing seeking a non existing element with the ">" or "<" options:
 * the starting node is not a key in that particular case, so we start the scan
 * from a node that does not represent the key set.
 *
 * The function returns 1 on success or 0 on out of memory. */
int raxIteratorNextStep(raxIterator *it, int noup) {
    if (it->flags & RAX_ITER_EOF) {
        return 1;
    } else if (it->flags & RAX_ITER_JUST_SEEKED) {
        it->flags &= ~RAX_ITER_JUST_SEEKED;
        return 1;
    }

    /* Save key len, stack items and the node where we are currently
     * so that on iterator EOF we can restore the current key and state. */
    size_t orig_key_len = it->key_len;
    size_t orig_stack_items = it->stack.items;
    raxNode *orig_node = it->node;
    int orig_leaf_slot_idx = it->leaf_slot_idx;

    /* Fixed-length leaf inlining: if we are currently parked on a virtual
     * leaf (leaf_slot_idx >= 0), advance to the next sibling slot within
     * the same leaf parent, or fall out of the virtual-leaf state and
     * continue with the standard go-up logic below. */
    if (it->leaf_slot_idx >= 0) {
        if (!it->node->iscompr) {
            int next_idx = it->leaf_slot_idx + 1;
            if (next_idx < (int)it->node->size) {
                it->key[it->key_len - 1] = it->node->data[next_idx];
                it->leaf_slot_idx = next_idx;
                it->data = raxLeafParentReadSlot(it->node, next_idx);
                return 1;
            }
        }
        /* No more slots. Exit virtual-leaf state. Standard go-up code
         * below uses them as `prevchild` and shifts the key correctly. */
        it->leaf_slot_idx = -1;
        noup = 1;
    }

    while(1) {
        int children = it->node->iscompr ? 1 : it->node->size;
        if (!noup && children) {
            debugf("GO DEEPER\n");
            /* Fixed-length leaf inlining: if the next descent would land
             * at the leaf depth, enter the leaf-parent's first slot as a
             * virtual leaf instead of dereferencing the slot as raxNode*. */
            size_t edge_len = raxStepLenNode(it->node);
            size_t child_depth = it->key_len + edge_len;
            if (raxSlotsAreValues(it->rt, child_depth)) {
                /* Append the edge byte(s) for slot 0 (the lex-smallest) */
                if (!raxIteratorAddChars(it, it->node->data, edge_len)) return 0;
                it->leaf_slot_idx = 0;
                it->data = raxLeafParentReadSlot(it->node, 0);
                return 1;
            }

            /* Seek the lexicographically smaller key in this subtree, which
             * is the first one found always going towards the first child
             * of every successive node. */
            if (!raxStackPush(&it->stack,it->node)) return 0;
            raxNode **cp = raxNodeFirstChildPtr(it->node);
            if (!raxIteratorAddChars(it,it->node->data,
                it->node->iscompr ? it->node->size : 1)) return 0;
            memcpy(&it->node,cp,sizeof(it->node));
            /* Call the node callback if any, and replace the node pointer
             * if the callback returns true. */
            if (it->node_cb && it->node_cb(&it->node, it->privdata))
                memcpy(cp,&it->node,sizeof(it->node));
            /* For "next" step, stop every time we find a key along the
             * way, since the key is lexicographically smaller compared to
             * what follows in the sub-children. */
            if (it->node->iskey) {
                it->data = raxGetData(it->node);
                return 1;
            }
        } else {
            /* If we finished exploring the previous sub-tree, switch to the
             * new one: go upper until a node is found where there are
             * children representing keys lexicographically greater than the
             * current key. */
            while(1) {
                int old_noup = noup;

                /* Already on head? Can't go up, iteration finished. */
                if (!noup && it->node == it->rt->head) {
                    it->flags |= RAX_ITER_EOF;
                    it->stack.items = orig_stack_items;
                    it->key_len = orig_key_len;
                    it->node = orig_node;
                    it->leaf_slot_idx = orig_leaf_slot_idx;
                    return 1;
                }
                /* If there are no children at the current node, try parent's
                 * next child. */
                unsigned char prevchild = it->key[it->key_len-1];
                if (!noup) {
                    it->node = raxStackPop(&it->stack);
                } else {
                    noup = 0;
                }
                /* Adjust the current key to represent the node we are
                 * at. */
                int todel = it->node->iscompr ? it->node->size : 1;
                raxIteratorDelChars(it,todel);

                /* Try visiting the next child if there was at least one
                 * additional child. */
                if (!it->node->iscompr && it->node->size > (old_noup ? 0 : 1)) {
                    raxNode **cp = raxNodeFirstChildPtr(it->node);
                    int i = 0;
                    while (i < it->node->size) {
                        debugf("SCAN NEXT %c\n", it->node->data[i]);
                        if (it->node->data[i] > prevchild) break;
                        i++;
                        cp++;
                    }
                    if (i != it->node->size) {
                        debugf("SCAN found a new node\n");
                        /* Fixed-length leaf inlining: at leaf depth slot `i`
                         * holds an inlined value, not a raxNode pointer.
                         * Park on the virtual leaf directly. */
                        if (raxSlotsAreValues(it->rt, it->key_len + 1)) {
                            if (!raxIteratorAddChars(it, it->node->data+i, 1))
                                return 0;
                            it->leaf_slot_idx = i;
                            it->data = raxLeafParentReadSlot(it->node, i);
                            return 1;
                        }
                        raxIteratorAddChars(it,it->node->data+i,1);
                        if (!raxStackPush(&it->stack,it->node)) return 0;
                        memcpy(&it->node,cp,sizeof(it->node));
                        /* Call the node callback if any, and replace the node
                         * pointer if the callback returns true. */
                        if (it->node_cb && it->node_cb(&it->node, it->privdata))
                            memcpy(cp,&it->node,sizeof(it->node));
                        if (it->node->iskey) {
                            it->data = raxGetData(it->node);
                            return 1;
                        }
                        break;
                    }
                }
            }
        }
    }
}

/* Seek the greatest key in the subtree at the current node. Return 0 on
 * out of memory, otherwise 1. On success the iterator is positioned at
 * the greatest key with `it->data` populated; for fixed-length trees the
 * position may be a virtual leaf (it->leaf_slot_idx >= 0). */
int raxSeekGreatest(raxIterator *it) {
    while(it->node->size) {
        /* Fixed-length leaf inlining: if the next descent lands at the
         * leaf depth, park on the last virtual leaf instead. */
        size_t edge_len = raxStepLenNode(it->node);
        size_t child_depth = it->key_len + edge_len;
        if (raxSlotsAreValues(it->rt, child_depth)) {
            int slot_idx = it->node->iscompr ? 0 : (int)it->node->size - 1;
            if (!raxIteratorAddChars(it, it->node->data + slot_idx, edge_len))
                return 0;
            it->leaf_slot_idx = slot_idx;
            it->data = raxLeafParentReadSlot(it->node, slot_idx);
            return 1;
        }
        if (it->node->iscompr) {
            if (!raxIteratorAddChars(it,it->node->data,
                it->node->size)) return 0;
        } else {
            if (!raxIteratorAddChars(it,it->node->data+it->node->size-1,1))
                return 0;
        }
        raxNode **cp = raxNodeLastChildPtr(it->node);
        if (!raxStackPush(&it->stack,it->node)) return 0;
        memcpy(&it->node,cp,sizeof(it->node));
    }
    /* Variable-length descent terminated at a leaf raxNode, which by
     * rax invariants must be a key. */
    assert(it->node->iskey);
    it->data = raxGetData(it->node);
    return 1;
}

/* Like raxIteratorNextStep() but implements an iteration step moving
 * to the lexicographically previous element. The 'noup' option has a similar
 * effect to the one of raxIteratorNextStep(). */
int raxIteratorPrevStep(raxIterator *it, int noup) {
    if (it->flags & RAX_ITER_EOF) {
        return 1;
    } else if (it->flags & RAX_ITER_JUST_SEEKED) {
        it->flags &= ~RAX_ITER_JUST_SEEKED;
        return 1;
    }

    /* Save key len, stack items and the node where we are currently
     * so that on iterator EOF we can restore the current key and state. */
    size_t orig_key_len = it->key_len;
    size_t orig_stack_items = it->stack.items;
    raxNode *orig_node = it->node;
    int orig_leaf_slot_idx = it->leaf_slot_idx;

    /* Fixed-length leaf inlining: leaving a virtual leaf -- clear the
     * inlined-slot state and force noup=1 so the loop below scans the
     * leaf parent's own slots for the previous one before going up. */
    if (it->leaf_slot_idx >= 0) {
        it->leaf_slot_idx = -1;
        noup = 1;
    }

    while(1) {
        int old_noup = noup;

        /* Already on head? Can't go up, iteration finished. */
        if (!noup && it->node == it->rt->head) {
            it->flags |= RAX_ITER_EOF;
            it->stack.items = orig_stack_items;
            it->key_len = orig_key_len;
            it->node = orig_node;
            it->leaf_slot_idx = orig_leaf_slot_idx;
            return 1;
        }

        unsigned char prevchild = it->key[it->key_len-1];
        if (!noup) {
            it->node = raxStackPop(&it->stack);
        } else {
            noup = 0;
        }

        /* Adjust the current key to represent the node we are
         * at. */
        int todel = it->node->iscompr ? it->node->size : 1;
        raxIteratorDelChars(it,todel);

        /* Try visiting the prev child if there is at least one
         * child. */
        if (!it->node->iscompr && it->node->size > (old_noup ? 0 : 1)) {
            raxNode **cp = raxNodeLastChildPtr(it->node);
            int i = it->node->size-1;
            while (i >= 0) {
                debugf("SCAN PREV %c\n", it->node->data[i]);
                if (it->node->data[i] < prevchild) break;
                i--;
                cp--;
            }
            /* If we found a new subtree to explore in this node,
             * go deeper following all the last children in order to
             * find the key lexicographically greater. */
            if (i != -1) {
                debugf("SCAN found a new node\n");
                /* Fixed-length leaf inlining: at leaf depth slot `i`
                 * holds an inlined value, not a raxNode pointer.
                 * Park on the virtual leaf directly. */
                if (raxSlotsAreValues(it->rt, it->key_len + 1)) {
                    if (!raxIteratorAddChars(it, it->node->data+i, 1)) return 0;
                    it->leaf_slot_idx = i;
                    it->data = raxLeafParentReadSlot(it->node, i);
                    return 1;
                }
                /* Enter the node we just found. */
                if (!raxIteratorAddChars(it,it->node->data+i,1)) return 0;
                if (!raxStackPush(&it->stack,it->node)) return 0;
                memcpy(&it->node,cp,sizeof(it->node));
                /* Seek sub-tree max (raxSeekGreatest populates it->data) */
                if (!raxSeekGreatest(it)) return 0;
                return 1;
            }
        }

        /* Return the key: this could be the key we found scanning a new
         * subtree, or if we did not find a new subtree to explore here,
         * before giving up with this node, check if it's a key itself. */
        if (it->node->iskey) {
            it->data = raxGetData(it->node);
            return 1;
        }
    }
}

/* Seek an iterator at the specified element.
 * Return 0 if the seek failed for syntax error or out of memory. Otherwise
 * 1 is returned. When 0 is returned for out of memory, errno is set to
 * the ENOMEM value. */
int raxSeek(raxIterator *it, const char *op, unsigned char *ele, size_t len) {
    int eq = 0, lt = 0, gt = 0, first = 0, last = 0;

    it->stack.items = 0; /* Just resetting. Initialized by raxStart(). */
    it->flags |= RAX_ITER_JUST_SEEKED;
    it->flags &= ~RAX_ITER_EOF;
    it->key_len = 0;
    it->node = NULL;
    it->leaf_slot_idx = -1;

    /* Set flags according to the operator used to perform the seek. */
    if (op[0] == '>') {
        gt = 1;
        if (op[1] == '=') eq = 1;
    } else if (op[0] == '<') {
        lt = 1;
        if (op[1] == '=') eq = 1;
    } else if (op[0] == '=') {
        eq = 1;
    } else if (op[0] == '^') {
        first = 1;
    } else if (op[0] == '$') {
        last = 1;
    } else {
        errno = 0;
        return 0; /* Error. */
    }

    /* If there are no elements, set the EOF condition immediately and
     * return. */
    if (it->rt->numele == 0) {
        it->flags |= RAX_ITER_EOF;
        return 1;
    }

    if (first) {
        /* Seeking the first key greater or equal to the empty string
         * is equivalent to seeking the smaller key available. */
        return raxSeek(it,">=",NULL,0);
    }

    if (last) {
        /* Find the greatest key taking always the last child till a
         * final node is found. */
        it->node = it->rt->head;
        if (!raxSeekGreatest(it)) return 0;
        return 1;
    }

    /* We need to seek the specified key. What we do here is to actually
     * perform a lookup, and later invoke the prev/next key code that
     * we already use for iteration. */
    raxNodeLink link;
    raxLowWalk(it->rt,ele,len,NULL,&link,&it->stack);
    it->node = link.stopnode;
    size_t i = link.consumed;
    int splitpos = link.splitpos;
    int leafslot = link.leafSlot;

    /* Return OOM on incomplete stack info. */
    if (it->stack.oom) return 0;

    /* Fixed-length leaf-parent stop: the walk consumed all `len` bytes and
     * parked at the leaf parent without descending into the leaf slot. This
     * only happens on an exact match, so `leafslot` is always a valid slot
     * (0 for iscompr=1, the matched edge index for iscompr=0). A leaf-parent
     * *miss* instead breaks the walk with splitpos==0 and falls through to
     * the common path below, exactly like a normal-node mismatch.
     *
     * Position the iterator on the virtual leaf; for non-eq seeks, step
     * prev/next to advance off it. */
    if (splitpos == RAX_LEAF_PARENT_STOP) {
        if (!raxIteratorAddChars(it, ele, len)) return 0;
        it->leaf_slot_idx = leafslot;
        it->data = raxLeafParentReadSlot(it->node, leafslot);
        if (eq) return 1;
        /* gt/lt without eq: step past the match. */
        it->flags &= ~RAX_ITER_JUST_SEEKED;
        if (gt && !raxIteratorNextStep(it, 0)) return 0;
        if (lt && !raxIteratorPrevStep(it, 0)) return 0;
        it->flags |= RAX_ITER_JUST_SEEKED;
        return 1;
    }

    if (eq && i == len && (!it->node->iscompr || splitpos == 0) &&
        it->node->iskey)
    {
        /* We found our node, since the key matches and we have an
         * "equal" condition. */
        if (!raxIteratorAddChars(it,ele,len)) return 0; /* OOM. */
        it->data = raxGetData(it->node);
    } else if (lt || gt) {
        /* Exact key not found or eq flag not set. We have to set as current
         * key the one represented by the node we stopped at, and perform
         * a next/prev operation to seek. */
        raxIteratorAddChars(it, ele, i-splitpos);

        /* We need to set the iterator in the correct state to call next/prev
         * step in order to seek the desired element. */
        debugf("After initial seek: i=%d len=%d key=%.*s\n",
            (int)i, (int)len, (int)it->key_len, it->key);
        if (i != len && !it->node->iscompr) {
            /* If we stopped in the middle of a normal node because of a
             * mismatch, add the mismatching character to the current key
             * and call the iterator with the 'noup' flag so that it will try
             * to seek the next/prev child in the current node directly based
             * on the mismatching character. */
            if (!raxIteratorAddChars(it,ele+i,1)) return 0;
            debugf("Seek normal node on mismatch: %.*s\n",
                (int)it->key_len, (char*)it->key);

            it->flags &= ~RAX_ITER_JUST_SEEKED;
            if (lt && !raxIteratorPrevStep(it,1)) return 0;
            if (gt && !raxIteratorNextStep(it,1)) return 0;
            it->flags |= RAX_ITER_JUST_SEEKED; /* Ignore next call. */
        } else if (i != len && it->node->iscompr) {
            debugf("Compressed mismatch: %.*s\n",
                (int)it->key_len, (char*)it->key);
            /* In case of a mismatch within a compressed node. */
            int nodechar = it->node->data[splitpos];
            int keychar = ele[i];
            it->flags &= ~RAX_ITER_JUST_SEEKED;
            if (gt) {
                /* If the key the compressed node represents is greater
                 * than our seek element, continue forward, otherwise set the
                 * state in order to go back to the next sub-tree. */
                if (nodechar > keychar) {
                    if (!raxIteratorNextStep(it,0)) return 0;
                } else {
                    if (!raxIteratorAddChars(it,it->node->data,it->node->size))
                        return 0;
                    if (!raxIteratorNextStep(it,1)) return 0;
                }
            }
            if (lt) {
                /* If the key the compressed node represents is smaller
                 * than our seek element, seek the greater key in this
                 * subtree, otherwise set the state in order to go back to
                 * the previous sub-tree. */
                if (nodechar < keychar) {
                    if (!raxSeekGreatest(it)) return 0;
                } else {
                    if (!raxIteratorAddChars(it,it->node->data,it->node->size))
                        return 0;
                    if (!raxIteratorPrevStep(it,1)) return 0;
                }
            }
            it->flags |= RAX_ITER_JUST_SEEKED; /* Ignore next call. */
        } else {
            debugf("No mismatch: %.*s\n",
                (int)it->key_len, (char*)it->key);
            /* If there was no mismatch we are into a node representing the
             * key, (but which is not a key or the seek operator does not
             * include 'eq'), or we stopped in the middle of a compressed node
             * after processing all the key. Continue iterating as this was
             * a legitimate key we stopped at. */
            it->flags &= ~RAX_ITER_JUST_SEEKED;
            if (it->node->iscompr && it->node->iskey && splitpos && lt) {
                /* If we stopped in the middle of a compressed node with
                 * perfect match, and the condition is to seek a key "<" than
                 * the specified one, then if this node is a key it already
                 * represents our match. For instance we may have nodes:
                 *
                 * "f" -> "oobar" = 1 -> "" = 2
                 *
                 * Representing keys "f" = 1, "foobar" = 2. A seek for
                 * the key < "foo" will stop in the middle of the "oobar"
                 * node, but will be our match, representing the key "f".
                 *
                 * So in that case, we don't seek backward. */
                it->data = raxGetData(it->node);
            } else {
                if (gt && !raxIteratorNextStep(it,0)) return 0;
                if (lt && !raxIteratorPrevStep(it,0)) return 0;
            }
            it->flags |= RAX_ITER_JUST_SEEKED; /* Ignore next call. */
        }
    } else {
        /* If we are here just eq was set but no match was found. */
        it->flags |= RAX_ITER_EOF;
        return 1;
    }
    return 1;
}

/* Go to the next element in the scope of the iterator 'it'.
 * If EOF (or out of memory) is reached, 0 is returned, otherwise 1 is
 * returned. In case 0 is returned because of OOM, errno is set to ENOMEM. */
int raxNext(raxIterator *it) {
    if (!raxIteratorNextStep(it,0)) {
        errno = ENOMEM;
        return 0;
    }
    if (it->flags & RAX_ITER_EOF) {
        errno = 0;
        return 0;
    }
    return 1;
}

/* Go to the previous element in the scope of the iterator 'it'.
 * If EOF (or out of memory) is reached, 0 is returned, otherwise 1 is
 * returned. In case 0 is returned because of OOM, errno is set to ENOMEM. */
int raxPrev(raxIterator *it) {
    if (!raxIteratorPrevStep(it,0)) {
        errno = ENOMEM;
        return 0;
    }
    if (it->flags & RAX_ITER_EOF) {
        errno = 0;
        return 0;
    }
    return 1;
}

/* Perform a random walk starting in the current position of the iterator.
 * Return 0 if the tree is empty or on out of memory. Otherwise 1 is returned
 * and the iterator is set to the node reached after doing a random walk
 * of 'steps' steps. If the 'steps' argument is 0, the random walk is performed
 * using a random number of steps between 1 and two times the logarithm of
 * the number of elements.
 *
 * NOTE: if you use this function to generate random elements from the radix
 * tree, expect a disappointing distribution. A random walk produces good
 * random elements if the tree is not sparse, however in the case of a radix
 * tree certain keys will be reported much more often than others. At least
 * this function should be able to explore every possible element eventually. */
int raxRandomWalk(raxIterator *it, size_t steps) {
    if (it->rt->numele == 0) {
        it->flags |= RAX_ITER_EOF;
        return 0;
    }

    if (steps == 0) {
        size_t fle = 1+floor(log(it->rt->numele));
        fle *= 2;
        steps = 1 + rand() % fle;
    }

    raxNode *n = it->node;
    while(steps > 0 || (!n->iskey && it->leaf_slot_idx < 0)) {
        int numchildren = n->iscompr ? 1 : n->size;
        int r = rand() % (numchildren+(n != it->rt->head));

        if (r == numchildren) {
            /* Go up: if parked on a virtual leaf, exit it (the leaf parent
             * stays as n). Otherwise pop the real parent. Either way, the
             * edge to strip is owned by the resulting `n`. */
            if (it->leaf_slot_idx >= 0)
                it->leaf_slot_idx = -1;
            else
                n = raxStackPop(&it->stack);
            int todel = n->iscompr ? n->size : 1;
            raxIteratorDelChars(it,todel);
        } else {
            /* Select a random child. Fixed-length: if descent lands at
             * leaf depth, park on the chosen slot as a virtual leaf. */
            size_t child_depth = it->key_len + raxStepLenNode(n);
            if (raxSlotsAreValues(it->rt, child_depth)) {
                uint32_t slot = n->iscompr ? 0 : r;
                uint32_t size = n->iscompr ? n->size : 1;
                if (!raxIteratorAddChars(it, n->data + slot, size)) return 0;
                it->data = raxLeafParentReadSlot(n, slot);
                it->leaf_slot_idx = slot;
                if (steps > 0) steps--; /* virtual leaves count as keys */
                continue;
            }
            if (n->iscompr) {
                if (!raxIteratorAddChars(it,n->data,n->size)) return 0;
            } else {
                if (!raxIteratorAddChars(it,n->data+r,1)) return 0;
            }
            raxNode **cp = raxNodeFirstChildPtr(n)+r;
            if (!raxStackPush(&it->stack,n)) return 0;
            memcpy(&n,cp,sizeof(n));
        }
        if (n->iskey) steps--;
    }
    it->node = n;
    /* When parked on a virtual leaf the data was set on slot entry */
    if (it->leaf_slot_idx < 0) it->data = raxGetData(it->node);
    return 1;
}

/* Compare the key currently pointed by the iterator to the specified
 * key according to the specified operator. Returns 1 if the comparison is
 * true, otherwise 0 is returned. */
int raxCompare(raxIterator *iter, const char *op, unsigned char *key, size_t key_len) {
    int eq = 0, lt = 0, gt = 0;

    if (op[0] == '=' || op[1] == '=') eq = 1;
    if (op[0] == '>') gt = 1;
    else if (op[0] == '<') lt = 1;
    else if (op[1] != '=') return 0; /* Syntax error. */

    size_t minlen = key_len < iter->key_len ? key_len : iter->key_len;
    int cmp = memcmp(iter->key,key,minlen);

    /* Handle == */
    if (lt == 0 && gt == 0) return cmp == 0 && key_len == iter->key_len;

    /* Handle >, >=, <, <= */
    if (cmp == 0) {
        /* Same prefix: longer wins. */
        if (eq && key_len == iter->key_len) return 1;
        else if (lt) return iter->key_len < key_len;
        else if (gt) return iter->key_len > key_len;
        else return 0; /* Avoid warning, just 'eq' is handled before. */
    } else if (cmp > 0) {
        return gt ? 1 : 0;
    } else /* (cmp < 0) */ {
        return lt ? 1 : 0;
    }
}

/* Free the iterator. */
void raxStop(raxIterator *it) {
    if (it->key != it->key_static_string) rax_free(it->key);
    raxStackFree(&it->stack);
}

/* Return if the iterator is in an EOF state. This happens when raxSeek()
 * failed to seek an appropriate element, so that raxNext() or raxPrev()
 * will return zero, or when an EOF condition was reached while iterating
 * with raxNext() and raxPrev(). */
int raxEOF(raxIterator *it) {
    return it->flags & RAX_ITER_EOF;
}

/* Return the number of elements inside the radix tree. */
uint64_t raxSize(rax *rax) {
    return rax->numele;
}

/* ----------------------------- Introspection ------------------------------ */

/* This function is mostly used for debugging and learning purposes.
 * It shows an ASCII representation of a tree on standard output, outline
 * all the nodes and the contained keys.
 *
 * The representation is as follow:
 *
 *  "foobar" (compressed node)
 *  [abc] (normal node with three children)
 *  [abc]=0x12345678 (node is a key, pointing to value 0x12345678)
 *  [] (a normal empty node)
 *
 *  Children are represented in new indented lines, each children prefixed by
 *  the "`-(x)" string, where "x" is the edge byte.
 *
 *  [abc]
 *   `-(a) "ladin"
 *   `-(b) [kj]
 *   `-(c) []
 *
 *  However when a node has a single child the following representation
 *  is used instead:
 *
 *  [abc] -> "ladin" -> []
 */

/* The actual implementation of raxShow(). */
void raxRecursiveShow(int level, int lpad, raxNode *n) {
    char s = n->iscompr ? '"' : '[';
    char e = n->iscompr ? '"' : ']';

    int numchars = printf("%c%.*s%c", s, n->size, n->data, e);
    if (n->iskey) {
        numchars += printf("=%p",raxGetData(n));
    }

    int numchildren = n->iscompr ? 1 : n->size;
    /* Note that 7 and 4 magic constants are the string length
     * of " `-(x) " and " -> " respectively. */
    if (level) {
        lpad += (numchildren > 1) ? 7 : 4;
        if (numchildren == 1) lpad += numchars;
    }
    raxNode **cp = raxNodeFirstChildPtr(n);
    for (int i = 0; i < numchildren; i++) {
        char *branch = " `-(%c) ";
        if (numchildren > 1) {
            printf("\n");
            for (int j = 0; j < lpad; j++) putchar(' ');
            printf(branch,n->data[i]);
        } else {
            printf(" -> ");
        }
        raxNode *child;
        memcpy(&child,cp,sizeof(child));
        raxRecursiveShow(level+1,lpad,child);
        cp++;
    }
}

/* Show a tree, as outlined in the comment above. */
void raxShow(rax *rax) {
    raxRecursiveShow(0,0,rax->head);
    putchar('\n');
}

/* Used by debugnode() macro to show info about a given node. */
void raxDebugShowNode(const char *msg, raxNode *n) {
    if (raxDebugMsg == 0) return;
    printf("%s: %p [%.*s] key:%u size:%u children:",
        msg, (void*)n, (int)n->size, (char*)n->data, n->iskey, n->size);
    int numcld = n->iscompr ? 1 : n->size;
    raxNode **cldptr = raxNodeLastChildPtr(n) - (numcld-1);
    while(numcld--) {
        raxNode *child;
        memcpy(&child,cldptr,sizeof(child));
        cldptr++;
        printf("%p ", (void*)child);
    }
    printf("\n");
    fflush(stdout);
}

/* Touch all the nodes of a tree returning a check sum. This is useful
 * in order to make Valgrind detect if there is something wrong while
 * reading the data structure.
 *
 * This function was used in order to identify Rax bugs after a big refactoring
 * using this technique:
 *
 * 1. The rax-test is executed using Valgrind, adding a printf() so that for
 *    the fuzz tester we see what iteration in the loop we are in.
 * 2. After every modification of the radix tree made by the fuzz tester
 *    in rax-test.c, we add a call to raxTouch().
 * 3. Now as soon as an operation will corrupt the tree, raxTouch() will
 *    detect it (via Valgrind) immediately. We can add more calls to narrow
 *    the state.
 * 4. At this point a good idea is to enable Rax debugging messages immediately
 *    before the moment the tree is corrupted, to see what happens.
 */
unsigned long raxTouch(raxNode *n) {
    debugf("Touching %p\n", (void*)n);
    unsigned long sum = 0;
    if (n->iskey) {
        sum += (unsigned long)raxGetData(n);
    }

    int numchildren = n->iscompr ? 1 : n->size;
    raxNode **cp = raxNodeFirstChildPtr(n);
    int count = 0;
    for (int i = 0; i < numchildren; i++) {
        if (numchildren > 1) {
            sum += (long)n->data[i];
        }
        raxNode *child;
        memcpy(&child,cp,sizeof(child));
        if (child == (void*)0x65d1760) count++;
        if (count > 1) exit(1);
        sum += raxTouch(child);
        cp++;
    }
    return sum;
}

/* The rest of this file is test cases and test helpers. */
#ifdef REDIS_TEST
#include "testhelp.h"
#include <stdlib.h>

#define UNUSED(x) (void)(x)

#define yell(str, ...) printf("ERROR! " str "\n\n", __VA_ARGS__)

#define ERR(x, ...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __func__, __LINE__);                   \
        printf("ERROR! " x "\n", __VA_ARGS__);                                 \
        err++;                                                                 \
    } while (0)

#define TEST(name) printf("test — %s\n", name);

/* Verify rax memory accounting by calculating actual memory usage */
static int _rax_verify_alloc_size(rax *rax, size_t have) {
    int errors = 0;
    size_t want = rax_malloc_usable_size(rax);
    raxNode *node;
    raxStack stack;

    raxStackInit(&stack);
    raxStackPush(&stack, rax->head);
    while ((node = raxStackPop(&stack))) {
        want += rax_malloc_usable_size(node);
        if (!node->iscompr) {
            /* Non-compressed node: add all children */
            for (int i = 0; i < node->size; i++) {
                raxNode **child = raxNodeLastChildPtr(node) - i;
                if (*child) raxStackPush(&stack, *child);
            }
        } else {
            /* Compressed node: add single child */
            raxNode **child = raxNodeLastChildPtr(node);
            if (*child) raxStackPush(&stack, *child);
        }
    }
    raxStackFree(&stack);

    if (want != have) {
        yell("rax alloc_size is wrong: want: %zu, have: %zu\n", want, have);
        errors++;
    }

    return errors;
}

static void *createTestValue(size_t size) {
    size_t usable;
    void *val = rax_malloc_usable(size, &usable);
    memset(val, 'A', usable);
    return val;
}

/* qsort comparator for the fixed-length range-seek test below: compares
 * _rax_test_klen-byte keys, matching the rax's lexicographic order. */
static int _rax_test_klen = 0;
static int _rax_test_cmpkey(const void *a, const void *b) {
    return memcmp(a, b, _rax_test_klen);
}

int raxTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int err = 0;

    TEST("verify raxAllocSize() after raxInsert()/raxRemove()") {
        size_t alloc_size = 0;
        rax *r = raxNewEx(0, &alloc_size, 0);

        /* Insert values and verify accounting */
        void *val1 = createTestValue(100);
        assert(raxInsert(r, (unsigned char*)"key1", 4, val1, NULL) == 1);
        err += _rax_verify_alloc_size(r, alloc_size);

        void *val2 = createTestValue(200);
        assert(raxInsert(r, (unsigned char*)"key2", 4, val2, NULL) == 1);
        err += _rax_verify_alloc_size(r, alloc_size);

        void *val3 = createTestValue(10);
        assert(raxInsert(r, (unsigned char*)"3yek", 4, val3, NULL) == 1);
        err += _rax_verify_alloc_size(r, alloc_size);

        /* Remove a value and verify */
        void *removed;
        assert(raxRemove(r, (unsigned char*)"key1", 4, &removed) == 1);
        rax_free(removed);
        err += _rax_verify_alloc_size(r, alloc_size);

        raxFreeWithCallback(r, rax_free);
    }

    TEST("verify raxAllocSize() when replacing existing key") {
        size_t alloc_size = 0;
        rax *r = raxNewEx(0, &alloc_size, 0);

        void *val1 = createTestValue(100);
        assert(raxInsert(r, (unsigned char*)"key", 3, val1, NULL) == 1);
        err += _rax_verify_alloc_size(r, alloc_size);

        /* Update with different sized value */
        void *val2 = createTestValue(300);
        void *old;
        assert(raxInsert(r, (unsigned char*)"key", 3, val2, &old) == 0);
        rax_free(old);
        err += _rax_verify_alloc_size(r, alloc_size);

        raxFreeWithCallback(r, rax_free);
    }

    TEST("raxNodeLink: insert paths via raxFindLink + raxInsertAt") {
        /* Three not-found scenarios in one tree: empty insert, ALGO 1
         * mid-prefix split, ALGO 2 end-of-prefix split. */
        rax *r = raxNew();
        void *vK  = createTestValue(8);
        void *vAB = createTestValue(8);
        void *vXX = createTestValue(8);
        void *vAN = createTestValue(8);
        raxNodeLink link;
        void *val;

        /* (a) Empty tree: link points at head, InsertAt commits. */
        assert(raxFindLink(r, (unsigned char*)"K", 1, NULL, &link) == 0);
        assert(raxInsertAt(r, (unsigned char*)"K", 1, vK, NULL, &link) == 1);
        assert(raxFind(r, (unsigned char*)"K", 1, &val) == 1 && val == vK);

        /* Seed compressed node "ANNIBALE". */
        assert(raxInsert(r, (unsigned char*)"ANNIBALE", 8, vAB, NULL) == 1);

        /* (b) ALGO 1: mismatch mid-prefix. "ANXX" stops at splitpos > 0
         * inside the compressed "ANNIBALE" node. */
        assert(raxFindLink(r, (unsigned char*)"ANXX", 4, NULL, &link) == 0);
        assert(link.stopnode->iscompr && link.splitpos > 0);
        assert(raxInsertAt(r, (unsigned char*)"ANXX", 4, vXX, NULL, &link) == 1);

        /* (c) ALGO 2: query exhausts mid-prefix (i == len, splitpos > 0). */
        assert(raxFindLink(r, (unsigned char*)"ANNI", 4, NULL, &link) == 0);
        assert(raxInsertAt(r, (unsigned char*)"ANNI", 4, vAN, NULL, &link) == 1);

        /* All four keys reachable with the correct values. */
        assert(raxFind(r, (unsigned char*)"K",        1, &val) == 1 && val == vK);
        assert(raxFind(r, (unsigned char*)"ANNIBALE", 8, &val) == 1 && val == vAB);
        assert(raxFind(r, (unsigned char*)"ANXX",     4, &val) == 1 && val == vXX);
        assert(raxFind(r, (unsigned char*)"ANNI",     4, &val) == 1 && val == vAN);
        raxFreeWithCallback(r, rax_free);
    }

    TEST("raxNodeLink: existing-key paths (overwrite vs try-insert vs not-found)") {
        /* Three exists/not-exists scenarios on the same tree. */
        rax *r = raxNew();
        void *v1 = createTestValue(8), *v2 = createTestValue(8);
        void *v3 = createTestValue(8), *vNew = createTestValue(8);
        assert(raxInsert(r, (unsigned char*)"FOO", 3, v1, NULL) == 1);
        assert(raxInsert(r, (unsigned char*)"BAR", 3, v3, NULL) == 1);

        raxNodeLink link;
        void *val, *existing = NULL;

        /* (a) Overwrite path: FindLink returns 1, caller calls InsertAt
         * anyway -- value replaced, *old carries the prior pointer. */
        assert(raxFindLink(r, (unsigned char*)"FOO", 3, &existing, &link) == 1);
        assert(existing == v1);
        void *old = NULL;
        assert(raxInsertAt(r, (unsigned char*)"FOO", 3, v2, &old, &link) == 0);
        assert(old == v1);
        assert(raxFind(r, (unsigned char*)"FOO", 3, &val) == 1 && val == v2);
        rax_free(v1);

        /* (b) Try-insert path: FindLink reports existing -- caller
         * skips InsertAt entirely. No overwrite flag needed. */
        existing = NULL;
        assert(raxFindLink(r, (unsigned char*)"BAR", 3, &existing, &link) == 1);
        assert(existing == v3);
        /* Deliberately skip raxInsertAt -- v3 must survive. */
        rax_free(vNew);
        assert(raxFind(r, (unsigned char*)"BAR", 3, &val) == 1 && val == v3);

        /* (c) Not-found: FindLink on a missing key returns 0 and leaves
         * the tree untouched (caller didn't commit). */
        assert(raxFindLink(r, (unsigned char*)"BAZ", 3, NULL, &link) == 0);
        assert(raxFind(r, (unsigned char*)"BAZ", 3, NULL) == 0);
        assert(raxSize(r) == 2);
        raxFreeWithCallback(r, rax_free);
    }

    TEST("inline-leaf: fixed-length rax tree round-trips and saves numnodes") {
        rax *r = raxNewEx(0, NULL, 8);

        /* Value of each key in rax: k1->1, k2->2, k3->3, for easy verification */
        #define V(n) ((void*)(uintptr_t)(n))
        unsigned char k1[8] = {'k','e','y','0','0','0','0','1'};
        unsigned char k2[8] = {'k','e','y','0','0','0','0','2'};
        unsigned char k3[8] = {'z','z','z','z','z','z','z','z'};

        assert(raxInsert(r, k1, 8, V(1), NULL) == 1);
        assert(r->numnodes == 1);
        assert(raxFind(r, k1, 8, NULL) == 1);

        assert(raxInsert(r, k2, 8, V(2), NULL) == 1);
        assert(r->numnodes == 2);

        /* k3 diverges from k1/k2 at byte 0, so the shared "key0000" prefix
         * splits into a root branch node ('k','z'); the 'k' side keeps the
         * "ey0000" prefix + the '1'/'2' leaf parent, and the 'z' side gets a
         * "zzzzzzz" leaf parent. Four nodes total. */
        assert(raxInsert(r, k3, 8, V(3), NULL) == 1);
        assert(raxSize(r) == 3);
        assert(r->numnodes == 4);

        void *val = NULL;
        assert(raxFind(r, k1, 8, &val) == 1 && val == V(1));
        assert(raxFind(r, k2, 8, &val) == 1 && val == V(2));
        assert(raxFind(r, k3, 8, &val) == 1 && val == V(3));

        /* Overwrite via raxInsert returns 0 (existed) and surfaces the old value */
        void *old = NULL;
        assert(raxInsert(r, k1, 8, V(99), &old) == 0);
        assert(old == V(1));
        assert(raxFind(r, k1, 8, &val) == 1 && val == V(99));

        /* Remove returns the inlined value and shrinks the tree. */
        void *removed = NULL;
        assert(raxRemove(r, k2, 8, &removed) == 1 && removed == V(2));
        assert(raxSize(r) == 2);
        assert(raxFind(r, k2, 8, NULL) == 0);

        assert(raxRemove(r, k3, 8, &removed) == 1 && removed == V(3));
        assert(raxRemove(r, k1, 8, &removed) == 1 && removed == V(99));
        assert(raxSize(r) == 0);
        raxFree(r);
    }

    TEST("inline-leaf: dense leaf parent (256 distinct edge bytes)") {
        rax *r = raxNewEx(0, NULL, 2);
        unsigned char k[2] = {'P', 0};
        for (int i = 0; i < 256; i++) {
            k[1] = (unsigned char)i;
            assert(raxInsert(r, k, 2, (void*)(uintptr_t)(i+1), NULL) == 1);
            /* First key fuses into a single leaf-inlined node. The second key
             * splits off the "P" prefix and creates the dense leaf parent;
             * every later key just adds a slot to it, so the count stays at 2. */
            assert(r->numnodes == (i == 0 ? 1 : 2));
        }
        assert(raxSize(r) == 256);
        for (int i = 0; i < 256; i++) {
            k[1] = (unsigned char)i;
            void *val = NULL;
            assert(raxFind(r, k, 2, &val) == 1 && val == (void*)(uintptr_t)(i+1));
        }
        /* Remove half (even bytes) and verify the other half still finds. */
        for (int i = 0; i < 256; i += 2) {
            k[1] = (unsigned char)i;
            void *removed = NULL;
            assert(raxRemove(r, k, 2, &removed) == 1 &&
                   removed == (void*)(uintptr_t)(i+1));
        }
        assert(raxSize(r) == 128);
        for (int i = 1; i < 256; i += 2) {
            k[1] = (unsigned char)i;
            void *val = NULL;
            assert(raxFind(r, k, 2, &val) == 1 && val == (void*)(uintptr_t)(i+1));
        }
        raxFree(r);
    }

    TEST("inline-leaf: forward iteration through multiple inlined values") {
        rax *r = raxNewEx(0, NULL, 16);
        unsigned char keys[5][16];
        for (int i = 0; i < 5; i++) {
            memcpy(keys[i], "ssssmsmsmsmsmsms", 16);
            keys[i][15] = (unsigned char)i;
            assert(raxInsert(r, keys[i], 16, (void*)(uintptr_t)(i+1), NULL) == 1);
        }
        assert(raxSize(r) == 5);

        raxIterator ri;
        raxStart(&ri, r);
        assert(raxSeek(&ri, "^", NULL, 0) == 1);
        int yielded = 0;
        while (raxNext(&ri)) {
            assert(ri.key_len == 16);
            assert(ri.data == (void*)(uintptr_t)(yielded+1));
            assert(ri.key[15] == (unsigned char)yielded);
            yielded++;
        }
        assert(yielded == 5);
        raxStop(&ri);

        /* Reverse iteration. */
        raxStart(&ri, r);
        assert(raxSeek(&ri, "$", NULL, 0) == 1);
        int reverse_yielded = 5;
        while (raxPrev(&ri)) {
            reverse_yielded--;
            assert(ri.key_len == 16);
            assert(ri.data == (void*)(uintptr_t)(reverse_yielded+1));
            assert(ri.key[15] == (unsigned char)reverse_yielded);
        }
        assert(reverse_yielded == 0);
        raxStop(&ri);

        /* raxSeek "=" exact match on a virtual leaf. */
        raxStart(&ri, r);
        assert(raxSeek(&ri, "=", keys[2], 16) == 1);
        assert(raxEOF(&ri) == 0);
        assert(ri.data == (void*)(uintptr_t)3);
        raxStop(&ri);

        raxFree(r);
    }

    TEST("inline-leaf: iscompr=1 leaf parents (Streams-shape forward iter)") {
        /* Build a tree that mirrors the Streams 16-byte streamID shape:
         * 3 keys differing only at byte 7 produce iscompr=1 leaf parents
         * (compressed last 8 bytes) under an iscompr=0 splitnode. */
        rax *r = raxNewEx(0, NULL, 16);
        unsigned char keys[3][16];
        for (int i = 0; i < 3; i++) {
            memset(keys[i], 0, 16);
            keys[i][7] = (unsigned char)(i + 1);   /* 1-0, 2-0, 3-0 streamIDs */
            assert(raxInsert(r, keys[i], 16, (void*)(uintptr_t)(i+1), NULL) == 1);
        }
        assert(raxSize(r) == 3);

        raxIterator ri;
        raxStart(&ri, r);
        assert(raxSeek(&ri, "^", NULL, 0) == 1);
        int n = 0;
        while (raxNext(&ri)) {
            assert(ri.key_len == 16);
            assert(ri.data == (void*)(uintptr_t)(n+1));
            assert(memcmp(ri.key, keys[n], 16) == 0);
            n++;
        }
        assert(n == 3);
        raxStop(&ri);

        /* Reverse. */
        raxStart(&ri, r);
        assert(raxSeek(&ri, "$", NULL, 0) == 1);
        n = 3;
        while (raxPrev(&ri)) {
            n--;
            assert(ri.key_len == 16);
            assert(ri.data == (void*)(uintptr_t)(n+1));
            assert(memcmp(ri.key, keys[n], 16) == 0);
        }
        assert(n == 0);
        raxStop(&ri);

        /* Re-create the iterator multiple times -- mirrors XRANGE which
         * starts a fresh iterator for each call and steps until EOF. */
        for (int restart = 0; restart < 3; restart++) {
            raxStart(&ri, r);
            assert(raxSeek(&ri, "^", NULL, 0) == 1);
            int yielded = 0;
            while (raxNext(&ri)) {
                assert(ri.key_len == 16);
                assert(ri.data == (void*)(uintptr_t)(yielded+1));
                yielded++;
            }
            assert(yielded == 3);
            raxStop(&ri);
        }

        raxFree(r);
    }

    TEST("inline-leaf: delete triggers recompression into a sibling leaf parent") {
        /* k1 and k2 diverge at byte 1, each with a long unique suffix:
         *   "A" -> {A,B branch} -> "AAAAAA"(leaf parent, v1)   = k1
         *                       -> "BBBBBB"(leaf parent, v2)   = k2
         * Deleting k1 drops the branch to a single child and triggers
         * downward recompression, which must walk INTO the surviving B-side
         * chain and STOP at its singleton leaf parent without dereferencing
         * the inlined value as a raxNode* (the case the depth guard protects;
         * before the guard this path corrupted/crashed). */
        rax *r = raxNewEx(0, NULL, 8);
        unsigned char k1[8] = {'A','A','A','A','A','A','A','A'};
        unsigned char k2[8] = {'A','B','B','B','B','B','B','B'};
        assert(raxInsert(r, k1, 8, (void*)(uintptr_t)1, NULL) == 1);
        assert(raxInsert(r, k2, 8, (void*)(uintptr_t)2, NULL) == 1);
        assert(raxSize(r) == 2);

        void *removed = NULL;
        assert(raxRemove(r, k1, 8, &removed) == 1 && removed == (void*)(uintptr_t)1);

        /* Surviving key still resolves; deleted key is gone. */
        void *val = NULL;
        assert(raxFind(r, k2, 8, &val) == 1 && val == (void*)(uintptr_t)2);
        assert(raxFind(r, k1, 8, NULL) == 0);
        assert(raxSize(r) == 1);
        /* Recompression fused "A" + the single-child branch into one node, so
         * only the fused prefix node + the leaf parent remain. (Without
         * recompression the dropped-to-one-child branch would linger: 3.) */
        assert(r->numnodes == 2);

        /* Iteration still yields exactly k2. */
        raxIterator ri;
        raxStart(&ri, r);
        assert(raxSeek(&ri, "^", NULL, 0) == 1);
        assert(raxNext(&ri) == 1 && ri.key_len == 8 &&
               memcmp(ri.key, k2, 8) == 0 && ri.data == (void*)(uintptr_t)2);
        assert(raxNext(&ri) == 0);
        raxStop(&ri);

        assert(raxRemove(r, k2, 8, &removed) == 1 && removed == (void*)(uintptr_t)2);
        assert(raxSize(r) == 0);
        raxFree(r);
    }

    TEST("inline-leaf: range seeks (>,>=,<,<=) match brute-force reference") {
        /* Range seeks on a fixed-length tree exercise raxSeek's leaf-parent
         * stop and next/prev step machinery across virtual leaves. Random
         * keys at these lengths produce both dense (iscompr=0) and singleton
         * (iscompr=1) leaf parents; validate against a sorted array + EOF. */
        enum { MAXK = 400 };
        static const int klens[] = {3, 4, 8};
        static const char *ops[4] = {">", ">=", "<", "<="};
        uint32_t seed = 0xC0FFEEu;
        #define NEXTRAND() (seed = seed*1664525u + 1013904223u)

        for (size_t ki = 0; ki < sizeof(klens)/sizeof(klens[0]); ki++) {
            int klen = klens[ki];
            _rax_test_klen = klen;
            for (int round = 0; round < 40; round++) {
                rax *r = raxNewEx(0, NULL, klen);
                unsigned char keys[MAXK][8];
                int cnt = 0;
                int target = 1 + (int)(NEXTRAND() % MAXK);
                for (int i = 0; i < target; i++) {
                    unsigned char k[8];
                    for (int b = 0; b < klen; b++) k[b] = NEXTRAND() & 0xff;
                    if (raxFind(r, k, klen, NULL)) continue; /* skip dup */
                    memcpy(keys[cnt], k, klen);
                    assert(raxInsert(r, k, klen, (void*)(uintptr_t)(cnt+1),
                                     NULL) == 1);
                    cnt++;
                }
                qsort(keys, cnt, 8, _rax_test_cmpkey);

                for (int t = 0; t < 30; t++) {
                    unsigned char probe[8];
                    if ((t & 1) && cnt) {
                        memcpy(probe, keys[NEXTRAND()%cnt], klen); /* hit */
                    } else {
                        for (int b = 0; b < klen; b++) probe[b]=NEXTRAND()&0xff;
                    }
                    for (int o = 0; o < 4; o++) {
                        int gt = ops[o][0] == '>';
                        int eq = ops[o][1] == '=';
                        /* Brute-force the expected result, -1 means EOF. */
                        int exp = -1;
                        if (gt) {
                            for (int i = 0; i < cnt; i++) {
                                int c = memcmp(keys[i], probe, klen);
                                if (c > 0 || (eq && c == 0)) { exp = i; break; }
                            }
                        } else {
                            for (int i = cnt-1; i >= 0; i--) {
                                int c = memcmp(keys[i], probe, klen);
                                if (c < 0 || (eq && c == 0)) { exp = i; break; }
                            }
                        }
                        raxIterator ri;
                        raxStart(&ri, r);
                        assert(raxSeek(&ri, ops[o], probe, klen) == 1);
                        int has = raxNext(&ri);
                        if (exp < 0) {
                            assert(!has); /* sought past the end -> EOF */
                        } else {
                            assert(has && ri.key_len == (size_t)klen &&
                                   memcmp(ri.key, keys[exp], klen) == 0);
                        }
                        raxStop(&ri);
                    }
                }
                raxFree(r); /* values are integers, not heap pointers */
            }
        }
    }

    if (!err)
        printf("ALL TESTS PASSED!\n");
    else
        ERR("Sorry, not all tests passed!  In fact, %d tests failed.", err);

    return err;
}

#endif
