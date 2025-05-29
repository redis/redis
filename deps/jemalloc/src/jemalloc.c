#define JEMALLOC_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/buf_writer.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/emap.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/fxp.h"
#include "jemalloc/internal/san.h"
#include "jemalloc/internal/hook.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/log.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/spin.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/ticker.h"
#include "jemalloc/internal/thread_event.h"
#include "jemalloc/internal/util.h"

/******************************************************************************/
/* Data. */

/* Runtime configuration options. */
const char	*je_malloc_conf
#ifndef _WIN32
    JEMALLOC_ATTR(weak)
#endif
    ;
/*
 * The usual rule is that the closer to runtime you are, the higher priority
 * your configuration settings are (so the jemalloc config options get lower
 * priority than the per-binary setting, which gets lower priority than the /etc
 * setting, which gets lower priority than the environment settings).
 *
 * But it's a fairly common use case in some testing environments for a user to
 * be able to control the binary, but nothing else (e.g. a performancy canary
 * uses the production OS and environment variables, but can run any binary in
 * those circumstances).  For these use cases, it's handy to have an in-binary
 * mechanism for overriding environment variable settings, with the idea that if
 * the results are positive they get promoted to the official settings, and
 * moved from the binary to the environment variable.
 *
 * We don't actually want this to be widespread, so we'll give it a silly name
 * and not mention it in headers or documentation.
 */
const char	*je_malloc_conf_2_conf_harder
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
bool	opt_trust_madvise =
#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS
    false
#else
    true
#endif
    ;

bool opt_cache_oblivious =
#ifdef JEMALLOC_CACHE_OBLIVIOUS
    true
#else
    false
#endif
    ;

zero_realloc_action_t opt_zero_realloc_action =
#ifdef JEMALLOC_ZERO_REALLOC_DEFAULT_FREE
    zero_realloc_action_free
#else
    zero_realloc_action_alloc
#endif
    ;

atomic_zu_t zero_realloc_count = ATOMIC_INIT(0);

const char *zero_realloc_mode_names[] = {
	"alloc",
	"free",
	"abort",
};

/*
 * These are the documented values for junk fill debugging facilities -- see the
 * man page.
 */
static const uint8_t junk_alloc_byte = 0xa5;
static const uint8_t junk_free_byte = 0x5a;

static void default_junk_alloc(void *ptr, size_t usize) {
	memset(ptr, junk_alloc_byte, usize);
}

static void default_junk_free(void *ptr, size_t usize) {
	memset(ptr, junk_free_byte, usize);
}

void (*junk_alloc_callback)(void *ptr, size_t size) = &default_junk_alloc;
void (*junk_free_callback)(void *ptr, size_t size) = &default_junk_free;

bool	opt_utrace = false;
bool	opt_xmalloc = false;
bool	opt_experimental_infallible_new = false;
bool	opt_zero = false;
unsigned	opt_narenas = 0;
fxp_t		opt_narenas_ratio = FXP_INIT_INT(4);

unsigned	ncpus;

/* Protects arenas initialization. */
malloc_mutex_t arenas_lock;

/* The global hpa, and whether it's on. */
bool opt_hpa = false;
hpa_shard_opts_t opt_hpa_opts = HPA_SHARD_OPTS_DEFAULT;
sec_opts_t opt_hpa_sec_opts = SEC_OPTS_DEFAULT;

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

malloc_init_t malloc_init_state = malloc_init_uninitialized;

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
		UTRACE_CALL(&ut, sizeof(ut));				\
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
 * FreeBSD's libc uses the bootstrap_*() functions in bootstrap-sensitive
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
arena_init_locked(tsdn_t *tsdn, unsigned ind, const arena_config_t *config) {
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
	arena = arena_new(tsdn, ind, config);

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
arena_init(tsdn_t *tsdn, unsigned ind, const arena_config_t *config) {
	arena_t *arena;

	malloc_mutex_lock(tsdn, &arenas_lock);
	arena = arena_init_locked(tsdn, ind, config);
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
arena_migrate(tsd_t *tsd, arena_t *oldarena, arena_t *newarena) {
	assert(oldarena != NULL);
	assert(newarena != NULL);

	arena_nthreads_dec(oldarena, false);
	arena_nthreads_inc(newarena, false);
	tsd_arena_set(tsd, newarena);

	if (arena_nthreads_get(oldarena, false) == 0) {
		/* Purge if the old arena has no associated threads anymore. */
		arena_decay(tsd_tsdn(tsd), oldarena,
		    /* is_background_thread */ false, /* all */ true);
	}
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
				    choose[j], &arena_config_default);
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
				tcache_slow_t *tcache_slow;

				malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
				ql_foreach(tcache_slow, &arena->tcache_ql,
				    link) {
					tcache_stats_merge(tsdn,
					    tcache_slow->tcache, arena);
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
#elif defined(CPU_COUNT)
	/*
	 * glibc >= 2.6 has the CPU_COUNT macro.
	 *
	 * glibc's sysconf() uses isspace().  glibc allocates for the first time
	 * *before* setting up the isspace tables.  Therefore we need a
	 * different method to get the number of CPUs.
	 *
	 * The getaffinity approach is also preferred when only a subset of CPUs
	 * is available, to avoid using more arenas than necessary.
	 */
	{
#  if defined(__FreeBSD__) || defined(__DragonFly__)
		cpuset_t set;
#  else
		cpu_set_t set;
#  endif
#  if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
		sched_getaffinity(0, sizeof(set), &set);
#  else
		pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
#  endif
		result = CPU_COUNT(&set);
	}
#else
	result = sysconf(_SC_NPROCESSORS_ONLN);
#endif
	return ((result == -1) ? 1 : (unsigned)result);
}

/*
 * Ensure that number of CPUs is determistinc, i.e. it is the same based on:
 * - sched_getaffinity()
 * - _SC_NPROCESSORS_ONLN
 * - _SC_NPROCESSORS_CONF
 * Since otherwise tricky things is possible with percpu arenas in use.
 */
static bool
malloc_cpu_count_is_deterministic()
{
#ifdef _WIN32
	return true;
#else
	long cpu_onln = sysconf(_SC_NPROCESSORS_ONLN);
	long cpu_conf = sysconf(_SC_NPROCESSORS_CONF);
	if (cpu_onln != cpu_conf) {
		return false;
	}
#  if defined(CPU_COUNT)
#    if defined(__FreeBSD__) || defined(__DragonFly__)
	cpuset_t set;
#    else
	cpu_set_t set;
#    endif /* __FreeBSD__ */
#    if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
	sched_getaffinity(0, sizeof(set), &set);
#    else /* !JEMALLOC_HAVE_SCHED_SETAFFINITY */
	pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
#    endif /* JEMALLOC_HAVE_SCHED_SETAFFINITY */
	long cpu_affinity = CPU_COUNT(&set);
	if (cpu_affinity != cpu_conf) {
		return false;
	}
#  endif /* CPU_COUNT */
	return true;
#endif
}

static void
init_opt_stats_opts(const char *v, size_t vlen, char *dest) {
	size_t opts_len = strlen(dest);
	assert(opts_len <= stats_print_tot_num_options);

	for (size_t i = 0; i < vlen; i++) {
		switch (v[i]) {
#define OPTION(o, v, d, s) case o: break;
			STATS_PRINT_OPTIONS
#undef OPTION
		default: continue;
		}

		if (strchr(dest, v[i]) != NULL) {
			/* Ignore repeated. */
			continue;
		}

		dest[opts_len++] = v[i];
		dest[opts_len] = '\0';
		assert(opts_len <= stats_print_tot_num_options);
	}
	assert(opts_len == strlen(dest));
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
				had_conf_error = true;
			}
			return true;
		default:
			malloc_write("<jemalloc>: Malformed conf string\n");
			had_conf_error = true;
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
				had_conf_error = true;
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
#define MALLOC_CONF_NSOURCES 5

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
	} case 4: {
		ret = je_malloc_conf_2_conf_harder;
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
		("\"name\" of the file referenced by the symbolic link named "
		    "/etc/malloc.conf"),
		"value of the environment variable MALLOC_CONF",
		("string pointed to by the global variable "
		    "malloc_conf_2_conf_harder"),
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

#define CONF_VALUE_READ(max_t, result)					\
	      char *end;						\
	      set_errno(0);						\
	      result = (max_t)malloc_strtoumax(v, &end, 0);
#define CONF_VALUE_READ_FAIL()						\
	      (get_errno() != 0 || (uintptr_t)end - (uintptr_t)v != vlen)

#define CONF_HANDLE_T(t, max_t, o, n, min, max, check_min, check_max, clip) \
			if (CONF_MATCH(n)) {				\
				max_t mv;				\
				CONF_VALUE_READ(max_t, mv)		\
				if (CONF_VALUE_READ_FAIL()) {		\
					CONF_ERROR("Invalid conf value",\
					    k, klen, v, vlen);		\
				} else if (clip) {			\
					if (check_min(mv, (t)(min))) {	\
						o = (t)(min);		\
					} else if (			\
					    check_max(mv, (t)(max))) {	\
						o = (t)(max);		\
					} else {			\
						o = (t)mv;		\
					}				\
				} else {				\
					if (check_min(mv, (t)(min)) ||	\
					    check_max(mv, (t)(max))) {	\
						CONF_ERROR(		\
						    "Out-of-range "	\
						    "conf value",	\
						    k, klen, v, vlen);	\
					} else {			\
						o = (t)mv;		\
					}				\
				}					\
				CONF_CONTINUE;				\
			}
#define CONF_HANDLE_T_U(t, o, n, min, max, check_min, check_max, clip)	\
	      CONF_HANDLE_T(t, uintmax_t, o, n, min, max, check_min,	\
			    check_max, clip)
#define CONF_HANDLE_T_SIGNED(t, o, n, min, max, check_min, check_max, clip)\
	      CONF_HANDLE_T(t, intmax_t, o, n, min, max, check_min,	\
			    check_max, clip)

#define CONF_HANDLE_UNSIGNED(o, n, min, max, check_min, check_max,	\
    clip)								\
			CONF_HANDLE_T_U(unsigned, o, n, min, max,	\
			    check_min, check_max, clip)
#define CONF_HANDLE_SIZE_T(o, n, min, max, check_min, check_max, clip)	\
			CONF_HANDLE_T_U(size_t, o, n, min, max,		\
			    check_min, check_max, clip)
#define CONF_HANDLE_INT64_T(o, n, min, max, check_min, check_max, clip)	\
			CONF_HANDLE_T_SIGNED(int64_t, o, n, min, max,	\
			    check_min, check_max, clip)
#define CONF_HANDLE_UINT64_T(o, n, min, max, check_min, check_max, clip)\
			CONF_HANDLE_T_U(uint64_t, o, n, min, max,	\
			    check_min, check_max, clip)
#define CONF_HANDLE_SSIZE_T(o, n, min, max)				\
			CONF_HANDLE_T_SIGNED(ssize_t, o, n, min, max,	\
			    CONF_CHECK_MIN, CONF_CHECK_MAX, false)
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
			CONF_HANDLE_BOOL(opt_trust_madvise, "trust_madvise")
			if (strncmp("metadata_thp", k, klen) == 0) {
				int m;
				bool match = false;
				for (m = 0; m < metadata_thp_mode_limit; m++) {
					if (strncmp(metadata_thp_mode_names[m],
					    v, vlen) == 0) {
						opt_metadata_thp = m;
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
				int m;
				bool match = false;
				for (m = 0; m < dss_prec_limit; m++) {
					if (strncmp(dss_prec_names[m], v, vlen)
					    == 0) {
						if (extent_dss_prec_set(m)) {
							CONF_ERROR(
							    "Error setting dss",
							    k, klen, v, vlen);
						} else {
							opt_dss =
							    dss_prec_names[m];
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
			if (CONF_MATCH("narenas")) {
				if (CONF_MATCH_VALUE("default")) {
					opt_narenas = 0;
					CONF_CONTINUE;
				} else {
					CONF_HANDLE_UNSIGNED(opt_narenas,
					    "narenas", 1, UINT_MAX,
					    CONF_CHECK_MIN, CONF_DONT_CHECK_MAX,
					    /* clip */ false)
				}
			}
			if (CONF_MATCH("narenas_ratio")) {
				char *end;
				bool err = fxp_parse(&opt_narenas_ratio, v,
				    &end);
				if (err || (size_t)(end - v) != vlen) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
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
			CONF_HANDLE_INT64_T(opt_mutex_max_spin,
			    "mutex_max_spin", -1, INT64_MAX, CONF_CHECK_MIN,
			    CONF_DONT_CHECK_MAX, false);
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
				init_opt_stats_opts(v, vlen,
				    opt_stats_print_opts);
				CONF_CONTINUE;
			}
			CONF_HANDLE_INT64_T(opt_stats_interval,
			    "stats_interval", -1, INT64_MAX,
			    CONF_CHECK_MIN, CONF_DONT_CHECK_MAX, false)
			if (CONF_MATCH("stats_interval_opts")) {
				init_opt_stats_opts(v, vlen,
				    opt_stats_interval_opts);
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
			if (config_enable_cxx) {
				CONF_HANDLE_BOOL(
				    opt_experimental_infallible_new,
				    "experimental_infallible_new")
			}

			CONF_HANDLE_BOOL(opt_tcache, "tcache")
			CONF_HANDLE_SIZE_T(opt_tcache_max, "tcache_max",
			    0, TCACHE_MAXCLASS_LIMIT, CONF_DONT_CHECK_MIN,
			    CONF_CHECK_MAX, /* clip */ true)
			if (CONF_MATCH("lg_tcache_max")) {
				size_t m;
				CONF_VALUE_READ(size_t, m)
				if (CONF_VALUE_READ_FAIL()) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				} else {
					/* clip if necessary */
					if (m > TCACHE_LG_MAXCLASS_LIMIT) {
						m = TCACHE_LG_MAXCLASS_LIMIT;
					}
					opt_tcache_max = (size_t)1 << m;
				}
				CONF_CONTINUE;
			}
			/*
			 * Anyone trying to set a value outside -16 to 16 is
			 * deeply confused.
			 */
			CONF_HANDLE_SSIZE_T(opt_lg_tcache_nslots_mul,
			    "lg_tcache_nslots_mul", -16, 16)
			/* Ditto with values past 2048. */
			CONF_HANDLE_UNSIGNED(opt_tcache_nslots_small_min,
			    "tcache_nslots_small_min", 1, 2048,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, /* clip */ true)
			CONF_HANDLE_UNSIGNED(opt_tcache_nslots_small_max,
			    "tcache_nslots_small_max", 1, 2048,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, /* clip */ true)
			CONF_HANDLE_UNSIGNED(opt_tcache_nslots_large,
			    "tcache_nslots_large", 1, 2048,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, /* clip */ true)
			CONF_HANDLE_SIZE_T(opt_tcache_gc_incr_bytes,
			    "tcache_gc_incr_bytes", 1024, SIZE_T_MAX,
			    CONF_CHECK_MIN, CONF_DONT_CHECK_MAX,
			    /* clip */ true)
			CONF_HANDLE_SIZE_T(opt_tcache_gc_delay_bytes,
			    "tcache_gc_delay_bytes", 0, SIZE_T_MAX,
			    CONF_DONT_CHECK_MIN, CONF_DONT_CHECK_MAX,
			    /* clip */ false)
			CONF_HANDLE_UNSIGNED(opt_lg_tcache_flush_small_div,
			    "lg_tcache_flush_small_div", 1, 16,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, /* clip */ true)
			CONF_HANDLE_UNSIGNED(opt_lg_tcache_flush_large_div,
			    "lg_tcache_flush_large_div", 1, 16,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, /* clip */ true)

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
				for (int m = percpu_arena_mode_names_base; m <
				    percpu_arena_mode_names_limit; m++) {
					if (strncmp(percpu_arena_mode_names[m],
					    v, vlen) == 0) {
						if (!have_percpu_arena) {
							CONF_ERROR(
							    "No getcpu support",
							    k, klen, v, vlen);
						}
						opt_percpu_arena = m;
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
			CONF_HANDLE_BOOL(opt_hpa, "hpa")
			CONF_HANDLE_SIZE_T(opt_hpa_opts.slab_max_alloc,
			    "hpa_slab_max_alloc", PAGE, HUGEPAGE,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, true);

			/*
			 * Accept either a ratio-based or an exact hugification
			 * threshold.
			 */
			CONF_HANDLE_SIZE_T(opt_hpa_opts.hugification_threshold,
			    "hpa_hugification_threshold", PAGE, HUGEPAGE,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, true);
			if (CONF_MATCH("hpa_hugification_threshold_ratio")) {
				fxp_t ratio;
				char *end;
				bool err = fxp_parse(&ratio, v,
				    &end);
				if (err || (size_t)(end - v) != vlen
				    || ratio > FXP_INIT_INT(1)) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				} else {
					opt_hpa_opts.hugification_threshold =
					    fxp_mul_frac(HUGEPAGE, ratio);
				}
				CONF_CONTINUE;
			}

			CONF_HANDLE_UINT64_T(
			    opt_hpa_opts.hugify_delay_ms, "hpa_hugify_delay_ms",
			    0, 0, CONF_DONT_CHECK_MIN, CONF_DONT_CHECK_MAX,
			    false);

			CONF_HANDLE_UINT64_T(
			    opt_hpa_opts.min_purge_interval_ms,
			    "hpa_min_purge_interval_ms", 0, 0,
			    CONF_DONT_CHECK_MIN, CONF_DONT_CHECK_MAX, false);

			if (CONF_MATCH("hpa_dirty_mult")) {
				if (CONF_MATCH_VALUE("-1")) {
					opt_hpa_opts.dirty_mult = (fxp_t)-1;
					CONF_CONTINUE;
				}
				fxp_t ratio;
				char *end;
				bool err = fxp_parse(&ratio, v,
				    &end);
				if (err || (size_t)(end - v) != vlen) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				} else {
					opt_hpa_opts.dirty_mult = ratio;
				}
				CONF_CONTINUE;
			}

			CONF_HANDLE_SIZE_T(opt_hpa_sec_opts.nshards,
			    "hpa_sec_nshards", 0, 0, CONF_CHECK_MIN,
			    CONF_DONT_CHECK_MAX, true);
			CONF_HANDLE_SIZE_T(opt_hpa_sec_opts.max_alloc,
			    "hpa_sec_max_alloc", PAGE, 0, CONF_CHECK_MIN,
			    CONF_DONT_CHECK_MAX, true);
			CONF_HANDLE_SIZE_T(opt_hpa_sec_opts.max_bytes,
			    "hpa_sec_max_bytes", PAGE, 0, CONF_CHECK_MIN,
			    CONF_DONT_CHECK_MAX, true);
			CONF_HANDLE_SIZE_T(opt_hpa_sec_opts.bytes_after_flush,
			    "hpa_sec_bytes_after_flush", PAGE, 0,
			    CONF_CHECK_MIN, CONF_DONT_CHECK_MAX, true);
			CONF_HANDLE_SIZE_T(opt_hpa_sec_opts.batch_fill_extra,
			    "hpa_sec_batch_fill_extra", 0, HUGEPAGE_PAGES,
			    CONF_CHECK_MIN, CONF_CHECK_MAX, true);

			if (CONF_MATCH("slab_sizes")) {
				if (CONF_MATCH_VALUE("default")) {
					sc_data_init(sc_data);
					CONF_CONTINUE;
				}
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
				CONF_HANDLE_BOOL(opt_prof_leak_error,
				    "prof_leak_error")
				CONF_HANDLE_BOOL(opt_prof_log, "prof_log")
				CONF_HANDLE_SSIZE_T(opt_prof_recent_alloc_max,
				    "prof_recent_alloc_max", -1, SSIZE_MAX)
				CONF_HANDLE_BOOL(opt_prof_stats, "prof_stats")
				CONF_HANDLE_BOOL(opt_prof_sys_thread_name,
				    "prof_sys_thread_name")
				if (CONF_MATCH("prof_time_resolution")) {
					if (CONF_MATCH_VALUE("default")) {
						opt_prof_time_res =
						    prof_time_res_default;
					} else if (CONF_MATCH_VALUE("high")) {
						if (!config_high_res_timer) {
							CONF_ERROR(
							    "No high resolution"
							    " timer support",
							    k, klen, v, vlen);
						} else {
							opt_prof_time_res =
							    prof_time_res_high;
						}
					} else {
						CONF_ERROR("Invalid conf value",
						    k, klen, v, vlen);
					}
					CONF_CONTINUE;
				}
				/*
				 * Undocumented.  When set to false, don't
				 * correct for an unbiasing bug in jeprof
				 * attribution.  This can be handy if you want
				 * to get consistent numbers from your binary
				 * across different jemalloc versions, even if
				 * those numbers are incorrect.  The default is
				 * true.
				 */
				CONF_HANDLE_BOOL(opt_prof_unbias, "prof_unbias")
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
				for (int m = 0; m < thp_mode_names_limit; m++) {
					if (strncmp(thp_mode_names[m],v, vlen)
					    == 0) {
						if (!have_madvise_huge && !have_memcntl) {
							CONF_ERROR(
							    "No THP support",
							    k, klen, v, vlen);
						}
						opt_thp = m;
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
			if (CONF_MATCH("zero_realloc")) {
				if (CONF_MATCH_VALUE("alloc")) {
					opt_zero_realloc_action
					    = zero_realloc_action_alloc;
				} else if (CONF_MATCH_VALUE("free")) {
					opt_zero_realloc_action
					    = zero_realloc_action_free;
				} else if (CONF_MATCH_VALUE("abort")) {
					opt_zero_realloc_action
					    = zero_realloc_action_abort;
				} else {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			if (config_uaf_detection &&
			    CONF_MATCH("lg_san_uaf_align")) {
				ssize_t a;
				CONF_VALUE_READ(ssize_t, a)
				if (CONF_VALUE_READ_FAIL() || a < -1) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				if (a == -1) {
					opt_lg_san_uaf_align = -1;
					CONF_CONTINUE;
				}

				/* clip if necessary */
				ssize_t max_allowed = (sizeof(size_t) << 3) - 1;
				ssize_t min_allowed = LG_PAGE;
				if (a > max_allowed) {
					a = max_allowed;
				} else if (a < min_allowed) {
					a = min_allowed;
				}

				opt_lg_san_uaf_align = a;
				CONF_CONTINUE;
			}

			CONF_HANDLE_SIZE_T(opt_san_guard_small,
			    "san_guard_small", 0, SIZE_T_MAX,
			    CONF_DONT_CHECK_MIN, CONF_DONT_CHECK_MAX, false)
			CONF_HANDLE_SIZE_T(opt_san_guard_large,
			    "san_guard_large", 0, SIZE_T_MAX,
			    CONF_DONT_CHECK_MIN, CONF_DONT_CHECK_MAX, false)

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
#undef CONF_HANDLE_T
#undef CONF_HANDLE_T_U
#undef CONF_HANDLE_T_SIGNED
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

static bool
malloc_conf_init_check_deps(void) {
	if (opt_prof_leak_error && !opt_prof_final) {
		malloc_printf("<jemalloc>: prof_leak_error is set w/o "
		    "prof_final.\n");
		return true;
	}

	return false;
}

static void
malloc_conf_init(sc_data_t *sc_data, unsigned bin_shard_sizes[SC_NBINS]) {
	const char *opts_cache[MALLOC_CONF_NSOURCES] = {NULL, NULL, NULL, NULL,
		NULL};
	char buf[PATH_MAX + 1];

	/* The first call only set the confirm_conf option and opts_cache */
	malloc_conf_init_helper(NULL, NULL, true, opts_cache, buf);
	malloc_conf_init_helper(sc_data, bin_shard_sizes, false, opts_cache,
	    NULL);
	if (malloc_conf_init_check_deps()) {
		/* check_deps does warning msg only; abort below if needed. */
		if (opt_abort_conf) {
			malloc_abort_invalid_conf();
		}
	}
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
	 * before sz_boot and bin_info_boot, which assume that the values they
	 * read out of sc_data_global are final.
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
	san_init(opt_lg_san_uaf_align);
	sz_boot(&sc_data, opt_cache_oblivious);
	bin_info_boot(&sc_data, bin_shard_sizes);

	if (opt_stats_print) {
		/* Print statistics at exit. */
		if (atexit(stats_print_atexit) != 0) {
			malloc_write("<jemalloc>: Error in atexit()\n");
			if (opt_abort) {
				abort();
			}
		}
	}

	if (stats_boot()) {
		return true;
	}
	if (pages_boot()) {
		return true;
	}
	if (base_boot(TSDN_NULL)) {
		return true;
	}
	/* emap_global is static, hence zeroed. */
	if (emap_init(&arena_emap_global, b0get(), /* zeroed */ true)) {
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
	if (opt_hpa && !hpa_supported()) {
		malloc_printf("<jemalloc>: HPA not supported in the current "
		    "configuration; %s.",
		    opt_abort_conf ? "aborting" : "disabling");
		if (opt_abort_conf) {
			malloc_abort_invalid_conf();
		} else {
			opt_hpa = false;
		}
	}
	if (arena_boot(&sc_data, b0get(), opt_hpa)) {
		return true;
	}
	if (tcache_boot(TSDN_NULL, b0get())) {
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
	if (arena_init(TSDN_NULL, 0, &arena_config_default) == NULL) {
		return true;
	}
	a0 = arena_get(TSDN_NULL, 0, false);

	if (opt_hpa && !hpa_supported()) {
		malloc_printf("<jemalloc>: HPA not supported in the current "
		    "configuration; %s.",
		    opt_abort_conf ? "aborting" : "disabling");
		if (opt_abort_conf) {
			malloc_abort_invalid_conf();
		} else {
			opt_hpa = false;
		}
	} else if (opt_hpa) {
		hpa_shard_opts_t hpa_shard_opts = opt_hpa_opts;
		hpa_shard_opts.deferral_allowed = background_thread_enabled();
		if (pa_shard_enable_hpa(TSDN_NULL, &a0->pa_shard,
		    &hpa_shard_opts, &opt_hpa_sec_opts)) {
			return true;
		}
	}

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
	if (opt_percpu_arena != percpu_arena_disabled) {
		bool cpu_count_is_deterministic =
		    malloc_cpu_count_is_deterministic();
		if (!cpu_count_is_deterministic) {
			/*
			 * If # of CPU is not deterministic, and narenas not
			 * specified, disables per cpu arena since it may not
			 * detect CPU IDs properly.
			 */
			if (opt_narenas == 0) {
				opt_percpu_arena = percpu_arena_disabled;
				malloc_write("<jemalloc>: Number of CPUs "
				    "detected is not deterministic. Per-CPU "
				    "arena disabled.\n");
				if (opt_abort_conf) {
					malloc_abort_invalid_conf();
				}
				if (opt_abort) {
					abort();
				}
			}
		}
	}

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
		fxp_t fxp_ncpus = FXP_INIT_INT(ncpus);
		fxp_t goal = fxp_mul(fxp_ncpus, opt_narenas_ratio);
		uint32_t int_goal = fxp_round_nearest(goal);
		if (int_goal == 0) {
			return 1;
		}
		return int_goal;
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
	if (malloc_init_narenas()
	    || background_thread_boot1(tsd_tsdn(tsd), b0get())) {
		UNLOCK_RETURN(tsd_tsdn(tsd), true, true)
	}
	if (config_prof && prof_boot2(tsd, b0get())) {
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

/*
 * ind parameter is optional and is only checked and filled if alignment == 0;
 * return true if result is out of range.
 */
JEMALLOC_ALWAYS_INLINE bool
aligned_usize_get(size_t size, size_t alignment, size_t *usize, szind_t *ind,
    bool bump_empty_aligned_alloc) {
	assert(usize != NULL);
	if (alignment == 0) {
		if (ind != NULL) {
			*ind = sz_size2index(size);
			if (unlikely(*ind >= SC_NSIZES)) {
				return true;
			}
			*usize = sz_index2size(*ind);
			assert(*usize > 0 && *usize <= SC_LARGE_MAXCLASS);
			return false;
		}
		*usize = sz_s2u(size);
	} else {
		if (bump_empty_aligned_alloc && unlikely(size == 0)) {
			size = 1;
		}
		*usize = sz_sa2u(size, alignment);
	}
	if (unlikely(*usize == 0 || *usize > SC_LARGE_MAXCLASS)) {
		return true;
	}
	return false;
}

JEMALLOC_ALWAYS_INLINE bool
zero_get(bool guarantee, bool slow) {
	if (config_fill && slow && unlikely(opt_zero)) {
		return true;
	} else {
		return guarantee;
	}
}

JEMALLOC_ALWAYS_INLINE tcache_t *
tcache_get_from_ind(tsd_t *tsd, unsigned tcache_ind, bool slow, bool is_alloc) {
	tcache_t *tcache;
	if (tcache_ind == TCACHE_IND_AUTOMATIC) {
		if (likely(!slow)) {
			/* Getting tcache ptr unconditionally. */
			tcache = tsd_tcachep_get(tsd);
			assert(tcache == tcache_get(tsd));
		} else if (is_alloc ||
		    likely(tsd_reentrancy_level_get(tsd) == 0)) {
			tcache = tcache_get(tsd);
		} else {
			tcache = NULL;
		}
	} else {
		/*
		 * Should not specify tcache on deallocation path when being
		 * reentrant.
		 */
		assert(is_alloc || tsd_reentrancy_level_get(tsd) == 0 ||
		    tsd_state_nocleanup(tsd));
		if (tcache_ind == TCACHE_IND_NONE) {
			tcache = NULL;
		} else {
			tcache = tcaches_get(tsd, tcache_ind);
		}
	}
	return tcache;
}

/* Return true if a manual arena is specified and arena_get() OOMs. */
JEMALLOC_ALWAYS_INLINE bool
arena_get_from_ind(tsd_t *tsd, unsigned arena_ind, arena_t **arena_p) {
	if (arena_ind == ARENA_IND_AUTOMATIC) {
		/*
		 * In case of automatic arena management, we defer arena
		 * computation until as late as we can, hoping to fill the
		 * allocation out of the tcache.
		 */
		*arena_p = NULL;
	} else {
		*arena_p = arena_get(tsd_tsdn(tsd), arena_ind, true);
		if (unlikely(*arena_p == NULL) && arena_ind >= narenas_auto) {
			return true;
		}
	}
	return false;
}

/* ind is ignored if dopts->alignment > 0. */
JEMALLOC_ALWAYS_INLINE void *
imalloc_no_sample(static_opts_t *sopts, dynamic_opts_t *dopts, tsd_t *tsd,
    size_t size, size_t usize, szind_t ind) {
	/* Fill in the tcache. */
	tcache_t *tcache = tcache_get_from_ind(tsd, dopts->tcache_ind,
	    sopts->slow, /* is_alloc */ true);

	/* Fill in the arena. */
	arena_t *arena;
	if (arena_get_from_ind(tsd, dopts->arena_ind, &arena)) {
		return NULL;
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

	dopts->alignment = prof_sample_align(dopts->alignment);
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
	assert(prof_sample_aligned(ret));

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
	 * The zero initialization for ind is actually dead store, in that its
	 * value is reset before any branch on its value is taken.  Sometimes
	 * though, it's convenient to pass it as arguments before this point.
	 * To avoid undefined behavior then, we initialize it with dummy stores.
	 */
	szind_t ind = 0;
	/* usize will always be properly initialized. */
	size_t usize;

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
	dopts->zero = zero_get(dopts->zero, sopts->slow);
	if (aligned_usize_get(size, dopts->alignment, &usize, &ind,
	    sopts->bump_empty_aligned_alloc)) {
		goto label_oom;
	}
	dopts->usize = usize;
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

	/*
	 * If dopts->alignment > 0, then ind is still 0, but usize was computed
	 * in the previous if statement.  Down the positive alignment path,
	 * imalloc_no_sample and imalloc_sample will ignore ind.
	 */

	/* If profiling is on, get our profiling context. */
	if (config_prof && opt_prof) {
		bool prof_active = prof_active_get_unlocked();
		bool sample_event = te_prof_sample_event_lookahead(tsd, usize);
		prof_tctx_t *tctx = prof_alloc_prep(tsd, prof_active,
		    sample_event);

		emap_alloc_ctx_t alloc_ctx;
		if (likely((uintptr_t)tctx == (uintptr_t)1U)) {
			alloc_ctx.slab = (usize <= SC_SMALL_MAXCLASS);
			allocation = imalloc_no_sample(
			    sopts, dopts, tsd, usize, usize, ind);
		} else if ((uintptr_t)tctx > (uintptr_t)1U) {
			allocation = imalloc_sample(
			    sopts, dopts, tsd, usize, ind);
			alloc_ctx.slab = false;
		} else {
			allocation = NULL;
		}

		if (unlikely(allocation == NULL)) {
			prof_alloc_rollback(tsd, tctx);
			goto label_oom;
		}
		prof_malloc(tsd, allocation, size, usize, &alloc_ctx, tctx);
	} else {
		assert(!opt_prof);
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

	thread_alloc_event(tsd, usize);

	assert(dopts->alignment == 0
	    || ((uintptr_t)allocation & (dopts->alignment - 1)) == ZU(0));

	assert(usize == isalloc(tsd_tsdn(tsd), allocation));

	if (config_fill && sopts->slow && !dopts->zero
	    && unlikely(opt_junk_alloc)) {
		junk_alloc_callback(allocation, usize);
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
malloc_default(size_t size, size_t *usize) {
	void *ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	/*
	 * This variant has logging hook on exit but not on entry.  It's callled
	 * only by je_malloc, below, which emits the entry one for us (and, if
	 * it calls us, does so only via tail call).
	 */

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

	if (usize) *usize = dopts.usize;
	return ret;
}

/******************************************************************************/
/*
 * Begin malloc(3)-compatible functions.
 */

static inline void *je_malloc_internal(size_t size, size_t *usize) {
	return imalloc_fastpath(size, &malloc_default, usize);
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(1)
je_malloc(size_t size) {
	return je_malloc_internal(size, NULL);
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

static void *je_calloc_internal(size_t num, size_t size, size_t *usize) {
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

	if (usize) *usize = dopts.usize;
	return ret;
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE2(1, 2)
je_calloc(size_t num, size_t size) {
	return je_calloc_internal(num, size, NULL);
}

JEMALLOC_ALWAYS_INLINE void
ifree(tsd_t *tsd, void *ptr, tcache_t *tcache, bool slow_path, size_t *usable) {
	if (!slow_path) {
		tsd_assert_fast(tsd);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));
	if (tsd_reentrancy_level_get(tsd) != 0) {
		assert(slow_path);
	}

	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(tsd_tsdn(tsd), &arena_emap_global, ptr,
	    &alloc_ctx);
	assert(alloc_ctx.szind != SC_NSIZES);

	size_t usize = sz_index2size(alloc_ctx.szind);
	if (config_prof && opt_prof) {
		prof_free(tsd, ptr, usize, &alloc_ctx);
	}

	if (likely(!slow_path)) {
		idalloctm(tsd_tsdn(tsd), ptr, tcache, &alloc_ctx, false,
		    false);
	} else {
		if (config_fill && slow_path && opt_junk_free) {
			junk_free_callback(ptr, usize);
		}
		idalloctm(tsd_tsdn(tsd), ptr, tcache, &alloc_ctx, false,
		    true);
	}
	thread_dalloc_event(tsd, usize);
	if (usable) *usable = usize;
}

JEMALLOC_ALWAYS_INLINE bool
maybe_check_alloc_ctx(tsd_t *tsd, void *ptr, emap_alloc_ctx_t *alloc_ctx) {
	if (config_opt_size_checks) {
		emap_alloc_ctx_t dbg_ctx;
		emap_alloc_ctx_lookup(tsd_tsdn(tsd), &arena_emap_global, ptr,
		    &dbg_ctx);
		if (alloc_ctx->szind != dbg_ctx.szind) {
			safety_check_fail_sized_dealloc(
			    /* current_dealloc */ true, ptr,
			    /* true_size */ sz_size2index(dbg_ctx.szind),
			    /* input_size */ sz_size2index(alloc_ctx->szind));
			return true;
		}
		if (alloc_ctx->slab != dbg_ctx.slab) {
			safety_check_fail(
			    "Internal heap corruption detected: "
			    "mismatch in slab bit");
			return true;
		}
	}
	return false;
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

	emap_alloc_ctx_t alloc_ctx;
	if (!config_prof) {
		alloc_ctx.szind = sz_size2index(usize);
		alloc_ctx.slab = (alloc_ctx.szind < SC_NBINS);
	} else {
		if (likely(!prof_sample_aligned(ptr))) {
			/*
			 * When the ptr is not page aligned, it was not sampled.
			 * usize can be trusted to determine szind and slab.
			 */
			alloc_ctx.szind = sz_size2index(usize);
			alloc_ctx.slab = (alloc_ctx.szind < SC_NBINS);
		} else if (opt_prof) {
			emap_alloc_ctx_lookup(tsd_tsdn(tsd), &arena_emap_global,
			    ptr, &alloc_ctx);

			if (config_opt_safety_checks) {
				/* Small alloc may have !slab (sampled). */
				if (unlikely(alloc_ctx.szind !=
				    sz_size2index(usize))) {
					safety_check_fail_sized_dealloc(
					    /* current_dealloc */ true, ptr,
					    /* true_size */ sz_index2size(
					    alloc_ctx.szind),
					    /* input_size */ usize);
				}
			}
		} else {
			alloc_ctx.szind = sz_size2index(usize);
			alloc_ctx.slab = (alloc_ctx.szind < SC_NBINS);
		}
	}
	bool fail = maybe_check_alloc_ctx(tsd, ptr, &alloc_ctx);
	if (fail) {
		/*
		 * This is a heap corruption bug.  In real life we'll crash; for
		 * the unit test we just want to avoid breaking anything too
		 * badly to get a test result out.  Let's leak instead of trying
		 * to free.
		 */
		return;
	}

	if (config_prof && opt_prof) {
		prof_free(tsd, ptr, usize, &alloc_ctx);
	}
	if (likely(!slow_path)) {
		isdalloct(tsd_tsdn(tsd), ptr, usize, tcache, &alloc_ctx,
		    false);
	} else {
		if (config_fill && slow_path && opt_junk_free) {
			junk_free_callback(ptr, usize);
		}
		isdalloct(tsd_tsdn(tsd), ptr, usize, tcache, &alloc_ctx,
		    true);
	}
	thread_dalloc_event(tsd, usize);
}

JEMALLOC_NOINLINE
void
free_default(void *ptr, size_t *usize) {
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

		if (likely(tsd_fast(tsd))) {
			tcache_t *tcache = tcache_get_from_ind(tsd,
			    TCACHE_IND_AUTOMATIC, /* slow */ false,
			    /* is_alloc */ false);
			ifree(tsd, ptr, tcache, /* slow */ false, usize);
		} else {
			tcache_t *tcache = tcache_get_from_ind(tsd,
			    TCACHE_IND_AUTOMATIC, /* slow */ true,
			    /* is_alloc */ false);
			uintptr_t args_raw[3] = {(uintptr_t)ptr};
			hook_invoke_dalloc(hook_dalloc_free, ptr, args_raw);
			ifree(tsd, ptr, tcache, /* slow */ true, usize);
		}

		check_entry_exit_locking(tsd_tsdn(tsd));
	}
}

JEMALLOC_ALWAYS_INLINE bool
free_fastpath_nonfast_aligned(void *ptr, bool check_prof) {
	/*
	 * free_fastpath do not handle two uncommon cases: 1) sampled profiled
	 * objects and 2) sampled junk & stash for use-after-free detection.
	 * Both have special alignments which are used to escape the fastpath.
	 *
	 * prof_sample is page-aligned, which covers the UAF check when both
	 * are enabled (the assertion below).  Avoiding redundant checks since
	 * this is on the fastpath -- at most one runtime branch from this.
	 */
	if (config_debug && cache_bin_nonfast_aligned(ptr)) {
		assert(prof_sample_aligned(ptr));
	}

	if (config_prof && check_prof) {
		/* When prof is enabled, the prof_sample alignment is enough. */
		if (prof_sample_aligned(ptr)) {
			return true;
		} else {
			return false;
		}
	}

	if (config_uaf_detection) {
		if (cache_bin_nonfast_aligned(ptr)) {
			return true;
		} else {
			return false;
		}
	}

	return false;
}

/* Returns whether or not the free attempt was successful. */
JEMALLOC_ALWAYS_INLINE
bool free_fastpath(void *ptr, size_t size, bool size_hint, size_t *usable_size) {
	tsd_t *tsd = tsd_get(false);
	/* The branch gets optimized away unless tsd_get_allocates(). */
	if (unlikely(tsd == NULL)) {
		return false;
	}
	/*
	 *  The tsd_fast() / initialized checks are folded into the branch
	 *  testing (deallocated_after >= threshold) later in this function.
	 *  The threshold will be set to 0 when !tsd_fast.
	 */
	assert(tsd_fast(tsd) ||
	    *tsd_thread_deallocated_next_event_fastp_get_unsafe(tsd) == 0);

	emap_alloc_ctx_t alloc_ctx;
	if (!size_hint) {
		bool err = emap_alloc_ctx_try_lookup_fast(tsd,
		    &arena_emap_global, ptr, &alloc_ctx);

		/* Note: profiled objects will have alloc_ctx.slab set */
		if (unlikely(err || !alloc_ctx.slab ||
		    free_fastpath_nonfast_aligned(ptr,
		    /* check_prof */ false))) {
			return false;
		}
		assert(alloc_ctx.szind != SC_NSIZES);
	} else {
		/*
		 * Check for both sizes that are too large, and for sampled /
		 * special aligned objects.  The alignment check will also check
		 * for null ptr.
		 */
		if (unlikely(size > SC_LOOKUP_MAXCLASS ||
		    free_fastpath_nonfast_aligned(ptr,
		    /* check_prof */ true))) {
			return false;
		}
		alloc_ctx.szind = sz_size2index_lookup(size);
		/* Max lookup class must be small. */
		assert(alloc_ctx.szind < SC_NBINS);
		/* This is a dead store, except when opt size checking is on. */
		alloc_ctx.slab = true;
	}
	/*
	 * Currently the fastpath only handles small sizes.  The branch on
	 * SC_LOOKUP_MAXCLASS makes sure of it.  This lets us avoid checking
	 * tcache szind upper limit (i.e. tcache_maxclass) as well.
	 */
	assert(alloc_ctx.slab);

	uint64_t deallocated, threshold;
	te_free_fastpath_ctx(tsd, &deallocated, &threshold);

	size_t usize = sz_index2size(alloc_ctx.szind);
	uint64_t deallocated_after = deallocated + usize;
	/*
	 * Check for events and tsd non-nominal (fast_threshold will be set to
	 * 0) in a single branch.  Note that this handles the uninitialized case
	 * as well (TSD init will be triggered on the non-fastpath).  Therefore
	 * anything depends on a functional TSD (e.g. the alloc_ctx sanity check
	 * below) needs to be after this branch.
	 */
	if (unlikely(deallocated_after >= threshold)) {
		return false;
	}
	assert(tsd_fast(tsd));
	bool fail = maybe_check_alloc_ctx(tsd, ptr, &alloc_ctx);
	if (fail) {
		/* See the comment in isfree. */
		if (usable_size) *usable_size = usize;
		return true;
	}

	tcache_t *tcache = tcache_get_from_ind(tsd, TCACHE_IND_AUTOMATIC,
	    /* slow */ false, /* is_alloc */ false);
	cache_bin_t *bin = &tcache->bins[alloc_ctx.szind];

	/*
	 * If junking were enabled, this is where we would do it.  It's not
	 * though, since we ensured above that we're on the fast path.  Assert
	 * that to double-check.
	 */
	assert(!opt_junk_free);

	if (!cache_bin_dalloc_easy(bin, ptr)) {
		return false;
	}

	*tsd_thread_deallocatedp_get(tsd) = deallocated_after;

	if (usable_size) *usable_size = usize;
	return true;
}

static inline void je_free_internal(void *ptr, size_t *usize) {
	LOG("core.free.entry", "ptr: %p", ptr);

	if (!free_fastpath(ptr, 0, false, usize)) {
		free_default(ptr, usize);
	}

	LOG("core.free.exit", "");
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_free(void *ptr) {
	je_free_internal(ptr, NULL);
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
#include <features.h> // defines __GLIBC__ if we are compiling against glibc

JEMALLOC_EXPORT void (*__free_hook)(void *ptr) = je_free;
JEMALLOC_EXPORT void *(*__malloc_hook)(size_t size) = je_malloc;
JEMALLOC_EXPORT void *(*__realloc_hook)(void *ptr, size_t size) = je_realloc;
#  ifdef JEMALLOC_GLIBC_MEMALIGN_HOOK
JEMALLOC_EXPORT void *(*__memalign_hook)(size_t alignment, size_t size) =
    je_memalign;
#  endif

#  ifdef __GLIBC__
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

JEMALLOC_ALWAYS_INLINE unsigned
mallocx_tcache_get(int flags) {
	if (likely((flags & MALLOCX_TCACHE_MASK) == 0)) {
		return TCACHE_IND_AUTOMATIC;
	} else if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE) {
		return TCACHE_IND_NONE;
	} else {
		return MALLOCX_TCACHE_GET(flags);
	}
}

JEMALLOC_ALWAYS_INLINE unsigned
mallocx_arena_get(int flags) {
	if (unlikely((flags & MALLOCX_ARENA_MASK) != 0)) {
		return MALLOCX_ARENA_GET(flags);
	} else {
		return ARENA_IND_AUTOMATIC;
	}
}

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
		dopts.alignment = MALLOCX_ALIGN_GET(flags);
		dopts.zero = MALLOCX_ZERO_GET(flags);
		dopts.tcache_ind = mallocx_tcache_get(flags);
		dopts.arena_ind = mallocx_arena_get(flags);
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
		dopts.alignment = MALLOCX_ALIGN_GET(flags);
		dopts.zero = MALLOCX_ZERO_GET(flags);
		dopts.tcache_ind = mallocx_tcache_get(flags);
		dopts.arena_ind = mallocx_arena_get(flags);
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

	alignment = prof_sample_align(alignment);
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
	assert(prof_sample_aligned(p));

	return p;
}

JEMALLOC_ALWAYS_INLINE void *
irallocx_prof(tsd_t *tsd, void *old_ptr, size_t old_usize, size_t size,
    size_t alignment, size_t usize, bool zero, tcache_t *tcache,
    arena_t *arena, emap_alloc_ctx_t *alloc_ctx,
    hook_ralloc_args_t *hook_args) {
	prof_info_t old_prof_info;
	prof_info_get_and_reset_recent(tsd, old_ptr, alloc_ctx, &old_prof_info);
	bool prof_active = prof_active_get_unlocked();
	bool sample_event = te_prof_sample_event_lookahead(tsd, usize);
	prof_tctx_t *tctx = prof_alloc_prep(tsd, prof_active, sample_event);
	void *p;
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U)) {
		p = irallocx_prof_sample(tsd_tsdn(tsd), old_ptr, old_usize,
		    usize, alignment, zero, tcache, arena, tctx, hook_args);
	} else {
		p = iralloct(tsd_tsdn(tsd), old_ptr, old_usize, size, alignment,
		    zero, tcache, arena, hook_args);
	}
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx);
		return NULL;
	}
	assert(usize == isalloc(tsd_tsdn(tsd), p));
	prof_realloc(tsd, p, size, usize, tctx, prof_active, old_ptr,
	    old_usize, &old_prof_info, sample_event);

	return p;
}

static void *
do_rallocx(void *ptr, size_t size, int flags, bool is_realloc, size_t *old_usable_size, size_t *new_usable_size) {
	void *p;
	tsd_t *tsd;
	size_t usize;
	size_t old_usize;
	size_t alignment = MALLOCX_ALIGN_GET(flags);
	arena_t *arena;

	assert(ptr != NULL);
	assert(size != 0);
	assert(malloc_initialized() || IS_INITIALIZER);
	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));

	bool zero = zero_get(MALLOCX_ZERO_GET(flags), /* slow */ true);

	unsigned arena_ind = mallocx_arena_get(flags);
	if (arena_get_from_ind(tsd, arena_ind, &arena)) {
		goto label_oom;
	}

	unsigned tcache_ind = mallocx_tcache_get(flags);
	tcache_t *tcache = tcache_get_from_ind(tsd, tcache_ind,
	    /* slow */ true, /* is_alloc */ true);

	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(tsd_tsdn(tsd), &arena_emap_global, ptr,
	    &alloc_ctx);
	assert(alloc_ctx.szind != SC_NSIZES);
	old_usize = sz_index2size(alloc_ctx.szind);
	assert(old_usize == isalloc(tsd_tsdn(tsd), ptr));
	if (aligned_usize_get(size, alignment, &usize, NULL, false)) {
		goto label_oom;
	}

	hook_ralloc_args_t hook_args = {is_realloc, {(uintptr_t)ptr, size,
		flags, 0}};
	if (config_prof && opt_prof) {
		p = irallocx_prof(tsd, ptr, old_usize, size, alignment, usize,
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
		assert(usize == isalloc(tsd_tsdn(tsd), p));
	}
	assert(alignment == 0 || ((uintptr_t)p & (alignment - 1)) == ZU(0));
	thread_alloc_event(tsd, usize);
	thread_dalloc_event(tsd, old_usize);

	UTRACE(ptr, size, p);
	check_entry_exit_locking(tsd_tsdn(tsd));

	if (config_fill && unlikely(opt_junk_alloc) && usize > old_usize
	    && !zero) {
		size_t excess_len = usize - old_usize;
		void *excess_start = (void *)((uintptr_t)p + old_usize);
		junk_alloc_callback(excess_start, excess_len);
	}

	if (old_usable_size) *old_usable_size = old_usize;
	if (new_usable_size) *new_usable_size = usize;
	return p;
label_oom:
	if (config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write("<jemalloc>: Error in rallocx(): out of memory\n");
		abort();
	}
	UTRACE(ptr, size, 0);
	check_entry_exit_locking(tsd_tsdn(tsd));

	return NULL;
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2)
je_rallocx(void *ptr, size_t size, int flags) {
	LOG("core.rallocx.entry", "ptr: %p, size: %zu, flags: %d", ptr,
	    size, flags);
	void *ret = do_rallocx(ptr, size, flags, false, NULL, NULL);
	LOG("core.rallocx.exit", "result: %p", ret);
	return ret;
}

static void *
do_realloc_nonnull_zero(void *ptr, size_t *old_usize, size_t *new_usize) {
	if (config_stats) {
		atomic_fetch_add_zu(&zero_realloc_count, 1, ATOMIC_RELAXED);
	}
	if (opt_zero_realloc_action == zero_realloc_action_alloc) {
		/*
		 * The user might have gotten an alloc setting while expecting a
		 * free setting.  If that's the case, we at least try to
		 * reduce the harm, and turn off the tcache while allocating, so
		 * that we'll get a true first fit.
		 */
		return do_rallocx(ptr, 1, MALLOCX_TCACHE_NONE, true, old_usize, new_usize);
	} else if (opt_zero_realloc_action == zero_realloc_action_free) {
		UTRACE(ptr, 0, 0);
		tsd_t *tsd = tsd_fetch();
		check_entry_exit_locking(tsd_tsdn(tsd));

		tcache_t *tcache = tcache_get_from_ind(tsd,
		    TCACHE_IND_AUTOMATIC, /* slow */ true,
		    /* is_alloc */ false);
		uintptr_t args[3] = {(uintptr_t)ptr, 0};
		hook_invoke_dalloc(hook_dalloc_realloc, ptr, args);
		size_t usize;
		ifree(tsd, ptr, tcache, true, &usize);
		if (old_usize) *old_usize = usize;
		if (new_usize) *new_usize = 0;

		check_entry_exit_locking(tsd_tsdn(tsd));
		return NULL;
	} else {
		safety_check_fail("Called realloc(non-null-ptr, 0) with "
		    "zero_realloc:abort set\n");
		/* In real code, this will never run; the safety check failure
		 * will call abort.  In the unit test, we just want to bail out
		 * without corrupting internal state that the test needs to
		 * finish.
		 */
		return NULL;
	}
}

static inline void *je_realloc_internal(void *ptr, size_t size, size_t *old_usize, size_t *new_usize) {
	LOG("core.realloc.entry", "ptr: %p, size: %zu\n", ptr, size);

	if (likely(ptr != NULL && size != 0)) {
		void *ret = do_rallocx(ptr, size, 0, true, old_usize, new_usize);
		LOG("core.realloc.exit", "result: %p", ret);
		return ret;
	} else if (ptr != NULL && size == 0) {
		void *ret = do_realloc_nonnull_zero(ptr, old_usize, new_usize);
		LOG("core.realloc.exit", "result: %p", ret);
		return ret;
	} else {
		/* realloc(NULL, size) is equivalent to malloc(size). */
		void *ret;

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
			uintptr_t args[3] = {(uintptr_t)ptr, size};
			hook_invoke_alloc(hook_alloc_realloc, ret,
			    (uintptr_t)ret, args);
		}
		LOG("core.realloc.exit", "result: %p", ret);
		if (old_usize) *old_usize = 0;
		if (new_usize) *new_usize = dopts.usize;
		return ret;
	}
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2)
je_realloc(void *ptr, size_t size) {
	return je_realloc_internal(ptr, size, NULL, NULL);
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
	/* Sampled allocation needs to be page aligned. */
	if (tctx == NULL || !prof_sample_aligned(ptr)) {
		return old_usize;
	}

	return ixallocx_helper(tsdn, ptr, old_usize, size, extra, alignment,
	    zero);
}

JEMALLOC_ALWAYS_INLINE size_t
ixallocx_prof(tsd_t *tsd, void *ptr, size_t old_usize, size_t size,
    size_t extra, size_t alignment, bool zero, emap_alloc_ctx_t *alloc_ctx) {
	/*
	 * old_prof_info is only used for asserting that the profiling info
	 * isn't changed by the ixalloc() call.
	 */
	prof_info_t old_prof_info;
	prof_info_get(tsd, ptr, alloc_ctx, &old_prof_info);

	/*
	 * usize isn't knowable before ixalloc() returns when extra is non-zero.
	 * Therefore, compute its maximum possible value and use that in
	 * prof_alloc_prep() to decide whether to capture a backtrace.
	 * prof_realloc() will use the actual usize to decide whether to sample.
	 */
	size_t usize_max;
	if (aligned_usize_get(size + extra, alignment, &usize_max, NULL,
	    false)) {
		/*
		 * usize_max is out of range, and chances are that allocation
		 * will fail, but use the maximum possible value and carry on
		 * with prof_alloc_prep(), just in case allocation succeeds.
		 */
		usize_max = SC_LARGE_MAXCLASS;
	}
	bool prof_active = prof_active_get_unlocked();
	bool sample_event = te_prof_sample_event_lookahead(tsd, usize_max);
	prof_tctx_t *tctx = prof_alloc_prep(tsd, prof_active, sample_event);

	size_t usize;
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U)) {
		usize = ixallocx_prof_sample(tsd_tsdn(tsd), ptr, old_usize,
		    size, extra, alignment, zero, tctx);
	} else {
		usize = ixallocx_helper(tsd_tsdn(tsd), ptr, old_usize, size,
		    extra, alignment, zero);
	}

	/*
	 * At this point we can still safely get the original profiling
	 * information associated with the ptr, because (a) the edata_t object
	 * associated with the ptr still lives and (b) the profiling info
	 * fields are not touched.  "(a)" is asserted in the outer je_xallocx()
	 * function, and "(b)" is indirectly verified below by checking that
	 * the alloc_tctx field is unchanged.
	 */
	prof_info_t prof_info;
	if (usize == old_usize) {
		prof_info_get(tsd, ptr, alloc_ctx, &prof_info);
		prof_alloc_rollback(tsd, tctx);
	} else {
		prof_info_get_and_reset_recent(tsd, ptr, alloc_ctx, &prof_info);
		assert(usize <= usize_max);
		sample_event = te_prof_sample_event_lookahead(tsd, usize);
		prof_realloc(tsd, ptr, size, usize, tctx, prof_active, ptr,
		    old_usize, &prof_info, sample_event);
	}

	assert(old_prof_info.alloc_tctx == prof_info.alloc_tctx);
	return usize;
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_xallocx(void *ptr, size_t size, size_t extra, int flags) {
	tsd_t *tsd;
	size_t usize, old_usize;
	size_t alignment = MALLOCX_ALIGN_GET(flags);
	bool zero = zero_get(MALLOCX_ZERO_GET(flags), /* slow */ true);

	LOG("core.xallocx.entry", "ptr: %p, size: %zu, extra: %zu, "
	    "flags: %d", ptr, size, extra, flags);

	assert(ptr != NULL);
	assert(size != 0);
	assert(SIZE_T_MAX - size >= extra);
	assert(malloc_initialized() || IS_INITIALIZER);
	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));

	/*
	 * old_edata is only for verifying that xallocx() keeps the edata_t
	 * object associated with the ptr (though the content of the edata_t
	 * object can be changed).
	 */
	edata_t *old_edata = emap_edata_lookup(tsd_tsdn(tsd),
	    &arena_emap_global, ptr);

	emap_alloc_ctx_t alloc_ctx;
	emap_alloc_ctx_lookup(tsd_tsdn(tsd), &arena_emap_global, ptr,
	    &alloc_ctx);
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

	/*
	 * xallocx() should keep using the same edata_t object (though its
	 * content can be changed).
	 */
	assert(emap_edata_lookup(tsd_tsdn(tsd), &arena_emap_global, ptr)
	    == old_edata);

	if (unlikely(usize == old_usize)) {
		goto label_not_resized;
	}
	thread_alloc_event(tsd, usize);
	thread_dalloc_event(tsd, old_usize);

	if (config_fill && unlikely(opt_junk_alloc) && usize > old_usize &&
	    !zero) {
		size_t excess_len = usize - old_usize;
		void *excess_start = (void *)((uintptr_t)ptr + old_usize);
		junk_alloc_callback(excess_start, excess_len);
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

	tsd_t *tsd = tsd_fetch_min();
	bool fast = tsd_fast(tsd);
	check_entry_exit_locking(tsd_tsdn(tsd));

	unsigned tcache_ind = mallocx_tcache_get(flags);
	tcache_t *tcache = tcache_get_from_ind(tsd, tcache_ind, !fast,
	    /* is_alloc */ false);

	UTRACE(ptr, 0, 0);
	if (likely(fast)) {
		tsd_assert_fast(tsd);
		ifree(tsd, ptr, tcache, false, NULL);
	} else {
		uintptr_t args_raw[3] = {(uintptr_t)ptr, flags};
		hook_invoke_dalloc(hook_dalloc_dallocx, ptr, args_raw);
		ifree(tsd, ptr, tcache, true, NULL);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.dallocx.exit", "");
}

JEMALLOC_ALWAYS_INLINE size_t
inallocx(tsdn_t *tsdn, size_t size, int flags) {
	check_entry_exit_locking(tsdn);
	size_t usize;
	/* In case of out of range, let the user see it rather than fail. */
	aligned_usize_get(size, MALLOCX_ALIGN_GET(flags), &usize, NULL, false);
	check_entry_exit_locking(tsdn);
	return usize;
}

JEMALLOC_NOINLINE void
sdallocx_default(void *ptr, size_t size, int flags) {
	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	tsd_t *tsd = tsd_fetch_min();
	bool fast = tsd_fast(tsd);
	size_t usize = inallocx(tsd_tsdn(tsd), size, flags);
	check_entry_exit_locking(tsd_tsdn(tsd));

	unsigned tcache_ind = mallocx_tcache_get(flags);
	tcache_t *tcache = tcache_get_from_ind(tsd, tcache_ind, !fast,
	    /* is_alloc */ false);

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

	if (flags != 0 || !free_fastpath(ptr, size, true, NULL)) {
		sdallocx_default(ptr, size, flags);
	}

	LOG("core.sdallocx.exit", "");
}

void JEMALLOC_NOTHROW
je_sdallocx_noflags(void *ptr, size_t size) {
	LOG("core.sdallocx.entry", "ptr: %p, size: %zu, flags: 0", ptr,
		size);

	if (!free_fastpath(ptr, size, true, NULL)) {
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

#define STATS_PRINT_BUFSIZE 65536
JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_malloc_stats_print(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *opts) {
	tsdn_t *tsdn;

	LOG("core.malloc_stats_print.entry", "");

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	if (config_debug) {
		stats_print(write_cb, cbopaque, opts);
	} else {
		buf_writer_t buf_writer;
		buf_writer_init(tsdn, &buf_writer, write_cb, cbopaque, NULL,
		    STATS_PRINT_BUFSIZE);
		stats_print(buf_writer_cb, &buf_writer, opts);
		buf_writer_terminate(tsdn, &buf_writer);
	}

	check_entry_exit_locking(tsdn);
	LOG("core.malloc_stats_print.exit", "");
}
#undef STATS_PRINT_BUFSIZE

JEMALLOC_ALWAYS_INLINE size_t
je_malloc_usable_size_impl(JEMALLOC_USABLE_SIZE_CONST void *ptr) {
	assert(malloc_initialized() || IS_INITIALIZER);

	tsdn_t *tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	size_t ret;
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

	return ret;
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_malloc_usable_size(JEMALLOC_USABLE_SIZE_CONST void *ptr) {
	LOG("core.malloc_usable_size.entry", "ptr: %p", ptr);

	size_t ret = je_malloc_usable_size_impl(ptr);

	LOG("core.malloc_usable_size.exit", "result: %zu", ret);
	return ret;
}

#ifdef JEMALLOC_HAVE_MALLOC_SIZE
JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_malloc_size(const void *ptr) {
	LOG("core.malloc_size.entry", "ptr: %p", ptr);

	size_t ret = je_malloc_usable_size_impl(ptr);

	LOG("core.malloc_size.exit", "result: %zu", ret);
	return ret;
}
#endif

static void
batch_alloc_prof_sample_assert(tsd_t *tsd, size_t batch, size_t usize) {
	assert(config_prof && opt_prof);
	bool prof_sample_event = te_prof_sample_event_lookahead(tsd,
	    batch * usize);
	assert(!prof_sample_event);
	size_t surplus;
	prof_sample_event = te_prof_sample_event_lookahead_surplus(tsd,
	    (batch + 1) * usize, &surplus);
	assert(prof_sample_event);
	assert(surplus < usize);
}

size_t
batch_alloc(void **ptrs, size_t num, size_t size, int flags) {
	LOG("core.batch_alloc.entry",
	    "ptrs: %p, num: %zu, size: %zu, flags: %d", ptrs, num, size, flags);

	tsd_t *tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));

	size_t filled = 0;

	if (unlikely(tsd == NULL || tsd_reentrancy_level_get(tsd) > 0)) {
		goto label_done;
	}

	size_t alignment = MALLOCX_ALIGN_GET(flags);
	size_t usize;
	if (aligned_usize_get(size, alignment, &usize, NULL, false)) {
		goto label_done;
	}
	szind_t ind = sz_size2index(usize);
	bool zero = zero_get(MALLOCX_ZERO_GET(flags), /* slow */ true);

	/*
	 * The cache bin and arena will be lazily initialized; it's hard to
	 * know in advance whether each of them needs to be initialized.
	 */
	cache_bin_t *bin = NULL;
	arena_t *arena = NULL;

	size_t nregs = 0;
	if (likely(ind < SC_NBINS)) {
		nregs = bin_infos[ind].nregs;
		assert(nregs > 0);
	}

	while (filled < num) {
		size_t batch = num - filled;
		size_t surplus = SIZE_MAX; /* Dead store. */
		bool prof_sample_event = config_prof && opt_prof
		    && prof_active_get_unlocked()
		    && te_prof_sample_event_lookahead_surplus(tsd,
		    batch * usize, &surplus);

		if (prof_sample_event) {
			/*
			 * Adjust so that the batch does not trigger prof
			 * sampling.
			 */
			batch -= surplus / usize + 1;
			batch_alloc_prof_sample_assert(tsd, batch, usize);
		}

		size_t progress = 0;

		if (likely(ind < SC_NBINS) && batch >= nregs) {
			if (arena == NULL) {
				unsigned arena_ind = mallocx_arena_get(flags);
				if (arena_get_from_ind(tsd, arena_ind,
				    &arena)) {
					goto label_done;
				}
				if (arena == NULL) {
					arena = arena_choose(tsd, NULL);
				}
				if (unlikely(arena == NULL)) {
					goto label_done;
				}
			}
			size_t arena_batch = batch - batch % nregs;
			size_t n = arena_fill_small_fresh(tsd_tsdn(tsd), arena,
			    ind, ptrs + filled, arena_batch, zero);
			progress += n;
			filled += n;
		}

		if (likely(ind < nhbins) && progress < batch) {
			if (bin == NULL) {
				unsigned tcache_ind = mallocx_tcache_get(flags);
				tcache_t *tcache = tcache_get_from_ind(tsd,
				    tcache_ind, /* slow */ true,
				    /* is_alloc */ true);
				if (tcache != NULL) {
					bin = &tcache->bins[ind];
				}
			}
			/*
			 * If we don't have a tcache bin, we don't want to
			 * immediately give up, because there's the possibility
			 * that the user explicitly requested to bypass the
			 * tcache, or that the user explicitly turned off the
			 * tcache; in such cases, we go through the slow path,
			 * i.e. the mallocx() call at the end of the while loop.
			 */
			if (bin != NULL) {
				size_t bin_batch = batch - progress;
				/*
				 * n can be less than bin_batch, meaning that
				 * the cache bin does not have enough memory.
				 * In such cases, we rely on the slow path,
				 * i.e. the mallocx() call at the end of the
				 * while loop, to fill in the cache, and in the
				 * next iteration of the while loop, the tcache
				 * will contain a lot of memory, and we can
				 * harvest them here.  Compared to the
				 * alternative approach where we directly go to
				 * the arena bins here, the overhead of our
				 * current approach should usually be minimal,
				 * since we never try to fetch more memory than
				 * what a slab contains via the tcache.  An
				 * additional benefit is that the tcache will
				 * not be empty for the next allocation request.
				 */
				size_t n = cache_bin_alloc_batch(bin, bin_batch,
				    ptrs + filled);
				if (config_stats) {
					bin->tstats.nrequests += n;
				}
				if (zero) {
					for (size_t i = 0; i < n; ++i) {
						memset(ptrs[filled + i], 0,
						    usize);
					}
				}
				if (config_prof && opt_prof
				    && unlikely(ind >= SC_NBINS)) {
					for (size_t i = 0; i < n; ++i) {
						prof_tctx_reset_sampled(tsd,
						    ptrs[filled + i]);
					}
				}
				progress += n;
				filled += n;
			}
		}

		/*
		 * For thread events other than prof sampling, trigger them as
		 * if there's a single allocation of size (n * usize).  This is
		 * fine because:
		 * (a) these events do not alter the allocation itself, and
		 * (b) it's possible that some event would have been triggered
		 *     multiple times, instead of only once, if the allocations
		 *     were handled individually, but it would do no harm (or
		 *     even be beneficial) to coalesce the triggerings.
		 */
		thread_alloc_event(tsd, progress * usize);

		if (progress < batch || prof_sample_event) {
			void *p = je_mallocx(size, flags);
			if (p == NULL) { /* OOM */
				break;
			}
			if (progress == batch) {
				assert(prof_sampled(tsd, p));
			}
			ptrs[filled++] = p;
		}
	}

label_done:
	check_entry_exit_locking(tsd_tsdn(tsd));
	LOG("core.batch_alloc.exit", "result: %zu", filled);
	return filled;
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
	for (i = 0; i < 9; i++) {
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
				case 8:
					arena_prefork8(tsd_tsdn(tsd), arena);
					break;
				default: not_reached();
				}
			}
		}

	}
	prof_prefork1(tsd_tsdn(tsd));
	stats_prefork(tsd_tsdn(tsd));
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
	stats_postfork_parent(tsd_tsdn(tsd));
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
	stats_postfork_child(tsd_tsdn(tsd));
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

/* Helps the application decide if a pointer is worth re-allocating in order to reduce fragmentation.
 * returns 1 if the allocation should be moved, and 0 if the allocation be kept.
 * If the application decides to re-allocate it should use MALLOCX_TCACHE_NONE when doing so. */
JEMALLOC_EXPORT int JEMALLOC_NOTHROW
get_defrag_hint(void* ptr) {
	assert(ptr != NULL);
	return iget_defrag_hint(TSDN_NULL, ptr);
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(1)
malloc_with_usize(size_t size, size_t *usize) {
	return je_malloc_internal(size, usize);
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE2(1, 2)
calloc_with_usize(size_t num, size_t size, size_t *usize) {
	return je_calloc_internal(num, size, usize);
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2)
realloc_with_usize(void *ptr, size_t size, size_t *old_usize, size_t *new_usize) {
	return je_realloc_internal(ptr, size, old_usize, new_usize);
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
free_with_usize(void *ptr, size_t *usize) {
	je_free_internal(ptr, usize);
}
