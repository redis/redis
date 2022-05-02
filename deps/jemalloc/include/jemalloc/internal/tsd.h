#ifndef JEMALLOC_INTERNAL_TSD_H
#define JEMALLOC_INTERNAL_TSD_H

#include "jemalloc/internal/arena_types.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/bin_types.h"
#include "jemalloc/internal/jemalloc_internal_externs.h"
#include "jemalloc/internal/prof_types.h"
#include "jemalloc/internal/ql.h"
#include "jemalloc/internal/rtree_tsd.h"
#include "jemalloc/internal/tcache_types.h"
#include "jemalloc/internal/tcache_structs.h"
#include "jemalloc/internal/util.h"
#include "jemalloc/internal/witness.h"

/*
 * Thread-Specific-Data layout
 * --- data accessed on tcache fast path: state, rtree_ctx, stats, prof ---
 * s: state
 * e: tcache_enabled
 * m: thread_allocated (config_stats)
 * f: thread_deallocated (config_stats)
 * p: prof_tdata (config_prof)
 * c: rtree_ctx (rtree cache accessed on deallocation)
 * t: tcache
 * --- data not accessed on tcache fast path: arena-related fields ---
 * d: arenas_tdata_bypass
 * r: reentrancy_level
 * x: narenas_tdata
 * i: iarena
 * a: arena
 * o: arenas_tdata
 * Loading TSD data is on the critical path of basically all malloc operations.
 * In particular, tcache and rtree_ctx rely on hot CPU cache to be effective.
 * Use a compact layout to reduce cache footprint.
 * +--- 64-bit and 64B cacheline; 1B each letter; First byte on the left. ---+
 * |----------------------------  1st cacheline  ----------------------------|
 * | sedrxxxx mmmmmmmm ffffffff pppppppp [c * 32  ........ ........ .......] |
 * |----------------------------  2nd cacheline  ----------------------------|
 * | [c * 64  ........ ........ ........ ........ ........ ........ .......] |
 * |----------------------------  3nd cacheline  ----------------------------|
 * | [c * 32  ........ ........ .......] iiiiiiii aaaaaaaa oooooooo [t...... |
 * +-------------------------------------------------------------------------+
 * Note: the entire tcache is embedded into TSD and spans multiple cachelines.
 *
 * The last 3 members (i, a and o) before tcache isn't really needed on tcache
 * fast path.  However we have a number of unused tcache bins and witnesses
 * (never touched unless config_debug) at the end of tcache, so we place them
 * there to avoid breaking the cachelines and possibly paging in an extra page.
 */
#ifdef JEMALLOC_JET
typedef void (*test_callback_t)(int *);
#  define MALLOC_TSD_TEST_DATA_INIT 0x72b65c10
#  define MALLOC_TEST_TSD \
    O(test_data,		int,			int)		\
    O(test_callback,		test_callback_t,	int)
#  define MALLOC_TEST_TSD_INITIALIZER , MALLOC_TSD_TEST_DATA_INIT, NULL
#else
#  define MALLOC_TEST_TSD
#  define MALLOC_TEST_TSD_INITIALIZER
#endif

/*  O(name,			type,			nullable type */
#define MALLOC_TSD							\
    O(tcache_enabled,		bool,			bool)		\
    O(arenas_tdata_bypass,	bool,			bool)		\
    O(reentrancy_level,		int8_t,			int8_t)		\
    O(narenas_tdata,		uint32_t,		uint32_t)	\
    O(offset_state,		uint64_t,		uint64_t)	\
    O(thread_allocated,		uint64_t,		uint64_t)	\
    O(thread_deallocated,	uint64_t,		uint64_t)	\
    O(bytes_until_sample,	int64_t,		int64_t)	\
    O(prof_tdata,		prof_tdata_t *,		prof_tdata_t *)	\
    O(rtree_ctx,		rtree_ctx_t,		rtree_ctx_t)	\
    O(iarena,			arena_t *,		arena_t *)	\
    O(arena,			arena_t *,		arena_t *)	\
    O(arenas_tdata,		arena_tdata_t *,	arena_tdata_t *)\
    O(binshards,		tsd_binshards_t,	tsd_binshards_t)\
    O(tcache,			tcache_t,		tcache_t)	\
    O(witness_tsd,              witness_tsd_t,		witness_tsdn_t)	\
    MALLOC_TEST_TSD

#define TSD_INITIALIZER {						\
    ATOMIC_INIT(tsd_state_uninitialized),				\
    TCACHE_ENABLED_ZERO_INITIALIZER,					\
    false,								\
    0,									\
    0,									\
    0,									\
    0,									\
    0,									\
    0,									\
    NULL,								\
    RTREE_CTX_ZERO_INITIALIZER,						\
    NULL,								\
    NULL,								\
    NULL,								\
    TSD_BINSHARDS_ZERO_INITIALIZER,					\
    TCACHE_ZERO_INITIALIZER,						\
    WITNESS_TSD_INITIALIZER						\
    MALLOC_TEST_TSD_INITIALIZER						\
}

void *malloc_tsd_malloc(size_t size);
void malloc_tsd_dalloc(void *wrapper);
void malloc_tsd_cleanup_register(bool (*f)(void));
tsd_t *malloc_tsd_boot0(void);
void malloc_tsd_boot1(void);
void tsd_cleanup(void *arg);
tsd_t *tsd_fetch_slow(tsd_t *tsd, bool internal);
void tsd_state_set(tsd_t *tsd, uint8_t new_state);
void tsd_slow_update(tsd_t *tsd);
void tsd_prefork(tsd_t *tsd);
void tsd_postfork_parent(tsd_t *tsd);
void tsd_postfork_child(tsd_t *tsd);

/*
 * Call ..._inc when your module wants to take all threads down the slow paths,
 * and ..._dec when it no longer needs to.
 */
void tsd_global_slow_inc(tsdn_t *tsdn);
void tsd_global_slow_dec(tsdn_t *tsdn);
bool tsd_global_slow();

enum {
	/* Common case --> jnz. */
	tsd_state_nominal = 0,
	/* Initialized but on slow path. */
	tsd_state_nominal_slow = 1,
	/*
	 * Some thread has changed global state in such a way that all nominal
	 * threads need to recompute their fast / slow status the next time they
	 * get a chance.
	 *
	 * Any thread can change another thread's status *to* recompute, but
	 * threads are the only ones who can change their status *from*
	 * recompute.
	 */
	tsd_state_nominal_recompute = 2,
	/*
	 * The above nominal states should be lower values.  We use
	 * tsd_nominal_max to separate nominal states from threads in the
	 * process of being born / dying.
	 */
	tsd_state_nominal_max = 2,

	/*
	 * A thread might free() during its death as its only allocator action;
	 * in such scenarios, we need tsd, but set up in such a way that no
	 * cleanup is necessary.
	 */
	tsd_state_minimal_initialized = 3,
	/* States during which we know we're in thread death. */
	tsd_state_purgatory = 4,
	tsd_state_reincarnated = 5,
	/*
	 * What it says on the tin; tsd that hasn't been initialized.  Note
	 * that even when the tsd struct lives in TLS, when need to keep track
	 * of stuff like whether or not our pthread destructors have been
	 * scheduled, so this really truly is different than the nominal state.
	 */
	tsd_state_uninitialized = 6
};

/*
 * Some TSD accesses can only be done in a nominal state.  To enforce this, we
 * wrap TSD member access in a function that asserts on TSD state, and mangle
 * field names to prevent touching them accidentally.
 */
#define TSD_MANGLE(n) cant_access_tsd_items_directly_use_a_getter_or_setter_##n

#ifdef JEMALLOC_U8_ATOMICS
#  define tsd_state_t atomic_u8_t
#  define tsd_atomic_load atomic_load_u8
#  define tsd_atomic_store atomic_store_u8
#  define tsd_atomic_exchange atomic_exchange_u8
#else
#  define tsd_state_t atomic_u32_t
#  define tsd_atomic_load atomic_load_u32
#  define tsd_atomic_store atomic_store_u32
#  define tsd_atomic_exchange atomic_exchange_u32
#endif

/* The actual tsd. */
struct tsd_s {
	/*
	 * The contents should be treated as totally opaque outside the tsd
	 * module.  Access any thread-local state through the getters and
	 * setters below.
	 */

	/*
	 * We manually limit the state to just a single byte.  Unless the 8-bit
	 * atomics are unavailable (which is rare).
	 */
	tsd_state_t state;
#define O(n, t, nt)							\
	t TSD_MANGLE(n);
MALLOC_TSD
#undef O
};

JEMALLOC_ALWAYS_INLINE uint8_t
tsd_state_get(tsd_t *tsd) {
	/*
	 * This should be atomic.  Unfortunately, compilers right now can't tell
	 * that this can be done as a memory comparison, and forces a load into
	 * a register that hurts fast-path performance.
	 */
	/* return atomic_load_u8(&tsd->state, ATOMIC_RELAXED); */
	return *(uint8_t *)&tsd->state;
}

/*
 * Wrapper around tsd_t that makes it possible to avoid implicit conversion
 * between tsd_t and tsdn_t, where tsdn_t is "nullable" and has to be
 * explicitly converted to tsd_t, which is non-nullable.
 */
struct tsdn_s {
	tsd_t tsd;
};
#define TSDN_NULL ((tsdn_t *)0)
JEMALLOC_ALWAYS_INLINE tsdn_t *
tsd_tsdn(tsd_t *tsd) {
	return (tsdn_t *)tsd;
}

JEMALLOC_ALWAYS_INLINE bool
tsdn_null(const tsdn_t *tsdn) {
	return tsdn == NULL;
}

JEMALLOC_ALWAYS_INLINE tsd_t *
tsdn_tsd(tsdn_t *tsdn) {
	assert(!tsdn_null(tsdn));

	return &tsdn->tsd;
}

/*
 * We put the platform-specific data declarations and inlines into their own
 * header files to avoid cluttering this file.  They define tsd_boot0,
 * tsd_boot1, tsd_boot, tsd_booted_get, tsd_get_allocates, tsd_get, and tsd_set.
 */
#ifdef JEMALLOC_MALLOC_THREAD_CLEANUP
#include "jemalloc/internal/tsd_malloc_thread_cleanup.h"
#elif (defined(JEMALLOC_TLS))
#include "jemalloc/internal/tsd_tls.h"
#elif (defined(_WIN32))
#include "jemalloc/internal/tsd_win.h"
#else
#include "jemalloc/internal/tsd_generic.h"
#endif

/*
 * tsd_foop_get_unsafe(tsd) returns a pointer to the thread-local instance of
 * foo.  This omits some safety checks, and so can be used during tsd
 * initialization and cleanup.
 */
#define O(n, t, nt)							\
JEMALLOC_ALWAYS_INLINE t *						\
tsd_##n##p_get_unsafe(tsd_t *tsd) {					\
	return &tsd->TSD_MANGLE(n);					\
}
MALLOC_TSD
#undef O

/* tsd_foop_get(tsd) returns a pointer to the thread-local instance of foo. */
#define O(n, t, nt)							\
JEMALLOC_ALWAYS_INLINE t *						\
tsd_##n##p_get(tsd_t *tsd) {						\
	/*								\
	 * Because the state might change asynchronously if it's	\
	 * nominal, we need to make sure that we only read it once.	\
	 */								\
	uint8_t state = tsd_state_get(tsd);				\
	assert(state == tsd_state_nominal ||				\
	    state == tsd_state_nominal_slow ||				\
	    state == tsd_state_nominal_recompute ||			\
	    state == tsd_state_reincarnated ||				\
	    state == tsd_state_minimal_initialized);			\
	return tsd_##n##p_get_unsafe(tsd);				\
}
MALLOC_TSD
#undef O

/*
 * tsdn_foop_get(tsdn) returns either the thread-local instance of foo (if tsdn
 * isn't NULL), or NULL (if tsdn is NULL), cast to the nullable pointer type.
 */
#define O(n, t, nt)							\
JEMALLOC_ALWAYS_INLINE nt *						\
tsdn_##n##p_get(tsdn_t *tsdn) {						\
	if (tsdn_null(tsdn)) {						\
		return NULL;						\
	}								\
	tsd_t *tsd = tsdn_tsd(tsdn);					\
	return (nt *)tsd_##n##p_get(tsd);				\
}
MALLOC_TSD
#undef O

/* tsd_foo_get(tsd) returns the value of the thread-local instance of foo. */
#define O(n, t, nt)							\
JEMALLOC_ALWAYS_INLINE t						\
tsd_##n##_get(tsd_t *tsd) {						\
	return *tsd_##n##p_get(tsd);					\
}
MALLOC_TSD
#undef O

/* tsd_foo_set(tsd, val) updates the thread-local instance of foo to be val. */
#define O(n, t, nt)							\
JEMALLOC_ALWAYS_INLINE void						\
tsd_##n##_set(tsd_t *tsd, t val) {					\
	assert(tsd_state_get(tsd) != tsd_state_reincarnated &&		\
	    tsd_state_get(tsd) != tsd_state_minimal_initialized);	\
	*tsd_##n##p_get(tsd) = val;					\
}
MALLOC_TSD
#undef O

JEMALLOC_ALWAYS_INLINE void
tsd_assert_fast(tsd_t *tsd) {
	/*
	 * Note that our fastness assertion does *not* include global slowness
	 * counters; it's not in general possible to ensure that they won't
	 * change asynchronously from underneath us.
	 */
	assert(!malloc_slow && tsd_tcache_enabled_get(tsd) &&
	    tsd_reentrancy_level_get(tsd) == 0);
}

JEMALLOC_ALWAYS_INLINE bool
tsd_fast(tsd_t *tsd) {
	bool fast = (tsd_state_get(tsd) == tsd_state_nominal);
	if (fast) {
		tsd_assert_fast(tsd);
	}

	return fast;
}

JEMALLOC_ALWAYS_INLINE tsd_t *
tsd_fetch_impl(bool init, bool minimal) {
	tsd_t *tsd = tsd_get(init);

	if (!init && tsd_get_allocates() && tsd == NULL) {
		return NULL;
	}
	assert(tsd != NULL);

	if (unlikely(tsd_state_get(tsd) != tsd_state_nominal)) {
		return tsd_fetch_slow(tsd, minimal);
	}
	assert(tsd_fast(tsd));
	tsd_assert_fast(tsd);

	return tsd;
}

/* Get a minimal TSD that requires no cleanup.  See comments in free(). */
JEMALLOC_ALWAYS_INLINE tsd_t *
tsd_fetch_min(void) {
	return tsd_fetch_impl(true, true);
}

/* For internal background threads use only. */
JEMALLOC_ALWAYS_INLINE tsd_t *
tsd_internal_fetch(void) {
	tsd_t *tsd = tsd_fetch_min();
	/* Use reincarnated state to prevent full initialization. */
	tsd_state_set(tsd, tsd_state_reincarnated);

	return tsd;
}

JEMALLOC_ALWAYS_INLINE tsd_t *
tsd_fetch(void) {
	return tsd_fetch_impl(true, false);
}

static inline bool
tsd_nominal(tsd_t *tsd) {
	return (tsd_state_get(tsd) <= tsd_state_nominal_max);
}

JEMALLOC_ALWAYS_INLINE tsdn_t *
tsdn_fetch(void) {
	if (!tsd_booted_get()) {
		return NULL;
	}

	return tsd_tsdn(tsd_fetch_impl(false, false));
}

JEMALLOC_ALWAYS_INLINE rtree_ctx_t *
tsd_rtree_ctx(tsd_t *tsd) {
	return tsd_rtree_ctxp_get(tsd);
}

JEMALLOC_ALWAYS_INLINE rtree_ctx_t *
tsdn_rtree_ctx(tsdn_t *tsdn, rtree_ctx_t *fallback) {
	/*
	 * If tsd cannot be accessed, initialize the fallback rtree_ctx and
	 * return a pointer to it.
	 */
	if (unlikely(tsdn_null(tsdn))) {
		rtree_ctx_data_init(fallback);
		return fallback;
	}
	return tsd_rtree_ctx(tsdn_tsd(tsdn));
}

#endif /* JEMALLOC_INTERNAL_TSD_H */
