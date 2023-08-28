#ifndef JEMALLOC_INTERNAL_QR_H
#define JEMALLOC_INTERNAL_QR_H

/*
 * A ring implementation based on an embedded circular doubly-linked list.
 *
 * You define your struct like so:
 *
 * typedef struct my_s my_t;
 * struct my_s {
 *   int data;
 *   qr(my_t) my_link;
 * };
 *
 * And then pass a my_t * into macros for a_qr arguments, and the token
 * "my_link" into a_field fields.
 */

/* Ring definitions. */
#define qr(a_type)							\
struct {								\
	a_type	*qre_next;						\
	a_type	*qre_prev;						\
}

/*
 * Initialize a qr link.  Every link must be initialized before being used, even
 * if that initialization is going to be immediately overwritten (say, by being
 * passed into an insertion macro).
 */
#define qr_new(a_qr, a_field) do {					\
	(a_qr)->a_field.qre_next = (a_qr);				\
	(a_qr)->a_field.qre_prev = (a_qr);				\
} while (0)

/*
 * Go forwards or backwards in the ring.  Note that (the ring being circular), this
 * always succeeds -- you just keep looping around and around the ring if you
 * chase pointers without end.
 */
#define qr_next(a_qr, a_field) ((a_qr)->a_field.qre_next)
#define qr_prev(a_qr, a_field) ((a_qr)->a_field.qre_prev)

/*
 * Given two rings:
 *    a -> a_1 -> ... -> a_n --
 *    ^                       |
 *    |------------------------
 *
 *    b -> b_1 -> ... -> b_n --
 *    ^                       |
 *    |------------------------
 *
 * Results in the ring:
 *   a -> a_1 -> ... -> a_n -> b -> b_1 -> ... -> b_n --
 *   ^                                                 |
 *   |-------------------------------------------------|
 *
 * a_qr_a can directly be a qr_next() macro, but a_qr_b cannot.
 */
#define qr_meld(a_qr_a, a_qr_b, a_field) do {				\
	(a_qr_b)->a_field.qre_prev->a_field.qre_next =			\
	    (a_qr_a)->a_field.qre_prev;					\
	(a_qr_a)->a_field.qre_prev = (a_qr_b)->a_field.qre_prev;	\
	(a_qr_b)->a_field.qre_prev =					\
	    (a_qr_b)->a_field.qre_prev->a_field.qre_next;		\
	(a_qr_a)->a_field.qre_prev->a_field.qre_next = (a_qr_a);	\
	(a_qr_b)->a_field.qre_prev->a_field.qre_next = (a_qr_b);	\
} while (0)

/*
 * Logically, this is just a meld.  The intent, though, is that a_qrelm is a
 * single-element ring, so that "before" has a more obvious interpretation than
 * meld.
 */
#define qr_before_insert(a_qrelm, a_qr, a_field)			\
	qr_meld((a_qrelm), (a_qr), a_field)

/* Ditto, but inserting after rather than before. */
#define qr_after_insert(a_qrelm, a_qr, a_field)				\
	qr_before_insert(qr_next(a_qrelm, a_field), (a_qr), a_field)

/*
 * Inverts meld; given the ring:
 *   a -> a_1 -> ... -> a_n -> b -> b_1 -> ... -> b_n --
 *   ^                                                 |
 *   |-------------------------------------------------|
 *
 * Results in two rings:
 *    a -> a_1 -> ... -> a_n --
 *    ^                       |
 *    |------------------------
 *
 *    b -> b_1 -> ... -> b_n --
 *    ^                       |
 *    |------------------------
 *
 * qr_meld() and qr_split() are functionally equivalent, so there's no need to
 * have two copies of the code.
 */
#define qr_split(a_qr_a, a_qr_b, a_field)				\
	qr_meld((a_qr_a), (a_qr_b), a_field)

/*
 * Splits off a_qr from the rest of its ring, so that it becomes a
 * single-element ring.
 */
#define qr_remove(a_qr, a_field)					\
	qr_split(qr_next(a_qr, a_field), (a_qr), a_field)

/*
 * Helper macro to iterate over each element in a ring exactly once, starting
 * with a_qr.  The usage is (assuming my_t defined as above):
 *
 * int sum(my_t *item) {
 *   int sum = 0;
 *   my_t *iter;
 *   qr_foreach(iter, item, link) {
 *     sum += iter->data;
 *   }
 *   return sum;
 * }
 */
#define qr_foreach(var, a_qr, a_field)					\
	for ((var) = (a_qr);						\
	    (var) != NULL;						\
	    (var) = (((var)->a_field.qre_next != (a_qr))		\
	    ? (var)->a_field.qre_next : NULL))

/*
 * The same (and with the same usage) as qr_foreach, but in the opposite order,
 * ending with a_qr.
 */
#define qr_reverse_foreach(var, a_qr, a_field)				\
	for ((var) = ((a_qr) != NULL) ? qr_prev(a_qr, a_field) : NULL;	\
	    (var) != NULL;						\
	    (var) = (((var) != (a_qr))					\
	    ? (var)->a_field.qre_prev : NULL))

#endif /* JEMALLOC_INTERNAL_QR_H */
