/* A space/time efficient FIFO queue of pointers.
 *
 * Implemented with an unrolled single-linked list, the implementation packs multiple pointers into
 * a single block.  This increases space efficiency and cache locality over the Redis `list` for the
 * purpose of a simple FIFO queue.
 */
#ifndef __FIFO_H_
#define __FIFO_H_

typedef struct Fifo Fifo;

// Create a new FIFO queue.
Fifo *fifoCreate();

// Push an item onto the end of the queue.
void fifoPush(Fifo *q, void *ptr);

// Look at the first item in the queue (without removing it).
// NOTE: asserts if the queue is empty.
void *fifoPeek(Fifo *q);

// Return and remove the first item from the queue.
// NOTE: asserts if the queue is empty.
void *fifoPop(Fifo *q);

// Return the number of items in the queue.
long fifoLength(Fifo *q);

// Delete the queue.
// NOTE: this does not free items which may be referenced by inserted pointers.
void fifoDelete(Fifo *q);

// Joins the Fifo "other" to the end of "q".  "other" becomes empty, but remains valid.
//  This is an O(1) operation.
void fifoJoin(Fifo *q, Fifo *other);

// Returns a new Fifo, containing all of the items from "q".  "q" remains valid, but becomes empty.
//  This is an O(1) operation.
Fifo * fifoPopAll(Fifo *q);

#endif
