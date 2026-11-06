#ifndef JEMALLOC_INTERNAL_QL_H
#define JEMALLOC_INTERNAL_QL_H

#include "jemalloc/internal/qr.h"

/*
 * A linked-list implementation.
 *
 * This is built on top of the ring implementation, but that can be viewed as an
 * implementation detail (i.e. trying to advance past the tail of the list
 * doesn't wrap around).
 *
 * You define a struct like so:
 * typedef strucy my_s my_t;
 * struct my_s {
 *   int data;
 *   ql_elm(my_t) my_link;
 * };
 *
 * // We wobble between "list" and "head" for this type; we're now mostly
 * // heading towards "list".
 * typedef ql_head(my_t) my_list_t;
 *
 * You then pass a my_list_t * for a_head arguments, a my_t * for a_elm
 * arguments, the token "my_link" for a_field arguments, and the token "my_t"
 * for a_type arguments.
 */

/* List definitions. */
#define ql_head(a_type)							\
struct {								\
	a_type *qlh_first;						\
}

/* Static initializer for an empty list. */
#define ql_head_initializer(a_head) {NULL}

/* The field definition. */
#define ql_elm(a_type)	qr(a_type)

/* A pointer to the first element in the list, or NULL if the list is empty. */
#define ql_first(a_head) ((a_head)->qlh_first)

/* Dynamically initializes a list. */
#define ql_new(a_head) do {						\
	ql_first(a_head) = NULL;					\
} while (0)

/*
 * Sets dest to be the contents of src (overwriting any elements there), leaving
 * src empty.
 */
#define ql_move(a_head_dest, a_head_src) do {				\
	ql_first(a_head_dest) = ql_first(a_head_src);			\
	ql_new(a_head_src);						\
} while (0)

/* True if the list is empty, otherwise false. */
#define ql_empty(a_head) (ql_first(a_head) == NULL)

/*
 * Initializes a ql_elm.  Must be called even if the field is about to be
 * overwritten.
 */
#define ql_elm_new(a_elm, a_field) qr_new((a_elm), a_field)

/*
 * Obtains the last item in the list.
 */
#define ql_last(a_head, a_field)					\
	(ql_empty(a_head) ? NULL : qr_prev(ql_first(a_head), a_field))

/*
 * Gets a pointer to the next/prev element in the list.  Trying to advance past
 * the end or retreat before the beginning of the list returns NULL.
 */
#define ql_next(a_head, a_elm, a_field)					\
	((ql_last(a_head, a_field) != (a_elm))				\
	    ? qr_next((a_elm), a_field)	: NULL)
#define ql_prev(a_head, a_elm, a_field)					\
	((ql_first(a_head) != (a_elm)) ? qr_prev((a_elm), a_field)	\
				       : NULL)

/* Inserts a_elm before a_qlelm in the list. */
#define ql_before_insert(a_head, a_qlelm, a_elm, a_field) do {		\
	qr_before_insert((a_qlelm), (a_elm), a_field);			\
	if (ql_first(a_head) == (a_qlelm)) {				\
		ql_first(a_head) = (a_elm);				\
	}								\
} while (0)

/* Inserts a_elm after a_qlelm in the list. */
#define ql_after_insert(a_qlelm, a_elm, a_field)			\
	qr_after_insert((a_qlelm), (a_elm), a_field)

/* Inserts a_elm as the first item in the list. */
#define ql_head_insert(a_head, a_elm, a_field) do {			\
	if (!ql_empty(a_head)) {					\
		qr_before_insert(ql_first(a_head), (a_elm), a_field);	\
	}								\
	ql_first(a_head) = (a_elm);					\
} while (0)

/* Inserts a_elm as the last item in the list. */
#define ql_tail_insert(a_head, a_elm, a_field) do {			\
	if (!ql_empty(a_head)) {					\
		qr_before_insert(ql_first(a_head), (a_elm), a_field);	\
	}								\
	ql_first(a_head) = qr_next((a_elm), a_field);			\
} while (0)

/*
 * Given lists a = [a_1, ..., a_n] and [b_1, ..., b_n], results in:
 * a = [a1, ..., a_n, b_1, ..., b_n] and b = [].
 */
#define ql_concat(a_head_a, a_head_b, a_field) do {			\
	if (ql_empty(a_head_a)) {					\
		ql_move(a_head_a, a_head_b);				\
	} else if (!ql_empty(a_head_b)) {				\
		qr_meld(ql_first(a_head_a), ql_first(a_head_b),		\
		    a_field);						\
		ql_new(a_head_b);					\
	}								\
} while (0)

/* Removes a_elm from the list. */
#define ql_remove(a_head, a_elm, a_field) do {				\
	if (ql_first(a_head) == (a_elm)) {				\
		ql_first(a_head) = qr_next(ql_first(a_head), a_field);	\
	}								\
	if (ql_first(a_head) != (a_elm)) {				\
		qr_remove((a_elm), a_field);				\
	} else {							\
		ql_new(a_head);						\
	}								\
} while (0)

/* Removes the first item in the list. */
#define ql_head_remove(a_head, a_type, a_field) do {			\
	a_type *t = ql_first(a_head);					\
	ql_remove((a_head), t, a_field);				\
} while (0)

/* Removes the last item in the list. */
#define ql_tail_remove(a_head, a_type, a_field) do {			\
	a_type *t = ql_last(a_head, a_field);				\
	ql_remove((a_head), t, a_field);				\
} while (0)

/*
 * Given a = [a_1, a_2, ..., a_n-1, a_n, a_n+1, ...],
 * ql_split(a, a_n, b, some_field) results in
 *   a = [a_1, a_2, ..., a_n-1]
 * and replaces b's contents with:
 *   b = [a_n, a_n+1, ...]
 */
#define ql_split(a_head_a, a_elm, a_head_b, a_field) do {		\
	if (ql_first(a_head_a) == (a_elm)) {				\
		ql_move(a_head_b, a_head_a);				\
	} else {							\
		qr_split(ql_first(a_head_a), (a_elm), a_field);		\
		ql_first(a_head_b) = (a_elm);				\
	}								\
} while (0)

/*
 * An optimized version of:
 *	a_type *t = ql_first(a_head);
 *	ql_remove((a_head), t, a_field);
 *	ql_tail_insert((a_head), t, a_field);
 */
#define ql_rotate(a_head, a_field) do {					\
	ql_first(a_head) = qr_next(ql_first(a_head), a_field);		\
} while (0)

/*
 * Helper macro to iterate over each element in a list in order, starting from
 * the head (or in reverse order, starting from the tail).  The usage is
 * (assuming my_t and my_list_t defined as above).
 *
 * int sum(my_list_t *list) {
 *   int sum = 0;
 *   my_t *iter;
 *   ql_foreach(iter, list, link) {
 *     sum += iter->data;
 *   }
 *   return sum;
 * }
 */

#define ql_foreach(a_var, a_head, a_field)				\
	qr_foreach((a_var), ql_first(a_head), a_field)

#define ql_reverse_foreach(a_var, a_head, a_field)			\
	qr_reverse_foreach((a_var), ql_first(a_head), a_field)

#endif /* JEMALLOC_INTERNAL_QL_H */
