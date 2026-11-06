#ifndef JEMALLOC_INTERNAL_SEQ_H
#define JEMALLOC_INTERNAL_SEQ_H

#include "jemalloc/internal/atomic.h"

/*
 * A simple seqlock implementation.
 */

#define seq_define(type, short_type)					\
typedef struct {							\
	atomic_zu_t seq;						\
	atomic_zu_t data[						\
	    (sizeof(type) + sizeof(size_t) - 1) / sizeof(size_t)];	\
} seq_##short_type##_t;							\
									\
/*									\
 * No internal synchronization -- the caller must ensure that there's	\
 * only a single writer at a time.					\
 */									\
static inline void							\
seq_store_##short_type(seq_##short_type##_t *dst, type *src) {		\
	size_t buf[sizeof(dst->data) / sizeof(size_t)];			\
	buf[sizeof(buf) / sizeof(size_t) - 1] = 0;			\
	memcpy(buf, src, sizeof(type));					\
	size_t old_seq = atomic_load_zu(&dst->seq, ATOMIC_RELAXED);	\
	atomic_store_zu(&dst->seq, old_seq + 1, ATOMIC_RELAXED);	\
	atomic_fence(ATOMIC_RELEASE);					\
	for (size_t i = 0; i < sizeof(buf) / sizeof(size_t); i++) {	\
		atomic_store_zu(&dst->data[i], buf[i], ATOMIC_RELAXED);	\
	}								\
	atomic_store_zu(&dst->seq, old_seq + 2, ATOMIC_RELEASE);	\
}									\
									\
/* Returns whether or not the read was consistent. */			\
static inline bool							\
seq_try_load_##short_type(type *dst, seq_##short_type##_t *src) {	\
	size_t buf[sizeof(src->data) / sizeof(size_t)];			\
	size_t seq1 = atomic_load_zu(&src->seq, ATOMIC_ACQUIRE);	\
	if (seq1 % 2 != 0) {						\
		return false;						\
	}								\
	for (size_t i = 0; i < sizeof(buf) / sizeof(size_t); i++) {	\
		buf[i] = atomic_load_zu(&src->data[i], ATOMIC_RELAXED);	\
	}								\
	atomic_fence(ATOMIC_ACQUIRE);					\
	size_t seq2 = atomic_load_zu(&src->seq, ATOMIC_RELAXED);	\
	if (seq1 != seq2) {						\
		return false;						\
	}								\
	memcpy(dst, buf, sizeof(type));					\
	return true;							\
}

#endif /* JEMALLOC_INTERNAL_SEQ_H */
