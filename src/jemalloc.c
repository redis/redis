#define JEMALLOC_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/hook.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/log.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/spin.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/ticker.h"
#include "jemalloc/internal/util.h"

/******************************************************************************/
/* Data. */

/* Runtime configuration options. */
const char	*je_malloc_conf
#ifndef _WIN32
    JEMALLOC_ATTR(weak)
#endif
    ;
bool	opt_abort =
#ifdef JEMALLOC_DEBUG
    true
#else
    false
#endif
    ;
bool	opt_abort_conf =
#ifdef JEMALLOC_DEBUG
    true
#else
    false
#endif
    ;
/* Intentionally default off, even with debug builds. */
bool	opt_confirm_conf = false;
const char	*opt_junk =
#if (defined(JEMALLOC_DEBUG) && defined(JEMALLOC_FILL))
    "true"
#else
    "false"
#endif
    ;
bool	opt_junk_alloc =
#if (defined(JEMALLOC_DEBUG) && defined(JEMALLOC_FILL))
    true
#else
    false
#endif
    ;
bool	opt_junk_free =
#if (defined(JEMALLOC_DEBUG) && defined(JEMALLOC_FILL))
    true
#else
    false
#endif
    ;

bool	opt_utrace = false;
bool	opt_xmalloc = false;
bool	opt_zero = false;
unsigned	opt_narenas = 0;

unsigned	ncpus;

/* Protects arenas initialization. */
malloc_mutex_t arenas_lock;
/*
 * Arenas that are used to service external requests.  Not all elements of the
 * arenas array are necessarily used; arenas are created lazily as needed.
 *
 * arenas[0..narenas_auto) are used for automatic multiplexing of threads and
 * arenas.  arenas[narenas_auto..narenas_total) are only used if the application
 * takes some action to create them and allocate from them.
 *
 * Points to an arena_t.
 */
JEMALLOC_ALIGNED(CACHELINE)
atomic_p_t		arenas[MALLOCX_ARENA_LIMIT];
static atomic_u_t	narenas_total; /* Use narenas_total_*(). */
/* Below three are read-only after initialization. */
static arena_t		*a0; /* arenas[0]. */
unsigned		narenas_auto;
unsigned		manual_arena_base;

typedef enum {
	malloc_init_uninitialized	= 3,
	malloc_init_a0_initialized	= 2,
	malloc_init_recursible		= 1,
	malloc_init_initialized		= 0 /* Common case --> jnz. */
} malloc_init_t;
static malloc_init_t	malloc_init_state = malloc_init_uninitialized;

/* False should be the common case.  Set to true to trigger initialization. */
bool			malloc_slow = true;

/* When malloc_slow is true, set the corresponding bits for sanity check. */
enum {
	flag_opt_junk_alloc	= (1U),
	flag_opt_junk_free	= (1U << 1),
	flag_opt_zero		= (1U << 2),
	flag_opt_utrace		= (1U << 3),
	flag_opt_xmalloc	= (1U << 4)
};
static uint8_t	malloc_slow_flags;

#ifdef JEMALLOC_THREADED_INIT
/* Used to let the initializing thread recursively allocate. */
#  define NO_INITIALIZER	((unsigned long)0)
#  define INITIALIZER		pthread_self()
#  define IS_INITIALIZER	(malloc_initializer == pthread_self())
static pthread_t		malloc_initializer = NO_INITIALIZER;
#else
#  define NO_INITIALIZER	false
#  define INITIALIZER		true
#  define IS_INITIALIZER	malloc_initializer
static bool			malloc_initializer = NO_INITIALIZER;
#endif

/* Used to avoid initialization races. */
#ifdef _WIN32
#if _WIN32_WINNT >= 0x0600
static malloc_mutex_t	init_lock = SRWLOCK_INIT;
#else
static malloc_mutex_t	init_lock;
static bool init_lock_initialized = false;

JEMALLOC_ATTR(constructor)
static void WINAPI
_init_init_lock(void) {
	/*
	 * If another constructor in the same binary is using mallctl to e.g.
	 * set up extent hooks, it may end up running before this one, and
	 * malloc_init_hard will crash trying to lock the uninitialized lock. So
	 * we force an initialization of the lock in malloc_init_hard as well.
	 * We don't try to care about atomicity of the accessed to the
	 * init_lock_initialized boolean, since it really only matters early in
	 * the process creation, before any separate thread normally starts
	 * doing anything.
	 */
	if (!init_lock_initialized) {
		malloc_mutex_init(&init_lock, "init", WITNESS_RANK_INIT,
		    malloc_mutex_rank_exclusive);
	}
	init_lock_initialized = true;
}

#ifdef _MSC_VER
#  pragma section(".CRT$XCU", read)
JEMALLOC_SECTION(".CRT$XCU") JEMALLOC_ATTR(used)
static const void (WINAPI *init_init_lock)(void) = _init_init_lock;
#endif
#endif
#else
static malloc_mutex_t	init_lock = MALLOC_MUTEX_INITIALIZER;
#endif

typedef struct {
	void	*p;	/* Input pointer (as in realloc(p, s)). */
	size_t	s;	/* Request size. */
	void	*r;	/* Result pointer. */
} malloc_utrace_t;

#ifdef JEMALLOC_UTRACE
#  define UTRACE(a, b, c) do {						\
	if (unlikely(opt_utrace)) {					\
		int utrace_serrno = errno;				\
		malloc_utrace_t ut;					\
		ut.p = (a);						\
		ut.s = (b);						\
		ut.r = (c);						\
		utrace(&ut, sizeof(ut));				\
		errno = utrace_serrno;					\
	}								\
} while (0)
#else
#  define UTRACE(a, b, c)
#endif

/* Whether encountered any invalid config options. */
static bool had_conf_error = false;

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static bool	malloc_init_hard_a0(void);
static bool	malloc_init_hard(void);

/******************************************************************************/
/*
 * Begin miscellaneous support functions.
 */

bool
malloc_initialized(void) {
	return (malloc_init_state == malloc_init_initialized);
}

JEMALLOC_ALWAYS_INLINE bool
malloc_init_a0(void) {
	if (unlikely(malloc_init_state == malloc_init_uninitialized)) {
		return malloc_init_hard_a0();
	}
	return false;
}

JEMALLOC_ALWAYS_INLINE bool
malloc_init(void) {
	if (unlikely(!malloc_initialized()) && malloc_init_hard()) {
		return true;
	}
	return false;
}

/*
 * The a0*() functions are used instead of i{d,}alloc() in situations that
 * cannot tolerate TLS variable access.
 */

static void *
a0ialloc(size_t size, bool zero, bool is_internal) {
	if (unlikely(malloc_init_a0())) {
		return NULL;
	}

	return iallocztm(TSDN_NULL, size, sz_size2index(size), zero, NULL,
	    is_internal, arena_get(TSDN_NULL, 0, true), true);
}

static void
a0idalloc(void *ptr, bool is_internal) {
	idalloctm(TSDN_NULL, ptr, NULL, NULL, is_internal, true);
}

void *
a0malloc(size_t size) {
	return a0ialloc(size, false, true);
}

void
a0dalloc(void *ptr) {
	a0idalloc(ptr, true);
}

/*
 * FreeBSD's libc uses the bootstrap_*() functions in bootstrap-senstive
 * situations that cannot tolerate TLS variable access (TLS allocation and very
 * early internal data structure initialization).
 */

void *
bootstrap_malloc(size_t size) {
	if (unlikely(size == 0)) {
		size = 1;
	}

	return a0ialloc(size, false, false);
}

void *
bootstrap_calloc(size_t num, size_t size) {
	size_t num_size;

	num_size = num * size;
	if (unlikely(num_size == 0)) {
		assert(num == 0 || size == 0);
		num_size = 1;
	}

	return a0ialloc(num_size, true, false);
}

void
bootstrap_free(void *ptr) {
	if (unlikely(ptr == NULL)) {
		return;
	}

	a0idalloc(ptr, false);
}

void
arena_set(unsigned ind, arena_t *arena) {
	atomic_store_p(&arenas[ind], arena, ATOMIC_RELEASE);
}

static void
narenas_total_set(unsigned narenas) {
	atomic_store_u(&narenas_total, narenas, ATOMIC_RELEASE);
}

static void
narenas_total_inc(void) {
	atomic_fetch_add_u(&narenas_total, 1, ATOMIC_RELEASE);
}

unsigned
narenas_total_get(void) {
	return atomic_load_u(&narenas_total, ATOMIC_ACQUIRE);
}

/* Create a new arena and insert it into the arenas array at index ind. */
static arena_t *
arena_init_locked(tsdn_t *tsdn, unsigned ind, extent_hooks_t *extent_hooks) {
	arena_t *arena;

	assert(ind <= narenas_total_get());
	if (ind >= MALLOCX_ARENA_LIMIT) {
		return NULL;
	}
	if (ind == narenas_total_get()) {
		narenas_total_inc();
	}

	/*
	 * Another thread may have already initialized arenas[ind] if it's an
	 * auto arena.
	 */
	arena = arena_get(tsdn, ind, false);
	if (arena != NULL) {
		assert(arena_is_auto(arena));
		return arena;
	}

	/* Actually initialize the arena. */
	arena = arena_new(tsdn, ind, extent_hooks);

	return arena;
}

static void
arena_new_create_background_thread(tsdn_t *tsdn, unsigned ind) {
	if (ind == 0) {
		return;
	}
	/*
	 * Avoid creating a new background thread just for the huge arena, which
	 * purges eagerly by default.
	 */
	if (have_background_thread && !arena_is_huge(ind)) {
		if (background_thread_create(tsdn_tsd(tsdn), ind)) {
			malloc_printf("<jemalloc>: error in background thread "
				      "creation for arena %u. Abort.\n", ind);
			abort();
		}
	}
}

arena_t *
arena_init(tsdn_t *tsdn, unsigned ind, extent_hooks_t *extent_hooks) {
	arena_t *arena;

	malloc_mutex_lock(tsdn, &arenas_lock);
	arena = arena_init_locked(tsdn, ind, extent_hooks);
	malloc_mutex_unlock(tsdn, &arenas_lock);

	arena_new_create_background_thread(tsdn, ind);

	return arena;
}

static void
arena_bind(tsd_t *tsd, unsigned ind, bool internal) {
	arena_t *arena = arena_get(tsd_tsdn(tsd), ind, false);
	arena_nthreads_inc(arena, internal);

	if (internal) {
		tsd_iarena_set(tsd, arena);
	} else {
		tsd_arena_set(tsd, arena);
		unsigned shard = atomic_fetch_add_u(&arena->binshard_next, 1,
		    ATOMIC_RELAXED);
		tsd_binshards_t *bins = tsd_binshardsp_get(tsd);
		for (unsigned i = 0; i < SC_NBINS; i++) {
			assert(bin_infos[i].n_shards > 0 &&
			    bin_infos[i].n_shards <= BIN_SHARDS_MAX);
			bins->binshard[i] = shard % bin_infos[i].n_shards;
		}
	}
}

void
arena_migrate(tsd_t *tsd, unsigned oldind, unsigned newind) {
	arena_t *oldarena, *newarena;

	oldarena = arena_get(tsd_tsdn(tsd), oldind, false);
	newarena = arena_get(tsd_tsdn(tsd), newind, false);
	arena_nthreads_dec(oldarena, false);
	arena_nthreads_inc(newarena, false);
	tsd_arena_set(tsd, newarena);
}

static void
arena_unbind(tsd_t *tsd, unsigned ind, bool internal) {
	arena_t *arena;

	arena = arena_get(tsd_tsdn(tsd), ind, false);
	arena_nthreads_dec(arena, internal);

	if (internal) {
		tsd_iarena_set(tsd, NULL);
	} else {
		tsd_arena_set(tsd, NULL);
	}
}

arena_tdata_t *
arena_tdata_get_hard(tsd_t *tsd, unsigned ind) {
	arena_tdata_t *tdata, *arenas_tdata_old;
	arena_tdata_t *arenas_tdata = tsd_arenas_tdata_get(tsd);
	unsigned narenas_tdata_old, i;
	unsigned narenas_tdata = tsd_narenas_tdata_get(tsd);
	unsigned narenas_actual = narenas_total_get();

	/*
	 * Dissociate old tdata array (and set up for deallocation upon return)
	 * if it's too small.
	 */
	if (arenas_tdata != NULL && narenas_tdata < narenas_actual) {
		arenas_tdata_old = arenas_tdata;
		narenas_tdata_old = narenas_tdata;
		arenas_tdata = NULL;
		narenas_tdata = 0;
		tsd_arenas_tdata_set(tsd, arenas_tdata);
		tsd_narenas_tdata_set(tsd, narenas_tdata);
	} else {
		arenas_tdata_old = NULL;
		narenas_tdata_old = 0;
	}

	/* Allocate tdata array if it's missing. */
	if (arenas_tdata == NULL) {
		bool *arenas_tdata_bypassp = tsd_arenas_tdata_bypassp_get(tsd);
		narenas_tdata = (ind < narenas_actual) ? narenas_actual : ind+1;

		if (tsd_nominal(tsd) && !*arenas_tdata_bypassp) {
			*arenas_tdata_bypassp = true;
			arenas_tdata = (arena_tdata_t *)a0malloc(
			    sizeof(arena_tdata_t) * narenas_tdata);
			*arenas_tdata_bypassp = false;
		}
		if (arenas_tdata == NULL) {
			tdata = NULL;
			goto label_return;
		}
		assert(tsd_nominal(tsd) && !*arenas_tdata_bypassp);
		tsd_arenas_tdata_set(tsd, arenas_tdata);
		tsd_narenas_tdata_set(tsd, narenas_tdata);
	}

	/*
	 * Copy to tdata array.  It's possible that the actual number of arenas
	 * has increased since narenas_total_get() was called above, but that
	 * causes no correctness issues unless two threads concurrently execute
	 * the arenas.create mallctl, which we trust mallctl synchronization to
	 * prevent.
	 */

	/* Copy/initialize tickers. */
	for (i = 0; i < narenas_actual; i++) {
		if (i < narenas_tdata_old) {
			ticker_copy(&arenas_tdata[i].decay_ticker,
			    &arenas_tdata_old[i].decay_ticker);
		} else {
			ticker_init(&arenas_tdata[i].decay_ticker,
			    DECAY_NTICKS_PER_UPDATE);
		}
	}
	if (narenas_tdata > narenas_actual) {
		memset(&arenas_tdata[narenas_actual], 0, sizeof(arena_tdata_t)
		    * (narenas_tdata - narenas_actual));
	}

	/* Read the refreshed tdata array. */
	tdata = &arenas_tdata[ind];
label_return:
	if (arenas_tdata_old != NULL) {
		a0dalloc(arenas_tdata_old);
	}
	return tdata;
}

/* Slow path, called only by arena_choose(). */
arena_t *
arena_choose_hard(tsd_t *tsd, bool internal) {
	arena_t *ret JEMALLOC_CC_SILENCE_INIT(NULL);

	if (have_percpu_arena && PERCPU_ARENA_ENABLED(opt_percpu_arena)) {
		unsigned choose = percpu_arena_choose();
		ret = arena_get(tsd_tsdn(tsd), choose, true);
		assert(ret != NULL);
		arena_bind(tsd, arena_ind_get(ret), false);
		arena_bind(tsd, arena_ind_get(ret), true);

		return ret;
	}

	if (narenas_auto > 1) {
		unsigned i, j, choose[2], first_null;
		bool is_new_arena[2];

		/*
		 * Determine binding for both non-internal and internal
		 * allocation.
		 *
		 *   choose[0]: For application allocation.
		 *   choose[1]: For internal metadata allocation.
		 */

		for (j = 0; j < 2; j++) {
			choose[j] = 0;
			is_new_arena[j] = false;
		}

		first_null = narenas_auto;
		malloc_mutex_lock(tsd_tsdn(tsd), &arenas_lock);
		assert(arena_get(tsd_tsdn(tsd), 0, false) != NULL);
		for (i = 1; i < narenas_auto; i++) {
			if (arena_get(tsd_tsdn(tsd), i, false) != NULL) {
				/*
				 * Choose the first arena that has the lowest
				 * number of threads assigned to it.
				 */
				for (j = 0; j < 2; j++) {
					if (arena_nthreads_get(arena_get(
					    tsd_tsdn(tsd), i, false), !!j) <
					    arena_nthreads_get(arena_get(
					    tsd_tsdn(tsd), choose[j], false),
					    !!j)) {
						choose[j] = i;
					}
				}
			} else if (first_null == narenas_auto) {
				/*
				 * Record the index of the first uninitialized
				 * arena, in case all extant arenas are in use.
				 *
				 * NB: It is possible for there to be
				 * discontinuities in terms of initialized
				 * versus uninitialized arenas, due to the
				 * "thread.arena" mallctl.
				 */
				first_null = i;
			}
		}

		for (j = 0; j < 2; j++) {
			if (arena_nthreads_get(arena_get(tsd_tsdn(tsd),
			    choose[j], false), !!j) == 0 || first_null ==
			    narenas_auto) {
				/*
				 * Use an unloaded arena, or the least loaded
				 * arena if all arenas are already initialized.
				 */
				if (!!j == internal) {
					ret = arena_get(tsd_tsdn(tsd),
					    choose[j], false);
				}
			} else {
				arena_t *arena;

				/* Initialize a new arena. */
				choose[j] = first_null;
				arena = arena_init_locked(tsd_tsdn(tsd),
				    choose[j],
				    (extent_hooks_t *)&extent_hooks_default);
				if (arena == NULL) {
					malloc_mutex_unlock(tsd_tsdn(tsd),
					    &arenas_lock);
					return NULL;
				}
				is_new_arena[j] = true;
				if (!!j == internal) {
					ret = arena;
				}
			}
			arena_bind(tsd, choose[j], !!j);
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &arenas_lock);

		for (j = 0; j < 2; j++) {
			if (is_new_arena[j]) {
				assert(choose[j] > 0);
				arena_new_create_background_thread(
				    tsd_tsdn(tsd), choose[j]);
			}
		}

	} else {
		ret = arena_get(tsd_tsdn(tsd), 0, false);
		arena_bind(tsd, 0, false);
		arena_bind(tsd, 0, true);
	}

	return ret;
}

void
iarena_cleanup(tsd_t *tsd) {
	arena_t *iarena;

	iarena = tsd_iarena_get(tsd);
	if (iarena != NULL) {
		arena_unbind(tsd, arena_ind_get(iarena), true);
	}
}

void
arena_cleanup(tsd_t *tsd) {
	arena_t *arena;

	arena = tsd_arena_get(tsd);
	if (arena != NULL) {
		arena_unbind(tsd, arena_ind_get(arena), false);
	}
}

void
arenas_tdata_cleanup(tsd_t *tsd) {
	arena_tdata_t *arenas_tdata;

	/* Prevent tsd->arenas_tdata from being (re)created. */
	*tsd_arenas_tdata_bypassp_get(tsd) = true;

	arenas_tdata = tsd_arenas_tdata_get(tsd);
	if (arenas_tdata != NULL) {
		tsd_arenas_tdata_set(tsd, NULL);
		a0dalloc(arenas_tdata);
	}
}

static void
stats_print_atexit(void) {
	if (config_stats) {
		tsdn_t *tsdn;
		unsigned narenas, i;

		tsdn = tsdn_fetch();

		/*
		 * Merge stats from extant threads.  This is racy, since
		 * individual threads do not lock when recording tcache stats
		 * events.  As a consequence, the final stats may be slightly
		 * out of date by the time they are reported, if other threads
		 * continue to allocate.
		 */
		for (i = 0, narenas = narenas_total_get(); i < narenas; i++) {
			arena_t *arena = arena_get(tsdn, i, false);
			if (arena != NULL) {
				tcache_t *tcache;

				malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
				ql_foreach(tcache, &arena->tcache_ql, link) {
					tcache_stats_merge(tsdn, tcache, arena);
				}
				malloc_mutex_unlock(tsdn,
				    &arena->tcache_ql_mtx);
			}
		}
	}
	je_malloc_stats_print(NULL, NULL, opt_stats_print_opts);
}

/*
 * Ensure that we don't hold any locks upon entry to or exit from allocator
 * code (in a "broad" sense that doesn't count a reentrant allocation as an
 * entrance or exit).
 */
JEMALLOC_ALWAYS_INLINE void
check_entry_exit_locking(tsdn_t *tsdn) {
	if (!config_debug) {
		return;
	}
	if (tsdn_null(tsdn)) {
		return;
	}
	tsd_t *tsd = tsdn_tsd(tsdn);
	/*
	 * It's possible we hold locks at entry/exit if we're in a nested
	 * allocation.
	 */
	int8_t reentrancy_level = tsd_reentrancy_level_get(tsd);
	if (reentrancy_level != 0) {
		return;
	}
	witness_assert_lockless(tsdn_witness_tsdp_get(tsdn));
}

/*
 * End miscellaneous support functions.
 */
/******************************************************************************/
/*
 * Begin initialization functions.
 */

static char *
jemalloc_secure_getenv(const char *name) {
#ifdef JEMALLOC_HAVE_SECURE_GETENV
	return secure_getenv(name);
#else
#  ifdef JEMALLOC_HAVE_ISSETUGID
	if (issetugid() != 0) {
		return NULL;
	}
#  endif
	return getenv(name);
#endif
}

static unsigned
malloc_ncpus(void) {
	long result;

#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	result = si.dwNumberOfProcessors;
#elif defined(JEMALLOC_GLIBC_MALLOC_HOOK) && defined(CPU_COUNT)
	/*
	 * glibc >= 2.6 has the CPU_COUNT macro.
	 *
	 * glibc's sysconf() uses isspace().  glibc allocates for the first time
	 * *before* setting up the isspace tables.  Therefore we need a
	 * different method to get the number of CPUs.
	 */
	{
		cpu_set_t set;

		pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
		result = CPU_COUNT(&set);
	}
#else
	result = sysconf(_SC_NPROCESSORS_ONLN);
#endif
	return ((result == -1) ? 1 : (unsigned)result);
}

static void
init_opt_stats_print_opts(const char *v, size_t vlen) {
	size_t opts_len = strlen(opt_stats_print_opts);
	assert(opts_len <= stats_print_tot_num_options);

	for (size_t i = 0; i < vlen; i++) {
		switch (v[i]) {
#define OPTION(o, v, d, s) case o: break;
			STATS_PRINT_OPTIONS
#undef OPTION
		default: continue;
		}

		if (strchr(opt_stats_print_opts, v[i]) != NULL) {
			/* Ignore repeated. */
			continue;
		}

		opt_stats_print_opts[opts_len++] = v[i];
		opt_stats_print_opts[opts_len] = '\0';
		assert(opts_len <= stats_print_tot_num_options);
	}
	assert(opts_len == strlen(opt_stats_print_opts));
}

/* Reads the next size pair in a multi-sized option. */
static bool
malloc_conf_multi_sizes_next(const char **slab_size_segment_cur,
    size_t *vlen_left, size_t *slab_start, size_t *slab_end, size_t *new_size) {
	const char *cur = *slab_size_segment_cur;
	char *end;
	uintmax_t um;

	set_errno(0);

	/* First number, then '-' */
	um = malloc_strtoumax(cur, &end, 0);
	if (get_errno() != 0 || *end != '-') {
		return true;
	}
	*slab_start = (size_t)um;
	cur = end + 1;

	/* Second number, then ':' */
	um = malloc_strtoumax(cur, &end, 0);
	if (get_errno() != 0 || *end != ':') {
		return true;
	}
	*slab_end = (size_t)um;
	cur = end + 1;

	/* Last number */
	um = malloc_strtoumax(cur, &end, 0);
	if (get_errno() != 0) {
		return true;
	}
	*new_size = (size_t)um;

	/* Consume the separator if there is one. */
	if (*end == '|') {
		end++;
	}

	*vlen_left -= end - *slab_size_segment_cur;
	*slab_size_segment_cur = end;

	return false;
}

static bool
malloc_conf_next(char const **opts_p, char const **k_p, size_t *klen_p,
    char const **v_p, size_t *vlen_p) {
	bool accept;
	const char *opts = *opts_p;

	*k_p = opts;

	for (accept = false; !accept;) {
		switch (*opts) {
		case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
		case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
		case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
		case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
		case 'Y': case 'Z':
		case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
		case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
		case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
		case 's': case 't': case 'u': case 'v': case 'w': case 'x':
		case 'y': case 'z':
		case '0': case '1': case '2': case '3': case '4': case '5':
		case '6': case '7': case '8': case '9':
		case '_':
			opts++;
			break;
		case ':':
			opts++;
			*klen_p = (uintptr_t)opts - 1 - (uintptr_t)*k_p;
			*v_p = opts;
			accept = true;
			break;
		case '\0':
			if (opts != *opts_p) {
				malloc_write("<jemalloc>: Conf string ends "
				    "with key\n");
			}
			return true;
		default:
			malloc_write("<jemalloc>: Malformed conf string\n");
			return true;
		}
	}

	for (accept = false; !accept;) {
		switch (*opts) {
		case ',':
			opts++;
			/*
			 * Look ahead one character here, because the next time
			 * this function is called, it will assume that end of
			 * input has been cleanly reached if no input remains,
			 * but we have optimistically already consumed the
			 * comma if one exists.
			 */
			if (*opts == '\0') {
				malloc_write("<jemalloc>: Conf string ends "
				    "with comma\n");
			}
			*vlen_p = (uintptr_t)opts - 1 - (uintptr_t)*v_p;
			accept = true;
			break;
		case '\0':
			*vlen_p = (uintptr_t)opts - (uintptr_t)*v_p;
			accept = true;
			break;
		default:
			opts++;
			break;
		}
	}

	*opts_p = opts;
	return false;
}

static void
malloc_abort_invalid_conf(void) {
	assert(opt_abort_conf);
	malloc_printf("<jemalloc>: Abort (abort_conf:true) on invalid conf "
	    "value (see above).\n");
	abort();
}

static void
malloc_conf_error(const char *msg, const char *k, size_t klen, const char *v,
    size_t vlen) {
	malloc_printf("<jemalloc>: %s: %.*s:%.*s\n", msg, (int)klen, k,
	    (int)vlen, v);
	/* If abort_conf is set, error out after processing all options. */
	const char *experimental = "experimental_";
	if (strncmp(k, experimental, strlen(experimental)) == 0) {
		/* However, tolerate experimental features. */
		return;
	}
	had_conf_error = true;
}

static void
malloc_slow_flag_init(void) {
	/*
	 * Combine the runtime options into malloc_slow for fast path.  Called
	 * after processing all the options.
	 */
	malloc_slow_flags |= (opt_junk_alloc ? flag_opt_junk_alloc : 0)
	    | (opt_junk_free ? flag_opt_junk_free : 0)
	    | (opt_zero ? flag_opt_zero : 0)
	    | (opt_utrace ? flag_opt_utrace : 0)
	    | (opt_xmalloc ? flag_opt_xmalloc : 0);

	malloc_slow = (malloc_slow_flags != 0);
}

/* Number of sources for initializing malloc_conf */
#define MALLOC_CONF_NSOURCES 4

static const char *
obtain_malloc_conf(unsigned which_source, char buf[PATH_MAX + 1]) {
	if (config_debug) {
		static unsigned read_source = 0;
		/*
		 * Each source should only be read once, to minimize # of
		 * syscalls on init.
		 */
		assert(read_source++ == which_source);
	}
	assert(which_source < MALLOC_CONF_NSOURCES);

	const char *ret;
	switch (which_source) {
	case 0:
		ret = config_malloc_conf;
		break;
	case 1:
		if (je_malloc_conf != NULL) {
			/* Use options that were compiled into the program. */
			ret = je_malloc_conf;
		} else {
			/* No configuration specified. */
			ret = NULL;
		}
		break;
	case 2: {
		ssize_t linklen = 0;
#ifndef _WIN32
		int saved_errno = errno;
		const char *linkname =
#  ifdef JEMALLOC_PREFIX
		    "/etc/"JEMALLOC_PREFIX"malloc.conf"
#  else
		    "/etc/malloc.conf"
#  endif
		    ;

		/*
		 * Try to use the contents of the "/etc/malloc.conf" symbolic
		 * link's name.
		 */
#ifndef JEMALLOC_READLINKAT
		linklen = readlink(linkname, buf, PATH_MAX);
#else
		linklen = readlinkat(AT_FDCWD, linkname, buf, PATH_MAX);
#endif
		if (linklen == -1) {
			/* No configuration specified. */
			linklen = 0;
			/* Restore errno. */
			set_errno(saved_errno);
		}
#endif
		buf[linklen] = '\0';
		ret = buf;
		break;
	} case 3: {
		const char *envname =
#ifdef JEMALLOC_PREFIX
		    JEMALLOC_CPREFIX"MALLOC_CONF"
#else
		    "MALLOC_CONF"
#endif
		    ;

		if ((ret = jemalloc_secure_getenv(envname)) != NULL) {
			/*
			 * Do nothing; opts is already initialized to the value
			 * of the MALLOC_CONF environment variable.
			 */
		} else {
			/* No configuration specified. */
			ret = NULL;
		}
		break;
	} default:
		not_reached();
		ret = NULL;
	}
	return ret;
}

static void
malloc_conf_init_helper(sc_data_t *sc_data, unsigned bin_shard_sizes[SC_NBINS],
    bool initial_call, const char *opts_cache[MALLOC_CONF_NSOURCES],
    char buf[PATH_MAX + 1]) {
	static const char *opts_explain[MALLOC_CONF_NSOURCES] = {
		"string specified via --with-malloc-conf",
		"string pointed to by the global variable malloc_conf",
		"\"name\" of the file referenced by the symbolic link named "
		    "/etc/malloc.conf",
		"value of the environment variable MALLOC_CONF"
	};
	unsigned i;
	const char *opts, *k, *v;
	size_t klen, vlen;

	for (i = 0; i < MALLOC_CONF_NSOURCES; i++) {
		/* Get runtime configuration. */
		if (initial_call) {
			opts_cache[i] = obtain_malloc_conf(i, buf);
		}
		opts = opts_cache[i];
		if (!initial_call && opt_confirm_conf) {
			malloc_printf(
			    "<jemalloc>: malloc_conf #%u (%s): \"%s\"\n",
			    i + 1, opts_explain[i], opts != NULL ? opts : "");
		}
		if (opts == NULL) {
			continue;
		}

		while (*opts != '\0' && !malloc_conf_next(&opts, &k, &klen, &v,
		    &vlen)) {

#define CONF_ERROR(msg, k, klen, v, vlen)				\
			if (!initial_call) {				\
				malloc_conf_error(			\
				    msg, k, klen, v, vlen);		\
				cur_opt_valid = false;			\
			}
#define CONF_CONTINUE	{						\
				if (!initial_call && opt_confirm_conf	\
				    && cur_opt_valid) {			\
					malloc_printf("<jemalloc>: -- "	\
					    "Set conf value: %.*s:%.*s"	\
					    "\n", (int)klen, k,		\
					    (int)vlen, v);		\
				}					\
				continue;				\
			}
#define CONF_MATCH(n)							\
	(sizeof(n)-1 == klen && strncmp(n, k, klen) == 0)
#define CONF_MATCH_VALUE(n)						\
	(sizeof(n)-1 == vlen && strncmp(n, v, vlen) == 0)
#define CONF_HANDLE_BOOL(o, n)						\
			if (CONF_MATCH(n)) {				\
				if (CONF_MATCH_VALUE("true")) {		\
					o = true;			\
				} else if (CONF_MATCH_VALUE("false")) {	\
					o = false;			\
				} else {				\
					CONF_ERROR("Invalid conf value",\
					    k, klen, v, vlen);		\
				}					\
				CONF_CONTINUE;				\
			}
      /*
       * One of the CONF_MIN macros below expands, in one of the use points,
       * to "unsigned integer < 0", which is always false, triggering the
       * GCC -Wtype-limits warning, which we disable here and re-enable below.
       */
      JEMALLOC_DIAGNOSTIC_PUSH
      JEMALLOC_DIAGNOSTIC_IGNORE_TYPE_LIMITS

#define CONF_DONT_CHECK_MIN(um, min)	false
#define CONF_CHECK_MIN(um, min)	((um) < (min))
#define CONF_DONT_CHECK_MAX(um, max)	false
#define CONF_CHECK_MAX(um, max)	((um) > (max))
#define CONF_HANDLE_T_U(t, o, n, min, max, check_min, check_max, clip)	\
			if (CONF_MATCH(n)) {				\
				uintmax_t um;				\
				char *end;				\
									\
				set_errno(0);				\
				um = malloc_strtoumax(v, &end, 0);	\
				if (get_errno() != 0 || (uintptr_t)end -\
				    (uintptr_t)v != vlen) {		\
					CONF_ERROR("Invalid conf value",\
					    k, klen, v, vlen);		\
				} else if (clip) {			\
					if (check_min(um, (t)(min))) {	\
						o = (t)(min);		\
					} else if (			\
					    check_max(um, (t)(max))) {	\
						o = (t)(max);		\
					} else {			\
						o = (t)um;		\
					}				\
				} else {				\
					if (check_min(um, (t)(min)) ||	\
					    check_max(um, (t)(max))) {	\
						CONF_ERROR(		\
						    "Out-of-range "	\
						    "conf value",	\
						    k, klen, v, vlen);	\
					} else {			\
						o = (t)um;		\
					}				\
				}					\
				CONF_CONTINUE;				\
			}
#define CONF_HANDLE_UNSIGNED(o, n, min, max, check_min, check_max,	\
    clip)								\
			CONF_HANDLE_T_U(unsigned, o, n, min, max,	\
			    check_min, check_max, clip)
#define CONF_HANDLE_SIZE_T(o, n, min, max, check_min, check_max, clip)	\
			CONF_HANDLE_T_U(size_t, o, n, min, max,		\
			    check_min, check_max, clip)
#define CONF_HANDLE_SSIZE_T(o, n, min, max)				\
			if (CONF_MATCH(n)) {				\
				long l;					\
				char *end;				\
									\
				set_errno(0);				\
				l = strtol(v, &end, 0);			\
				if (get_errno() != 0 || (uintptr_t)end -\
				    (uintptr_t)v != vlen) {		\
					CONF_ERROR("Invalid conf value",\
					    k, klen, v, vlen);		\
				} else if (l < (ssize_t)(min) || l >	\
				    (ssize_t)(max)) {			\
					CONF_ERROR(			\
					    "Out-of-range conf value",	\
					    k, klen, v, vlen);		\
				} else {				\
					o = l;				\
				}					\
				CONF_CONTINUE;				\
			}
#define CONF_HANDLE_CHAR_P(o, n, d)					\
			if (CONF_MATCH(n)) {				\
				size_t cpylen = (vlen <=		\
				    sizeof(o)-1) ? vlen :		\
				    sizeof(o)-1;			\
				strncpy(o, v, cpylen);			\
				o[cpylen] = '\0';			\
				CONF_CONTINUE;				\
			}

			bool cur_opt_valid = true;

			CONF_HANDLE_BOOL(opt_confirm_conf, "confirm_conf")
			if (initial_call) {
				continue;
			}

			CONF_HANDLE_BOOL(opt_abort, "abort")
			CONF_HANDLE_BOOL(opt_abort_conf, "abort_conf")
			if (strncmp("metadata_thp", k, klen) == 0) {
				int i;
				bool match = false;
				for (i = 0; i < metadata_thp_mode_limit; i++) {
					if (strncmp(metadata_thp_mode_names[i],
					    v, vlen) == 0) {
						opt_metadata_thp = i;
						match = true;
						break;
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			CONF_HANDLE_BOOL(opt_retain, "retain")
			if (strncmp("dss", k, klen) == 0) {
				int i;
				bool match = false;
				for (i = 0; i < dss_prec_limit; i++) {
					if (strncmp(dss_prec_names[i], v, vlen)
					    == 0) {
						if (extent_dss_prec_set(i)) {
							CONF_ERROR(
							    "Error setting dss",
							    k, klen, v, vlen);
						} else {
							opt_dss =
							    dss_prec_names[i];
							match = true;
							break;
						}
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			CONF_HANDLE_UNSIGNED(opt_narenas, "narenas", 1,
			    UINT_MAX, CONF_CHECK_MIN, CONF_DONT_CHECK_MAX,
			    false)
			if (CONF_MATCH("bin_shards")) {
				const char *bin_shards_segment_cur = v;
				size_t vlen_left = vlen;
				do {
					size_t size_start;
					size_t size_end;
					size_t nshards;
					bool err = malloc_conf_multi_sizes_next(
					    &bin_shards_segment_cur, &vlen_left,
					    &size_start, &size_end, &nshards);
					if (err || bin_update_shard_size(
					    bin_shard_sizes, size_start,
					    size_end, nshards)) {
						CONF_ERROR(
						    "Invalid settings for "
						    "bin_shards", k, klen, v,
						    vlen);
						break;
					}
				} while (vlen_left > 0);
				CONF_CONTINUE;
			}
			CONF_HANDLE_SSIZE_T(opt_dirty_decay_ms,
			    "dirty_decay_ms", -1, NSTIME_SEC_MAX * KQU(1000) <
			    QU(SSIZE_MAX) ? NSTIME_SEC_MAX * KQU(1000) :
			    SSIZE_MAX);
			CONF_HANDLE_SSIZE_T(opt_muzzy_decay_ms,
			    "muzzy_decay_ms", -1, NSTIME_SEC_MAX * KQU(1000) <
			    QU(SSIZE_MAX) ? NSTIME_SEC_MAX * KQU(1000) :
			    SSIZE_MAX);
			CONF_HANDLE_BOOL(opt_stats_print, "stats_print")
			if (CONF_MATCH("stats_print_opts")) {
				init_opt_stats_print_opts(v, vlen);
				CONF_CONTINUE;
			}
			if (config_fill) {
				if (CONF_MATCH("junk")) {
					if (CONF_MATCH_VALUE("true")) {
						opt_junk = "true";
						opt_junk_alloc = opt_junk_free =
						    true;
					} else if (CONF_MATCH_VALUE("false")) {
						opt_junk = "false";
						opt_junk_alloc = opt_junk_free =
						    false;
					} else if (CONF_MATCH_VALUE("alloc")) {
						opt_junk = "alloc";
						opt_junk_alloc = true;
						opt_junk_free = false;
					} else if (CONF_MATCH_VALUE("free")) {
						opt_junk = "free";
						opt_junk_alloc = false;
						opt_junk_free = true;
					} else {
						CONF_ERROR(
						    "Invalid conf value",
						    k, klen, v, vlen);
					}
					CONF_CONTINUE;
				}
				CONF_HANDLE_BOOL(opt_zero, "zero")
			}
			if (config_utrace) {
				CONF_HANDLE_BOOL(opt_utrace, "utrace")
			}
			if (config_xmalloc) {
				CONF_HANDLE_BOOL(opt_xmalloc, "xmalloc")
			}
			CONF_HANDLE_BOOL(opt_tcache, "tcache")
			CONF_HANDLE_SSIZE_T(opt_lg_tcache_max, "lg_tcache_max",
			    -1, (sizeof(size_t) << 3) - 1)

			/*
			 * The runtime option of oversize_threshold remains
			 * undocumented.  It may be tweaked in the next major
			 * release (6.0).  The default value 8M is rather
			 * conservative / safe.  Tuning it further down may
			 * improve fragmentation a bit more, but may also cause
			 * contention on the huge arena.
			 */
			CONF_HANDLE_SIZE_T(opt_oversize_threshold,
			    "oversize_threshold", 0, SC_LARGE_MAXCLASS,
			    CONF_DONT_CHECK_MIN, CONF_CHECK_MAX, false)
			CONF_HANDLE_SIZE_T(opt_lg_extent_max_active_fit,
			    "lg_extent_max_active_fit", 0,
			    (sizeof(size_t) << 3), CONF_DONT_CHECK_MIN,
			    CONF_CHECK_MAX, false)

			if (strncmp("percpu_arena", k, klen) == 0) {
				bool match = false;
				for (int i = percpu_arena_mode_names_base; i <
				    percpu_arena_mode_names_limit; i++) {
					if (strncmp(percpu_arena_mode_names[i],
					    v, vlen) == 0) {
						if (!have_percpu_arena) {
							CONF_ERROR(
							    "No getcpu support",
							    k, klen, v, vlen);
						}
						opt_percpu_arena = i;
						match = true;
						break;
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			CONF_HANDLE_BOOL(opt_background_thread,
			    "background_thread");
			CONF_HANDLE_SIZE_T(opt_max_background_threads,
					   "max_background_threads", 1,
					   opt_max_background_threads,
					   CONF_CHECK_MIN, CONF_CHECK_MAX,
					   true);
			if (CONF_MATCH("slab_sizes")) {
				bool err;
				const char *slab_size_segment_cur = v;
				size_t vlen_left = vlen;
				do {
					size_t slab_start;
					size_t slab_end;
					size_t pgs;
					err = malloc_conf_multi_sizes_next(
					    &slab_size_segment_cur,
					    &vlen_left, &slab_start, &slab_end,
					    &pgs);
					if (!err) {
						sc_data_update_slab_size(
						    sc_data, slab_start,
						    slab_end, (int)pgs);
					} else {
						CONF_ERROR("Invalid settings "
						    "for slab_sizes",
						    k, klen, v, vlen);
					}
				} while (!err && vlen_left > 0);
				CONF_CONTINUE;
			}
			if (config_prof) {
				CONF_HANDLE_BOOL(opt_prof, "prof")
				CONF_HANDLE_CHAR_P(opt_prof_prefix,
				    "prof_prefix", "jeprof")
				CONF_HANDLE_BOOL(opt_prof_active, "prof_active")
				CONF_HANDLE_BOOL(opt_prof_thread_active_init,
				    "prof_thread_active_init")
				CONF_HANDLE_SIZE_T(opt_lg_prof_sample,
				    "lg_prof_sample", 0, (sizeof(uint64_t) << 3)
				    - 1, CONF_DONT_CHECK_MIN, CONF_CHECK_MAX,
				    true)
				CONF_HANDLE_BOOL(opt_prof_accum, "prof_accum")
				CONF_HANDLE_SSIZE_T(opt_lg_prof_interval,
				    "lg_prof_interval", -1,
				    (sizeof(uint64_t) << 3) - 1)
				CONF_HANDLE_BOOL(opt_prof_gdump, "prof_gdump")
				CONF_HANDLE_BOOL(opt_prof_final, "prof_final")
				CONF_HANDLE_BOOL(opt_prof_leak, "prof_leak")
				CONF_HANDLE_BOOL(opt_prof_log, "prof_log")
			}
			if (config_log) {
				if (CONF_MATCH("log")) {
					size_t cpylen = (
					    vlen <= sizeof(log_var_names) ?
					    vlen : sizeof(log_var_names) - 1);
					strncpy(log_var_names, v, cpylen);
					log_var_names[cpylen] = '\0';
					CONF_CONTINUE;
				}
			}
			if (CONF_MATCH("thp")) {
				bool match = false;
				for (int i = 0; i < thp_mode_names_limit; i++) {
					if (strncmp(thp_mode_names[i],v, vlen)
					    == 0) {
						if (!have_madvise_huge) {
							CONF_ERROR(
							    "No THP support",
							    k, klen, v, vlen);
						}
						opt_thp = i;
						match = true;
						break;
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			CONF_ERROR("Invalid conf pair", k, klen, v, vlen);
#undef CONF_ERROR
#undef CONF_CONTINUE
#undef CONF_MATCH
#undef CONF_MATCH_VALUE
#undef CONF_HANDLE_BOOL
#undef CONF_DONT_CHECK_MIN
#undef CONF_CHECK_MIN
#undef CONF_DONT_CHECK_MAX
#undef CONF_CHECK_MAX
#undef CONF_HANDLE_T_U
#undef CONF_HANDLE_UNSIGNED
#undef CONF_HANDLE_SIZE_T
#undef CONF_HANDLE_SSIZE_T
#undef CONF_HANDLE_CHAR_P
    /* Re-enable diagnostic "-Wtype-limits" */
    JEMALLOC_DIAGNOSTIC_POP
		}
		if (opt_abort_conf && had_conf_error) {
			malloc_abort_invalid_conf();
		}
	}
	atomic_store_b(&log_init_done, true, ATOMIC_RELEASE);
}

static void
malloc_conf_init(sc_data_t *sc_data, unsigned bin_shard_sizes[SC_NBINS]) {
	const char *opts_cache[MALLOC_CONF_NSOURCES] = {NULL, NULL, NULL, NULL};
	char buf[PATH_MAX + 1];

	/* The first call only set the confirm_conf option and opts_cache */
	malloc_conf_init_helper(NULL, NULL, true, opts_cache, buf);
	malloc_conf_init_helper(sc_data, bin_shard_sizes, false, opts_cache,
	    NULL);
}

#undef MALLOC_CONF_NSOURCES

static bool
malloc_init_hard_needed(void) {
	if (malloc_initialized() || (IS_INITIALIZER && malloc_init_state ==
	    malloc_init_recursible)) {
		/*
		 * Another thread initialized the allocator before this one
		 * acquired init_lock, or this thread is the initializing
		 * thread, and it is recursively allocating.
		 */
		return false;
	}
#ifdef JEMALLOC_THREADED_INIT
	if (malloc_initializer != NO_INITIALIZER && !IS_INITIALIZER) {
		/* Busy-wait until the initializing thread completes. */
		spin_t spinner = SPIN_INITIALIZER;
		do {
			malloc_mutex_unlock(TSDN_NULL, &init_lock);
			spin_adaptive(&spinner);
			malloc_mutex_lock(TSDN_NULL, &init_lock);
		} while (!malloc_initialized());
		return false;
	}
#endif
	return true;
}

static bool
malloc_init_hard_a0_locked() {
	malloc_initializer = INITIALIZER;

	JEMALLOC_DIAGNOSTIC_PUSH
	JEMALLOC_DIAGNOSTIC_IGNORE_MISSING_STRUCT_FIELD_INITIALIZERS
	sc_data_t sc_data = {0};
	JEMALLOC_DIAGNOSTIC_POP

	/*
	 * Ordering here is somewhat tricky; we need sc_boot() first, since that
	 * determines what the size classes will be, and then
	 * malloc_conf_init(), since any slab size tweaking will need to be done
	 * before sz_boot and bin_boot, which assume that the values they read
	 * out of sc_data_global are final.
	 */
	sc_boot(&sc_data);
	unsigned bin_shard_sizes[SC_NBINS];
	bin_shard_sizes_boot(bin_shard_sizes);
	/*
	 * prof_boot0 only initializes opt_prof_prefix.  We need to do it before
	 * we parse malloc_conf options, in case malloc_conf parsing overwrites
	 * it.
	 */
	if (config_prof) {
		prof_boot0();
	}
	malloc_conf_init(&sc_data, bin_shard_sizes);
	sz_boot(&sc_data);
	bin_boot(&sc_data, bin_shard_sizes);

	if (opt_stats_print) {
		/* Print statistics at exit. */
		if (atexit(stats_print_atexit) != 0) {
			malloc_write("<jemalloc>: Error in atexit()\n");
			if (opt_abort) {
				abort();
			}
		}
	}
	if (pages_boot()) {
		return true;
	}
	if (base_boot(TSDN_NULL)) {
		return true;
	}
	if (extent_boot()) {
		return true;
	}
	if (ctl_boot()) {
		return true;
	}
	if (config_prof) {
		prof_boot1();
	}
	arena_boot(&sc_data);
	if (tcache_boot(TSDN_NULL)) {
		return true;
	}
	if (malloc_mutex_init(&arenas_lock, "arenas", WITNESS_RANK_ARENAS,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}
	hook_boot();
	/*
	 * Create enough scaffolding to allow recursive allocation in
	 * malloc_ncpus().
	 */
	narenas_auto = 1;
	manual_arena_base = narenas_auto + 1;
	memset(arenas, 0, sizeof(arena_t *) * narenas_auto);
	/*
	 * Initialize one arena here.  The rest are lazily created in
	 * arena_choose_hard().
	 */
	if (arena_init(TSDN_NULL, 0, (extent_hooks_t *)&extent_hooks_default)
	    == NULL) {
		return true;
	}
	a0 = arena_get(TSDN_NULL, 0, false);
	malloc_init_state = malloc_init_a0_initialized;

	return false;
}

static bool
malloc_init_hard_a0(void) {
	bool ret;

	malloc_mutex_lock(TSDN_NULL, &init_lock);
	ret = malloc_init_hard_a0_locked();
	malloc_mutex_unlock(TSDN_NULL, &init_lock);
	return ret;
}

/* Initialize data structures which may trigger recursive allocation. */
static bool
malloc_init_hard_recursible(void) {
	malloc_init_state = malloc_init_recursible;

	ncpus = malloc_ncpus();

#if (defined(JEMALLOC_HAVE_PTHREAD_ATFORK) && !defined(JEMALLOC_MUTEX_INIT_CB) \
    && !defined(JEMALLOC_ZONE) && !defined(_WIN32) && \
    !defined(__native_client__))
	/* LinuxThreads' pthread_atfork() allocates. */
	if (pthread_atfork(jemalloc_prefork, jemalloc_postfork_parent,
	    jemalloc_postfork_child) != 0) {
		malloc_write("<jemalloc>: Error in pthread_atfork()\n");
		if (opt_abort) {
			abort();
		}
		return true;
	}
#endif

	if (background_thread_boot0()) {
		return true;
	}

	return false;
}

static unsigned
malloc_narenas_default(void) {
	assert(ncpus > 0);
	/*
	 * For SMP systems, create more than one arena per CPU by
	 * default.
	 */
	if (ncpus > 1) {
		return ncpus << 2;
	} else {
		return 1;
	}
}

static percpu_arena_mode_t
percpu_arena_as_initialized(percpu_arena_mode_t mode) {
	assert(!malloc_initialized());
	assert(mode <= percpu_arena_disabled);

	if (mode != percpu_arena_disabled) {
		mode += percpu_arena_mode_enabled_base;
	}

	return mode;
}

static bool
malloc_init_narenas(void) {
	assert(ncpus > 0);

	if (opt_percpu_arena != percpu_arena_disabled) {
		if (!have_percpu_arena || malloc_getcpu() < 0) {
			opt_percpu_arena = percpu_arena_disabled;
			malloc_printf("<jemalloc>: perCPU arena getcpu() not "
			    "available. Setting narenas to %u.\n", opt_narenas ?
			    opt_narenas : malloc_narenas_default());
			if (opt_abort) {
				abort();
			}
		} else {
			if (ncpus >= MALLOCX_ARENA_LIMIT) {
				malloc_printf("<jemalloc>: narenas w/ percpu"
				    "arena beyond limit (%d)\n", ncpus);
				if (opt_abort) {
					abort();
				}
				return true;
			}
			/* NB: opt_percpu_arena isn't fully initialized yet. */
			if (percpu_arena_as_initialized(opt_percpu_arena) ==
			    per_phycpu_arena && ncpus % 2 != 0) {
				malloc_printf("<jemalloc>: invalid "
				    "configuration -- per physical CPU arena "
				    "with odd number (%u) of CPUs (no hyper "
				    "threading?).\n", ncpus);
				if (opt_abort)
					abort();
			}
			unsigned n = percpu_arena_ind_limit(
			    percpu_arena_as_initialized(opt_percpu_arena));
			if (opt_narenas < n) {
				/*
				 * If narenas is specified with percpu_arena
				 * enabled, actual narenas is set as the greater
				 * of the two. percpu_arena_choose will be free
				 * to use any of the arenas based on CPU
				 * id. This is conservative (at a small cost)
				 * but ensures correctness.
				 *
				 * If for some reason the ncpus determined at
				 * boot is not the actual number (e.g. because
				 * of affinity setting from numactl), reserving
				 * narenas this way provides a workaround for
				 * percpu_arena.
				 */
				opt_narenas = n;
			}
		}
	}
	if (opt_narenas == 0) {
		opt_narenas = malloc_narenas_default();
	}
	assert(opt_narenas > 0);

	narenas_auto = opt_narenas;
	/*
	 * Limit the number of arenas to the indexing range of MALLOCX_ARENA().
	 */
	if (narenas_auto >= MALLOCX_ARENA_LIMIT) {
		narenas_auto = MALLOCX_ARENA_LIMIT - 1;
		malloc_printf("<jemalloc>: Reducing narenas to limit (%d)\n",
		    narenas_auto);
	}
	narenas_total_set(narenas_auto);
	if (arena_init_huge()) {
		narenas_total_inc();
	}
	manual_arena_base = narenas_total_get();

	return false;
}

static void
malloc_init_percpu(void) {
	opt_percpu_arena = percpu_arena_as_initialized(opt_percpu_arena);
}

static bool
malloc_init_hard_finish(void) {
	if (malloc_mutex_boot()) {
		return true;
	}

	malloc_init_state = malloc_init_initialized;
	malloc_slow_flag_init();

	return false;
}

static void
malloc_init_hard_cleanup(tsdn_t *tsdn, bool reentrancy_set) {
	malloc_mutex_assert_owner(tsdn, &init_lock);
	malloc_mutex_unlock(tsdn, &init_lock);
	if (reentrancy_set) {
		assert(!tsdn_null(tsdn));
		tsd_t *tsd = tsdn_tsd(tsdn);
		assert(tsd_reentrancy_level_get(tsd) > 0);
		post_reentrancy(tsd);
	}
}

static bool
malloc_init_hard(void) {
	tsd_t *tsd;

#if defined(_WIN32) && _WIN32_WINNT < 0x0600
	_init_init_lock();
#endif
	malloc_mutex_lock(TSDN_NULL, &init_lock);

#define UNLOCK_RETURN(tsdn, ret, reentrancy)		\
	malloc_init_hard_cleanup(tsdn, reentrancy);	\
	return ret;

	if (!malloc_init_hard_needed()) {
		UNLOCK_RETURN(TSDN_NULL, false, false)
	}

	if (malloc_init_state != malloc_init_a0_initialized &&
	    malloc_init_hard_a0_locked()) {
		UNLOCK_RETURN(TSDN_NULL, true, false)
	}

	malloc_mutex_unlock(TSDN_NULL, &init_lock);
	/* Recursive allocation relies on functional tsd. */
	tsd = malloc_tsd_boot0();
	if (tsd == NULL) {
		return true;
	}
	if (malloc_init_hard_recursible()) {
		return true;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &init_lock);
	/* Set reentrancy level to 1 during init. */
	pre_reentrancy(tsd, NULL);
	/* Initialize narenas before prof_boot2 (for allocation). */
	if (malloc_init_narenas() || background_thread_boot1(tsd_tsdn(tsd))) {
		UNLOCK_RETURN(tsd_tsdn(tsd), true, true)
	}
	if (config_prof && prof_boot2(tsd)) {
		UNLOCK_RETURN(tsd_tsdn(tsd), true, true)
	}

	malloc_init_percpu();

	if (malloc_init_hard_finish()) {
		UNLOCK_RETURN(tsd_tsdn(tsd), true, true)
	}
	post_reentrancy(tsd);
	malloc_mutex_unlock(tsd_tsdn(tsd), &init_lock);

	witness_assert_lockless(witness_tsd_tsdn(
	    tsd_witness_tsdp_get_unsafe(tsd)));
	malloc_tsd_boot1();
	/* Update TSD after tsd_boot1. */
	tsd = tsd_fetch();
	if (opt_background_thread) {
		assert(have_background_thread);
		/*
		 * Need to finish init & unlock first before creating background
		 * threads (pthread_create depends on malloc).  ctl_init (which
		 * sets isthreaded) needs to be called without holding any lock.
		 */
		background_thread_ctl_init(tsd_tsdn(tsd));
		if (background_thread_create(tsd, 0)) {
			return true;
		}
	}
#undef UNLOCK_RETURN
	return false;
}

/*
 * End initialization functions.
 */
/******************************************************************************/
/*
 * Begin allocation-path internal functions and data structures.
 */

/*
 * Settings determined by the documented behavior of the allocation functions.
 */
typedef struct static_opts_s static_opts_t;
struct static_opts_s {
	/* Whether or not allocation size may overflow. */
	bool may_overflow;

	/*
	 * Whether or not allocations (with alignment) of size 0 should be
	 * treated as size 1.
	 */
	bool bump_empty_aligned_alloc;
	/*
	 * Whether to assert that allocations are not of size 0 (after any
	 * bumping).
	 */
	bool assert_nonempty_alloc;

	/*
	 * Whether or not to modify the 'result' argument to malloc in case of
	 * error.
	 */
	bool null_out_result_on_error;
	/* Whether to set errno when we encounter an error condition. */
	bool set_errno_on_error;

	/*
	 * The minimum valid alignment for functions requesting aligned storage.
	 */
	size_t min_alignment;

	/* The error string to use if we oom. */
	const char *oom_string;
	/* The error string to use if the passed-in alignment is invalid. */
	const char *invalid_alignment_string;

	/*
	 * False if we're configured to skip some time-consuming operations.
	 *
	 * This isn't really a malloc "behavior", but it acts as a useful
	 * summary of several other static (or at least, static after program
	 * initialization) options.
	 */
	bool slow;
	/*
	 * Return size.
	 */
	bool usize;
};

JEMALLOC_ALWAYS_INLINE void
static_opts_init(static_opts_t *static_opts) {
	static_opts->may_overflow = false;
	static_opts->bump_empty_aligned_alloc = false;
	static_opts->assert_nonempty_alloc = false;
	static_opts->null_out_result_on_error = false;
	static_opts->set_errno_on_error = false;
	static_opts->min_alignment = 0;
	static_opts->oom_string = "";
	static_opts->invalid_alignment_string = "";
	static_opts->slow = false;
	static_opts->usize = false;
}

/*
 * These correspond to the macros in jemalloc/jemalloc_macros.h.  Broadly, we
 * should have one constant here per magic value there.  Note however that the
 * representations need not be related.
 */
#define TCACHE_IND_NONE ((unsigned)-1)
#define TCACHE_IND_AUTOMATIC ((unsigned)-2)
#define ARENA_IND_AUTOMATIC ((unsigned)-1)

typedef struct dynamic_opts_s dynamic_opts_t;
struct dynamic_opts_s {
	void **result;
	size_t usize;
	size_t num_items;
	size_t item_size;
	size_t alignment;
	bool zero;
	unsigned tcache_ind;
	unsigned arena_ind;
};

JEMALLOC_ALWAYS_INLINE void
dynamic_opts_init(dynamic_opts_t *dynamic_opts) {
	dynamic_opts->result = NULL;
	dynamic_opts->usize = 0;
	dynamic_opts->num_items = 0;
	dynamic_opts->item_size = 0;
	dynamic_opts->alignment = 0;
	dynamic_opts->zero = false;
	dynamic_opts->tcache_ind = TCACHE_IND_AUTOMATIC;
	dynamic_opts->arena_ind = ARENA_IND_AUTOMATIC;
}

/* ind is ignored if dopts->alignment > 0. */
JEMALLOC_ALWAYS_INLINE void *
imalloc_no_sample(static_opts_t *sopts, dynamic_opts_t *dopts, tsd_t *tsd,
    size_t size, size_t usize, szind_t ind) {
	tcache_t *tcache;
	arena_t *arena;

	/* Fill in the tcache. */
	if (dopts->tcache_ind == TCACHE_IND_AUTOMATIC) {
		if (likely(!sopts->slow)) {
			/* Getting tcache ptr unconditionally. */
			tcache = tsd_tcachep_get(tsd);
			assert(tcache == tcache_get(tsd));
		} else {
			tcache = tcache_get(tsd);
		}
	} else if (dopts->tcache_ind == TCACHE_IND_NONE) {
		tcache = NULL;
	} else {
		tcache = tcaches_get(tsd, dopts->tcache_ind);
	}

	/* Fill in the arena. */
	if (dopts->arena_ind == ARENA_IND_AUTOMATIC) {
		/*
		 * In case of automatic arena management, we defer arena
		 * computation until as late as we can, hoping to fill the
		 * allocation out of the tcache.
		 */
		arena = NULL;
	} else {
		arena = arena_get(tsd_tsdn(tsd), dopts->arena_ind, true);
	}

	if (unlikely(dopts->alignment != 0)) {
		return ipalloct(tsd_tsdn(tsd), usize, dopts->alignment,
		    dopts->zero, tcache, arena);
	}

	return iallocztm(tsd_tsdn(tsd), size, ind, dopts->zero, tcache, false,
	    arena, sopts->slow);
}

JEMALLOC_ALWAYS_INLINE void *
imalloc_sample(static_opts_t *sopts, dynamic_opts_t *dopts, tsd_t *tsd,
    size_t usize, szind_t ind) {
	void *ret;

	/*
	 * For small allocations, sampling bumps the usize.  If so, we allocate
	 * from the ind_large bucket.
	 */
	szind_t ind_large;
	size_t bumped_usize = usize;

	if (usize <= SC_SMALL_MAXCLASS) {
		assert(((dopts->alignment == 0) ?
		    sz_s2u(SC_LARGE_MINCLASS) :
		    sz_sa2u(SC_LARGE_MINCLASS, dopts->alignment))
			== SC_LARGE_MINCLASS);
		ind_large = sz_size2index(SC_LARGE_MINCLASS);
		bumped_usize = sz_s2u(SC_LARGE_MINCLASS);
		ret = imalloc_no_sample(sopts, dopts, tsd, bumped_usize,
		    bumped_usize, ind_large);
		if (unlikely(ret == NULL)) {
			return NULL;
		}
		arena_prof_promote(tsd_tsdn(tsd), ret, usize);
	} else {
		ret = imalloc_no_sample(sopts, dopts, tsd, usize, usize, ind);
	}

	return ret;
}

/*
 * Returns true if the allocation will overflow, and false otherwise.  Sets
 * *size to the product either way.
 */
JEMALLOC_ALWAYS_INLINE bool
compute_size_with_overflow(bool may_overflow, dynamic_opts_t *dopts,
    size_t *size) {
	/*
	 * This function is just num_items * item_size, except that we may have
	 * to check for overflow.
	 */

	if (!may_overflow) {
		assert(dopts->num_items == 1);
		*size = dopts->item_size;
		return false;
	}

	/* A size_t with its high-half bits all set to 1. */
	static const size_t high_bits = SIZE_T_MAX << (sizeof(size_t) * 8 / 2);

	*size = dopts->item_size * dopts->num_items;

	if (unlikely(*size == 0)) {
		return (dopts->num_items != 0 && dopts->item_size != 0);
	}

	/*
	 * We got a non-zero size, but we don't know if we overflowed to get
	 * there.  To avoid having to do a divide, we'll be clever and note that
	 * if both A and B can be represented in N/2 bits, then their product
	 * can be represented in N bits (without the possibility of overflow).
	 */
	if (likely((high_bits & (dopts->num_items | dopts->item_size)) == 0)) {
		return false;
	}
	if (likely(*size / dopts->item_size == dopts->num_items)) {
		return false;
	}
	return true;
}

JEMALLOC_ALWAYS_INLINE int
imalloc_body(static_opts_t *sopts, dynamic_opts_t *dopts, tsd_t *tsd) {
	/* Where the actual allocated memory will live. */
	void *allocation = NULL;
	/* Filled in by compute_size_with_overflow below. */
	size_t size = 0;
	/*
	 * For unaligned allocations, we need only ind.  For aligned
	 * allocations, or in case of stats or profiling we need usize.
	 *
	 * These are actually dead stores, in that their values are reset before
	 * any branch on their value is taken.  Sometimes though, it's
	 * convenient to pass them as arguments before this point.  To avoid
	 * undefined behavior then, we initialize them with dummy stores.
	 */
	szind_t ind = 0;
	size_t usize = 0;

	/* Reentrancy is only checked on slow path. */
	int8_t reentrancy_level;

	/* Compute the amount of memory the user wants. */
	if (unlikely(compute_size_with_overflow(sopts->may_overflow, dopts,
	    &size))) {
		goto label_oom;
	}

	if (unlikely(dopts->alignment < sopts->min_alignment
	    || (dopts->alignment & (dopts->alignment - 1)) != 0)) {
		goto label_invalid_alignment;
	}

	/* This is the beginning of the "core" algorithm. */

	if (dopts->alignment == 0) {
		ind = sz_size2index(size);
		if (unlikely(ind >= SC_NSIZES)) {
			goto label_oom;
		}
		if (config_stats || (config_prof && opt_prof) || sopts->usize) {
			usize = sz_index2size(ind);
			dopts->usize = usize;
			assert(usize > 0 && usize
			    <= SC_LARGE_MAXCLASS);
		}
	} else {
		if (sopts->bump_empty_aligned_alloc) {
			if (unlikely(size == 0)) {
				size = 1;
			}
		}
		usize = sz_sa2u(size, dopts->alignment);
		dopts->usize = usize;
		if (unlikely(usize == 0
		    || usize > SC_LARGE_MAXCLASS)) {
			goto label_oom;
		}
	}
	/* Validate the user input. */
	if (sopts->assert_nonempty_alloc) {
		assert (size != 0);
	}

	check_entry_exit_locking(tsd_tsdn(tsd));

	/*
	 * If we need to handle reentrancy, we can do it out of a
	 * known-initialized arena (i.e. arena 0).
	 */
	reentrancy_level = tsd_reentrancy_level_get(tsd);
	if (sopts->slow && unlikely(reentrancy_level > 0)) {
		/*
		 * We should never specify particular arenas or tcaches from
		 * within our internal allocations.
		 */
		assert(dopts->tcache_ind == TCACHE_IND_AUTOMATIC ||
		    dopts->tcache_ind == TCACHE_IND_NONE);
		assert(dopts->arena_ind == ARENA_IND_AUTOMATIC);
		dopts->tcache_ind = TCACHE_IND_NONE;
		/* We know that arena 0 has already been initialized. */
		dopts->arena_ind = 0;
	}

	/* If profiling is on, get our profiling context. */
	if (config_prof && opt_prof) {
		/*
		 * Note that if we're going down this path, usize must have been
		 * initialized in the previous if statement.
		 */
		prof_tctx_t *tctx = prof_alloc_prep(
		    tsd, usize, prof_active_get_unlocked(), true);

		alloc_ctx_t alloc_ctx;
		if (likely((uintptr_t)tctx == (uintptr_t)1U)) {
			alloc_ctx.slab = (usize
			    <= SC_SMALL_MAXCLASS);
			allocation = imalloc_no_sample(
			    sopts, dopts, tsd, usize, usize, ind);
		} else if ((uintptr_t)tctx > (uintptr_t)1U) {
			/*
			 * Note that ind might still be 0 here.  This is fine;
			 * imalloc_sample ignores ind if dopts->alignment > 0.
			 */
			allocation = imalloc_sample(
			    sopts, dopts, tsd, usize, ind);
			alloc_ctx.slab = false;
		} else {
			allocation = NULL;
		}

		if (unlikely(allocation == NULL)) {
			prof_alloc_rollback(tsd, tctx, true);
			goto label_oom;
		}
		prof_malloc(tsd_tsdn(tsd), allocation, usize, &alloc_ctx, tctx);
	} else {
		/*
		 * If dopts->alignment > 0, then ind is still 0, but usize was
		 * computed in the previous if statement.  Down the positive
		 * alignment path, imalloc_no_sample ignores ind and size
		 * (relying only on usize).
		 */
		allocation = imalloc_no_sample(sopts, dopts, tsd, size, usize,
		    ind);
		if (unlikely(allocation == NULL)) {
			goto label_oom;
		}
	}

	/*
	 * Allocation has been done at this point.  We still have some
	 * post-allocation work to do though.
	 */
	assert(dopts->alignment == 0
	    || ((uintptr_t)allocation & (dopts->alignment - 1)) == ZU(0));

	if (config_stats) {
		assert(usize == isalloc(tsd_tsdn(tsd), allocation));
		*tsd_thread_allocatedp_get(tsd) += usize;
	}

	if (sopts->slow) {
		UTRACE(0, size, allocation);
	}

	/* Success! */
	check_entry_exit_locking(tsd_tsdn(tsd));
	*dopts->result = allocation;
	return 0;

label_oom:
	if (unlikely(sopts->slow) && config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write(sopts->oom_string);
		abort();
	}

	if (sopts->slow) {
		UTRACE(NULL, size, NULL);
	}

	check_entry_exit_locking(tsd_tsdn(tsd));

	if (sopts->set_errno_on_error) {
		set_errno(ENOMEM);
	}

	if (sopts->null_out_result_on_error) {
		*dopts->result = NULL;
	}

	return ENOMEM;

	/*
	 * This label is only jumped to by one goto; we move it out of line
	 * anyways to avoid obscuring the non-error paths, and for symmetry with
	 * the oom case.
	 */
label_invalid_alignment:
	if (config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write(sopts->invalid_alignment_string);
		abort();
	}

	if (sopts->set_errno_on_error) {
		set_errno(EINVAL);
	}

	if (sopts->slow) {
		UTRACE(NULL, size, NULL);
	}

	check_entry_exit_locking(tsd_tsdn(tsd));

	if (sopts->null_out_result_on_error) {
		*dopts->result = NULL;
	}

	return EINVAL;
}

JEMALLOC_ALWAYS_INLINE bool
imalloc_init_check(static_opts_t *sopts, dynamic_opts_t *dopts) {
	if (unlikely(!malloc_initialized()) && unlikely(malloc_init())) {
		if (config_xmalloc && unlikely(opt_xmalloc)) {
			malloc_write(sopts->oom_string);
			abort();
		}
		UTRACE(NULL, dopts->num_items * dopts->item_size, NULL);
		set_errno(ENOMEM);
		*dopts->result = NULL;

		return false;
	}

	return true;
}

/* Returns the errno-style error code of the allocation. */
JEMALLOC_ALWAYS_INLINE int
imalloc(static_opts_t *sopts, dynamic_opts_t *dopts) {
	if (tsd_get_allocates() && !imalloc_init_check(sopts, dopts)) {
		return ENOMEM;
	}

	/* We always need the tsd.  Let's grab it right away. */
	tsd_t *tsd = tsd_fetch();
	assert(tsd);
	if (likely(tsd_fast(tsd))) {
		/* Fast and common path. */
		tsd_assert_fast(tsd);
		sopts->slow = false;
		return imalloc_body(sopts, dopts, tsd);
	} else {
		if (!tsd_get_allocates() && !imalloc_init_check(sopts, dopts)) {
			return ENOMEM;
		}

		sopts->slow = true;
		return imalloc_body(sopts, dopts, tsd);
	}
}

JEMALLOC_NOINLINE
void *
malloc_default(size_t size) {
	void *ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.malloc.entry", "size: %zu", size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.null_out_result_on_error = true;
	sopts.set_errno_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in malloc(): out of memory\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;

	imalloc(&sopts, &dopts);
	/*
	 * Note that this branch gets optimized away -- it immediately follows
	 * the check on tsd_fast that sets sopts.slow.
	 */
	if (sopts.slow) {
		uintptr_t args[3] = {size};
		hook_invoke_alloc(hook_alloc_malloc, ret, (uintptr_t)ret, args);
	}

	LOG("core.malloc.exit", "result: %p", ret);

	return ret;
}

/******************************************************************************/
/*
 * Begin malloc(3)-compatible functions.
 */

/*
 * malloc() fastpath.
 *
 * Fastpath assumes size <= SC_LOOKUP_MAXCLASS, and that we hit
 * tcache.  If either of these is false, we tail-call to the slowpath,
 * malloc_default().  Tail-calling is used to avoid any caller-saved
 * registers.
 *
 * fastpath supports ticker and profiling, both of which will also
 * tail-call to the slowpath if they fire.
 */
JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(1)
je_malloc(size_t size) {
	LOG("core.malloc.entry", "size: %zu", size);

	if (tsd_get_allocates() && unlikely(!malloc_initialized())) {
		return malloc_default(size);
	}

	tsd_t *tsd = tsd_get(false);
	if (unlikely(!tsd || !tsd_fast(tsd) || (size > SC_LOOKUP_MAXCLASS))) {
		return malloc_default(size);
	}

	tcache_t *tcache = tsd_tcachep_get(tsd);

	if (unlikely(ticker_trytick(&tcache->gc_ticker))) {
		return malloc_default(size);
	}

	szind_t ind = sz_size2index_lookup(size);
	size_t usize;
	if (config_stats || config_prof) {
		usize = sz_index2size(ind);
	}
	/* Fast path relies on size being a bin. I.e. SC_LOOKUP_MAXCLASS < SC_SMALL_MAXCLASS */
	assert(ind < SC_NBINS);
	assert(size <= SC_SMALL_MAXCLASS);

	if (config_prof) {
		int64_t bytes_until_sample = tsd_bytes_until_sample_get(tsd);
		bytes_until_sample -= usize;
		tsd_bytes_until_sample_set(tsd, bytes_until_sample);

		if (unlikely(bytes_until_sample < 0)) {
			/*
			 * Avoid a prof_active check on the fastpath.
			 * If prof_active is false, set bytes_until_sample to
			 * a large value.  If prof_active is set to true,
			 * bytes_until_sample will be reset.
			 */
			if (!prof_active) {
				tsd_bytes_until_sample_set(tsd, SSIZE_MAX);
			}
			return malloc_default(size);
		}
	}

	cache_bin_t *bin = tcache_small_bin_get(tcache, ind);
	bool tcache_success;
	void* ret = cache_bin_alloc_easy(bin, &tcache_success);

	if (tcache_success) {
		if (config_stats) {
			*tsd_thread_allocatedp_get(tsd) += usize;
			bin->tstats.nrequests++;
		}
		if (config_prof) {
			tcache->prof_accumbytes += usize;
		}

		LOG("core.malloc.exit", "result: %p", ret);

		/* Fastpath success */
		return ret;
	}

	return malloc_default(size);
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
JEMALLOC_ATTR(nonnull(1))
je_posix_memalign(void **memptr, size_t alignment, size_t size) {
	int ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.posix_memalign.entry", "mem ptr: %p, alignment: %zu, "
	    "size: %zu", memptr, alignment, size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.bump_empty_aligned_alloc = true;
	sopts.min_alignment = sizeof(void *);
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";

	dopts.result = memptr;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = alignment;

	ret = imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {(uintptr_t)memptr, (uintptr_t)alignment,
			(uintptr_t)size};
		hook_invoke_alloc(hook_alloc_posix_memalign, *memptr,
		    (uintptr_t)ret, args);
	}

	LOG("core.posix_memalign.exit", "result: %d, alloc ptr: %p", ret,
	    *memptr);

	return ret;
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(2)
je_aligned_alloc(size_t alignment, size_t size) {
	void *ret;

	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.aligned_alloc.entry", "alignment: %zu, size: %zu\n",
	    alignment, size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.bump_empty_aligned_alloc = true;
	sopts.null_out_result_on_error = true;
	sopts.set_errno_on_error = true;
	sopts.min_alignment = 1;
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = alignment;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {(uintptr_t)alignment, (uintptr_t)size};
		hook_invoke_alloc(hook_alloc_aligned_alloc, ret,
		    (uintptr_t)ret, args);
	}

	LOG("core.aligned_alloc.exit", "result: %p", ret);

	return ret;
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE2(1, 2)
je_calloc(size_t num, size_t size) {
	void *ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.calloc.entry", "num: %zu, size: %zu\n", num, size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.may_overflow = true;
	sopts.null_out_result_on_error = true;
	sopts.set_errno_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in calloc(): out of memory\n";

	dopts.result = &ret;
	dopts.num_items = num;
	dopts.item_size = size;
	dopts.zero = true;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {(uintptr_t)num, (uintptr_t)size};
		hook_invoke_alloc(hook_alloc_calloc, ret, (uintptr_t)ret, args);
	}

	LOG("core.calloc.exit", "result: %p", ret);

	return ret;
}

static void *
irealloc_prof_sample(tsd_t *tsd, void *old_ptr, size_t old_usize, size_t usize,
    prof_tctx_t *tctx, hook_ralloc_args_t *hook_args) {
	void *p;

	if (tctx == NULL) {
		return NULL;
	}
	if (usize <= SC_SMALL_MAXCLASS) {
		p = iralloc(tsd, old_ptr, old_usize,
		    SC_LARGE_MINCLASS, 0, false, hook_args);
		if (p == NULL) {
			return NULL;
		}
		arena_prof_promote(tsd_tsdn(tsd), p, usize);
	} else {
		p = iralloc(tsd, old_ptr, old_usize, usize, 0, false,
		    hook_args);
	}

	return p;
}

JEMALLOC_ALWAYS_INLINE void *
irealloc_prof(tsd_t *tsd, void *old_ptr, size_t old_usize, size_t usize,
   alloc_ctx_t *alloc_ctx, hook_ralloc_args_t *hook_args) {
	void *p;
	bool prof_active;
	prof_tctx_t *old_tctx, *tctx;

	prof_active = prof_active_get_unlocked();
	old_tctx = prof_tctx_get(tsd_tsdn(tsd), old_ptr, alloc_ctx);
	tctx = prof_alloc_prep(tsd, usize, prof_active, true);
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U)) {
		p = irealloc_prof_sample(tsd, old_ptr, old_usize, usize, tctx,
		    hook_args);
	} else {
		p = iralloc(tsd, old_ptr, old_usize, usize, 0, false,
		    hook_args);
	}
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx, true);
		return NULL;
	}
	prof_realloc(tsd, p, usize, tctx, prof_active, true, old_ptr, old_usize,
	    old_tctx);

	return p;
}

JEMALLOC_ALWAYS_INLINE void
ifree(tsd_t *tsd, void *ptr, tcache_t *tcache, bool slow_path) {
	if (!slow_path) {
		tsd_assert_fast(tsd);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));
	if (tsd_reentrancy_level_get(tsd) != 0) {
		assert(slow_path);
	}

	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	alloc_ctx_t alloc_ctx;
	rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
	rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree, rtree_ctx,
	    (uintptr_t)ptr, true, &alloc_ctx.szind, &alloc_ctx.slab);
	assert(alloc_ctx.szind != SC_NSIZES);

	size_t usize;
	if (config_prof && opt_prof) {
		usize = sz_index2size(alloc_ctx.szind);
		prof_free(tsd, ptr, usize, &alloc_ctx);
	} else if (config_stats) {
		usize = sz_index2size(alloc_ctx.szind);
	}
	if (config_stats) {
		*tsd_thread_deallocatedp_get(tsd) += usize;
	}

	if (likely(!slow_path)) {
		idalloctm(tsd_tsdn(tsd), ptr, tcache, &alloc_ctx, false,
		    false);
	} else {
		idalloctm(tsd_tsdn(tsd), ptr, tcache, &alloc_ctx, false,
		    true);
	}
}

JEMALLOC_ALWAYS_INLINE void
isfree(tsd_t *tsd, void *ptr, size_t usize, tcache_t *tcache, bool slow_path) {
	if (!slow_path) {
		tsd_assert_fast(tsd);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));
	if (tsd_reentrancy_level_get(tsd) != 0) {
		assert(slow_path);
	}

	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	alloc_ctx_t alloc_ctx, *ctx;
	if (!config_cache_oblivious && ((uintptr_t)ptr & PAGE_MASK) != 0) {
		/*
		 * When cache_oblivious is disabled and ptr is not page aligned,
		 * the allocation was not sampled -- usize can be used to
		 * determine szind directly.
		 */
		alloc_ctx.szind = sz_size2index(usize);
		alloc_ctx.slab = true;
		ctx = &alloc_ctx;
		if (config_debug) {
			alloc_ctx_t dbg_ctx;
			rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
			rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree,
			    rtree_ctx, (uintptr_t)ptr, true, &dbg_ctx.szind,
			    &dbg_ctx.slab);
			assert(dbg_ctx.szind == alloc_ctx.szind);
			assert(dbg_ctx.slab == alloc_ctx.slab);
		}
	} else if (config_prof && opt_prof) {
		rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
		rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree, rtree_ctx,
		    (uintptr_t)ptr, true, &alloc_ctx.szind, &alloc_ctx.slab);
		assert(alloc_ctx.szind == sz_size2index(usize));
		ctx = &alloc_ctx;
	} else {
		ctx = NULL;
	}

	if (config_prof && opt_prof) {
		prof_free(tsd, ptr, usize, ctx);
	}
	if (config_stats) {
		*tsd_thread_deallocatedp_get(tsd) += usize;
	}

	if (likely(!slow_path)) {
		isdalloct(tsd_tsdn(tsd), ptr, usize, tcache, ctx, false);
	} else {
		isdalloct(tsd_tsdn(tsd), ptr, usize, tcache, ctx, true);
	}
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2)
je_realloc(void *ptr, size_t arg_size) {
	void *ret;
	tsdn_t *tsdn JEMALLOC_CC_SILENCE_INIT(NULL);
	size_t usize JEMALLOC_CC_SILENCE_INIT(0);
	size_t old_usize = 0;
	size_t size = arg_size;

	LOG("core.realloc.entry", "ptr: %p, size: %zu\n", ptr, size);

	if (unlikely(size == 0)) {
		if (ptr != NULL) {
			/* realloc(ptr, 0) is equivalent to free(ptr). */
			UTRACE(ptr, 0, 0);
			tcache_t *tcache;
			tsd_t *tsd = tsd_fetch();
			if (tsd_reentrancy_level_get(tsd) == 0) {
				tcache = tcache_get(tsd);
			} else {
				tcache = NULL;
			}

			uintptr_t args[3] = {(uintptr_t)ptr, size};
			hook_invoke_dalloc(hook_dalloc_realloc, ptr, args);

			ifree(tsd, ptr, tcache, true);

			LOG("core.realloc.exit", "result: %p", NULL);
			return NULL;
		}
		size = 1;
	}

	if (likely(ptr != NULL)) {
		assert(malloc_initialized() || IS_INITIALIZER);
		tsd_t *tsd = tsd_fetch();

		check_entry_exit_locking(tsd_tsdn(tsd));


		hook_ralloc_args_t hook_args = {true, {(uintptr_t)ptr,
			(uintptr_t)arg_size, 0, 0}};

		alloc_ctx_t alloc_ctx;
		rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
		rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree, rtree_ctx,
		    (uintptr_t)ptr, true, &alloc_ctx.szind, &alloc_ctx.slab);
		assert(alloc_ctx.szind != SC_NSIZES);
		old_usize = sz_index2size(alloc_ctx.szind);
		assert(old_usize == isalloc(tsd_tsdn(tsd), ptr));
		if (config_prof && opt_prof) {
			usize = sz_s2u(size);
			if (unlikely(usize == 0
			    || usize > SC_LARGE_MAXCLASS)) {
				ret = NULL;
			} else {
				ret = irealloc_prof(tsd, ptr, old_usize, usize,
				    &alloc_ctx, &hook_args);
			}
		} else {
			if (config_stats) {
				usize = sz_s2u(size);
			}
			ret = iralloc(tsd, ptr, old_usize, size, 0, false,
			    &hook_args);
		}
		tsdn = tsd_tsdn(tsd);
	} else {
		/* realloc(NULL, size) is equivalent to malloc(size). */
		static_opts_t sopts;
		dynamic_opts_t dopts;

		static_opts_init(&sopts);
		dynamic_opts_init(&dopts);

		sopts.null_out_result_on_error = true;
		sopts.set_errno_on_error = true;
		sopts.oom_string =
		    "<jemalloc>: Error in realloc(): out of memory\n";

		dopts.result = &ret;
		dopts.num_items = 1;
		dopts.item_size = size;

		imalloc(&sopts, &dopts);
		if (sopts.slow) {
			uintptr_t args[3] = {(uintptr_t)ptr, arg_size};
			hook_invoke_alloc(hook_alloc_realloc, ret,
			    (uintptr_t)ret, args);
		}

		return ret;
	}

	if (unlikely(ret == NULL)) {
		if (config_xmalloc && unlikely(opt_xmalloc)) {
			malloc_write("<jemalloc>: Error in realloc(): "
			    "out of memory\n");
			abort();
		}
		set_errno(ENOMEM);
	}
	if (config_stats && likely(ret != NULL)) {
		tsd_t *tsd;

		assert(usize == isalloc(tsdn, ret));
		tsd = tsdn_tsd(tsdn);
		*tsd_thread_allocatedp_get(tsd) += usize;
		*tsd_thread_deallocatedp_get(tsd) += old_usize;
	}
	UTRACE(ptr, size, ret);
	check_entry_exit_locking(tsdn);

	LOG("core.realloc.exit", "result: %p", ret);
	return ret;
}

JEMALLOC_NOINLINE
void
free_default(void *ptr) {
	UTRACE(ptr, 0, 0);
	if (likely(ptr != NULL)) {
		/*
		 * We avoid setting up tsd fully (e.g. tcache, arena binding)
		 * based on only free() calls -- other activities trigger the
		 * minimal to full transition.  This is because free() may
		 * happen during thread shutdown after tls deallocation: if a
		 * thread never had any malloc activities until then, a
		 * fully-setup tsd won't be destructed properly.
		 */
		tsd_t *tsd = tsd_fetch_min();
		check_entry_exit_locking(tsd_tsdn(tsd));

		tcache_t *tcache;
		if (likely(tsd_fast(tsd))) {
			tsd_assert_fast(tsd);
			/* Unconditionally get tcache ptr on fast path. */
			tcache = tsd_tcachep_get(tsd);
			ifree(tsd, ptr, tcache, false);
		} else {
			if (likely(tsd_reentrancy_level_get(tsd) == 0)) {
				tcache = tcache_get(tsd);
			} else {
				tcache = NULL;
			}
			uintptr_t args_raw[3] = {(uintptr_t)ptr};
			hook_invoke_dalloc(hook_dalloc_free, ptr, args_raw);
			ifree(tsd, ptr, tcache, true);
		}
		check_entry_exit_locking(tsd_tsdn(tsd));
	}
}

JEMALLOC_ALWAYS_INLINE
bool free_fastpath(void *ptr, size_t size, bool size_hint) {
	tsd_t *tsd = tsd_get(false);
	if (unlikely(!tsd || !tsd_fast(tsd))) {
		return false;
	}

	tcache_t *tcache = tsd_tcachep_get(tsd);

	alloc_ctx_t alloc_ctx;
	/*
	 * If !config_cache_oblivious, we can check PAGE alignment to
	 * detect sampled objects.  Otherwise addresses are
	 * randomized, and we have to look it up in the rtree anyway.
	 * See also isfree().
	 */
	if (!size_hint || config_cache_oblivious) {
		rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
		bool res = rtree_szind_slab_read_fast(tsd_tsdn(tsd), &extents_rtree,
						      rtree_ctx, (uintptr_t)ptr,
						      &alloc_ctx.szind, &alloc_ctx.slab);

		/* Note: profiled objects will have alloc_ctx.slab set */
		if (!res || !alloc_ctx.slab) {
			return false;
		}
		assert(alloc_ctx.szind != SC_NSIZES);
	} else {
		/*
		 * Check for both sizes that are too large, and for sampled objects.
		 * Sampled objects are always page-aligned.  The sampled object check
		 * will also check for null ptr.
		 */
		if (size > SC_LOOKUP_MAXCLASS || (((uintptr_t)ptr & PAGE_MASK) == 0)) {
			return false;
		}
		alloc_ctx.szind = sz_size2index_lookup(size);
	}

	if (unlikely(ticker_trytick(&tcache->gc_ticker))) {
		return false;
	}

	cache_bin_t *bin = tcache_small_bin_get(tcache, alloc_ctx.szind);
	cache_bin_info_t *bin_info = &tcache_bin_info[alloc_ctx.szind];
	if (!cache_bin_dalloc_easy(bin, bin_info, ptr)) {
		return false;
	}

	if (config_stats) {
		size_t usize = sz_index2size(alloc_ctx.szind);
		*tsd_thread_deallocatedp_get(tsd) += usize;
	}

	return true;
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_free(void *ptr) {
	LOG("core.free.entry", "ptr: %p", ptr);

	if (!free_fastpath(ptr, 0, false)) {
		free_default(ptr);
	}

	LOG("core.free.exit", "");
}

/*
 * End malloc(3)-compatible functions.
 */
/******************************************************************************/
/*
 * Begin non-standard override functions.
 */

#ifdef JEMALLOC_OVERRIDE_MEMALIGN
JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc)
je_memalign(size_t alignment, size_t size) {
	void *ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.memalign.entry", "alignment: %zu, size: %zu\n", alignment,
	    size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.min_alignment = 1;
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";
	sopts.null_out_result_on_error = true;

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = alignment;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {alignment, size};
		hook_invoke_alloc(hook_alloc_memalign, ret, (uintptr_t)ret,
		    args);
	}

	LOG("core.memalign.exit", "result: %p", ret);
	return ret;
}
#endif

#ifdef JEMALLOC_OVERRIDE_VALLOC
JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc)
je_valloc(size_t size) {
	void *ret;

	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.valloc.entry", "size: %zu\n", size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.null_out_result_on_error = true;
	sopts.min_alignment = PAGE;
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = PAGE;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {size};
		hook_invoke_alloc(hook_alloc_valloc, ret, (uintptr_t)ret, args);
	}

	LOG("core.valloc.exit", "result: %p\n", ret);
	return ret;
}
#endif

#if defined(JEMALLOC_IS_MALLOC) && defined(JEMALLOC_GLIBC_MALLOC_HOOK)
/*
 * glibc provides the RTLD_DEEPBIND flag for dlopen which can make it possible
 * to inconsistently reference libc's malloc(3)-compatible functions
 * (https://bugzilla.mozilla.org/show_bug.cgi?id=493541).
 *
 * These definitions interpose hooks in glibc.  The functions are actually
 * passed an extra argument for the caller return address, which will be
 * ignored.
 */
JEMALLOC_EXPORT void (*__free_hook)(void *ptr) = je_free;
JEMALLOC_EXPORT void *(*__malloc_hook)(size_t size) = je_malloc;
JEMALLOC_EXPORT void *(*__realloc_hook)(void *ptr, size_t size) = je_realloc;
#  ifdef JEMALLOC_GLIBC_MEMALIGN_HOOK
JEMALLOC_EXPORT void *(*__memalign_hook)(size_t alignment, size_t size) =
    je_memalign;
#  endif

#  ifdef CPU_COUNT
/*
 * To enable static linking with glibc, the libc specific malloc interface must
 * be implemented also, so none of glibc's malloc.o functions are added to the
 * link.
 */
#    define ALIAS(je_fn)	__attribute__((alias (#je_fn), used))
/* To force macro expansion of je_ prefix before stringification. */
#    define PREALIAS(je_fn)	ALIAS(je_fn)
#    ifdef JEMALLOC_OVERRIDE___LIBC_CALLOC
void *__libc_calloc(size_t n, size_t size) PREALIAS(je_calloc);
#    endif
#    ifdef JEMALLOC_OVERRIDE___LIBC_FREE
void __libc_free(void* ptr) PREALIAS(je_free);
#    endif
#    ifdef JEMALLOC_OVERRIDE___LIBC_MALLOC
void *__libc_malloc(size_t size) PREALIAS(je_malloc);
#    endif
#    ifdef JEMALLOC_OVERRIDE___LIBC_MEMALIGN
void *__libc_memalign(size_t align, size_t s) PREALIAS(je_memalign);
#    endif
#    ifdef JEMALLOC_OVERRIDE___LIBC_REALLOC
void *__libc_realloc(void* ptr, size_t size) PREALIAS(je_realloc);
#    endif
#    ifdef JEMALLOC_OVERRIDE___LIBC_VALLOC
void *__libc_valloc(size_t size) PREALIAS(je_valloc);
#    endif
#    ifdef JEMALLOC_OVERRIDE___POSIX_MEMALIGN
int __posix_memalign(void** r, size_t a, size_t s) PREALIAS(je_posix_memalign);
#    endif
#    undef PREALIAS
#    undef ALIAS
#  endif
#endif

/*
 * End non-standard override functions.
 */
/******************************************************************************/
/*
 * Begin non-standard functions.
 */

#ifdef JEMALLOC_EXPERIMENTAL_SMALLOCX_API

#define JEMALLOC_SMALLOCX_CONCAT_HELPER(x, y) x ## y
#define JEMALLOC_SMALLOCX_CONCAT_HELPER2(x, y)  \
  JEMALLOC_SMALLOCX_CONCAT_HELPER(x, y)

typedef struct {
	void *ptr;
	size_t size;
} smallocx_return_t;

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
smallocx_return_t JEMALLOC_NOTHROW
/*
 * The attribute JEMALLOC_ATTR(malloc) cannot be used due to:
 *  - https://gcc.gnu.org/bugzilla/show_bug.cgi?id=86488
 */
JEMALLOC_SMALLOCX_CONCAT_HELPER2(je_smallocx_, JEMALLOC_VERSION_GID_IDENT)
  (size_t size, int flags) {
	/*
	 * Note: the attribute JEMALLOC_ALLOC_SIZE(1) cannot be
	 * used here because it makes writing beyond the `size`
	 * of the `ptr` undefined behavior, but the objective
	 * of this function is to allow writing beyond `size`
	 * up to `smallocx_return_t::size`.
	 */
	smallocx_return_t ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.smallocx.entry", "size: %zu, flags: %d", size, flags);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.assert_nonempty_alloc = true;
	sopts.null_out_result_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in mallocx(): out of memory\n";
	sopts.usize = true;

	dopts.result = &ret.ptr;
	dopts.num_items = 1;
	dopts.item_size = size;
	if (unlikely(flags != 0)) {
		if ((flags & MALLOCX_LG_ALIGN_MASK) != 0) {
			dopts.alignment = MALLOCX_ALIGN_GET_SPECIFIED(flags);
		}

		dopts.zero = MALLOCX_ZERO_GET(flags);

		if ((flags & MALLOCX_TCACHE_MASK) != 0) {
			if ((flags & MALLOCX_TCACHE_MASK)
			    == MALLOCX_TCACHE_NONE) {
				dopts.tcache_ind = TCACHE_IND_NONE;
			} else {
				dopts.tcache_ind = MALLOCX_TCACHE_GET(flags);
			}
		} else {
			dopts.tcache_ind = TCACHE_IND_AUTOMATIC;
		}

		if ((flags & MALLOCX_ARENA_MASK) != 0)
			dopts.arena_ind = MALLOCX_ARENA_GET(flags);
	}

	imalloc(&sopts, &dopts);
	assert(dopts.usize == je_nallocx(size, flags));
	ret.size = dopts.usize;

	LOG("core.smallocx.exit", "result: %p, size: %zu", ret.ptr, ret.size);
	return ret;
}
#undef JEMALLOC_SMALLOCX_CONCAT_HELPER
#undef JEMALLOC_SMALLOCX_CONCAT_HELPER2
#endif

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(1)
je_mallocx(size_t size, int flags) {
	void *ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.mallocx.entry", "size: %zu, flags: %d", size, flags);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.assert_nonempty_alloc = true;
	sopts.null_out_result_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in mallocx(): out of memory\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	if (unlikely(flags != 0)) {
		if ((flags & MALLOCX_LG_ALIGN_MASK) != 0) {
			dopts.alignment = MALLOCX_ALIGN_GET_SPECIFIED(flags);
		}

		dopts.zero = MALLOCX_ZERO_GET(flags);

		if ((flags & MALLOCX_TCACHE_MASK) != 0) {
			if ((flags & MALLOCX_TCACHE_MASK)
			    == MALLOCX_TCACHE_NONE) {
				dopts.tcache_ind = TCACHE_IND_NONE;
			} else {
				dopts.tcache_ind = MALLOCX_TCACHE_GET(flags);
			}
		} else {
			dopts.tcache_ind = TCACHE_IND_AUTOMATIC;
		}

		if ((flags & MALLOCX_ARENA_MASK) != 0)
			dopts.arena_ind = MALLOCX_ARENA_GET(flags);
	}

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {size, flags};
		hook_invoke_alloc(hook_alloc_mallocx, ret, (uintptr_t)ret,
		    args);
	}

	LOG("core.mallocx.exit", "result: %p", ret);
	return ret;
}

static void *
irallocx_prof_sample(tsdn_t *tsdn, void *old_ptr, size_t old_usize,
    size_t usize, size_t alignment, bool zero, tcache_t *tcache, arena_t *arena,
    prof_tctx_t *tctx, hook_ralloc_args_t *hook_args) {
	void *p;

	if (tctx == NULL) {
		return NULL;
	}
	if (usize <= SC_SMALL_MAXCLASS) {
		p = iralloct(tsdn, old_ptr, old_usize,
		    SC_LARGE_MINCLASS, alignment, zero, tcache,
		    arena, hook_args);
		if (p == NULL) {
			return NULL;
		}
		arena_prof_promote(tsdn, p, usize);
	} else {
		p = iralloct(tsdn, old_ptr, old_usize, usize, alignment, zero,
		    tcache, arena, hook_args);
	}

	return p;
}

JEMALLOC_ALWAYS_INLINE void *
irallocx_prof(tsd_t *tsd, void *old_ptr, size_t old_usize, size_t size,
    size_t alignment, size_t *usize, bool zero, tcache_t *tcache,
    arena_t *arena, alloc_ctx_t *alloc_ctx, hook_ralloc_args_t *hook_args) {
	void *p;
	bool prof_active;
	prof_tctx_t *old_tctx, *tctx;

	prof_active = prof_active_get_unlocked();
	old_tctx = prof_tctx_get(tsd_tsdn(tsd), old_ptr, alloc_ctx);
	tctx = prof_alloc_prep(tsd, *usize, prof_active, false);
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U)) {
		p = irallocx_prof_sample(tsd_tsdn(tsd), old_ptr, old_usize,
		    *usize, alignment, zero, tcache, arena, tctx, hook_args);
	} else {
		p = iralloct(tsd_tsdn(tsd), old_ptr, old_usize, size, alignment,
		    zero, tcache, arena, hook_args);
	}
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx, false);
		return NULL;
	}

	if (p == old_ptr && alignment != 0) {
		/*
		 * The allocation did not move, so it is possible that the size
		 * class is smaller than would guarantee the requested
		 * alignment, and that the alignment constraint was
		 * serendipitously satisfied.  Additionally, old_usize may not
		 * be the same as the current usize because of in-place large
		 * reallocation.  Therefore, query the actual value of usize.
		 */
		*usize = isalloc(tsd_tsdn(tsd), p);
	}
	prof_realloc(tsd, p, *usize, tctx, prof_active, false, old_ptr,
	    old_usize, old_tctx);

	return p;
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2)
je_rallocx(void *ptr, size_t size, int flags) {
	void *p;
	tsd_t *tsd;
	size_t usize;
	size_t old_usize;
	size_t alignment = MALLOCX_ALIGN_GET(flags);
	bool zero = flags & MALLOCX_ZERO;
	arena_t *arena;
	tcache_t *tcache;

	LOG("core.rallocx.entry", "ptr: %p, size: %zu, flags: %d", ptr,
	    size, flags);


	assert(ptr != NULL);
	assert(size != 0);
	assert(malloc_initialized() || IS_INITIALIZER);
	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));

	if (unlikely((flags & MALLOCX_ARENA_MASK) != 0)) {
		unsigned arena_ind = MALLOCX_ARENA_GET(flags);
		arena = arena_get(tsd_tsdn(tsd), arena_ind, true);
		if (unlikely(arena == NULL)) {
			goto label_oom;
		}
	} else {
		arena = NULL;
	}

	if (unlikely((flags & MALLOCX_TCACHE_MASK) != 0)) {
		if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE) {
			tcache = NULL;
		} else {
			tcache = tcaches_get(tsd, MALLOCX_TCACHE_GET(flags));
		}
	} else {
		tcache = tcache_get(tsd);
	}

	alloc_ctx_t alloc_ctx;
	rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
	rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree, rtree_ctx,
	    (uintptr_t)ptr, true, &alloc_ctx.szind, &alloc_ctx.slab);
	assert(alloc_ctx.szind != SC_NSIZES);
	old_usize = sz_index2size(alloc_ctx.szind);
	assert(old_usize == isalloc(tsd_tsdn(tsd), ptr));

	hook_ralloc_args_t hook_args = {false, {(uintptr_t)ptr, size, flags,
		0}};
	if (config_prof && opt_prof) {
		usize = (alignment == 0) ?
		    sz_s2u(size) : sz_sa2u(size, alignment);
		if (unlikely(usize == 0
		    || usize > SC_LARGE_MAXCLASS)) {
			goto label_oom;
		}
		p = irallocx_prof(tsd, ptr, old_usize, size, alignment, &usize,
		    zero, tcache, arena, &alloc_ctx, &hook_args);
		if (unlikely(p == NULL)) {
			goto label_oom;
		}
	} else {
		p = iralloct(tsd_tsdn(tsd), ptr, old_usize, size, alignment,
		    zero, tcache, arena, &hook_args);
		if (unlikely(p == NULL)) {
			goto label_oom;
		}
		if (config_stats) {
			usize = isalloc(tsd_tsdn(tsd), p);
		}
	}
	assert(alignment == 0 || ((uintptr_t)p & (alignment - 1)) == ZU(0));

	if (config_stats) {
		*tsd_thread_allocatedp_get(tsd) += usize;
		*tsd_thread_deallocatedp_get(tsd) += old_usize;
	}
	UTRACE(ptr, size, p);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.rallocx.exit", "result: %p", p);
	return p;
label_oom:
	if (config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write("<jemalloc>: Error in rallocx(): out of memory\n");
		abort();
	}
	UTRACE(ptr, size, 0);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.rallocx.exit", "result: %p", NULL);
	return NULL;
}

JEMALLOC_ALWAYS_INLINE size_t
ixallocx_helper(tsdn_t *tsdn, void *ptr, size_t old_usize, size_t size,
    size_t extra, size_t alignment, bool zero) {
	size_t newsize;

	if (ixalloc(tsdn, ptr, old_usize, size, extra, alignment, zero,
	    &newsize)) {
		return old_usize;
	}

	return newsize;
}

static size_t
ixallocx_prof_sample(tsdn_t *tsdn, void *ptr, size_t old_usize, size_t size,
    size_t extra, size_t alignment, bool zero, prof_tctx_t *tctx) {
	size_t usize;

	if (tctx == NULL) {
		return old_usize;
	}
	usize = ixallocx_helper(tsdn, ptr, old_usize, size, extra, alignment,
	    zero);

	return usize;
}

JEMALLOC_ALWAYS_INLINE size_t
ixallocx_prof(tsd_t *tsd, void *ptr, size_t old_usize, size_t size,
    size_t extra, size_t alignment, bool zero, alloc_ctx_t *alloc_ctx) {
	size_t usize_max, usize;
	bool prof_active;
	prof_tctx_t *old_tctx, *tctx;

	prof_active = prof_active_get_unlocked();
	old_tctx = prof_tctx_get(tsd_tsdn(tsd), ptr, alloc_ctx);
	/*
	 * usize isn't knowable before ixalloc() returns when extra is non-zero.
	 * Therefore, compute its maximum possible value and use that in
	 * prof_alloc_prep() to decide whether to capture a backtrace.
	 * prof_realloc() will use the actual usize to decide whether to sample.
	 */
	if (alignment == 0) {
		usize_max = sz_s2u(size+extra);
		assert(usize_max > 0
		    && usize_max <= SC_LARGE_MAXCLASS);
	} else {
		usize_max = sz_sa2u(size+extra, alignment);
		if (unlikely(usize_max == 0
		    || usize_max > SC_LARGE_MAXCLASS)) {
			/*
			 * usize_max is out of range, and chances are that
			 * allocation will fail, but use the maximum possible
			 * value and carry on with prof_alloc_prep(), just in
			 * case allocation succeeds.
			 */
			usize_max = SC_LARGE_MAXCLASS;
		}
	}
	tctx = prof_alloc_prep(tsd, usize_max, prof_active, false);

	if (unlikely((uintptr_t)tctx != (uintptr_t)1U)) {
		usize = ixallocx_prof_sample(tsd_tsdn(tsd), ptr, old_usize,
		    size, extra, alignment, zero, tctx);
	} else {
		usize = ixallocx_helper(tsd_tsdn(tsd), ptr, old_usize, size,
		    extra, alignment, zero);
	}
	if (usize == old_usize) {
		prof_alloc_rollback(tsd, tctx, false);
		return usize;
	}
	prof_realloc(tsd, ptr, usize, tctx, prof_active, false, ptr, old_usize,
	    old_tctx);

	return usize;
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_xallocx(void *ptr, size_t size, size_t extra, int flags) {
	tsd_t *tsd;
	size_t usize, old_usize;
	size_t alignment = MALLOCX_ALIGN_GET(flags);
	bool zero = flags & MALLOCX_ZERO;

	LOG("core.xallocx.entry", "ptr: %p, size: %zu, extra: %zu, "
	    "flags: %d", ptr, size, extra, flags);

	assert(ptr != NULL);
	assert(size != 0);
	assert(SIZE_T_MAX - size >= extra);
	assert(malloc_initialized() || IS_INITIALIZER);
	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));

	alloc_ctx_t alloc_ctx;
	rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
	rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree, rtree_ctx,
	    (uintptr_t)ptr, true, &alloc_ctx.szind, &alloc_ctx.slab);
	assert(alloc_ctx.szind != SC_NSIZES);
	old_usize = sz_index2size(alloc_ctx.szind);
	assert(old_usize == isalloc(tsd_tsdn(tsd), ptr));
	/*
	 * The API explicitly absolves itself of protecting against (size +
	 * extra) numerical overflow, but we may need to clamp extra to avoid
	 * exceeding SC_LARGE_MAXCLASS.
	 *
	 * Ordinarily, size limit checking is handled deeper down, but here we
	 * have to check as part of (size + extra) clamping, since we need the
	 * clamped value in the above helper functions.
	 */
	if (unlikely(size > SC_LARGE_MAXCLASS)) {
		usize = old_usize;
		goto label_not_resized;
	}
	if (unlikely(SC_LARGE_MAXCLASS - size < extra)) {
		extra = SC_LARGE_MAXCLASS - size;
	}

	if (config_prof && opt_prof) {
		usize = ixallocx_prof(tsd, ptr, old_usize, size, extra,
		    alignment, zero, &alloc_ctx);
	} else {
		usize = ixallocx_helper(tsd_tsdn(tsd), ptr, old_usize, size,
		    extra, alignment, zero);
	}
	if (unlikely(usize == old_usize)) {
		goto label_not_resized;
	}

	if (config_stats) {
		*tsd_thread_allocatedp_get(tsd) += usize;
		*tsd_thread_deallocatedp_get(tsd) += old_usize;
	}
label_not_resized:
	if (unlikely(!tsd_fast(tsd))) {
		uintptr_t args[4] = {(uintptr_t)ptr, size, extra, flags};
		hook_invoke_expand(hook_expand_xallocx, ptr, old_usize,
		    usize, (uintptr_t)usize, args);
	}

	UTRACE(ptr, size, ptr);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.xallocx.exit", "result: %zu", usize);
	return usize;
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
JEMALLOC_ATTR(pure)
je_sallocx(const void *ptr, int flags) {
	size_t usize;
	tsdn_t *tsdn;

	LOG("core.sallocx.entry", "ptr: %p, flags: %d", ptr, flags);

	assert(malloc_initialized() || IS_INITIALIZER);
	assert(ptr != NULL);

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	if (config_debug || force_ivsalloc) {
		usize = ivsalloc(tsdn, ptr);
		assert(force_ivsalloc || usize != 0);
	} else {
		usize = isalloc(tsdn, ptr);
	}

	check_entry_exit_locking(tsdn);

	LOG("core.sallocx.exit", "result: %zu", usize);
	return usize;
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_dallocx(void *ptr, int flags) {
	LOG("core.dallocx.entry", "ptr: %p, flags: %d", ptr, flags);

	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	tsd_t *tsd = tsd_fetch();
	bool fast = tsd_fast(tsd);
	check_entry_exit_locking(tsd_tsdn(tsd));

	tcache_t *tcache;
	if (unlikely((flags & MALLOCX_TCACHE_MASK) != 0)) {
		/* Not allowed to be reentrant and specify a custom tcache. */
		assert(tsd_reentrancy_level_get(tsd) == 0);
		if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE) {
			tcache = NULL;
		} else {
			tcache = tcaches_get(tsd, MALLOCX_TCACHE_GET(flags));
		}
	} else {
		if (likely(fast)) {
			tcache = tsd_tcachep_get(tsd);
			assert(tcache == tcache_get(tsd));
		} else {
			if (likely(tsd_reentrancy_level_get(tsd) == 0)) {
				tcache = tcache_get(tsd);
			}  else {
				tcache = NULL;
			}
		}
	}

	UTRACE(ptr, 0, 0);
	if (likely(fast)) {
		tsd_assert_fast(tsd);
		ifree(tsd, ptr, tcache, false);
	} else {
		uintptr_t args_raw[3] = {(uintptr_t)ptr, flags};
		hook_invoke_dalloc(hook_dalloc_dallocx, ptr, args_raw);
		ifree(tsd, ptr, tcache, true);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.dallocx.exit", "");
}

JEMALLOC_ALWAYS_INLINE size_t
inallocx(tsdn_t *tsdn, size_t size, int flags) {
	check_entry_exit_locking(tsdn);

	size_t usize;
	if (likely((flags & MALLOCX_LG_ALIGN_MASK) == 0)) {
		usize = sz_s2u(size);
	} else {
		usize = sz_sa2u(size, MALLOCX_ALIGN_GET_SPECIFIED(flags));
	}
	check_entry_exit_locking(tsdn);
	return usize;
}

JEMALLOC_NOINLINE void
sdallocx_default(void *ptr, size_t size, int flags) {
	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	tsd_t *tsd = tsd_fetch();
	bool fast = tsd_fast(tsd);
	size_t usize = inallocx(tsd_tsdn(tsd), size, flags);
	assert(usize == isalloc(tsd_tsdn(tsd), ptr));
	check_entry_exit_locking(tsd_tsdn(tsd));

	tcache_t *tcache;
	if (unlikely((flags & MALLOCX_TCACHE_MASK) != 0)) {
		/* Not allowed to be reentrant and specify a custom tcache. */
		assert(tsd_reentrancy_level_get(tsd) == 0);
		if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE) {
			tcache = NULL;
		} else {
			tcache = tcaches_get(tsd, MALLOCX_TCACHE_GET(flags));
		}
	} else {
		if (likely(fast)) {
			tcache = tsd_tcachep_get(tsd);
			assert(tcache == tcache_get(tsd));
		} else {
			if (likely(tsd_reentrancy_level_get(tsd) == 0)) {
				tcache = tcache_get(tsd);
			} else {
				tcache = NULL;
			}
		}
	}

	UTRACE(ptr, 0, 0);
	if (likely(fast)) {
		tsd_assert_fast(tsd);
		isfree(tsd, ptr, usize, tcache, false);
	} else {
		uintptr_t args_raw[3] = {(uintptr_t)ptr, size, flags};
		hook_invoke_dalloc(hook_dalloc_sdallocx, ptr, args_raw);
		isfree(tsd, ptr, usize, tcache, true);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));

}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_sdallocx(void *ptr, size_t size, int flags) {
	LOG("core.sdallocx.entry", "ptr: %p, size: %zu, flags: %d", ptr,
		size, flags);

	if (flags !=0 || !free_fastpath(ptr, size, true)) {
		sdallocx_default(ptr, size, flags);
	}

	LOG("core.sdallocx.exit", "");
}

void JEMALLOC_NOTHROW
je_sdallocx_noflags(void *ptr, size_t size) {
	LOG("core.sdallocx.entry", "ptr: %p, size: %zu, flags: 0", ptr,
		size);

	if (!free_fastpath(ptr, size, true)) {
		sdallocx_default(ptr, size, 0);
	}

	LOG("core.sdallocx.exit", "");
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
JEMALLOC_ATTR(pure)
je_nallocx(size_t size, int flags) {
	size_t usize;
	tsdn_t *tsdn;

	assert(size != 0);

	if (unlikely(malloc_init())) {
		LOG("core.nallocx.exit", "result: %zu", ZU(0));
		return 0;
	}

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	usize = inallocx(tsdn, size, flags);
	if (unlikely(usize > SC_LARGE_MAXCLASS)) {
		LOG("core.nallocx.exit", "result: %zu", ZU(0));
		return 0;
	}

	check_entry_exit_locking(tsdn);
	LOG("core.nallocx.exit", "result: %zu", usize);
	return usize;
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	int ret;
	tsd_t *tsd;

	LOG("core.mallctl.entry", "name: %s", name);

	if (unlikely(malloc_init())) {
		LOG("core.mallctl.exit", "result: %d", EAGAIN);
		return EAGAIN;
	}

	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));
	ret = ctl_byname(tsd, name, oldp, oldlenp, newp, newlen);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.mallctl.exit", "result: %d", ret);
	return ret;
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctlnametomib(const char *name, size_t *mibp, size_t *miblenp) {
	int ret;

	LOG("core.mallctlnametomib.entry", "name: %s", name);

	if (unlikely(malloc_init())) {
		LOG("core.mallctlnametomib.exit", "result: %d", EAGAIN);
		return EAGAIN;
	}

	tsd_t *tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));
	ret = ctl_nametomib(tsd, name, mibp, miblenp);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.mallctlnametomib.exit", "result: %d", ret);
	return ret;
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctlbymib(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
  void *newp, size_t newlen) {
	int ret;
	tsd_t *tsd;

	LOG("core.mallctlbymib.entry", "");

	if (unlikely(malloc_init())) {
		LOG("core.mallctlbymib.exit", "result: %d", EAGAIN);
		return EAGAIN;
	}

	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));
	ret = ctl_bymib(tsd, mib, miblen, oldp, oldlenp, newp, newlen);
	check_entry_exit_locking(tsd_tsdn(tsd));
	LOG("core.mallctlbymib.exit", "result: %d", ret);
	return ret;
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_malloc_stats_print(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *opts) {
	tsdn_t *tsdn;

	LOG("core.malloc_stats_print.entry", "");

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);
	stats_print(write_cb, cbopaque, opts);
	check_entry_exit_locking(tsdn);
	LOG("core.malloc_stats_print.exit", "");
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_malloc_usable_size(JEMALLOC_USABLE_SIZE_CONST void *ptr) {
	size_t ret;
	tsdn_t *tsdn;

	LOG("core.malloc_usable_size.entry", "ptr: %p", ptr);

	assert(malloc_initialized() || IS_INITIALIZER);

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	if (unlikely(ptr == NULL)) {
		ret = 0;
	} else {
		if (config_debug || force_ivsalloc) {
			ret = ivsalloc(tsdn, ptr);
			assert(force_ivsalloc || ret != 0);
		} else {
			ret = isalloc(tsdn, ptr);
		}
	}

	check_entry_exit_locking(tsdn);
	LOG("core.malloc_usable_size.exit", "result: %zu", ret);
	return ret;
}

/*
 * End non-standard functions.
 */
/******************************************************************************/
/*
 * The following functions are used by threading libraries for protection of
 * malloc during fork().
 */

/*
 * If an application creates a thread before doing any allocation in the main
 * thread, then calls fork(2) in the main thread followed by memory allocation
 * in the child process, a race can occur that results in deadlock within the
 * child: the main thread may have forked while the created thread had
 * partially initialized the allocator.  Ordinarily jemalloc prevents
 * fork/malloc races via the following functions it registers during
 * initialization using pthread_atfork(), but of course that does no good if
 * the allocator isn't fully initialized at fork time.  The following library
 * constructor is a partial solution to this problem.  It may still be possible
 * to trigger the deadlock described above, but doing so would involve forking
 * via a library constructor that runs before jemalloc's runs.
 */
#ifndef JEMALLOC_JET
JEMALLOC_ATTR(constructor)
static void
jemalloc_constructor(void) {
	malloc_init();
}
#endif

#ifndef JEMALLOC_MUTEX_INIT_CB
void
jemalloc_prefork(void)
#else
JEMALLOC_EXPORT void
_malloc_prefork(void)
#endif
{
	tsd_t *tsd;
	unsigned i, j, narenas;
	arena_t *arena;

#ifdef JEMALLOC_MUTEX_INIT_CB
	if (!malloc_initialized()) {
		return;
	}
#endif
	assert(malloc_initialized());

	tsd = tsd_fetch();

	narenas = narenas_total_get();

	witness_prefork(tsd_witness_tsdp_get(tsd));
	/* Acquire all mutexes in a safe order. */
	ctl_prefork(tsd_tsdn(tsd));
	tcache_prefork(tsd_tsdn(tsd));
	malloc_mutex_prefork(tsd_tsdn(tsd), &arenas_lock);
	if (have_background_thread) {
		background_thread_prefork0(tsd_tsdn(tsd));
	}
	prof_prefork0(tsd_tsdn(tsd));
	if (have_background_thread) {
		background_thread_prefork1(tsd_tsdn(tsd));
	}
	/* Break arena prefork into stages to preserve lock order. */
	for (i = 0; i < 8; i++) {
		for (j = 0; j < narenas; j++) {
			if ((arena = arena_get(tsd_tsdn(tsd), j, false)) !=
			    NULL) {
				switch (i) {
				case 0:
					arena_prefork0(tsd_tsdn(tsd), arena);
					break;
				case 1:
					arena_prefork1(tsd_tsdn(tsd), arena);
					break;
				case 2:
					arena_prefork2(tsd_tsdn(tsd), arena);
					break;
				case 3:
					arena_prefork3(tsd_tsdn(tsd), arena);
					break;
				case 4:
					arena_prefork4(tsd_tsdn(tsd), arena);
					break;
				case 5:
					arena_prefork5(tsd_tsdn(tsd), arena);
					break;
				case 6:
					arena_prefork6(tsd_tsdn(tsd), arena);
					break;
				case 7:
					arena_prefork7(tsd_tsdn(tsd), arena);
					break;
				default: not_reached();
				}
			}
		}
	}
	prof_prefork1(tsd_tsdn(tsd));
	tsd_prefork(tsd);
}

#ifndef JEMALLOC_MUTEX_INIT_CB
void
jemalloc_postfork_parent(void)
#else
JEMALLOC_EXPORT void
_malloc_postfork(void)
#endif
{
	tsd_t *tsd;
	unsigned i, narenas;

#ifdef JEMALLOC_MUTEX_INIT_CB
	if (!malloc_initialized()) {
		return;
	}
#endif
	assert(malloc_initialized());

	tsd = tsd_fetch();

	tsd_postfork_parent(tsd);

	witness_postfork_parent(tsd_witness_tsdp_get(tsd));
	/* Release all mutexes, now that fork() has completed. */
	for (i = 0, narenas = narenas_total_get(); i < narenas; i++) {
		arena_t *arena;

		if ((arena = arena_get(tsd_tsdn(tsd), i, false)) != NULL) {
			arena_postfork_parent(tsd_tsdn(tsd), arena);
		}
	}
	prof_postfork_parent(tsd_tsdn(tsd));
	if (have_background_thread) {
		background_thread_postfork_parent(tsd_tsdn(tsd));
	}
	malloc_mutex_postfork_parent(tsd_tsdn(tsd), &arenas_lock);
	tcache_postfork_parent(tsd_tsdn(tsd));
	ctl_postfork_parent(tsd_tsdn(tsd));
}

void
jemalloc_postfork_child(void) {
	tsd_t *tsd;
	unsigned i, narenas;

	assert(malloc_initialized());

	tsd = tsd_fetch();

	tsd_postfork_child(tsd);

	witness_postfork_child(tsd_witness_tsdp_get(tsd));
	/* Release all mutexes, now that fork() has completed. */
	for (i = 0, narenas = narenas_total_get(); i < narenas; i++) {
		arena_t *arena;

		if ((arena = arena_get(tsd_tsdn(tsd), i, false)) != NULL) {
			arena_postfork_child(tsd_tsdn(tsd), arena);
		}
	}
	prof_postfork_child(tsd_tsdn(tsd));
	if (have_background_thread) {
		background_thread_postfork_child(tsd_tsdn(tsd));
	}
	malloc_mutex_postfork_child(tsd_tsdn(tsd), &arenas_lock);
	tcache_postfork_child(tsd_tsdn(tsd));
	ctl_postfork_child(tsd_tsdn(tsd));
}

/******************************************************************************/
