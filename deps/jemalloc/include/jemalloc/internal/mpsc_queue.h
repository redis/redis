#ifndef JEMALLOC_INTERNAL_MPSC_QUEUE_H
#define JEMALLOC_INTERNAL_MPSC_QUEUE_H

#include "jemalloc/internal/atomic.h"

/*
 * A concurrent implementation of a multi-producer, single-consumer queue.  It
 * supports three concurrent operations:
 * - Push
 * - Push batch
 * - Pop batch
 *
 * These operations are all lock-free.
 *
 * The implementation is the simple two-stack queue built on a Treiber stack.
 * It's not terribly efficient, but this isn't expected to go into anywhere with
 * hot code.  In fact, we don't really even need queue semantics in any
 * anticipated use cases; we could get away with just the stack.  But this way
 * lets us frame the API in terms of the existing list types, which is a nice
 * convenience.  We can save on cache misses by introducing our own (parallel)
 * single-linked list type here, and dropping FIFO semantics, if we need this to
 * get faster.  Since we're currently providing queue semantics though, we use
 * the prev field in the link rather than the next field for Treiber-stack
 * linkage, so that we can preserve order for bash-pushed lists (recall that the
 * two-stack tricks reverses orders in the lock-free first stack).
 */

#define mpsc_queue(a_type)						\
struct {								\
	atomic_p_t tail;						\
}

#define mpsc_queue_proto(a_attr, a_prefix, a_queue_type, a_type,	\
    a_list_type)							\
/* Initialize a queue. */						\
a_attr void								\
a_prefix##new(a_queue_type *queue);					\
/* Insert all items in src into the queue, clearing src. */		\
a_attr void								\
a_prefix##push_batch(a_queue_type *queue, a_list_type *src);		\
/* Insert node into the queue. */					\
a_attr void								\
a_prefix##push(a_queue_type *queue, a_type *node);			\
/*									\
 * Pop all items in the queue into the list at dst.  dst should already	\
 * be initialized (and may contain existing items, which then remain	\
 * in dst).								\
 */									\
a_attr void								\
a_prefix##pop_batch(a_queue_type *queue, a_list_type *dst);

#define mpsc_queue_gen(a_attr, a_prefix, a_queue_type, a_type,		\
    a_list_type, a_link)						\
a_attr void								\
a_prefix##new(a_queue_type *queue) {					\
	atomic_store_p(&queue->tail, NULL, ATOMIC_RELAXED);		\
}									\
a_attr void								\
a_prefix##push_batch(a_queue_type *queue, a_list_type *src) {		\
	/*								\
	 * Reuse the ql list next field as the Treiber stack next	\
	 * field.							\
	 */								\
	a_type *first = ql_first(src);					\
	a_type *last = ql_last(src, a_link);				\
	void* cur_tail = atomic_load_p(&queue->tail, ATOMIC_RELAXED);	\
	do {								\
		/*							\
		 * Note that this breaks the queue ring structure;	\
		 * it's not a ring any more!				\
		 */							\
		first->a_link.qre_prev = cur_tail;			\
		/*							\
		 * Note: the upcoming CAS doesn't need an atomic; every	\
		 * push only needs to synchronize with the next pop,	\
		 * which we get from the release sequence rules.	\
		 */							\
	} while (!atomic_compare_exchange_weak_p(&queue->tail,		\
	    &cur_tail, last, ATOMIC_RELEASE, ATOMIC_RELAXED));		\
	ql_new(src);							\
}									\
a_attr void								\
a_prefix##push(a_queue_type *queue, a_type *node) {			\
	ql_elm_new(node, a_link);					\
	a_list_type list;						\
	ql_new(&list);							\
	ql_head_insert(&list, node, a_link);				\
	a_prefix##push_batch(queue, &list);				\
}									\
a_attr void								\
a_prefix##pop_batch(a_queue_type *queue, a_list_type *dst) {		\
	a_type *tail = atomic_load_p(&queue->tail, ATOMIC_RELAXED);	\
	if (tail == NULL) {						\
		/*							\
		 * In the common special case where there are no	\
		 * pending elements, bail early without a costly RMW.	\
		 */							\
		return;							\
	}								\
	tail = atomic_exchange_p(&queue->tail, NULL, ATOMIC_ACQUIRE);	\
	/*								\
	 * It's a single-consumer queue, so if cur started non-NULL,	\
	 * it'd better stay non-NULL.					\
	 */								\
	assert(tail != NULL);						\
	/*								\
	 * We iterate through the stack and both fix up the link	\
	 * structure (stack insertion broke the list requirement that	\
	 * the list be circularly linked).  It's just as efficient at	\
	 * this point to make the queue a "real" queue, so do that as	\
	 * well.							\
	 * If this ever gets to be a hot spot, we can omit this fixup	\
	 * and make the queue a bag (i.e. not necessarily ordered), but	\
	 * that would mean jettisoning the existing list API as the 	\
	 * batch pushing/popping interface.				\
	 */								\
	a_list_type reversed;						\
	ql_new(&reversed);						\
	while (tail != NULL) {						\
		/*							\
		 * Pop an item off the stack, prepend it onto the list	\
		 * (reversing the order).  Recall that we use the	\
		 * list prev field as the Treiber stack next field to	\
		 * preserve order of batch-pushed items when reversed.	\
		 */							\
		a_type *next = tail->a_link.qre_prev;			\
		ql_elm_new(tail, a_link);				\
		ql_head_insert(&reversed, tail, a_link);		\
		tail = next;						\
	}								\
	ql_concat(dst, &reversed, a_link);				\
}

#endif /* JEMALLOC_INTERNAL_MPSC_QUEUE_H */
