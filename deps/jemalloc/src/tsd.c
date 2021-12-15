#define JEMALLOC_TSD_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree.h"

/******************************************************************************/
/* Data. */

static unsigned ncleanups;
static malloc_tsd_cleanup_t cleanups[MALLOC_TSD_CLEANUPS_MAX];

/* TSD_INITIALIZER triggers "-Wmissing-field-initializer" */
JEMALLOC_DIAGNOSTIC_PUSH
JEMALLOC_DIAGNOSTIC_IGNORE_MISSING_STRUCT_FIELD_INITIALIZERS

#ifdef JEMALLOC_MALLOC_THREAD_CLEANUP
JEMALLOC_TSD_TYPE_ATTR(tsd_t) tsd_tls = TSD_INITIALIZER;
JEMALLOC_TSD_TYPE_ATTR(bool) JEMALLOC_TLS_MODEL tsd_initialized = false;
bool tsd_booted = false;
#elif (defined(JEMALLOC_TLS))
JEMALLOC_TSD_TYPE_ATTR(tsd_t) tsd_tls = TSD_INITIALIZER;
pthread_key_t tsd_tsd;
bool tsd_booted = false;
#elif (defined(_WIN32))
DWORD tsd_tsd;
tsd_wrapper_t tsd_boot_wrapper = {false, TSD_INITIALIZER};
bool tsd_booted = false;
#else

/*
 * This contains a mutex, but it's pretty convenient to allow the mutex code to
 * have a dependency on tsd.  So we define the struct here, and only refer to it
 * by pointer in the header.
 */
struct tsd_init_head_s {
	ql_head(tsd_init_block_t) blocks;
	malloc_mutex_t lock;
};

pthread_key_t tsd_tsd;
tsd_init_head_t	tsd_init_head = {
	ql_head_initializer(blocks),
	MALLOC_MUTEX_INITIALIZER
};

tsd_wrapper_t tsd_boot_wrapper = {
	false,
	TSD_INITIALIZER
};
bool tsd_booted = false;
#endif

JEMALLOC_DIAGNOSTIC_POP

/******************************************************************************/

/* A list of all the tsds in the nominal state. */
typedef ql_head(tsd_t) tsd_list_t;
static tsd_list_t tsd_nominal_tsds = ql_head_initializer(tsd_nominal_tsds);
static malloc_mutex_t tsd_nominal_tsds_lock;

/* How many slow-path-enabling features are turned on. */
static atomic_u32_t tsd_global_slow_count = ATOMIC_INIT(0);

static bool
tsd_in_nominal_list(tsd_t *tsd) {
	tsd_t *tsd_list;
	bool found = false;
	/*
	 * We don't know that tsd is nominal; it might not be safe to get data
	 * out of it here.
	 */
	malloc_mutex_lock(TSDN_NULL, &tsd_nominal_tsds_lock);
	ql_foreach(tsd_list, &tsd_nominal_tsds, TSD_MANGLE(tcache).tsd_link) {
		if (tsd == tsd_list) {
			found = true;
			break;
		}
	}
	malloc_mutex_unlock(TSDN_NULL, &tsd_nominal_tsds_lock);
	return found;
}

static void
tsd_add_nominal(tsd_t *tsd) {
	assert(!tsd_in_nominal_list(tsd));
	assert(tsd_state_get(tsd) <= tsd_state_nominal_max);
	ql_elm_new(tsd, TSD_MANGLE(tcache).tsd_link);
	malloc_mutex_lock(tsd_tsdn(tsd), &tsd_nominal_tsds_lock);
	ql_tail_insert(&tsd_nominal_tsds, tsd, TSD_MANGLE(tcache).tsd_link);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tsd_nominal_tsds_lock);
}

static void
tsd_remove_nominal(tsd_t *tsd) {
	assert(tsd_in_nominal_list(tsd));
	assert(tsd_state_get(tsd) <= tsd_state_nominal_max);
	malloc_mutex_lock(tsd_tsdn(tsd), &tsd_nominal_tsds_lock);
	ql_remove(&tsd_nominal_tsds, tsd, TSD_MANGLE(tcache).tsd_link);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tsd_nominal_tsds_lock);
}

static void
tsd_force_recompute(tsdn_t *tsdn) {
	/*
	 * The stores to tsd->state here need to synchronize with the exchange
	 * in tsd_slow_update.
	 */
	atomic_fence(ATOMIC_RELEASE);
	malloc_mutex_lock(tsdn, &tsd_nominal_tsds_lock);
	tsd_t *remote_tsd;
	ql_foreach(remote_tsd, &tsd_nominal_tsds, TSD_MANGLE(tcache).tsd_link) {
		assert(tsd_atomic_load(&remote_tsd->state, ATOMIC_RELAXED)
		    <= tsd_state_nominal_max);
		tsd_atomic_store(&remote_tsd->state, tsd_state_nominal_recompute,
		    ATOMIC_RELAXED);
	}
	malloc_mutex_unlock(tsdn, &tsd_nominal_tsds_lock);
}

void
tsd_global_slow_inc(tsdn_t *tsdn) {
	atomic_fetch_add_u32(&tsd_global_slow_count, 1, ATOMIC_RELAXED);
	/*
	 * We unconditionally force a recompute, even if the global slow count
	 * was already positive.  If we didn't, then it would be possible for us
	 * to return to the user, have the user synchronize externally with some
	 * other thread, and then have that other thread not have picked up the
	 * update yet (since the original incrementing thread might still be
	 * making its way through the tsd list).
	 */
	tsd_force_recompute(tsdn);
}

void tsd_global_slow_dec(tsdn_t *tsdn) {
	atomic_fetch_sub_u32(&tsd_global_slow_count, 1, ATOMIC_RELAXED);
	/* See the note in ..._inc(). */
	tsd_force_recompute(tsdn);
}

static bool
tsd_local_slow(tsd_t *tsd) {
	return !tsd_tcache_enabled_get(tsd)
	    || tsd_reentrancy_level_get(tsd) > 0;
}

bool
tsd_global_slow() {
	return atomic_load_u32(&tsd_global_slow_count, ATOMIC_RELAXED) > 0;
}

/******************************************************************************/

static uint8_t
tsd_state_compute(tsd_t *tsd) {
	if (!tsd_nominal(tsd)) {
		return tsd_state_get(tsd);
	}
	/* We're in *a* nominal state; but which one? */
	if (malloc_slow || tsd_local_slow(tsd) || tsd_global_slow()) {
		return tsd_state_nominal_slow;
	} else {
		return tsd_state_nominal;
	}
}

void
tsd_slow_update(tsd_t *tsd) {
	uint8_t old_state;
	do {
		uint8_t new_state = tsd_state_compute(tsd);
		old_state = tsd_atomic_exchange(&tsd->state, new_state,
		    ATOMIC_ACQUIRE);
	} while (old_state == tsd_state_nominal_recompute);
}

void
tsd_state_set(tsd_t *tsd, uint8_t new_state) {
	/* Only the tsd module can change the state *to* recompute. */
	assert(new_state != tsd_state_nominal_recompute);
	uint8_t old_state = tsd_atomic_load(&tsd->state, ATOMIC_RELAXED);
	if (old_state > tsd_state_nominal_max) {
		/*
		 * Not currently in the nominal list, but it might need to be
		 * inserted there.
		 */
		assert(!tsd_in_nominal_list(tsd));
		tsd_atomic_store(&tsd->state, new_state, ATOMIC_RELAXED);
		if (new_state <= tsd_state_nominal_max) {
			tsd_add_nominal(tsd);
		}
	} else {
		/*
		 * We're currently nominal.  If the new state is non-nominal,
		 * great; we take ourselves off the list and just enter the new
		 * state.
		 */
		assert(tsd_in_nominal_list(tsd));
		if (new_state > tsd_state_nominal_max) {
			tsd_remove_nominal(tsd);
			tsd_atomic_store(&tsd->state, new_state,
			    ATOMIC_RELAXED);
		} else {
			/*
			 * This is the tricky case.  We're transitioning from
			 * one nominal state to another.  The caller can't know
			 * about any races that are occuring at the same time,
			 * so we always have to recompute no matter what.
			 */
			tsd_slow_update(tsd);
		}
	}
}

static bool
tsd_data_init(tsd_t *tsd) {
	/*
	 * We initialize the rtree context first (before the tcache), since the
	 * tcache initialization depends on it.
	 */
	rtree_ctx_data_init(tsd_rtree_ctxp_get_unsafe(tsd));

	/*
	 * A nondeterministic seed based on the address of tsd reduces
	 * the likelihood of lockstep non-uniform cache index
	 * utilization among identical concurrent processes, but at the
	 * cost of test repeatability.  For debug builds, instead use a
	 * deterministic seed.
	 */
	*tsd_offset_statep_get(tsd) = config_debug ? 0 :
	    (uint64_t)(uintptr_t)tsd;

	return tsd_tcache_enabled_data_init(tsd);
}

static void
assert_tsd_data_cleanup_done(tsd_t *tsd) {
	assert(!tsd_nominal(tsd));
	assert(!tsd_in_nominal_list(tsd));
	assert(*tsd_arenap_get_unsafe(tsd) == NULL);
	assert(*tsd_iarenap_get_unsafe(tsd) == NULL);
	assert(*tsd_arenas_tdata_bypassp_get_unsafe(tsd) == true);
	assert(*tsd_arenas_tdatap_get_unsafe(tsd) == NULL);
	assert(*tsd_tcache_enabledp_get_unsafe(tsd) == false);
	assert(*tsd_prof_tdatap_get_unsafe(tsd) == NULL);
}

static bool
tsd_data_init_nocleanup(tsd_t *tsd) {
	assert(tsd_state_get(tsd) == tsd_state_reincarnated ||
	    tsd_state_get(tsd) == tsd_state_minimal_initialized);
	/*
	 * During reincarnation, there is no guarantee that the cleanup function
	 * will be called (deallocation may happen after all tsd destructors).
	 * We set up tsd in a way that no cleanup is needed.
	 */
	rtree_ctx_data_init(tsd_rtree_ctxp_get_unsafe(tsd));
	*tsd_arenas_tdata_bypassp_get(tsd) = true;
	*tsd_tcache_enabledp_get_unsafe(tsd) = false;
	*tsd_reentrancy_levelp_get(tsd) = 1;
	assert_tsd_data_cleanup_done(tsd);

	return false;
}

tsd_t *
tsd_fetch_slow(tsd_t *tsd, bool minimal) {
	assert(!tsd_fast(tsd));

	if (tsd_state_get(tsd) == tsd_state_nominal_slow) {
		/*
		 * On slow path but no work needed.  Note that we can't
		 * necessarily *assert* that we're slow, because we might be
		 * slow because of an asynchronous modification to global state,
		 * which might be asynchronously modified *back*.
		 */
	} else if (tsd_state_get(tsd) == tsd_state_nominal_recompute) {
		tsd_slow_update(tsd);
	} else if (tsd_state_get(tsd) == tsd_state_uninitialized) {
		if (!minimal) {
			if (tsd_booted) {
				tsd_state_set(tsd, tsd_state_nominal);
				tsd_slow_update(tsd);
				/* Trigger cleanup handler registration. */
				tsd_set(tsd);
				tsd_data_init(tsd);
			}
		} else {
			tsd_state_set(tsd, tsd_state_minimal_initialized);
			tsd_set(tsd);
			tsd_data_init_nocleanup(tsd);
		}
	} else if (tsd_state_get(tsd) == tsd_state_minimal_initialized) {
		if (!minimal) {
			/* Switch to fully initialized. */
			tsd_state_set(tsd, tsd_state_nominal);
			assert(*tsd_reentrancy_levelp_get(tsd) >= 1);
			(*tsd_reentrancy_levelp_get(tsd))--;
			tsd_slow_update(tsd);
			tsd_data_init(tsd);
		} else {
			assert_tsd_data_cleanup_done(tsd);
		}
	} else if (tsd_state_get(tsd) == tsd_state_purgatory) {
		tsd_state_set(tsd, tsd_state_reincarnated);
		tsd_set(tsd);
		tsd_data_init_nocleanup(tsd);
	} else {
		assert(tsd_state_get(tsd) == tsd_state_reincarnated);
	}

	return tsd;
}

void *
malloc_tsd_malloc(size_t size) {
	return a0malloc(CACHELINE_CEILING(size));
}

void
malloc_tsd_dalloc(void *wrapper) {
	a0dalloc(wrapper);
}

#if defined(JEMALLOC_MALLOC_THREAD_CLEANUP) || defined(_WIN32)
#ifndef _WIN32
JEMALLOC_EXPORT
#endif
void
_malloc_thread_cleanup(void) {
	bool pending[MALLOC_TSD_CLEANUPS_MAX], again;
	unsigned i;

	for (i = 0; i < ncleanups; i++) {
		pending[i] = true;
	}

	do {
		again = false;
		for (i = 0; i < ncleanups; i++) {
			if (pending[i]) {
				pending[i] = cleanups[i]();
				if (pending[i]) {
					again = true;
				}
			}
		}
	} while (again);
}
#endif

void
malloc_tsd_cleanup_register(bool (*f)(void)) {
	assert(ncleanups < MALLOC_TSD_CLEANUPS_MAX);
	cleanups[ncleanups] = f;
	ncleanups++;
}

static void
tsd_do_data_cleanup(tsd_t *tsd) {
	prof_tdata_cleanup(tsd);
	iarena_cleanup(tsd);
	arena_cleanup(tsd);
	arenas_tdata_cleanup(tsd);
	tcache_cleanup(tsd);
	witnesses_cleanup(tsd_witness_tsdp_get_unsafe(tsd));
}

void
tsd_cleanup(void *arg) {
	tsd_t *tsd = (tsd_t *)arg;

	switch (tsd_state_get(tsd)) {
	case tsd_state_uninitialized:
		/* Do nothing. */
		break;
	case tsd_state_minimal_initialized:
		/* This implies the thread only did free() in its life time. */
		/* Fall through. */
	case tsd_state_reincarnated:
		/*
		 * Reincarnated means another destructor deallocated memory
		 * after the destructor was called.  Cleanup isn't required but
		 * is still called for testing and completeness.
		 */
		assert_tsd_data_cleanup_done(tsd);
		/* Fall through. */
	case tsd_state_nominal:
	case tsd_state_nominal_slow:
		tsd_do_data_cleanup(tsd);
		tsd_state_set(tsd, tsd_state_purgatory);
		tsd_set(tsd);
		break;
	case tsd_state_purgatory:
		/*
		 * The previous time this destructor was called, we set the
		 * state to tsd_state_purgatory so that other destructors
		 * wouldn't cause re-creation of the tsd.  This time, do
		 * nothing, and do not request another callback.
		 */
		break;
	default:
		not_reached();
	}
#ifdef JEMALLOC_JET
	test_callback_t test_callback = *tsd_test_callbackp_get_unsafe(tsd);
	int *data = tsd_test_datap_get_unsafe(tsd);
	if (test_callback != NULL) {
		test_callback(data);
	}
#endif
}

tsd_t *
malloc_tsd_boot0(void) {
	tsd_t *tsd;

	ncleanups = 0;
	if (malloc_mutex_init(&tsd_nominal_tsds_lock, "tsd_nominal_tsds_lock",
	    WITNESS_RANK_OMIT, malloc_mutex_rank_exclusive)) {
		return NULL;
	}
	if (tsd_boot0()) {
		return NULL;
	}
	tsd = tsd_fetch();
	*tsd_arenas_tdata_bypassp_get(tsd) = true;
	return tsd;
}

void
malloc_tsd_boot1(void) {
	tsd_boot1();
	tsd_t *tsd = tsd_fetch();
	/* malloc_slow has been set properly.  Update tsd_slow. */
	tsd_slow_update(tsd);
	*tsd_arenas_tdata_bypassp_get(tsd) = false;
}

#ifdef _WIN32
static BOOL WINAPI
_tls_callback(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	switch (fdwReason) {
#ifdef JEMALLOC_LAZY_LOCK
	case DLL_THREAD_ATTACH:
		isthreaded = true;
		break;
#endif
	case DLL_THREAD_DETACH:
		_malloc_thread_cleanup();
		break;
	default:
		break;
	}
	return true;
}

/*
 * We need to be able to say "read" here (in the "pragma section"), but have
 * hooked "read". We won't read for the rest of the file, so we can get away
 * with unhooking.
 */
#ifdef read
#  undef read
#endif

#ifdef _MSC_VER
#  ifdef _M_IX86
#    pragma comment(linker, "/INCLUDE:__tls_used")
#    pragma comment(linker, "/INCLUDE:_tls_callback")
#  else
#    pragma comment(linker, "/INCLUDE:_tls_used")
#    pragma comment(linker, "/INCLUDE:" STRINGIFY(tls_callback) )
#  endif
#  pragma section(".CRT$XLY",long,read)
#endif
JEMALLOC_SECTION(".CRT$XLY") JEMALLOC_ATTR(used)
BOOL	(WINAPI *const tls_callback)(HINSTANCE hinstDLL,
    DWORD fdwReason, LPVOID lpvReserved) = _tls_callback;
#endif

#if (!defined(JEMALLOC_MALLOC_THREAD_CLEANUP) && !defined(JEMALLOC_TLS) && \
    !defined(_WIN32))
void *
tsd_init_check_recursion(tsd_init_head_t *head, tsd_init_block_t *block) {
	pthread_t self = pthread_self();
	tsd_init_block_t *iter;

	/* Check whether this thread has already inserted into the list. */
	malloc_mutex_lock(TSDN_NULL, &head->lock);
	ql_foreach(iter, &head->blocks, link) {
		if (iter->thread == self) {
			malloc_mutex_unlock(TSDN_NULL, &head->lock);
			return iter->data;
		}
	}
	/* Insert block into list. */
	ql_elm_new(block, link);
	block->thread = self;
	ql_tail_insert(&head->blocks, block, link);
	malloc_mutex_unlock(TSDN_NULL, &head->lock);
	return NULL;
}

void
tsd_init_finish(tsd_init_head_t *head, tsd_init_block_t *block) {
	malloc_mutex_lock(TSDN_NULL, &head->lock);
	ql_remove(&head->blocks, block, link);
	malloc_mutex_unlock(TSDN_NULL, &head->lock);
}
#endif

void
tsd_prefork(tsd_t *tsd) {
	malloc_mutex_prefork(tsd_tsdn(tsd), &tsd_nominal_tsds_lock);
}

void
tsd_postfork_parent(tsd_t *tsd) {
	malloc_mutex_postfork_parent(tsd_tsdn(tsd), &tsd_nominal_tsds_lock);
}

void
tsd_postfork_child(tsd_t *tsd) {
	malloc_mutex_postfork_child(tsd_tsdn(tsd), &tsd_nominal_tsds_lock);
	ql_new(&tsd_nominal_tsds);

	if (tsd_state_get(tsd) <= tsd_state_nominal_max) {
		tsd_add_nominal(tsd);
	}
}
