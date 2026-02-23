#ifndef JEMALLOC_INTERNAL_TYPED_LIST_H
#define JEMALLOC_INTERNAL_TYPED_LIST_H

/*
 * This wraps the ql module to implement a list class in a way that's a little
 * bit easier to use; it handles ql_elm_new calls and provides type safety.
 */

#define TYPED_LIST(list_type, el_type, linkage)				\
typedef struct {							\
	ql_head(el_type) head;						\
} list_type##_t;							\
static inline void							\
list_type##_init(list_type##_t *list) {					\
	ql_new(&list->head);						\
}									\
static inline el_type *							\
list_type##_first(const list_type##_t *list) {				\
	return ql_first(&list->head);					\
}									\
static inline el_type *							\
list_type##_last(const list_type##_t *list) {				\
	return ql_last(&list->head, linkage);				\
}									\
static inline void							\
list_type##_append(list_type##_t *list, el_type *item) {		\
	ql_elm_new(item, linkage);					\
	ql_tail_insert(&list->head, item, linkage);			\
}									\
static inline void							\
list_type##_prepend(list_type##_t *list, el_type *item) {		\
	ql_elm_new(item, linkage);					\
	ql_head_insert(&list->head, item, linkage);			\
}									\
static inline void							\
list_type##_replace(list_type##_t *list, el_type *to_remove,		\
    el_type *to_insert) {						\
	ql_elm_new(to_insert, linkage);					\
	ql_after_insert(to_remove, to_insert, linkage);			\
	ql_remove(&list->head, to_remove, linkage);			\
}									\
static inline void							\
list_type##_remove(list_type##_t *list, el_type *item) {		\
	ql_remove(&list->head, item, linkage);				\
}									\
static inline bool							\
list_type##_empty(list_type##_t *list) {				\
	return ql_empty(&list->head);					\
}									\
static inline void							\
list_type##_concat(list_type##_t *list_a, list_type##_t *list_b) {	\
	ql_concat(&list_a->head, &list_b->head, linkage);		\
}

#endif /* JEMALLOC_INTERNAL_TYPED_LIST_H */
