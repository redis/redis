#include <stdint.h>

#include "fifo.h"
#include "redisassert.h"
#include "zmalloc.h"

// Items per block was chosen as 7 because, including the next pointer, this gives us a nice even
//  64-byte block.  Conveniently, the index values 0..6 will fit nicely in the 3 unused bits at the 
//  bottom of the next pointer, creating a very compact block.
#define ITEMS_PER_BLOCK 7
static const uintptr_t IDX_MASK = 0x0007;


/* The FifoBlock contains up to 7 items (pointers).  When compared with adlist, this results in
 * roughly 60% memory reduction and 7x fewer memory allocations.  Memory reduction is guaranteed
 * with 5+ items in queue.
 *
 * In each block, there are 7 slots for item pointers (pointers to the caller's FIFO item).
 *  We need to keep track of the first & last slot used.  Contextually, we will only need
 *  a single index - either the first slot used or the last slot used.  Based on context,
 *  we can determine what is needed.
 *
 * Blocks are linked together in a chain.  If the list is empty, there are no blocks.
 *  For non-empty lists, we will either have a single block OR a chain of blocks.
 *
 * For a SINGLE BLOCK containing (for example) 4 items, the layout looks like this:
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *  SINGLE BLOCK:  | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  | 
 *                 |  item  |  item  |  item  |  item  |   -    |   -    |   -    | lastIdx|
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *                                                ^
 *                                             lastIdx (3)
 *  In single blocks, the items are always shifted so that the first item is in slot 0.
 *  We need to keep track of the lastIdx so that we will know where to push the next item.
 *  The last index is stored in the final 3 bits of the (unused) next pointer
 *
 * When MULTIPLE BLOCKS are chained together, items will be popped from the first block, and
 *  pushed onto the last block.  All blocks in the middle are full(*).  In the first block, we keep
 *  the firstIdx (so we know where to pop) ... on the last block, we keep lastIdx (so we know
 *  where to push).
 * 
 * (*) While blocks in the middle of a chain are generally full, the Fifo supports O(1) joining of
 *     two lists.  In this case, a block at the join point may not be full.  In this case, it will
 *     look like the FIRST BLOCK below, with the first index stored in the indexing bits.
 *
 * Example FIRST BLOCK with 2 items remaining:
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *  FIRST BLOCK:   | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  | 
 *                 |   -    |   -    |   -    |   -    |   -    |  item  |  item  |firstIdx|
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *                                                                  ^
 *                                                              firstIdx (5)
 * Example LAST BLOCK with 3 items pushed so far:
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *  LAST BLOCK:    | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  | 
 *                 |  item  |  item  |  item  |   -    |   -    |   -    |   -    | lastIdx|
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *                                        ^
 *                                    lastIdx (2)
 */
typedef struct FifoBlock {
    void *items[ITEMS_PER_BLOCK];
    union {
        /* The last 3 bits of a pointer to a block allocated by malloc must always be zero as a
         *  minimum of 8-byte alignment is required for all such blocks.  These bits are used as
         *  an index into the block indicating the first or last item in the block, depending on
         *  context.
         *
         * This UNION overlays a pointer with an integral value.  This allows us to look at the
         *  pointer OR the integer without casting - but they use the same memory.
         *
         * If there is MORE THAN ONE block in the chain, the first block has a pointer/index that 
         *  looks like this.  However, if there is only a single block, it looks like the LAST block.
         *   +-----------------------------------------------------------+
         *   |                 next pointer                   | firstIdx |
         *   |                  (61 bits)                     | (3 bits) |
         *   +-----------------------------------------------------------+
         *     * The next pointer is only valid after zeroing out the last 3 bits.
         *     * "lastIdx" is implied to be 6 (because there are additional blocks).
         *     * "firstIdx" represents the first filled index (0..6).  POP occurs here.
         *
         * Any blocks in the middle of the chain have a regular pointer like this:
         *   +-----------------------------------------------------------+
         *   |                 next pointer                   |    0*    |
         *   |                  (61 bits)                     | (3 bits) |
         *   +-----------------------------------------------------------+
         *     * The next pointer is valid as-is
         *     * "lastIdx" is implied to be 6 in all middle blocks.
         *     * "firstIdx" is implied to be 0 in all middle blocks.
         *     * NOTE: In middle blocks, the index bits(0) are really still the firstIdx value.
         *             When Fifo's are joined, the O(1) operation may result in a partially
         *             full middle block.  In this case, the items are "right-justified" and
         *             firstIdx indicates where the items start.
         *
         * The last (or only) block in the chain contains only the lastIndex, the pointer is unused.
         *   +-----------------------------------------------------------+
         *   |                      0                         | lastIdx  |
         *   |                  (61 bits)                     | (3 bits) |
         *   +-----------------------------------------------------------+
         *     * The next pointer is unused and guaranteed NULL.
         *     * "lastIdx" represents the last filled index (0..6).
         *     * "firstIdx" is implied to be zero on the last (or only) block.
         */
        uintptr_t last_or_first_idx;
        struct FifoBlock *next;
    } u;
} FifoBlock;

struct Fifo {
    long length;        // Total number of items in queue
    FifoBlock *first;
    FifoBlock *last;
};


// Create a new FIFO queue.
Fifo *fifoCreate() {
    Fifo *q = zmalloc(sizeof(Fifo));
    q->length = 0;
    q->first = q->last = NULL;
    return q;
}


// Push an item onto the end of the queue.
void fifoPush(Fifo *q, void *ptr) {
    if (q->first == NULL) {
        // Queue was empty - create block
        assert(q->last == NULL && q->length == 0);
        q->last = q->first = zmalloc(sizeof(FifoBlock));
        q->last->u.last_or_first_idx = 0;   // Item 0 is the last item in this block
        q->last->items[0] = ptr;
    } else {
        int lastIdx = q->last->u.last_or_first_idx; // pointer portion is 0 on last (or only) block
        assert(lastIdx < ITEMS_PER_BLOCK);

        if (lastIdx < ITEMS_PER_BLOCK - 1) {
            // If the last block has space, just add the item
            q->last->items[lastIdx + 1] = ptr;
            q->last->u.last_or_first_idx++;
        } else {
            // Otherwise, last block is full - add a new block
            FifoBlock *newblock = zmalloc(sizeof(FifoBlock));
            newblock->u.last_or_first_idx = 0;
            newblock->items[0] = ptr;
            q->last->u.next = newblock;     // overwrites the index, setting it to 0
            q->last = newblock;
        }
    }

    q->length++;
}


// Push an item onto the FRONT of the queue.
void fifoPushFront(Fifo *q, void *ptr) {
    if (q->first == NULL) {
        fifoPush(q, ptr);
        return;
    }

    if (q->first == q->last && q->length < ITEMS_PER_BLOCK) {
        // With only 1 (non-full) block, shift items right and insert at 0
        q->first->u.last_or_first_idx++;     // This is LAST index, incr for new item
        int lastIdx = q->first->u.last_or_first_idx; // pointer portion is 0 on only block
        for (int i = lastIdx;  i > 0;  i--) {
            q->first->items[i] =  q->first->items[i - 1];
        }
        q->first->items[0] = ptr;
    } else {
        int firstIdx = (q->first == q->last)
                ? 0     // We've already determined above that the ONLY block is full
                : q->first->u.last_or_first_idx & IDX_MASK; // For other cases, firstIdx is here
        if (firstIdx > 0) {
            // This is the easy case.  Just insert before the others.
            q->first->items[firstIdx - 1] = ptr;
            q->first->u.last_or_first_idx--;
        } else {
            // Insert a new block in front.
            //  The new item will be in the LAST spot in the block.
            FifoBlock *newblock = zmalloc(sizeof(FifoBlock));
            firstIdx = ITEMS_PER_BLOCK - 1;         // Item goes at end of block
            newblock->items[firstIdx] = ptr;
            newblock->u.next = q->first;
            newblock->u.last_or_first_idx += firstIdx;  // Overlay bits onto pointer
            q->first = newblock;
        }
    }
    
    q->length++;
}


// Look at the first item in the queue (without removing it).
// NOTE: asserts if the queue is empty.
void *fifoPeek(Fifo *q) {
    assert(q->length > 0);
    int firstIdx = (q->first == q->last) ? 0 : q->first->u.last_or_first_idx & IDX_MASK;
    return q->first->items[firstIdx];
}


// Return and remove the first item from the queue.
// NOTE: asserts if the queue is empty.
void *fifoPop(Fifo *q) {
    assert(q->length > 0);
    void *item;

    if (q->first == q->last) {
        // With only 1 block, POP occurs at index 0 and items 1..6 are shifted.
        item = q->last->items[0];

        int lastIdx = q->last->u.last_or_first_idx; // pointer portion is 0 on last (or only) block
        assert(lastIdx < ITEMS_PER_BLOCK);

        if (lastIdx > 0) {
            // With only 1 block, shift the items rather than eventually needing new block.
            //  (This is cheap, shifting a max of 6 pointers.)
            for (int i = 0;  i < lastIdx;  i++) q->last->items[i] = q->last->items[i + 1];
            q->last->u.last_or_first_idx--;     // Decrement the last index
        } else {
            // Just finished the only block.  Delete it.
            zfree(q->last);
            q->first = q->last = NULL;
        }
    } else {
        // With more than 1 block, POP occurs at firstIdx, and firstIdx is incremented.
        int firstIdx = q->first->u.last_or_first_idx & IDX_MASK;
        item = q->first->items[firstIdx];

        if (firstIdx < ITEMS_PER_BLOCK - 1) {
            // Just increment the first index to the next slot.
            q->first->u.last_or_first_idx++;
        } else {
            // Finished with this block, move to next
            q->first->u.last_or_first_idx &= ~IDX_MASK;     // restores the next pointer
            FifoBlock *next = q->first->u.next;
            zfree(q->first);
            q->first = next;
        }
    }

    q->length--;

    return item;
}


// Return the number of items in the queue.
long fifoLength(Fifo *q) {
    return q->length;
}


// Delete the queue.
// NOTE: this does not free items which may be referenced by inserted pointers.
void fifoDelete(Fifo *q) {
    if (q->length > 0) {
        FifoBlock *cur = q->first;
        while (cur != NULL) {
            cur->u.last_or_first_idx &= ~IDX_MASK;      // zero out the last 3 bits
            FifoBlock *next = cur->u.next;
            zfree(cur);
            cur = next;
        }
    }
    zfree(q);
}


// Blindly overwrites target from source.
static void blindlyMoveFifoContents(Fifo *target, Fifo *source) {
    target->length = source->length;
    target->first = source->first;
    target->last = source->last;
    source->length = 0;
    source->first = source->last = NULL;
}


// Join an "other" Fifo onto this one (emptying "other")
void fifoJoin(Fifo *q, Fifo *other) {
    /* When joining a Fifo onto an existing Fifo, we might be left with partially full blocks in the
     * middle of the list.  In the usual case, any blocks in the middle of the list have the index
     * bits set to zero.  This actually represents the firstIdx - which would normally be zero for
     * blocks in the middle of the list.  In the case of joining lists, we allow partially full
     * blocks in the middle, but the values are "right-justified" and the firstIdx is set.
     *
     * To perform the join, we take the current last (or only) block - which is "left-justified" and
     * shift the items so that the block becomes right-justified.  Then the index is corrected,
     * replacing the lastIdx with the firstIdx.
     *
     * The "other" list is correct as-is.  If there is only a single block, it becomes the last
     * block and remains left-justified.  If there are multiple blocks, the first block of the 
     * "other" list is already right-justified and becomes a partially full middle block.
     */
    if (other->length == 0) return;

    if (q->length == 0) {
        // If "q" is empty, it's a simple operation.
        blindlyMoveFifoContents(q, other);
        return;
    }

    if (other->length < ITEMS_PER_BLOCK) {
        // In the case of a short "other" Fifo, move each item.  This prevents creation of a string
        //  of half-empty blocks if fifoJoin is repeatedly used on small Fifos.
        while (other->length > 0) fifoPush(q, fifoPop(other));
        return;
    }

    FifoBlock *curLast = q->last;
    int lastIdx = curLast->u.last_or_first_idx;
    // Shift the items in the last block if it is partially full
    int shift = (ITEMS_PER_BLOCK - 1) - lastIdx;
    if (shift > 0) {
        for (int i = lastIdx;  i >= 0;  i--)
            curLast->items[i + shift] = curLast->items[i];
    }

    // Now fix up the next pointer to point to the next block
    curLast->u.next = other->first;
    curLast->u.last_or_first_idx += shift;      // Mask on the firstIdx for the shifted block

    // Finally, clean up the main list structures
    q->length += other->length;
    q->last = other->last;
    other->length = 0;
    other->first = other->last = NULL;
}


// Copy all of the items into a new Fifo (emptying the original)
Fifo *fifoPopAll(Fifo *q) {
    Fifo *newQ = zmalloc(sizeof(Fifo));
    blindlyMoveFifoContents(newQ, q);
    return newQ;
}
