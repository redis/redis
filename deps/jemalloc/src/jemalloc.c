#define	JEMALLOC_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

/* Runtime configuration options. */
const char	*je_malloc_conf JEMALLOC_ATTR(weak);
bool	opt_abort =
#ifdef JEMALLOC_DEBUG
    true
#else
    false
#endif
    ;
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

size_t	opt_quarantine = ZU(0);
bool	opt_redzone = false;
bool	opt_utrace = false;
bool	opt_xmalloc = false;
bool	opt_zero = false;
size_t	opt_narenas = 0;

/* Initialized to true if the process is running inside Valgrind. */
bool	in_valgrind;

unsigned	ncpus;

/* Protects arenas initialization (arenas, narenas_total). */
static malloc_mutex_t	arenas_lock;
/*
 * Arenas that are used to service external requests.  Not all elements of the
 * arenas array are necessarily used; arenas are created lazily as needed.
 *
 * arenas[0..narenas_auto) are used for automatic multiplexing of threads and
 * arenas.  arenas[narenas_auto..narenas_total) are only used if the application
 * takes some action to create them and allocate from them.
 */
static arena_t		**arenas;
static unsigned		narenas_total;
static arena_t		*a0; /* arenas[0]; read-only after initialization. */
static unsigned		narenas_auto; /* Read-only after initialization. */

typedef enum {
	malloc_init_uninitialized	= 3,
	malloc_init_a0_initialized	= 2,
	malloc_init_recursible		= 1,
	malloc_init_initialized		= 0 /* Common case --> jnz. */
} malloc_init_t;
static malloc_init_t	malloc_init_state = malloc_init_uninitialized;

JEMALLOC_ALIGNED(CACHELINE)
const size_t	index2size_tab[NSIZES] = {
#define	SC(index, lg_grp, lg_delta, ndelta, bin, lg_delta_lookup) \
	((ZU(1)<<lg_grp) + (ZU(ndelta)<<lg_delta)),
	SIZE_CLASSES
#undef SC
};

JEMALLOC_ALIGNED(CACHELINE)
const uint8_t	size2index_tab[] = {
#if LG_TINY_MIN == 0
#warning "Dangerous LG_TINY_MIN"
#define	S2B_0(i)	i,
#elif LG_TINY_MIN == 1
#warning "Dangerous LG_TINY_MIN"
#define	S2B_1(i)	i,
#elif LG_TINY_MIN == 2
#warning "Dangerous LG_TINY_MIN"
#define	S2B_2(i)	i,
#elif LG_TINY_MIN == 3
#define	S2B_3(i)	i,
#elif LG_TINY_MIN == 4
#define	S2B_4(i)	i,
#elif LG_TINY_MIN == 5
#define	S2B_5(i)	i,
#elif LG_TINY_MIN == 6
#define	S2B_6(i)	i,
#elif LG_TINY_MIN == 7
#define	S2B_7(i)	i,
#elif LG_TINY_MIN == 8
#define	S2B_8(i)	i,
#elif LG_TINY_MIN == 9
#define	S2B_9(i)	i,
#elif LG_TINY_MIN == 10
#define	S2B_10(i)	i,
#elif LG_TINY_MIN == 11
#define	S2B_11(i)	i,
#else
#error "Unsupported LG_TINY_MIN"
#endif
#if LG_TINY_MIN < 1
#define	S2B_1(i)	S2B_0(i) S2B_0(i)
#endif
#if LG_TINY_MIN < 2
#define	S2B_2(i)	S2B_1(i) S2B_1(i)
#endif
#if LG_TINY_MIN < 3
#define	S2B_3(i)	S2B_2(i) S2B_2(i)
#endif
#if LG_TINY_MIN < 4
#define	S2B_4(i)	S2B_3(i) S2B_3(i)
#endif
#if LG_TINY_MIN < 5
#define	S2B_5(i)	S2B_4(i) S2B_4(i)
#endif
#if LG_TINY_MIN < 6
#define	S2B_6(i)	S2B_5(i) S2B_5(i)
#endif
#if LG_TINY_MIN < 7
#define	S2B_7(i)	S2B_6(i) S2B_6(i)
#endif
#if LG_TINY_MIN < 8
#define	S2B_8(i)	S2B_7(i) S2B_7(i)
#endif
#if LG_TINY_MIN < 9
#define	S2B_9(i)	S2B_8(i) S2B_8(i)
#endif
#if LG_TINY_MIN < 10
#define	S2B_10(i)	S2B_9(i) S2B_9(i)
#endif
#if LG_TINY_MIN < 11
#define	S2B_11(i)	S2B_10(i) S2B_10(i)
#endif
#define	S2B_no(i)
#define	SC(index, lg_grp, lg_delta, ndelta, bin, lg_delta_lookup) \
	S2B_##lg_delta_lookup(index)
	SIZE_CLASSES
#undef S2B_3
#undef S2B_4
#undef S2B_5
#undef S2B_6
#undef S2B_7
#undef S2B_8
#undef S2B_9
#undef S2B_10
#undef S2B_11
#undef S2B_no
#undef SC
};

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
_init_init_lock(void)
{

	/* If another constructor in the same binary is using mallctl to
	 * e.g. setup chunk hooks, it may end up running before this one,
	 * and malloc_init_hard will crash trying to lock the uninitialized
	 * lock. So we force an initialization of the lock in
	 * malloc_init_hard as well. We don't try to care about atomicity
	 * of the accessed to the init_lock_initialized boolean, since it
	 * really only matters early in the process creation, before any
	 * separate thread normally starts doing anything. */
	if (!init_lock_initialized)
		malloc_mutex_init(&init_lock);
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

JEMALLOC_ALWAYS_INLINE_C bool
malloc_initialized(void)
{

	return (malloc_init_state == malloc_init_initialized);
}

JEMALLOC_ALWAYS_INLINE_C void
malloc_thread_init(void)
{

	/*
	 * TSD initialization can't be safely done as a side effect of
	 * deallocation, because it is possible for a thread to do nothing but
	 * deallocate its TLS data via free(), in which case writing to TLS
	 * would cause write-after-free memory corruption.  The quarantine
	 * facility *only* gets used as a side effect of deallocation, so make
	 * a best effort attempt at initializing its TSD by hooking all
	 * allocation events.
	 */
	if (config_fill && unlikely(opt_quarantine))
		quarantine_alloc_hook();
}

JEMALLOC_ALWAYS_INLINE_C bool
malloc_init_a0(void)
{

	if (unlikely(malloc_init_state == malloc_init_uninitialized))
		return (malloc_init_hard_a0());
	return (false);
}

JEMALLOC_ALWAYS_INLINE_C bool
malloc_init(void)
{

	if (unlikely(!malloc_initialized()) && malloc_init_hard())
		return (true);
	malloc_thread_init();

	return (false);
}

/*
 * The a0*() functions are used instead of i[mcd]alloc() in situations that
 * cannot tolerate TLS variable access.
 */

arena_t *
a0get(void)
{

	assert(a0 != NULL);
	return (a0);
}

static void *
a0ialloc(size_t size, bool zero, bool is_metadata)
{

	if (unlikely(malloc_init_a0()))
		return (NULL);

	return (iallocztm(NULL, size, zero, false, is_metadata, a0get()));
}

static void
a0idalloc(void *ptr, bool is_metadata)
{

	idalloctm(NULL, ptr, false, is_metadata);
}

void *
a0malloc(size_t size)
{

	return (a0ialloc(size, false, true));
}

void
a0dalloc(void *ptr)
{

	a0idalloc(ptr, true);
}

/*
 * FreeBSD's libc uses the bootstrap_*() functions in bootstrap-senstive
 * situations that cannot tolerate TLS variable access (TLS allocation and very
 * early internal data structure initialization).
 */

void *
bootstrap_malloc(size_t size)
{

	if (unlikely(size == 0))
		size = 1;

	return (a0ialloc(size, false, false));
}

void *
bootstrap_calloc(size_t num, size_t size)
{
	size_t num_size;

	num_size = num * size;
	if (unlikely(num_size == 0)) {
		assert(num == 0 || size == 0);
		num_size = 1;
	}

	return (a0ialloc(num_size, true, false));
}

void
bootstrap_free(void *ptr)
{

	if (unlikely(ptr == NULL))
		return;

	a0idalloc(ptr, false);
}

/* Create a new arena and insert it into the arenas array at index ind. */
static arena_t *
arena_init_locked(unsigned ind)
{
	arena_t *arena;

	/* Expand arenas if necessary. */
	assert(ind <= narenas_total);
	if (ind > MALLOCX_ARENA_MAX)
		return (NULL);
	if (ind == narenas_total) {
		unsigned narenas_new = narenas_total + 1;
		arena_t **arenas_new =
		    (arena_t **)a0malloc(CACHELINE_CEILING(narenas_new *
		    sizeof(arena_t *)));
		if (arenas_new == NULL)
			return (NULL);
		memcpy(arenas_new, arenas, narenas_total * sizeof(arena_t *));
		arenas_new[ind] = NULL;
		/*
		 * Deallocate only if arenas came from a0malloc() (not
		 * base_alloc()).
		 */
		if (narenas_total != narenas_auto)
			a0dalloc(arenas);
		arenas = arenas_new;
		narenas_total = narenas_new;
	}

	/*
	 * Another thread may have already initialized arenas[ind] if it's an
	 * auto arena.
	 */
	arena = arenas[ind];
	if (arena != NULL) {
		assert(ind < narenas_auto);
		return (arena);
	}

	/* Actually initialize the arena. */
	arena = arenas[ind] = arena_new(ind);
	return (arena);
}

arena_t *
arena_init(unsigned ind)
{
	arena_t *arena;

	malloc_mutex_lock(&arenas_lock);
	arena = arena_init_locked(ind);
	malloc_mutex_unlock(&arenas_lock);
	return (arena);
}

unsigned
narenas_total_get(void)
{
	unsigned narenas;

	malloc_mutex_lock(&arenas_lock);
	narenas = narenas_total;
	malloc_mutex_unlock(&arenas_lock);

	return (narenas);
}

static void
arena_bind_locked(tsd_t *tsd, unsigned ind)
{
	arena_t *arena;

	arena = arenas[ind];
	arena->nthreads++;

	if (tsd_nominal(tsd))
		tsd_arena_set(tsd, arena);
}

static void
arena_bind(tsd_t *tsd, unsigned ind)
{

	malloc_mutex_lock(&arenas_lock);
	arena_bind_locked(tsd, ind);
	malloc_mutex_unlock(&arenas_lock);
}

void
arena_migrate(tsd_t *tsd, unsigned oldind, unsigned newind)
{
	arena_t *oldarena, *newarena;

	malloc_mutex_lock(&arenas_lock);
	oldarena = arenas[oldind];
	newarena = arenas[newind];
	oldarena->nthreads--;
	newarena->nthreads++;
	malloc_mutex_unlock(&arenas_lock);
	tsd_arena_set(tsd, newarena);
}

unsigned
arena_nbound(unsigned ind)
{
	unsigned nthreads;

	malloc_mutex_lock(&arenas_lock);
	nthreads = arenas[ind]->nthreads;
	malloc_mutex_unlock(&arenas_lock);
	return (nthreads);
}

static void
arena_unbind(tsd_t *tsd, unsigned ind)
{
	arena_t *arena;

	malloc_mutex_lock(&arenas_lock);
	arena = arenas[ind];
	arena->nthreads--;
	malloc_mutex_unlock(&arenas_lock);
	tsd_arena_set(tsd, NULL);
}

arena_t *
arena_get_hard(tsd_t *tsd, unsigned ind, bool init_if_missing)
{
	arena_t *arena;
	arena_t **arenas_cache = tsd_arenas_cache_get(tsd);
	unsigned narenas_cache = tsd_narenas_cache_get(tsd);
	unsigned narenas_actual = narenas_total_get();

	/* Deallocate old cache if it's too small. */
	if (arenas_cache != NULL && narenas_cache < narenas_actual) {
		a0dalloc(arenas_cache);
		arenas_cache = NULL;
		narenas_cache = 0;
		tsd_arenas_cache_set(tsd, arenas_cache);
		tsd_narenas_cache_set(tsd, narenas_cache);
	}

	/* Allocate cache if it's missing. */
	if (arenas_cache == NULL) {
		bool *arenas_cache_bypassp = tsd_arenas_cache_bypassp_get(tsd);
		assert(ind < narenas_actual || !init_if_missing);
		narenas_cache = (ind < narenas_actual) ? narenas_actual : ind+1;

		if (tsd_nominal(tsd) && !*arenas_cache_bypassp) {
			*arenas_cache_bypassp = true;
			arenas_cache = (arena_t **)a0malloc(sizeof(arena_t *) *
			    narenas_cache);
			*arenas_cache_bypassp = false;
		}
		if (arenas_cache == NULL) {
			/*
			 * This function must always tell the truth, even if
			 * it's slow, so don't let OOM, thread cleanup (note
			 * tsd_nominal check), nor recursive allocation
			 * avoidance (note arenas_cache_bypass check) get in the
			 * way.
			 */
			if (ind >= narenas_actual)
				return (NULL);
			malloc_mutex_lock(&arenas_lock);
			arena = arenas[ind];
			malloc_mutex_unlock(&arenas_lock);
			return (arena);
		}
		assert(tsd_nominal(tsd) && !*arenas_cache_bypassp);
		tsd_arenas_cache_set(tsd, arenas_cache);
		tsd_narenas_cache_set(tsd, narenas_cache);
	}

	/*
	 * Copy to cache.  It's possible that the actual number of arenas has
	 * increased since narenas_total_get() was called above, but that causes
	 * no correctness issues unless two threads concurrently execute the
	 * arenas.extend mallctl, which we trust mallctl synchronization to
	 * prevent.
	 */
	malloc_mutex_lock(&arenas_lock);
	memcpy(arenas_cache, arenas, sizeof(arena_t *) * narenas_actual);
	malloc_mutex_unlock(&arenas_lock);
	if (narenas_cache > narenas_actual) {
		memset(&arenas_cache[narenas_actual], 0, sizeof(arena_t *) *
		    (narenas_cache - narenas_actual));
	}

	/* Read the refreshed cache, and init the arena if necessary. */
	arena = arenas_cache[ind];
	if (init_if_missing && arena == NULL)
		arena = arenas_cache[ind] = arena_init(ind);
	return (arena);
}

/* Slow path, called only by arena_choose(). */
arena_t *
arena_choose_hard(tsd_t *tsd)
{
	arena_t *ret;

	if (narenas_auto > 1) {
		unsigned i, choose, first_null;

		choose = 0;
		first_null = narenas_auto;
		malloc_mutex_lock(&arenas_lock);
		assert(a0get() != NULL);
		for (i = 1; i < narenas_auto; i++) {
			if (arenas[i] != NULL) {
				/*
				 * Choose the first arena that has the lowest
				 * number of threads assigned to it.
				 */
				if (arenas[i]->nthreads <
				    arenas[choose]->nthreads)
					choose = i;
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

		if (arenas[choose]->nthreads == 0
		    || first_null == narenas_auto) {
			/*
			 * Use an unloaded arena, or the least loaded arena if
			 * all arenas are already initialized.
			 */
			ret = arenas[choose];
		} else {
			/* Initialize a new arena. */
			choose = first_null;
			ret = arena_init_locked(choose);
			if (ret == NULL) {
				malloc_mutex_unlock(&arenas_lock);
				return (NULL);
			}
		}
		arena_bind_locked(tsd, choose);
		malloc_mutex_unlock(&arenas_lock);
	} else {
		ret = a0get();
		arena_bind(tsd, 0);
	}

	return (ret);
}

void
thread_allocated_cleanup(tsd_t *tsd)
{

	/* Do nothing. */
}

void
thread_deallocated_cleanup(tsd_t *tsd)
{

	/* Do nothing. */
}

void
arena_cleanup(tsd_t *tsd)
{
	arena_t *arena;

	arena = tsd_arena_get(tsd);
	if (arena != NULL)
		arena_unbind(tsd, arena->ind);
}

void
arenas_cache_cleanup(tsd_t *tsd)
{
	arena_t **arenas_cache;

	arenas_cache = tsd_arenas_cache_get(tsd);
	if (arenas_cache != NULL) {
		tsd_arenas_cache_set(tsd, NULL);
		a0dalloc(arenas_cache);
	}
}

void
narenas_cache_cleanup(tsd_t *tsd)
{

	/* Do nothing. */
}

void
arenas_cache_bypass_cleanup(tsd_t *tsd)
{

	/* Do nothing. */
}

static void
stats_print_atexit(void)
{

	if (config_tcache && config_stats) {
		unsigned narenas, i;

		/*
		 * Merge stats from extant threads.  This is racy, since
		 * individual threads do not lock when recording tcache stats
		 * events.  As a consequence, the final stats may be slightly
		 * out of date by the time they are reported, if other threads
		 * continue to allocate.
		 */
		for (i = 0, narenas = narenas_total_get(); i < narenas; i++) {
			arena_t *arena = arenas[i];
			if (arena != NULL) {
				tcache_t *tcache;

				/*
				 * tcache_stats_merge() locks bins, so if any
				 * code is introduced that acquires both arena
				 * and bin locks in the opposite order,
				 * deadlocks may result.
				 */
				malloc_mutex_lock(&arena->lock);
				ql_foreach(tcache, &arena->tcache_ql, link) {
					tcache_stats_merge(tcache, arena);
				}
				malloc_mutex_unlock(&arena->lock);
			}
		}
	}
	je_malloc_stats_print(NULL, NULL, NULL);
}

/*
 * End miscellaneous support functions.
 */
/******************************************************************************/
/*
 * Begin initialization functions.
 */

#ifndef JEMALLOC_HAVE_SECURE_GETENV
static char *
secure_getenv(const char *name)
{

#  ifdef JEMALLOC_HAVE_ISSETUGID
	if (issetugid() != 0)
		return (NULL);
#  endif
	return (getenv(name));
}
#endif

static unsigned
malloc_ncpus(void)
{
	long result;

#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	result = si.dwNumberOfProcessors;
#else
	result = sysconf(_SC_NPROCESSORS_ONLN);
#endif
	return ((result == -1) ? 1 : (unsigned)result);
}

static bool
malloc_conf_next(char const **opts_p, char const **k_p, size_t *klen_p,
    char const **v_p, size_t *vlen_p)
{
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
			return (true);
		default:
			malloc_write("<jemalloc>: Malformed conf string\n");
			return (true);
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
	return (false);
}

static void
malloc_conf_error(const char *msg, const char *k, size_t klen, const char *v,
    size_t vlen)
{

	malloc_printf("<jemalloc>: %s: %.*s:%.*s\n", msg, (int)klen, k,
	    (int)vlen, v);
}

static void
malloc_conf_init(void)
{
	unsigned i;
	char buf[PATH_MAX + 1];
	const char *opts, *k, *v;
	size_t klen, vlen;

	/*
	 * Automatically configure valgrind before processing options.  The
	 * valgrind option remains in jemalloc 3.x for compatibility reasons.
	 */
	if (config_valgrind) {
		in_valgrind = (RUNNING_ON_VALGRIND != 0) ? true : false;
		if (config_fill && unlikely(in_valgrind)) {
			opt_junk = "false";
			opt_junk_alloc = false;
			opt_junk_free = false;
			assert(!opt_zero);
			opt_quarantine = JEMALLOC_VALGRIND_QUARANTINE_DEFAULT;
			opt_redzone = true;
		}
		if (config_tcache && unlikely(in_valgrind))
			opt_tcache = false;
	}

	for (i = 0; i < 3; i++) {
		/* Get runtime configuration. */
		switch (i) {
		case 0:
			if (je_malloc_conf != NULL) {
				/*
				 * Use options that were compiled into the
				 * program.
				 */
				opts = je_malloc_conf;
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		case 1: {
			int linklen = 0;
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
			 * Try to use the contents of the "/etc/malloc.conf"
			 * symbolic link's name.
			 */
			linklen = readlink(linkname, buf, sizeof(buf) - 1);
			if (linklen == -1) {
				/* No configuration specified. */
				linklen = 0;
				/* Restore errno. */
				set_errno(saved_errno);
			}
#endif
			buf[linklen] = '\0';
			opts = buf;
			break;
		} case 2: {
			const char *envname =
#ifdef JEMALLOC_PREFIX
			    JEMALLOC_CPREFIX"MALLOC_CONF"
#else
			    "MALLOC_CONF"
#endif
			    ;

			if ((opts = secure_getenv(envname)) != NULL) {
				/*
				 * Do nothing; opts is already initialized to
				 * the value of the MALLOC_CONF environment
				 * variable.
				 */
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		} default:
			not_reached();
			buf[0] = '\0';
			opts = buf;
		}

		while (*opts != '\0' && !malloc_conf_next(&opts, &k, &klen, &v,
		    &vlen)) {
#define	CONF_MATCH(n)							\
	(sizeof(n)-1 == klen && strncmp(n, k, klen) == 0)
#define	CONF_MATCH_VALUE(n)						\
	(sizeof(n)-1 == vlen && strncmp(n, v, vlen) == 0)
#define	CONF_HANDLE_BOOL(o, n, cont)					\
			if (CONF_MATCH(n)) {				\
				if (CONF_MATCH_VALUE("true"))		\
					o = true;			\
				else if (CONF_MATCH_VALUE("false"))	\
					o = false;			\
				else {					\
					malloc_conf_error(		\
					    "Invalid conf value",	\
					    k, klen, v, vlen);		\
				}					\
				if (cont)				\
					continue;			\
			}
#define	CONF_HANDLE_SIZE_T(o, n, min, max, clip)			\
			if (CONF_MATCH(n)) {				\
				uintmax_t um;				\
				char *end;				\
									\
				set_errno(0);				\
				um = malloc_strtoumax(v, &end, 0);	\
				if (get_errno() != 0 || (uintptr_t)end -\
				    (uintptr_t)v != vlen) {		\
					malloc_conf_error(		\
					    "Invalid conf value",	\
					    k, klen, v, vlen);		\
				} else if (clip) {			\
					if ((min) != 0 && um < (min))	\
						o = (min);		\
					else if (um > (max))		\
						o = (max);		\
					else				\
						o = um;			\
				} else {				\
					if (((min) != 0 && um < (min))	\
					    || um > (max)) {		\
						malloc_conf_error(	\
						    "Out-of-range "	\
						    "conf value",	\
						    k, klen, v, vlen);	\
					} else				\
						o = um;			\
				}					\
				continue;				\
			}
#define	CONF_HANDLE_SSIZE_T(o, n, min, max)				\
			if (CONF_MATCH(n)) {				\
				long l;					\
				char *end;				\
									\
				set_errno(0);				\
				l = strtol(v, &end, 0);			\
				if (get_errno() != 0 || (uintptr_t)end -\
				    (uintptr_t)v != vlen) {		\
					malloc_conf_error(		\
					    "Invalid conf value",	\
					    k, klen, v, vlen);		\
				} else if (l < (ssize_t)(min) || l >	\
				    (ssize_t)(max)) {			\
					malloc_conf_error(		\
					    "Out-of-range conf value",	\
					    k, klen, v, vlen);		\
				} else					\
					o = l;				\
				continue;				\
			}
#define	CONF_HANDLE_CHAR_P(o, n, d)					\
			if (CONF_MATCH(n)) {				\
				size_t cpylen = (vlen <=		\
				    sizeof(o)-1) ? vlen :		\
				    sizeof(o)-1;			\
				strncpy(o, v, cpylen);			\
				o[cpylen] = '\0';			\
				continue;				\
			}

			CONF_HANDLE_BOOL(opt_abort, "abort", true)
			/*
			 * Chunks always require at least one header page,
			 * as many as 2^(LG_SIZE_CLASS_GROUP+1) data pages, and
			 * possibly an additional page in the presence of
			 * redzones.  In order to simplify options processing,
			 * use a conservative bound that accommodates all these
			 * constraints.
			 */
			CONF_HANDLE_SIZE_T(opt_lg_chunk, "lg_chunk", LG_PAGE +
			    LG_SIZE_CLASS_GROUP + (config_fill ? 2 : 1),
			    (sizeof(size_t) << 3) - 1, true)
			if (strncmp("dss", k, klen) == 0) {
				int i;
				bool match = false;
				for (i = 0; i < dss_prec_limit; i++) {
					if (strncmp(dss_prec_names[i], v, vlen)
					    == 0) {
						if (chunk_dss_prec_set(i)) {
							malloc_conf_error(
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
					malloc_conf_error("Invalid conf value",
					    k, klen, v, vlen);
				}
				continue;
			}
			CONF_HANDLE_SIZE_T(opt_narenas, "narenas", 1,
			    SIZE_T_MAX, false)
			CONF_HANDLE_SSIZE_T(opt_lg_dirty_mult, "lg_dirty_mult",
			    -1, (sizeof(size_t) << 3) - 1)
			CONF_HANDLE_BOOL(opt_stats_print, "stats_print", true)
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
						malloc_conf_error(
						    "Invalid conf value", k,
						    klen, v, vlen);
					}
					continue;
				}
				CONF_HANDLE_SIZE_T(opt_quarantine, "quarantine",
				    0, SIZE_T_MAX, false)
				CONF_HANDLE_BOOL(opt_redzone, "redzone", true)
				CONF_HANDLE_BOOL(opt_zero, "zero", true)
			}
			if (config_utrace) {
				CONF_HANDLE_BOOL(opt_utrace, "utrace", true)
			}
			if (config_xmalloc) {
				CONF_HANDLE_BOOL(opt_xmalloc, "xmalloc", true)
			}
			if (config_tcache) {
				CONF_HANDLE_BOOL(opt_tcache, "tcache",
				    !config_valgrind || !in_valgrind)
				if (CONF_MATCH("tcache")) {
					assert(config_valgrind && in_valgrind);
					if (opt_tcache) {
						opt_tcache = false;
						malloc_conf_error(
						"tcache cannot be enabled "
						"while running inside Valgrind",
						k, klen, v, vlen);
					}
					continue;
				}
				CONF_HANDLE_SSIZE_T(opt_lg_tcache_max,
				    "lg_tcache_max", -1,
				    (sizeof(size_t) << 3) - 1)
			}
			if (config_prof) {
				CONF_HANDLE_BOOL(opt_prof, "prof", true)
				CONF_HANDLE_CHAR_P(opt_prof_prefix,
				    "prof_prefix", "jeprof")
				CONF_HANDLE_BOOL(opt_prof_active, "prof_active",
				    true)
				CONF_HANDLE_BOOL(opt_prof_thread_active_init,
				    "prof_thread_active_init", true)
				CONF_HANDLE_SIZE_T(opt_lg_prof_sample,
				    "lg_prof_sample", 0,
				    (sizeof(uint64_t) << 3) - 1, true)
				CONF_HANDLE_BOOL(opt_prof_accum, "prof_accum",
				    true)
				CONF_HANDLE_SSIZE_T(opt_lg_prof_interval,
				    "lg_prof_interval", -1,
				    (sizeof(uint64_t) << 3) - 1)
				CONF_HANDLE_BOOL(opt_prof_gdump, "prof_gdump",
				    true)
				CONF_HANDLE_BOOL(opt_prof_final, "prof_final",
				    true)
				CONF_HANDLE_BOOL(opt_prof_leak, "prof_leak",
				    true)
			}
			malloc_conf_error("Invalid conf pair", k, klen, v,
			    vlen);
#undef CONF_MATCH
#undef CONF_HANDLE_BOOL
#undef CONF_HANDLE_SIZE_T
#undef CONF_HANDLE_SSIZE_T
#undef CONF_HANDLE_CHAR_P
		}
	}
}

/* init_lock must be held. */
static bool
malloc_init_hard_needed(void)
{

	if (malloc_initialized() || (IS_INITIALIZER && malloc_init_state ==
	    malloc_init_recursible)) {
		/*
		 * Another thread initialized the allocator before this one
		 * acquired init_lock, or this thread is the initializing
		 * thread, and it is recursively allocating.
		 */
		return (false);
	}
#ifdef JEMALLOC_THREADED_INIT
	if (malloc_initializer != NO_INITIALIZER && !IS_INITIALIZER) {
		/* Busy-wait until the initializing thread completes. */
		do {
			malloc_mutex_unlock(&init_lock);
			CPU_SPINWAIT;
			malloc_mutex_lock(&init_lock);
		} while (!malloc_initialized());
		return (false);
	}
#endif
	return (true);
}

/* init_lock must be held. */
static bool
malloc_init_hard_a0_locked(void)
{

	malloc_initializer = INITIALIZER;

	if (config_prof)
		prof_boot0();
	malloc_conf_init();
	if (opt_stats_print) {
		/* Print statistics at exit. */
		if (atexit(stats_print_atexit) != 0) {
			malloc_write("<jemalloc>: Error in atexit()\n");
			if (opt_abort)
				abort();
		}
	}
	if (base_boot())
		return (true);
	if (chunk_boot())
		return (true);
	if (ctl_boot())
		return (true);
	if (config_prof)
		prof_boot1();
	if (arena_boot())
		return (true);
	if (config_tcache && tcache_boot())
		return (true);
	if (malloc_mutex_init(&arenas_lock))
		return (true);
	/*
	 * Create enough scaffolding to allow recursive allocation in
	 * malloc_ncpus().
	 */
	narenas_total = narenas_auto = 1;
	arenas = &a0;
	memset(arenas, 0, sizeof(arena_t *) * narenas_auto);
	/*
	 * Initialize one arena here.  The rest are lazily created in
	 * arena_choose_hard().
	 */
	if (arena_init(0) == NULL)
		return (true);
	malloc_init_state = malloc_init_a0_initialized;
	return (false);
}

static bool
malloc_init_hard_a0(void)
{
	bool ret;

	malloc_mutex_lock(&init_lock);
	ret = malloc_init_hard_a0_locked();
	malloc_mutex_unlock(&init_lock);
	return (ret);
}

/*
 * Initialize data structures which may trigger recursive allocation.
 *
 * init_lock must be held.
 */
static void
malloc_init_hard_recursible(void)
{

	malloc_init_state = malloc_init_recursible;
	malloc_mutex_unlock(&init_lock);

	ncpus = malloc_ncpus();

#if (!defined(JEMALLOC_MUTEX_INIT_CB) && !defined(JEMALLOC_ZONE) \
    && !defined(_WIN32) && !defined(__native_client__))
	/* LinuxThreads's pthread_atfork() allocates. */
	if (pthread_atfork(jemalloc_prefork, jemalloc_postfork_parent,
	    jemalloc_postfork_child) != 0) {
		malloc_write("<jemalloc>: Error in pthread_atfork()\n");
		if (opt_abort)
			abort();
	}
#endif
	malloc_mutex_lock(&init_lock);
}

/* init_lock must be held. */
static bool
malloc_init_hard_finish(void)
{

	if (mutex_boot())
		return (true);

	if (opt_narenas == 0) {
		/*
		 * For SMP systems, create more than one arena per CPU by
		 * default.
		 */
		if (ncpus > 1)
			opt_narenas = ncpus << 2;
		else
			opt_narenas = 1;
	}
	narenas_auto = opt_narenas;
	/*
	 * Make sure that the arenas array can be allocated.  In practice, this
	 * limit is enough to allow the allocator to function, but the ctl
	 * machinery will fail to allocate memory at far lower limits.
	 */
	if (narenas_auto > chunksize / sizeof(arena_t *)) {
		narenas_auto = chunksize / sizeof(arena_t *);
		malloc_printf("<jemalloc>: Reducing narenas to limit (%d)\n",
		    narenas_auto);
	}
	narenas_total = narenas_auto;

	/* Allocate and initialize arenas. */
	arenas = (arena_t **)base_alloc(sizeof(arena_t *) * narenas_total);
	if (arenas == NULL)
		return (true);
	/*
	 * Zero the array.  In practice, this should always be pre-zeroed,
	 * since it was just mmap()ed, but let's be sure.
	 */
	memset(arenas, 0, sizeof(arena_t *) * narenas_total);
	/* Copy the pointer to the one arena that was already initialized. */
	arenas[0] = a0;

	malloc_init_state = malloc_init_initialized;
	return (false);
}

static bool
malloc_init_hard(void)
{

#if defined(_WIN32) && _WIN32_WINNT < 0x0600
	_init_init_lock();
#endif
	malloc_mutex_lock(&init_lock);
	if (!malloc_init_hard_needed()) {
		malloc_mutex_unlock(&init_lock);
		return (false);
	}

	if (malloc_init_state != malloc_init_a0_initialized &&
	    malloc_init_hard_a0_locked()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}
	if (malloc_tsd_boot0()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}
	if (config_prof && prof_boot2()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	malloc_init_hard_recursible();

	if (malloc_init_hard_finish()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	malloc_mutex_unlock(&init_lock);
	malloc_tsd_boot1();
	return (false);
}

/*
 * End initialization functions.
 */
/******************************************************************************/
/*
 * Begin malloc(3)-compatible functions.
 */

static void *
imalloc_prof_sample(tsd_t *tsd, size_t usize, prof_tctx_t *tctx)
{
	void *p;

	if (tctx == NULL)
		return (NULL);
	if (usize <= SMALL_MAXCLASS) {
		p = imalloc(tsd, LARGE_MINCLASS);
		if (p == NULL)
			return (NULL);
		arena_prof_promoted(p, usize);
	} else
		p = imalloc(tsd, usize);

	return (p);
}

JEMALLOC_ALWAYS_INLINE_C void *
imalloc_prof(tsd_t *tsd, size_t usize)
{
	void *p;
	prof_tctx_t *tctx;

	tctx = prof_alloc_prep(tsd, usize, prof_active_get_unlocked(), true);
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U))
		p = imalloc_prof_sample(tsd, usize, tctx);
	else
		p = imalloc(tsd, usize);
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx, true);
		return (NULL);
	}
	prof_malloc(p, usize, tctx);

	return (p);
}

JEMALLOC_ALWAYS_INLINE_C void *
imalloc_body(size_t size, tsd_t **tsd, size_t *usize)
{

	if (unlikely(malloc_init()))
		return (NULL);
	*tsd = tsd_fetch();

	if (config_prof && opt_prof) {
		*usize = s2u(size);
		if (unlikely(*usize == 0))
			return (NULL);
		return (imalloc_prof(*tsd, *usize));
	}

	if (config_stats || (config_valgrind && unlikely(in_valgrind)))
		*usize = s2u(size);
	return (imalloc(*tsd, size));
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(1)
je_malloc(size_t size)
{
	void *ret;
	tsd_t *tsd;
	size_t usize JEMALLOC_CC_SILENCE_INIT(0);

	if (size == 0)
		size = 1;

	ret = imalloc_body(size, &tsd, &usize);
	if (unlikely(ret == NULL)) {
		if (config_xmalloc && unlikely(opt_xmalloc)) {
			malloc_write("<jemalloc>: Error in malloc(): "
			    "out of memory\n");
			abort();
		}
		set_errno(ENOMEM);
	}
	if (config_stats && likely(ret != NULL)) {
		assert(usize == isalloc(ret, config_prof));
		*tsd_thread_allocatedp_get(tsd) += usize;
	}
	UTRACE(0, size, ret);
	JEMALLOC_VALGRIND_MALLOC(ret != NULL, ret, usize, false);
	return (ret);
}

static void *
imemalign_prof_sample(tsd_t *tsd, size_t alignment, size_t usize,
    prof_tctx_t *tctx)
{
	void *p;

	if (tctx == NULL)
		return (NULL);
	if (usize <= SMALL_MAXCLASS) {
		assert(sa2u(LARGE_MINCLASS, alignment) == LARGE_MINCLASS);
		p = ipalloc(tsd, LARGE_MINCLASS, alignment, false);
		if (p == NULL)
			return (NULL);
		arena_prof_promoted(p, usize);
	} else
		p = ipalloc(tsd, usize, alignment, false);

	return (p);
}

JEMALLOC_ALWAYS_INLINE_C void *
imemalign_prof(tsd_t *tsd, size_t alignment, size_t usize)
{
	void *p;
	prof_tctx_t *tctx;

	tctx = prof_alloc_prep(tsd, usize, prof_active_get_unlocked(), true);
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U))
		p = imemalign_prof_sample(tsd, alignment, usize, tctx);
	else
		p = ipalloc(tsd, usize, alignment, false);
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx, true);
		return (NULL);
	}
	prof_malloc(p, usize, tctx);

	return (p);
}

JEMALLOC_ATTR(nonnull(1))
static int
imemalign(void **memptr, size_t alignment, size_t size, size_t min_alignment)
{
	int ret;
	tsd_t *tsd;
	size_t usize;
	void *result;

	assert(min_alignment != 0);

	if (unlikely(malloc_init())) {
		result = NULL;
		goto label_oom;
	}
	tsd = tsd_fetch();
	if (size == 0)
		size = 1;

	/* Make sure that alignment is a large enough power of 2. */
	if (unlikely(((alignment - 1) & alignment) != 0
	    || (alignment < min_alignment))) {
		if (config_xmalloc && unlikely(opt_xmalloc)) {
			malloc_write("<jemalloc>: Error allocating "
			    "aligned memory: invalid alignment\n");
			abort();
		}
		result = NULL;
		ret = EINVAL;
		goto label_return;
	}

	usize = sa2u(size, alignment);
	if (unlikely(usize == 0)) {
		result = NULL;
		goto label_oom;
	}

	if (config_prof && opt_prof)
		result = imemalign_prof(tsd, alignment, usize);
	else
		result = ipalloc(tsd, usize, alignment, false);
	if (unlikely(result == NULL))
		goto label_oom;
	assert(((uintptr_t)result & (alignment - 1)) == ZU(0));

	*memptr = result;
	ret = 0;
label_return:
	if (config_stats && likely(result != NULL)) {
		assert(usize == isalloc(result, config_prof));
		*tsd_thread_allocatedp_get(tsd) += usize;
	}
	UTRACE(0, size, result);
	return (ret);
label_oom:
	assert(result == NULL);
	if (config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write("<jemalloc>: Error allocating aligned memory: "
		    "out of memory\n");
		abort();
	}
	ret = ENOMEM;
	goto label_return;
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
JEMALLOC_ATTR(nonnull(1))
je_posix_memalign(void **memptr, size_t alignment, size_t size)
{
	int ret = imemalign(memptr, alignment, size, sizeof(void *));
	JEMALLOC_VALGRIND_MALLOC(ret == 0, *memptr, isalloc(*memptr,
	    config_prof), false);
	return (ret);
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(2)
je_aligned_alloc(size_t alignment, size_t size)
{
	void *ret;
	int err;

	if (unlikely((err = imemalign(&ret, alignment, size, 1)) != 0)) {
		ret = NULL;
		set_errno(err);
	}
	JEMALLOC_VALGRIND_MALLOC(err == 0, ret, isalloc(ret, config_prof),
	    false);
	return (ret);
}

static void *
icalloc_prof_sample(tsd_t *tsd, size_t usize, prof_tctx_t *tctx)
{
	void *p;

	if (tctx == NULL)
		return (NULL);
	if (usize <= SMALL_MAXCLASS) {
		p = icalloc(tsd, LARGE_MINCLASS);
		if (p == NULL)
			return (NULL);
		arena_prof_promoted(p, usize);
	} else
		p = icalloc(tsd, usize);

	return (p);
}

JEMALLOC_ALWAYS_INLINE_C void *
icalloc_prof(tsd_t *tsd, size_t usize)
{
	void *p;
	prof_tctx_t *tctx;

	tctx = prof_alloc_prep(tsd, usize, prof_active_get_unlocked(), true);
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U))
		p = icalloc_prof_sample(tsd, usize, tctx);
	else
		p = icalloc(tsd, usize);
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx, true);
		return (NULL);
	}
	prof_malloc(p, usize, tctx);

	return (p);
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE2(1, 2)
je_calloc(size_t num, size_t size)
{
	void *ret;
	tsd_t *tsd;
	size_t num_size;
	size_t usize JEMALLOC_CC_SILENCE_INIT(0);

	if (unlikely(malloc_init())) {
		num_size = 0;
		ret = NULL;
		goto label_return;
	}
	tsd = tsd_fetch();

	num_size = num * size;
	if (unlikely(num_size == 0)) {
		if (num == 0 || size == 0)
			num_size = 1;
		else {
			ret = NULL;
			goto label_return;
		}
	/*
	 * Try to avoid division here.  We know that it isn't possible to
	 * overflow during multiplication if neither operand uses any of the
	 * most significant half of the bits in a size_t.
	 */
	} else if (unlikely(((num | size) & (SIZE_T_MAX << (sizeof(size_t) <<
	    2))) && (num_size / size != num))) {
		/* size_t overflow. */
		ret = NULL;
		goto label_return;
	}

	if (config_prof && opt_prof) {
		usize = s2u(num_size);
		if (unlikely(usize == 0)) {
			ret = NULL;
			goto label_return;
		}
		ret = icalloc_prof(tsd, usize);
	} else {
		if (config_stats || (config_valgrind && unlikely(in_valgrind)))
			usize = s2u(num_size);
		ret = icalloc(tsd, num_size);
	}

label_return:
	if (unlikely(ret == NULL)) {
		if (config_xmalloc && unlikely(opt_xmalloc)) {
			malloc_write("<jemalloc>: Error in calloc(): out of "
			    "memory\n");
			abort();
		}
		set_errno(ENOMEM);
	}
	if (config_stats && likely(ret != NULL)) {
		assert(usize == isalloc(ret, config_prof));
		*tsd_thread_allocatedp_get(tsd) += usize;
	}
	UTRACE(0, num_size, ret);
	JEMALLOC_VALGRIND_MALLOC(ret != NULL, ret, usize, true);
	return (ret);
}

static void *
irealloc_prof_sample(tsd_t *tsd, void *old_ptr, size_t old_usize, size_t usize,
    prof_tctx_t *tctx)
{
	void *p;

	if (tctx == NULL)
		return (NULL);
	if (usize <= SMALL_MAXCLASS) {
		p = iralloc(tsd, old_ptr, old_usize, LARGE_MINCLASS, 0, false);
		if (p == NULL)
			return (NULL);
		arena_prof_promoted(p, usize);
	} else
		p = iralloc(tsd, old_ptr, old_usize, usize, 0, false);

	return (p);
}

JEMALLOC_ALWAYS_INLINE_C void *
irealloc_prof(tsd_t *tsd, void *old_ptr, size_t old_usize, size_t usize)
{
	void *p;
	bool prof_active;
	prof_tctx_t *old_tctx, *tctx;

	prof_active = prof_active_get_unlocked();
	old_tctx = prof_tctx_get(old_ptr);
	tctx = prof_alloc_prep(tsd, usize, prof_active, true);
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U))
		p = irealloc_prof_sample(tsd, old_ptr, old_usize, usize, tctx);
	else
		p = iralloc(tsd, old_ptr, old_usize, usize, 0, false);
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx, true);
		return (NULL);
	}
	prof_realloc(tsd, p, usize, tctx, prof_active, true, old_ptr, old_usize,
	    old_tctx);

	return (p);
}

JEMALLOC_INLINE_C void
ifree(tsd_t *tsd, void *ptr, tcache_t *tcache)
{
	size_t usize;
	UNUSED size_t rzsize JEMALLOC_CC_SILENCE_INIT(0);

	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	if (config_prof && opt_prof) {
		usize = isalloc(ptr, config_prof);
		prof_free(tsd, ptr, usize);
	} else if (config_stats || config_valgrind)
		usize = isalloc(ptr, config_prof);
	if (config_stats)
		*tsd_thread_deallocatedp_get(tsd) += usize;
	if (config_valgrind && unlikely(in_valgrind))
		rzsize = p2rz(ptr);
	iqalloc(tsd, ptr, tcache);
	JEMALLOC_VALGRIND_FREE(ptr, rzsize);
}

JEMALLOC_INLINE_C void
isfree(tsd_t *tsd, void *ptr, size_t usize, tcache_t *tcache)
{
	UNUSED size_t rzsize JEMALLOC_CC_SILENCE_INIT(0);

	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	if (config_prof && opt_prof)
		prof_free(tsd, ptr, usize);
	if (config_stats)
		*tsd_thread_deallocatedp_get(tsd) += usize;
	if (config_valgrind && unlikely(in_valgrind))
		rzsize = p2rz(ptr);
	isqalloc(tsd, ptr, usize, tcache);
	JEMALLOC_VALGRIND_FREE(ptr, rzsize);
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2)
je_realloc(void *ptr, size_t size)
{
	void *ret;
	tsd_t *tsd JEMALLOC_CC_SILENCE_INIT(NULL);
	size_t usize JEMALLOC_CC_SILENCE_INIT(0);
	size_t old_usize = 0;
	UNUSED size_t old_rzsize JEMALLOC_CC_SILENCE_INIT(0);

	if (unlikely(size == 0)) {
		if (ptr != NULL) {
			/* realloc(ptr, 0) is equivalent to free(ptr). */
			UTRACE(ptr, 0, 0);
			tsd = tsd_fetch();
			ifree(tsd, ptr, tcache_get(tsd, false));
			return (NULL);
		}
		size = 1;
	}

	if (likely(ptr != NULL)) {
		assert(malloc_initialized() || IS_INITIALIZER);
		malloc_thread_init();
		tsd = tsd_fetch();

		old_usize = isalloc(ptr, config_prof);
		if (config_valgrind && unlikely(in_valgrind))
			old_rzsize = config_prof ? p2rz(ptr) : u2rz(old_usize);

		if (config_prof && opt_prof) {
			usize = s2u(size);
			ret = unlikely(usize == 0) ? NULL : irealloc_prof(tsd,
			    ptr, old_usize, usize);
		} else {
			if (config_stats || (config_valgrind &&
			    unlikely(in_valgrind)))
				usize = s2u(size);
			ret = iralloc(tsd, ptr, old_usize, size, 0, false);
		}
	} else {
		/* realloc(NULL, size) is equivalent to malloc(size). */
		ret = imalloc_body(size, &tsd, &usize);
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
		assert(usize == isalloc(ret, config_prof));
		*tsd_thread_allocatedp_get(tsd) += usize;
		*tsd_thread_deallocatedp_get(tsd) += old_usize;
	}
	UTRACE(ptr, size, ret);
	JEMALLOC_VALGRIND_REALLOC(true, ret, usize, true, ptr, old_usize,
	    old_rzsize, true, false);
	return (ret);
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_free(void *ptr)
{

	UTRACE(ptr, 0, 0);
	if (likely(ptr != NULL)) {
		tsd_t *tsd = tsd_fetch();
		ifree(tsd, ptr, tcache_get(tsd, false));
	}
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
je_memalign(size_t alignment, size_t size)
{
	void *ret JEMALLOC_CC_SILENCE_INIT(NULL);
	if (unlikely(imemalign(&ret, alignment, size, 1) != 0))
		ret = NULL;
	JEMALLOC_VALGRIND_MALLOC(ret != NULL, ret, size, false);
	return (ret);
}
#endif

#ifdef JEMALLOC_OVERRIDE_VALLOC
JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc)
je_valloc(size_t size)
{
	void *ret JEMALLOC_CC_SILENCE_INIT(NULL);
	if (unlikely(imemalign(&ret, PAGE, size, 1) != 0))
		ret = NULL;
	JEMALLOC_VALGRIND_MALLOC(ret != NULL, ret, size, false);
	return (ret);
}
#endif

/*
 * is_malloc(je_malloc) is some macro magic to detect if jemalloc_defs.h has
 * #define je_malloc malloc
 */
#define	malloc_is_malloc 1
#define	is_malloc_(a) malloc_is_ ## a
#define	is_malloc(a) is_malloc_(a)

#if ((is_malloc(je_malloc) == 1) && defined(JEMALLOC_GLIBC_MALLOC_HOOK))
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
# ifdef JEMALLOC_GLIBC_MEMALIGN_HOOK
JEMALLOC_EXPORT void *(*__memalign_hook)(size_t alignment, size_t size) =
    je_memalign;
# endif
#endif

/*
 * End non-standard override functions.
 */
/******************************************************************************/
/*
 * Begin non-standard functions.
 */

JEMALLOC_ALWAYS_INLINE_C bool
imallocx_flags_decode_hard(tsd_t *tsd, size_t size, int flags, size_t *usize,
    size_t *alignment, bool *zero, tcache_t **tcache, arena_t **arena)
{

	if ((flags & MALLOCX_LG_ALIGN_MASK) == 0) {
		*alignment = 0;
		*usize = s2u(size);
	} else {
		*alignment = MALLOCX_ALIGN_GET_SPECIFIED(flags);
		*usize = sa2u(size, *alignment);
	}
	assert(*usize != 0);
	*zero = MALLOCX_ZERO_GET(flags);
	if ((flags & MALLOCX_TCACHE_MASK) != 0) {
		if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE)
			*tcache = NULL;
		else
			*tcache = tcaches_get(tsd, MALLOCX_TCACHE_GET(flags));
	} else
		*tcache = tcache_get(tsd, true);
	if ((flags & MALLOCX_ARENA_MASK) != 0) {
		unsigned arena_ind = MALLOCX_ARENA_GET(flags);
		*arena = arena_get(tsd, arena_ind, true, true);
		if (unlikely(*arena == NULL))
			return (true);
	} else
		*arena = NULL;
	return (false);
}

JEMALLOC_ALWAYS_INLINE_C bool
imallocx_flags_decode(tsd_t *tsd, size_t size, int flags, size_t *usize,
    size_t *alignment, bool *zero, tcache_t **tcache, arena_t **arena)
{

	if (likely(flags == 0)) {
		*usize = s2u(size);
		assert(*usize != 0);
		*alignment = 0;
		*zero = false;
		*tcache = tcache_get(tsd, true);
		*arena = NULL;
		return (false);
	} else {
		return (imallocx_flags_decode_hard(tsd, size, flags, usize,
		    alignment, zero, tcache, arena));
	}
}

JEMALLOC_ALWAYS_INLINE_C void *
imallocx_flags(tsd_t *tsd, size_t usize, size_t alignment, bool zero,
    tcache_t *tcache, arena_t *arena)
{

	if (unlikely(alignment != 0))
		return (ipalloct(tsd, usize, alignment, zero, tcache, arena));
	if (unlikely(zero))
		return (icalloct(tsd, usize, tcache, arena));
	return (imalloct(tsd, usize, tcache, arena));
}

static void *
imallocx_prof_sample(tsd_t *tsd, size_t usize, size_t alignment, bool zero,
    tcache_t *tcache, arena_t *arena)
{
	void *p;

	if (usize <= SMALL_MAXCLASS) {
		assert(((alignment == 0) ? s2u(LARGE_MINCLASS) :
		    sa2u(LARGE_MINCLASS, alignment)) == LARGE_MINCLASS);
		p = imallocx_flags(tsd, LARGE_MINCLASS, alignment, zero, tcache,
		    arena);
		if (p == NULL)
			return (NULL);
		arena_prof_promoted(p, usize);
	} else
		p = imallocx_flags(tsd, usize, alignment, zero, tcache, arena);

	return (p);
}

JEMALLOC_ALWAYS_INLINE_C void *
imallocx_prof(tsd_t *tsd, size_t size, int flags, size_t *usize)
{
	void *p;
	size_t alignment;
	bool zero;
	tcache_t *tcache;
	arena_t *arena;
	prof_tctx_t *tctx;

	if (unlikely(imallocx_flags_decode(tsd, size, flags, usize, &alignment,
	    &zero, &tcache, &arena)))
		return (NULL);
	tctx = prof_alloc_prep(tsd, *usize, prof_active_get_unlocked(), true);
	if (likely((uintptr_t)tctx == (uintptr_t)1U))
		p = imallocx_flags(tsd, *usize, alignment, zero, tcache, arena);
	else if ((uintptr_t)tctx > (uintptr_t)1U) {
		p = imallocx_prof_sample(tsd, *usize, alignment, zero, tcache,
		    arena);
	} else
		p = NULL;
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx, true);
		return (NULL);
	}
	prof_malloc(p, *usize, tctx);

	assert(alignment == 0 || ((uintptr_t)p & (alignment - 1)) == ZU(0));
	return (p);
}

JEMALLOC_ALWAYS_INLINE_C void *
imallocx_no_prof(tsd_t *tsd, size_t size, int flags, size_t *usize)
{
	void *p;
	size_t alignment;
	bool zero;
	tcache_t *tcache;
	arena_t *arena;

	if (likely(flags == 0)) {
		if (config_stats || (config_valgrind && unlikely(in_valgrind)))
			*usize = s2u(size);
		return (imalloc(tsd, size));
	}

	if (unlikely(imallocx_flags_decode_hard(tsd, size, flags, usize,
	    &alignment, &zero, &tcache, &arena)))
		return (NULL);
	p = imallocx_flags(tsd, *usize, alignment, zero, tcache, arena);
	assert(alignment == 0 || ((uintptr_t)p & (alignment - 1)) == ZU(0));
	return (p);
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(1)
je_mallocx(size_t size, int flags)
{
	tsd_t *tsd;
	void *p;
	size_t usize;

	assert(size != 0);

	if (unlikely(malloc_init()))
		goto label_oom;
	tsd = tsd_fetch();

	if (config_prof && opt_prof)
		p = imallocx_prof(tsd, size, flags, &usize);
	else
		p = imallocx_no_prof(tsd, size, flags, &usize);
	if (unlikely(p == NULL))
		goto label_oom;

	if (config_stats) {
		assert(usize == isalloc(p, config_prof));
		*tsd_thread_allocatedp_get(tsd) += usize;
	}
	UTRACE(0, size, p);
	JEMALLOC_VALGRIND_MALLOC(true, p, usize, MALLOCX_ZERO_GET(flags));
	return (p);
label_oom:
	if (config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write("<jemalloc>: Error in mallocx(): out of memory\n");
		abort();
	}
	UTRACE(0, size, 0);
	return (NULL);
}

static void *
irallocx_prof_sample(tsd_t *tsd, void *old_ptr, size_t old_usize,
    size_t usize, size_t alignment, bool zero, tcache_t *tcache, arena_t *arena,
    prof_tctx_t *tctx)
{
	void *p;

	if (tctx == NULL)
		return (NULL);
	if (usize <= SMALL_MAXCLASS) {
		p = iralloct(tsd, old_ptr, old_usize, LARGE_MINCLASS, alignment,
		    zero, tcache, arena);
		if (p == NULL)
			return (NULL);
		arena_prof_promoted(p, usize);
	} else {
		p = iralloct(tsd, old_ptr, old_usize, usize, alignment, zero,
		    tcache, arena);
	}

	return (p);
}

JEMALLOC_ALWAYS_INLINE_C void *
irallocx_prof(tsd_t *tsd, void *old_ptr, size_t old_usize, size_t size,
    size_t alignment, size_t *usize, bool zero, tcache_t *tcache,
    arena_t *arena)
{
	void *p;
	bool prof_active;
	prof_tctx_t *old_tctx, *tctx;

	prof_active = prof_active_get_unlocked();
	old_tctx = prof_tctx_get(old_ptr);
	tctx = prof_alloc_prep(tsd, *usize, prof_active, true);
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U)) {
		p = irallocx_prof_sample(tsd, old_ptr, old_usize, *usize,
		    alignment, zero, tcache, arena, tctx);
	} else {
		p = iralloct(tsd, old_ptr, old_usize, size, alignment, zero,
		    tcache, arena);
	}
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx, true);
		return (NULL);
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
		*usize = isalloc(p, config_prof);
	}
	prof_realloc(tsd, p, *usize, tctx, prof_active, true, old_ptr,
	    old_usize, old_tctx);

	return (p);
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2)
je_rallocx(void *ptr, size_t size, int flags)
{
	void *p;
	tsd_t *tsd;
	size_t usize;
	size_t old_usize;
	UNUSED size_t old_rzsize JEMALLOC_CC_SILENCE_INIT(0);
	size_t alignment = MALLOCX_ALIGN_GET(flags);
	bool zero = flags & MALLOCX_ZERO;
	arena_t *arena;
	tcache_t *tcache;

	assert(ptr != NULL);
	assert(size != 0);
	assert(malloc_initialized() || IS_INITIALIZER);
	malloc_thread_init();
	tsd = tsd_fetch();

	if (unlikely((flags & MALLOCX_ARENA_MASK) != 0)) {
		unsigned arena_ind = MALLOCX_ARENA_GET(flags);
		arena = arena_get(tsd, arena_ind, true, true);
		if (unlikely(arena == NULL))
			goto label_oom;
	} else
		arena = NULL;

	if (unlikely((flags & MALLOCX_TCACHE_MASK) != 0)) {
		if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE)
			tcache = NULL;
		else
			tcache = tcaches_get(tsd, MALLOCX_TCACHE_GET(flags));
	} else
		tcache = tcache_get(tsd, true);

	old_usize = isalloc(ptr, config_prof);
	if (config_valgrind && unlikely(in_valgrind))
		old_rzsize = u2rz(old_usize);

	if (config_prof && opt_prof) {
		usize = (alignment == 0) ? s2u(size) : sa2u(size, alignment);
		assert(usize != 0);
		p = irallocx_prof(tsd, ptr, old_usize, size, alignment, &usize,
		    zero, tcache, arena);
		if (unlikely(p == NULL))
			goto label_oom;
	} else {
		p = iralloct(tsd, ptr, old_usize, size, alignment, zero,
		     tcache, arena);
		if (unlikely(p == NULL))
			goto label_oom;
		if (config_stats || (config_valgrind && unlikely(in_valgrind)))
			usize = isalloc(p, config_prof);
	}
	assert(alignment == 0 || ((uintptr_t)p & (alignment - 1)) == ZU(0));

	if (config_stats) {
		*tsd_thread_allocatedp_get(tsd) += usize;
		*tsd_thread_deallocatedp_get(tsd) += old_usize;
	}
	UTRACE(ptr, size, p);
	JEMALLOC_VALGRIND_REALLOC(true, p, usize, false, ptr, old_usize,
	    old_rzsize, false, zero);
	return (p);
label_oom:
	if (config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write("<jemalloc>: Error in rallocx(): out of memory\n");
		abort();
	}
	UTRACE(ptr, size, 0);
	return (NULL);
}

JEMALLOC_ALWAYS_INLINE_C size_t
ixallocx_helper(void *ptr, size_t old_usize, size_t size, size_t extra,
    size_t alignment, bool zero)
{
	size_t usize;

	if (ixalloc(ptr, old_usize, size, extra, alignment, zero))
		return (old_usize);
	usize = isalloc(ptr, config_prof);

	return (usize);
}

static size_t
ixallocx_prof_sample(void *ptr, size_t old_usize, size_t size, size_t extra,
    size_t alignment, bool zero, prof_tctx_t *tctx)
{
	size_t usize;

	if (tctx == NULL)
		return (old_usize);
	usize = ixallocx_helper(ptr, old_usize, size, extra, alignment, zero);

	return (usize);
}

JEMALLOC_ALWAYS_INLINE_C size_t
ixallocx_prof(tsd_t *tsd, void *ptr, size_t old_usize, size_t size,
    size_t extra, size_t alignment, bool zero)
{
	size_t usize_max, usize;
	bool prof_active;
	prof_tctx_t *old_tctx, *tctx;

	prof_active = prof_active_get_unlocked();
	old_tctx = prof_tctx_get(ptr);
	/*
	 * usize isn't knowable before ixalloc() returns when extra is non-zero.
	 * Therefore, compute its maximum possible value and use that in
	 * prof_alloc_prep() to decide whether to capture a backtrace.
	 * prof_realloc() will use the actual usize to decide whether to sample.
	 */
	usize_max = (alignment == 0) ? s2u(size+extra) : sa2u(size+extra,
	    alignment);
	assert(usize_max != 0);
	tctx = prof_alloc_prep(tsd, usize_max, prof_active, false);
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U)) {
		usize = ixallocx_prof_sample(ptr, old_usize, size, extra,
		    alignment, zero, tctx);
	} else {
		usize = ixallocx_helper(ptr, old_usize, size, extra, alignment,
		    zero);
	}
	if (usize == old_usize) {
		prof_alloc_rollback(tsd, tctx, false);
		return (usize);
	}
	prof_realloc(tsd, ptr, usize, tctx, prof_active, false, ptr, old_usize,
	    old_tctx);

	return (usize);
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_xallocx(void *ptr, size_t size, size_t extra, int flags)
{
	tsd_t *tsd;
	size_t usize, old_usize;
	UNUSED size_t old_rzsize JEMALLOC_CC_SILENCE_INIT(0);
	size_t alignment = MALLOCX_ALIGN_GET(flags);
	bool zero = flags & MALLOCX_ZERO;

	assert(ptr != NULL);
	assert(size != 0);
	assert(SIZE_T_MAX - size >= extra);
	assert(malloc_initialized() || IS_INITIALIZER);
	malloc_thread_init();
	tsd = tsd_fetch();

	old_usize = isalloc(ptr, config_prof);

	/* Clamp extra if necessary to avoid (size + extra) overflow. */
	if (unlikely(size + extra > HUGE_MAXCLASS)) {
		/* Check for size overflow. */
		if (unlikely(size > HUGE_MAXCLASS)) {
			usize = old_usize;
			goto label_not_resized;
		}
		extra = HUGE_MAXCLASS - size;
	}

	if (config_valgrind && unlikely(in_valgrind))
		old_rzsize = u2rz(old_usize);

	if (config_prof && opt_prof) {
		usize = ixallocx_prof(tsd, ptr, old_usize, size, extra,
		    alignment, zero);
	} else {
		usize = ixallocx_helper(ptr, old_usize, size, extra, alignment,
		    zero);
	}
	if (unlikely(usize == old_usize))
		goto label_not_resized;

	if (config_stats) {
		*tsd_thread_allocatedp_get(tsd) += usize;
		*tsd_thread_deallocatedp_get(tsd) += old_usize;
	}
	JEMALLOC_VALGRIND_REALLOC(false, ptr, usize, false, ptr, old_usize,
	    old_rzsize, false, zero);
label_not_resized:
	UTRACE(ptr, size, ptr);
	return (usize);
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
JEMALLOC_ATTR(pure)
je_sallocx(const void *ptr, int flags)
{
	size_t usize;

	assert(malloc_initialized() || IS_INITIALIZER);
	malloc_thread_init();

	if (config_ivsalloc)
		usize = ivsalloc(ptr, config_prof);
	else
		usize = isalloc(ptr, config_prof);

	return (usize);
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_dallocx(void *ptr, int flags)
{
	tsd_t *tsd;
	tcache_t *tcache;

	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	tsd = tsd_fetch();
	if (unlikely((flags & MALLOCX_TCACHE_MASK) != 0)) {
		if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE)
			tcache = NULL;
		else
			tcache = tcaches_get(tsd, MALLOCX_TCACHE_GET(flags));
	} else
		tcache = tcache_get(tsd, false);

	UTRACE(ptr, 0, 0);
	ifree(tsd_fetch(), ptr, tcache);
}

JEMALLOC_ALWAYS_INLINE_C size_t
inallocx(size_t size, int flags)
{
	size_t usize;

	if (likely((flags & MALLOCX_LG_ALIGN_MASK) == 0))
		usize = s2u(size);
	else
		usize = sa2u(size, MALLOCX_ALIGN_GET_SPECIFIED(flags));
	assert(usize != 0);
	return (usize);
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_sdallocx(void *ptr, size_t size, int flags)
{
	tsd_t *tsd;
	tcache_t *tcache;
	size_t usize;

	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);
	usize = inallocx(size, flags);
	assert(usize == isalloc(ptr, config_prof));

	tsd = tsd_fetch();
	if (unlikely((flags & MALLOCX_TCACHE_MASK) != 0)) {
		if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE)
			tcache = NULL;
		else
			tcache = tcaches_get(tsd, MALLOCX_TCACHE_GET(flags));
	} else
		tcache = tcache_get(tsd, false);

	UTRACE(ptr, 0, 0);
	isfree(tsd, ptr, usize, tcache);
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
JEMALLOC_ATTR(pure)
je_nallocx(size_t size, int flags)
{

	assert(size != 0);

	if (unlikely(malloc_init()))
		return (0);

	return (inallocx(size, flags));
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen)
{

	if (unlikely(malloc_init()))
		return (EAGAIN);

	return (ctl_byname(name, oldp, oldlenp, newp, newlen));
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctlnametomib(const char *name, size_t *mibp, size_t *miblenp)
{

	if (unlikely(malloc_init()))
		return (EAGAIN);

	return (ctl_nametomib(name, mibp, miblenp));
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctlbymib(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
  void *newp, size_t newlen)
{

	if (unlikely(malloc_init()))
		return (EAGAIN);

	return (ctl_bymib(mib, miblen, oldp, oldlenp, newp, newlen));
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_malloc_stats_print(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *opts)
{

	stats_print(write_cb, cbopaque, opts);
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_malloc_usable_size(JEMALLOC_USABLE_SIZE_CONST void *ptr)
{
	size_t ret;

	assert(malloc_initialized() || IS_INITIALIZER);
	malloc_thread_init();

	if (config_ivsalloc)
		ret = ivsalloc(ptr, config_prof);
	else
		ret = (ptr == NULL) ? 0 : isalloc(ptr, config_prof);

	return (ret);
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
JEMALLOC_ATTR(constructor)
static void
jemalloc_constructor(void)
{

	malloc_init();
}

#ifndef JEMALLOC_MUTEX_INIT_CB
void
jemalloc_prefork(void)
#else
JEMALLOC_EXPORT void
_malloc_prefork(void)
#endif
{
	unsigned i;

#ifdef JEMALLOC_MUTEX_INIT_CB
	if (!malloc_initialized())
		return;
#endif
	assert(malloc_initialized());

	/* Acquire all mutexes in a safe order. */
	ctl_prefork();
	prof_prefork();
	malloc_mutex_prefork(&arenas_lock);
	for (i = 0; i < narenas_total; i++) {
		if (arenas[i] != NULL)
			arena_prefork(arenas[i]);
	}
	chunk_prefork();
	base_prefork();
}

#ifndef JEMALLOC_MUTEX_INIT_CB
void
jemalloc_postfork_parent(void)
#else
JEMALLOC_EXPORT void
_malloc_postfork(void)
#endif
{
	unsigned i;

#ifdef JEMALLOC_MUTEX_INIT_CB
	if (!malloc_initialized())
		return;
#endif
	assert(malloc_initialized());

	/* Release all mutexes, now that fork() has completed. */
	base_postfork_parent();
	chunk_postfork_parent();
	for (i = 0; i < narenas_total; i++) {
		if (arenas[i] != NULL)
			arena_postfork_parent(arenas[i]);
	}
	malloc_mutex_postfork_parent(&arenas_lock);
	prof_postfork_parent();
	ctl_postfork_parent();
}

void
jemalloc_postfork_child(void)
{
	unsigned i;

	assert(malloc_initialized());

	/* Release all mutexes, now that fork() has completed. */
	base_postfork_child();
	chunk_postfork_child();
	for (i = 0; i < narenas_total; i++) {
		if (arenas[i] != NULL)
			arena_postfork_child(arenas[i]);
	}
	malloc_mutex_postfork_child(&arenas_lock);
	prof_postfork_child();
	ctl_postfork_child();
}

/******************************************************************************/

/* Helps the application decide if a pointer is worth re-allocating in order to reduce fragmentation.
 * returns 0 if the allocation is in the currently active run,
 * or when it is not causing any frag issue (large or huge bin)
 * returns the bin utilization and run utilization both in fixed point 16:16.
 * If the application decides to re-allocate it should use MALLOCX_TCACHE_NONE when doing so. */
JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_get_defrag_hint(void* ptr, int *bin_util, int *run_util) {
    int defrag = 0;
    arena_chunk_t *chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
    if (likely(chunk != ptr)) { /* indication that this is not a HUGE alloc */
        size_t pageind = ((uintptr_t)ptr - (uintptr_t)chunk) >> LG_PAGE;
        size_t mapbits = arena_mapbits_get(chunk, pageind);
        if (likely((mapbits & CHUNK_MAP_LARGE) == 0)) { /* indication that this is not a LARGE alloc */
            arena_t *arena = extent_node_arena_get(&chunk->node);
            size_t rpages_ind = pageind - arena_mapbits_small_runind_get(chunk, pageind);
            arena_run_t *run = &arena_miscelm_get(chunk, rpages_ind)->run;
            arena_bin_t *bin = &arena->bins[run->binind];
            malloc_mutex_lock(&bin->lock);
            /* runs that are in the same chunk in as the current chunk, are likely to be the next currun */
            if (chunk != (arena_chunk_t *)CHUNK_ADDR2BASE(bin->runcur)) {
                arena_bin_info_t *bin_info = &arena_bin_info[run->binind];
                size_t availregs = bin_info->nregs * bin->stats.curruns;
                *bin_util = (bin->stats.curregs<<16) / availregs;
                *run_util = ((bin_info->nregs - run->nfree)<<16) / bin_info->nregs;
                defrag = 1;
            }
            malloc_mutex_unlock(&bin->lock);
        }
    }
    return defrag;
}
