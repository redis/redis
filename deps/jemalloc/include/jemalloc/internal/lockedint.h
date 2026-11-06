#ifndef JEMALLOC_INTERNAL_LOCKEDINT_H
#define JEMALLOC_INTERNAL_LOCKEDINT_H

/*
 * In those architectures that support 64-bit atomics, we use atomic updates for
 * our 64-bit values.  Otherwise, we use a plain uint64_t and synchronize
 * externally.
 */

typedef struct locked_u64_s locked_u64_t;
#ifdef JEMALLOC_ATOMIC_U64
struct locked_u64_s {
	atomic_u64_t val;
};
#else
/* Must hold the associated mutex. */
struct locked_u64_s {
	uint64_t val;
};
#endif

typedef struct locked_zu_s locked_zu_t;
struct locked_zu_s {
	atomic_zu_t val;
};

#ifndef JEMALLOC_ATOMIC_U64
#  define LOCKEDINT_MTX_DECLARE(name) malloc_mutex_t name;
#  define LOCKEDINT_MTX_INIT(mu, name, rank, rank_mode)			\
    malloc_mutex_init(&(mu), name, rank, rank_mode)
#  define LOCKEDINT_MTX(mtx) (&(mtx))
#  define LOCKEDINT_MTX_LOCK(tsdn, mu) malloc_mutex_lock(tsdn, &(mu))
#  define LOCKEDINT_MTX_UNLOCK(tsdn, mu) malloc_mutex_unlock(tsdn, &(mu))
#  define LOCKEDINT_MTX_PREFORK(tsdn, mu) malloc_mutex_prefork(tsdn, &(mu))
#  define LOCKEDINT_MTX_POSTFORK_PARENT(tsdn, mu)			\
    malloc_mutex_postfork_parent(tsdn, &(mu))
#  define LOCKEDINT_MTX_POSTFORK_CHILD(tsdn, mu)			\
    malloc_mutex_postfork_child(tsdn, &(mu))
#else
#  define LOCKEDINT_MTX_DECLARE(name)
#  define LOCKEDINT_MTX(mtx) NULL
#  define LOCKEDINT_MTX_INIT(mu, name, rank, rank_mode) false
#  define LOCKEDINT_MTX_LOCK(tsdn, mu)
#  define LOCKEDINT_MTX_UNLOCK(tsdn, mu)
#  define LOCKEDINT_MTX_PREFORK(tsdn, mu)
#  define LOCKEDINT_MTX_POSTFORK_PARENT(tsdn, mu)
#  define LOCKEDINT_MTX_POSTFORK_CHILD(tsdn, mu)
#endif

#ifdef JEMALLOC_ATOMIC_U64
#  define LOCKEDINT_MTX_ASSERT_INTERNAL(tsdn, mtx) assert((mtx) == NULL)
#else
#  define LOCKEDINT_MTX_ASSERT_INTERNAL(tsdn, mtx)			\
    malloc_mutex_assert_owner(tsdn, (mtx))
#endif

static inline uint64_t
locked_read_u64(tsdn_t *tsdn, malloc_mutex_t *mtx, locked_u64_t *p) {
	LOCKEDINT_MTX_ASSERT_INTERNAL(tsdn, mtx);
#ifdef JEMALLOC_ATOMIC_U64
	return atomic_load_u64(&p->val, ATOMIC_RELAXED);
#else
	return p->val;
#endif
}

static inline void
locked_inc_u64(tsdn_t *tsdn, malloc_mutex_t *mtx, locked_u64_t *p,
    uint64_t x) {
	LOCKEDINT_MTX_ASSERT_INTERNAL(tsdn, mtx);
#ifdef JEMALLOC_ATOMIC_U64
	atomic_fetch_add_u64(&p->val, x, ATOMIC_RELAXED);
#else
	p->val += x;
#endif
}

static inline void
locked_dec_u64(tsdn_t *tsdn, malloc_mutex_t *mtx, locked_u64_t *p,
    uint64_t x) {
	LOCKEDINT_MTX_ASSERT_INTERNAL(tsdn, mtx);
#ifdef JEMALLOC_ATOMIC_U64
	uint64_t r = atomic_fetch_sub_u64(&p->val, x, ATOMIC_RELAXED);
	assert(r - x <= r);
#else
	p->val -= x;
	assert(p->val + x >= p->val);
#endif
}

/* Increment and take modulus.  Returns whether the modulo made any change.  */
static inline bool
locked_inc_mod_u64(tsdn_t *tsdn, malloc_mutex_t *mtx, locked_u64_t *p,
    const uint64_t x, const uint64_t modulus) {
	LOCKEDINT_MTX_ASSERT_INTERNAL(tsdn, mtx);
	uint64_t before, after;
	bool overflow;
#ifdef JEMALLOC_ATOMIC_U64
	before = atomic_load_u64(&p->val, ATOMIC_RELAXED);
	do {
		after = before + x;
		assert(after >= before);
		overflow = (after >= modulus);
		if (overflow) {
			after %= modulus;
		}
	} while (!atomic_compare_exchange_weak_u64(&p->val, &before, after,
	    ATOMIC_RELAXED, ATOMIC_RELAXED));
#else
	before = p->val;
	after = before + x;
	overflow = (after >= modulus);
	if (overflow) {
		after %= modulus;
	}
	p->val = after;
#endif
	return overflow;
}

/*
 * Non-atomically sets *dst += src.  *dst needs external synchronization.
 * This lets us avoid the cost of a fetch_add when its unnecessary (note that
 * the types here are atomic).
 */
static inline void
locked_inc_u64_unsynchronized(locked_u64_t *dst, uint64_t src) {
#ifdef JEMALLOC_ATOMIC_U64
	uint64_t cur_dst = atomic_load_u64(&dst->val, ATOMIC_RELAXED);
	atomic_store_u64(&dst->val, src + cur_dst, ATOMIC_RELAXED);
#else
	dst->val += src;
#endif
}

static inline uint64_t
locked_read_u64_unsynchronized(locked_u64_t *p) {
#ifdef JEMALLOC_ATOMIC_U64
	return atomic_load_u64(&p->val, ATOMIC_RELAXED);
#else
	return p->val;
#endif
}

static inline void
locked_init_u64_unsynchronized(locked_u64_t *p, uint64_t x) {
#ifdef JEMALLOC_ATOMIC_U64
	atomic_store_u64(&p->val, x, ATOMIC_RELAXED);
#else
	p->val = x;
#endif
}

static inline size_t
locked_read_zu(tsdn_t *tsdn, malloc_mutex_t *mtx, locked_zu_t *p) {
	LOCKEDINT_MTX_ASSERT_INTERNAL(tsdn, mtx);
#ifdef JEMALLOC_ATOMIC_U64
	return atomic_load_zu(&p->val, ATOMIC_RELAXED);
#else
	return atomic_load_zu(&p->val, ATOMIC_RELAXED);
#endif
}

static inline void
locked_inc_zu(tsdn_t *tsdn, malloc_mutex_t *mtx, locked_zu_t *p,
    size_t x) {
	LOCKEDINT_MTX_ASSERT_INTERNAL(tsdn, mtx);
#ifdef JEMALLOC_ATOMIC_U64
	atomic_fetch_add_zu(&p->val, x, ATOMIC_RELAXED);
#else
	size_t cur = atomic_load_zu(&p->val, ATOMIC_RELAXED);
	atomic_store_zu(&p->val, cur + x, ATOMIC_RELAXED);
#endif
}

static inline void
locked_dec_zu(tsdn_t *tsdn, malloc_mutex_t *mtx, locked_zu_t *p,
    size_t x) {
	LOCKEDINT_MTX_ASSERT_INTERNAL(tsdn, mtx);
#ifdef JEMALLOC_ATOMIC_U64
	size_t r = atomic_fetch_sub_zu(&p->val, x, ATOMIC_RELAXED);
	assert(r - x <= r);
#else
	size_t cur = atomic_load_zu(&p->val, ATOMIC_RELAXED);
	atomic_store_zu(&p->val, cur - x, ATOMIC_RELAXED);
#endif
}

/* Like the _u64 variant, needs an externally synchronized *dst. */
static inline void
locked_inc_zu_unsynchronized(locked_zu_t *dst, size_t src) {
	size_t cur_dst = atomic_load_zu(&dst->val, ATOMIC_RELAXED);
	atomic_store_zu(&dst->val, src + cur_dst, ATOMIC_RELAXED);
}

/*
 * Unlike the _u64 variant, this is safe to call unconditionally.
 */
static inline size_t
locked_read_atomic_zu(locked_zu_t *p) {
	return atomic_load_zu(&p->val, ATOMIC_RELAXED);
}

#endif /* JEMALLOC_INTERNAL_LOCKEDINT_H */
