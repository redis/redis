#define	JEMALLOC_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

malloc_tsd_data(, arenas, arena_t *, NULL)
malloc_tsd_data(, thread_allocated, thread_allocated_t,
    THREAD_ALLOCATED_INITIALIZER)

/* Runtime configuration options. */
const char	*je_malloc_conf;
#ifdef JEMALLOC_DEBUG
bool	opt_abort = true;
#  ifdef JEMALLOC_FILL
bool	opt_junk = true;
#  else
bool	opt_junk = false;
#  endif
#else
bool	opt_abort = false;
bool	opt_junk = false;
#endif
size_t	opt_quarantine = ZU(0);
bool	opt_redzone = false;
bool	opt_utrace = false;
bool	opt_valgrind = false;
bool	opt_xmalloc = false;
bool	opt_zero = false;
size_t	opt_narenas = 0;

unsigned	ncpus;

malloc_mutex_t		arenas_lock;
arena_t			**arenas;
unsigned		narenas;

/* Set to true once the allocator has been initialized. */
static bool		malloc_initialized = false;

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
static malloc_mutex_t	init_lock;

JEMALLOC_ATTR(constructor)
static void WINAPI
_init_init_lock(void)
{

	malloc_mutex_init(&init_lock);
}

#ifdef _MSC_VER
#  pragma section(".CRT$XCU", read)
JEMALLOC_SECTION(".CRT$XCU") JEMALLOC_ATTR(used)
static const void (WINAPI *init_init_lock)(void) = _init_init_lock;
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
	if (opt_utrace) {						\
		malloc_utrace_t ut;					\
		ut.p = (a);						\
		ut.s = (b);						\
		ut.r = (c);						\
		utrace(&ut, sizeof(ut));				\
	}								\
} while (0)
#else
#  define UTRACE(a, b, c)
#endif

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	stats_print_atexit(void);
static unsigned	malloc_ncpus(void);
static bool	malloc_conf_next(char const **opts_p, char const **k_p,
    size_t *klen_p, char const **v_p, size_t *vlen_p);
static void	malloc_conf_error(const char *msg, const char *k, size_t klen,
    const char *v, size_t vlen);
static void	malloc_conf_init(void);
static bool	malloc_init_hard(void);
static int	imemalign(void **memptr, size_t alignment, size_t size,
    size_t min_alignment);

/******************************************************************************/
/*
 * Begin miscellaneous support functions.
 */

/* Create a new arena and insert it into the arenas array at index ind. */
arena_t *
arenas_extend(unsigned ind)
{
	arena_t *ret;

	ret = (arena_t *)base_alloc(sizeof(arena_t));
	if (ret != NULL && arena_new(ret, ind) == false) {
		arenas[ind] = ret;
		return (ret);
	}
	/* Only reached if there is an OOM error. */

	/*
	 * OOM here is quite inconvenient to propagate, since dealing with it
	 * would require a check for failure in the fast path.  Instead, punt
	 * by using arenas[0].  In practice, this is an extremely unlikely
	 * failure.
	 */
	malloc_write("<jemalloc>: Error initializing arena\n");
	if (opt_abort)
		abort();

	return (arenas[0]);
}

/* Slow path, called only by choose_arena(). */
arena_t *
choose_arena_hard(void)
{
	arena_t *ret;

	if (narenas > 1) {
		unsigned i, choose, first_null;

		choose = 0;
		first_null = narenas;
		malloc_mutex_lock(&arenas_lock);
		assert(arenas[0] != NULL);
		for (i = 1; i < narenas; i++) {
			if (arenas[i] != NULL) {
				/*
				 * Choose the first arena that has the lowest
				 * number of threads assigned to it.
				 */
				if (arenas[i]->nthreads <
				    arenas[choose]->nthreads)
					choose = i;
			} else if (first_null == narenas) {
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

		if (arenas[choose]->nthreads == 0 || first_null == narenas) {
			/*
			 * Use an unloaded arena, or the least loaded arena if
			 * all arenas are already initialized.
			 */
			ret = arenas[choose];
		} else {
			/* Initialize a new arena. */
			ret = arenas_extend(first_null);
		}
		ret->nthreads++;
		malloc_mutex_unlock(&arenas_lock);
	} else {
		ret = arenas[0];
		malloc_mutex_lock(&arenas_lock);
		ret->nthreads++;
		malloc_mutex_unlock(&arenas_lock);
	}

	arenas_tsd_set(&ret);

	return (ret);
}

static void
stats_print_atexit(void)
{

	if (config_tcache && config_stats) {
		unsigned i;

		/*
		 * Merge stats from extant threads.  This is racy, since
		 * individual threads do not lock when recording tcache stats
		 * events.  As a consequence, the final stats may be slightly
		 * out of date by the time they are reported, if other threads
		 * continue to allocate.
		 */
		for (i = 0; i < narenas; i++) {
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

static unsigned
malloc_ncpus(void)
{
	unsigned ret;
	long result;

#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	result = si.dwNumberOfProcessors;
#else
	result = sysconf(_SC_NPROCESSORS_ONLN);
	if (result == -1) {
		/* Error. */
		ret = 1;
	}
#endif
	ret = (unsigned)result;

	return (ret);
}

void
arenas_cleanup(void *arg)
{
	arena_t *arena = *(arena_t **)arg;

	malloc_mutex_lock(&arenas_lock);
	arena->nthreads--;
	malloc_mutex_unlock(&arenas_lock);
}

static inline bool
malloc_init(void)
{

	if (malloc_initialized == false)
		return (malloc_init_hard());

	return (false);
}

static bool
malloc_conf_next(char const **opts_p, char const **k_p, size_t *klen_p,
    char const **v_p, size_t *vlen_p)
{
	bool accept;
	const char *opts = *opts_p;

	*k_p = opts;

	for (accept = false; accept == false;) {
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

	for (accept = false; accept == false;) {
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
#ifndef _WIN32
			int linklen;
			const char *linkname =
#  ifdef JEMALLOC_PREFIX
			    "/etc/"JEMALLOC_PREFIX"malloc.conf"
#  else
			    "/etc/malloc.conf"
#  endif
			    ;

			if ((linklen = readlink(linkname, buf,
			    sizeof(buf) - 1)) != -1) {
				/*
				 * Use the contents of the "/etc/malloc.conf"
				 * symbolic link's name.
				 */
				buf[linklen] = '\0';
				opts = buf;
			} else
#endif
			{
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		} case 2: {
			const char *envname =
#ifdef JEMALLOC_PREFIX
			    JEMALLOC_CPREFIX"MALLOC_CONF"
#else
			    "MALLOC_CONF"
#endif
			    ;

			if ((opts = getenv(envname)) != NULL) {
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
			/* NOTREACHED */
			assert(false);
			buf[0] = '\0';
			opts = buf;
		}

		while (*opts != '\0' && malloc_conf_next(&opts, &k, &klen, &v,
		    &vlen) == false) {
#define	CONF_HANDLE_BOOL_HIT(o, n, hit)					\
			if (sizeof(n)-1 == klen && strncmp(n, k,	\
			    klen) == 0) {				\
				if (strncmp("true", v, vlen) == 0 &&	\
				    vlen == sizeof("true")-1)		\
					o = true;			\
				else if (strncmp("false", v, vlen) ==	\
				    0 && vlen == sizeof("false")-1)	\
					o = false;			\
				else {					\
					malloc_conf_error(		\
					    "Invalid conf value",	\
					    k, klen, v, vlen);		\
				}					\
				hit = true;				\
			} else						\
				hit = false;
#define	CONF_HANDLE_BOOL(o, n) {					\
			bool hit;					\
			CONF_HANDLE_BOOL_HIT(o, n, hit);		\
			if (hit)					\
				continue;				\
}
#define	CONF_HANDLE_SIZE_T(o, n, min, max)				\
			if (sizeof(n)-1 == klen && strncmp(n, k,	\
			    klen) == 0) {				\
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
				} else if (um < min || um > max) {	\
					malloc_conf_error(		\
					    "Out-of-range conf value",	\
					    k, klen, v, vlen);		\
				} else					\
					o = um;				\
				continue;				\
			}
#define	CONF_HANDLE_SSIZE_T(o, n, min, max)				\
			if (sizeof(n)-1 == klen && strncmp(n, k,	\
			    klen) == 0) {				\
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
				} else if (l < (ssize_t)min || l >	\
				    (ssize_t)max) {			\
					malloc_conf_error(		\
					    "Out-of-range conf value",	\
					    k, klen, v, vlen);		\
				} else					\
					o = l;				\
				continue;				\
			}
#define	CONF_HANDLE_CHAR_P(o, n, d)					\
			if (sizeof(n)-1 == klen && strncmp(n, k,	\
			    klen) == 0) {				\
				size_t cpylen = (vlen <=		\
				    sizeof(o)-1) ? vlen :		\
				    sizeof(o)-1;			\
				strncpy(o, v, cpylen);			\
				o[cpylen] = '\0';			\
				continue;				\
			}

			CONF_HANDLE_BOOL(opt_abort, "abort")
			/*
			 * Chunks always require at least one header page, plus
			 * one data page in the absence of redzones, or three
			 * pages in the presence of redzones.  In order to
			 * simplify options processing, fix the limit based on
			 * config_fill.
			 */
			CONF_HANDLE_SIZE_T(opt_lg_chunk, "lg_chunk", LG_PAGE +
			    (config_fill ? 2 : 1), (sizeof(size_t) << 3) - 1)
			CONF_HANDLE_SIZE_T(opt_narenas, "narenas", 1,
			    SIZE_T_MAX)
			CONF_HANDLE_SSIZE_T(opt_lg_dirty_mult, "lg_dirty_mult",
			    -1, (sizeof(size_t) << 3) - 1)
			CONF_HANDLE_BOOL(opt_stats_print, "stats_print")
			if (config_fill) {
				CONF_HANDLE_BOOL(opt_junk, "junk")
				CONF_HANDLE_SIZE_T(opt_quarantine, "quarantine",
				    0, SIZE_T_MAX)
				CONF_HANDLE_BOOL(opt_redzone, "redzone")
				CONF_HANDLE_BOOL(opt_zero, "zero")
			}
			if (config_utrace) {
				CONF_HANDLE_BOOL(opt_utrace, "utrace")
			}
			if (config_valgrind) {
				bool hit;
				CONF_HANDLE_BOOL_HIT(opt_valgrind,
				    "valgrind", hit)
				if (config_fill && opt_valgrind && hit) {
					opt_junk = false;
					opt_zero = false;
					if (opt_quarantine == 0) {
						opt_quarantine =
						    JEMALLOC_VALGRIND_QUARANTINE_DEFAULT;
					}
					opt_redzone = true;
				}
				if (hit)
					continue;
			}
			if (config_xmalloc) {
				CONF_HANDLE_BOOL(opt_xmalloc, "xmalloc")
			}
			if (config_tcache) {
				CONF_HANDLE_BOOL(opt_tcache, "tcache")
				CONF_HANDLE_SSIZE_T(opt_lg_tcache_max,
				    "lg_tcache_max", -1,
				    (sizeof(size_t) << 3) - 1)
			}
			if (config_prof) {
				CONF_HANDLE_BOOL(opt_prof, "prof")
				CONF_HANDLE_CHAR_P(opt_prof_prefix,
				    "prof_prefix", "jeprof")
				CONF_HANDLE_BOOL(opt_prof_active, "prof_active")
				CONF_HANDLE_SSIZE_T(opt_lg_prof_sample,
				    "lg_prof_sample", 0,
				    (sizeof(uint64_t) << 3) - 1)
				CONF_HANDLE_BOOL(opt_prof_accum, "prof_accum")
				CONF_HANDLE_SSIZE_T(opt_lg_prof_interval,
				    "lg_prof_interval", -1,
				    (sizeof(uint64_t) << 3) - 1)
				CONF_HANDLE_BOOL(opt_prof_gdump, "prof_gdump")
				CONF_HANDLE_BOOL(opt_prof_final, "prof_final")
				CONF_HANDLE_BOOL(opt_prof_leak, "prof_leak")
			}
			malloc_conf_error("Invalid conf pair", k, klen, v,
			    vlen);
#undef CONF_HANDLE_BOOL
#undef CONF_HANDLE_SIZE_T
#undef CONF_HANDLE_SSIZE_T
#undef CONF_HANDLE_CHAR_P
		}
	}
}

static bool
malloc_init_hard(void)
{
	arena_t *init_arenas[1];

	malloc_mutex_lock(&init_lock);
	if (malloc_initialized || IS_INITIALIZER) {
		/*
		 * Another thread initialized the allocator before this one
		 * acquired init_lock, or this thread is the initializing
		 * thread, and it is recursively allocating.
		 */
		malloc_mutex_unlock(&init_lock);
		return (false);
	}
#ifdef JEMALLOC_THREADED_INIT
	if (malloc_initializer != NO_INITIALIZER && IS_INITIALIZER == false) {
		/* Busy-wait until the initializing thread completes. */
		do {
			malloc_mutex_unlock(&init_lock);
			CPU_SPINWAIT;
			malloc_mutex_lock(&init_lock);
		} while (malloc_initialized == false);
		malloc_mutex_unlock(&init_lock);
		return (false);
	}
#endif
	malloc_initializer = INITIALIZER;

	malloc_tsd_boot();
	if (config_prof)
		prof_boot0();

	malloc_conf_init();

#if (!defined(JEMALLOC_MUTEX_INIT_CB) && !defined(JEMALLOC_ZONE) \
    && !defined(_WIN32))
	/* Register fork handlers. */
	if (pthread_atfork(jemalloc_prefork, jemalloc_postfork_parent,
	    jemalloc_postfork_child) != 0) {
		malloc_write("<jemalloc>: Error in pthread_atfork()\n");
		if (opt_abort)
			abort();
	}
#endif

	if (opt_stats_print) {
		/* Print statistics at exit. */
		if (atexit(stats_print_atexit) != 0) {
			malloc_write("<jemalloc>: Error in atexit()\n");
			if (opt_abort)
				abort();
		}
	}

	if (base_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (chunk_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (ctl_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (config_prof)
		prof_boot1();

	arena_boot();

	if (config_tcache && tcache_boot0()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (huge_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (malloc_mutex_init(&arenas_lock))
		return (true);

	/*
	 * Create enough scaffolding to allow recursive allocation in
	 * malloc_ncpus().
	 */
	narenas = 1;
	arenas = init_arenas;
	memset(arenas, 0, sizeof(arena_t *) * narenas);

	/*
	 * Initialize one arena here.  The rest are lazily created in
	 * choose_arena_hard().
	 */
	arenas_extend(0);
	if (arenas[0] == NULL) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	/* Initialize allocation counters before any allocations can occur. */
	if (config_stats && thread_allocated_tsd_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (arenas_tsd_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (config_tcache && tcache_boot1()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (config_fill && quarantine_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (config_prof && prof_boot2()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	/* Get number of CPUs. */
	malloc_mutex_unlock(&init_lock);
	ncpus = malloc_ncpus();
	malloc_mutex_lock(&init_lock);

	if (mutex_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

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
	narenas = opt_narenas;
	/*
	 * Make sure that the arenas array can be allocated.  In practice, this
	 * limit is enough to allow the allocator to function, but the ctl
	 * machinery will fail to allocate memory at far lower limits.
	 */
	if (narenas > chunksize / sizeof(arena_t *)) {
		narenas = chunksize / sizeof(arena_t *);
		malloc_printf("<jemalloc>: Reducing narenas to limit (%d)\n",
		    narenas);
	}

	/* Allocate and initialize arenas. */
	arenas = (arena_t **)base_alloc(sizeof(arena_t *) * narenas);
	if (arenas == NULL) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}
	/*
	 * Zero the array.  In practice, this should always be pre-zeroed,
	 * since it was just mmap()ed, but let's be sure.
	 */
	memset(arenas, 0, sizeof(arena_t *) * narenas);
	/* Copy the pointer to the one arena that was already initialized. */
	arenas[0] = init_arenas[0];

	malloc_initialized = true;
	malloc_mutex_unlock(&init_lock);
	return (false);
}

/*
 * End initialization functions.
 */
/******************************************************************************/
/*
 * Begin malloc(3)-compatible functions.
 */

void *
je_malloc(size_t size)
{
	void *ret;
	size_t usize JEMALLOC_CC_SILENCE_INIT(0);
	prof_thr_cnt_t *cnt JEMALLOC_CC_SILENCE_INIT(NULL);

	if (malloc_init()) {
		ret = NULL;
		goto label_oom;
	}

	if (size == 0)
		size = 1;

	if (config_prof && opt_prof) {
		usize = s2u(size);
		PROF_ALLOC_PREP(1, usize, cnt);
		if (cnt == NULL) {
			ret = NULL;
			goto label_oom;
		}
		if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U && usize <=
		    SMALL_MAXCLASS) {
			ret = imalloc(SMALL_MAXCLASS+1);
			if (ret != NULL)
				arena_prof_promoted(ret, usize);
		} else
			ret = imalloc(size);
	} else {
		if (config_stats || (config_valgrind && opt_valgrind))
			usize = s2u(size);
		ret = imalloc(size);
	}

label_oom:
	if (ret == NULL) {
		if (config_xmalloc && opt_xmalloc) {
			malloc_write("<jemalloc>: Error in malloc(): "
			    "out of memory\n");
			abort();
		}
		set_errno(ENOMEM);
	}
	if (config_prof && opt_prof && ret != NULL)
		prof_malloc(ret, usize, cnt);
	if (config_stats && ret != NULL) {
		assert(usize == isalloc(ret, config_prof));
		thread_allocated_tsd_get()->allocated += usize;
	}
	UTRACE(0, size, ret);
	JEMALLOC_VALGRIND_MALLOC(ret != NULL, ret, usize, false);
	return (ret);
}

JEMALLOC_ATTR(nonnull(1))
#ifdef JEMALLOC_PROF
/*
 * Avoid any uncertainty as to how many backtrace frames to ignore in
 * PROF_ALLOC_PREP().
 */
JEMALLOC_ATTR(noinline)
#endif
static int
imemalign(void **memptr, size_t alignment, size_t size,
    size_t min_alignment)
{
	int ret;
	size_t usize;
	void *result;
	prof_thr_cnt_t *cnt JEMALLOC_CC_SILENCE_INIT(NULL);

	assert(min_alignment != 0);

	if (malloc_init())
		result = NULL;
	else {
		if (size == 0)
			size = 1;

		/* Make sure that alignment is a large enough power of 2. */
		if (((alignment - 1) & alignment) != 0
		    || (alignment < min_alignment)) {
			if (config_xmalloc && opt_xmalloc) {
				malloc_write("<jemalloc>: Error allocating "
				    "aligned memory: invalid alignment\n");
				abort();
			}
			result = NULL;
			ret = EINVAL;
			goto label_return;
		}

		usize = sa2u(size, alignment);
		if (usize == 0) {
			result = NULL;
			ret = ENOMEM;
			goto label_return;
		}

		if (config_prof && opt_prof) {
			PROF_ALLOC_PREP(2, usize, cnt);
			if (cnt == NULL) {
				result = NULL;
				ret = EINVAL;
			} else {
				if (prof_promote && (uintptr_t)cnt !=
				    (uintptr_t)1U && usize <= SMALL_MAXCLASS) {
					assert(sa2u(SMALL_MAXCLASS+1,
					    alignment) != 0);
					result = ipalloc(sa2u(SMALL_MAXCLASS+1,
					    alignment), alignment, false);
					if (result != NULL) {
						arena_prof_promoted(result,
						    usize);
					}
				} else {
					result = ipalloc(usize, alignment,
					    false);
				}
			}
		} else
			result = ipalloc(usize, alignment, false);
	}

	if (result == NULL) {
		if (config_xmalloc && opt_xmalloc) {
			malloc_write("<jemalloc>: Error allocating aligned "
			    "memory: out of memory\n");
			abort();
		}
		ret = ENOMEM;
		goto label_return;
	}

	*memptr = result;
	ret = 0;

label_return:
	if (config_stats && result != NULL) {
		assert(usize == isalloc(result, config_prof));
		thread_allocated_tsd_get()->allocated += usize;
	}
	if (config_prof && opt_prof && result != NULL)
		prof_malloc(result, usize, cnt);
	UTRACE(0, size, result);
	return (ret);
}

int
je_posix_memalign(void **memptr, size_t alignment, size_t size)
{
	int ret = imemalign(memptr, alignment, size, sizeof(void *));
	JEMALLOC_VALGRIND_MALLOC(ret == 0, *memptr, isalloc(*memptr,
	    config_prof), false);
	return (ret);
}

void *
je_aligned_alloc(size_t alignment, size_t size)
{
	void *ret;
	int err;

	if ((err = imemalign(&ret, alignment, size, 1)) != 0) {
		ret = NULL;
		set_errno(err);
	}
	JEMALLOC_VALGRIND_MALLOC(err == 0, ret, isalloc(ret, config_prof),
	    false);
	return (ret);
}

void *
je_calloc(size_t num, size_t size)
{
	void *ret;
	size_t num_size;
	size_t usize JEMALLOC_CC_SILENCE_INIT(0);
	prof_thr_cnt_t *cnt JEMALLOC_CC_SILENCE_INIT(NULL);

	if (malloc_init()) {
		num_size = 0;
		ret = NULL;
		goto label_return;
	}

	num_size = num * size;
	if (num_size == 0) {
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
	} else if (((num | size) & (SIZE_T_MAX << (sizeof(size_t) << 2)))
	    && (num_size / size != num)) {
		/* size_t overflow. */
		ret = NULL;
		goto label_return;
	}

	if (config_prof && opt_prof) {
		usize = s2u(num_size);
		PROF_ALLOC_PREP(1, usize, cnt);
		if (cnt == NULL) {
			ret = NULL;
			goto label_return;
		}
		if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U && usize
		    <= SMALL_MAXCLASS) {
			ret = icalloc(SMALL_MAXCLASS+1);
			if (ret != NULL)
				arena_prof_promoted(ret, usize);
		} else
			ret = icalloc(num_size);
	} else {
		if (config_stats || (config_valgrind && opt_valgrind))
			usize = s2u(num_size);
		ret = icalloc(num_size);
	}

label_return:
	if (ret == NULL) {
		if (config_xmalloc && opt_xmalloc) {
			malloc_write("<jemalloc>: Error in calloc(): out of "
			    "memory\n");
			abort();
		}
		set_errno(ENOMEM);
	}

	if (config_prof && opt_prof && ret != NULL)
		prof_malloc(ret, usize, cnt);
	if (config_stats && ret != NULL) {
		assert(usize == isalloc(ret, config_prof));
		thread_allocated_tsd_get()->allocated += usize;
	}
	UTRACE(0, num_size, ret);
	JEMALLOC_VALGRIND_MALLOC(ret != NULL, ret, usize, true);
	return (ret);
}

void *
je_realloc(void *ptr, size_t size)
{
	void *ret;
	size_t usize JEMALLOC_CC_SILENCE_INIT(0);
	size_t old_size = 0;
	size_t old_rzsize JEMALLOC_CC_SILENCE_INIT(0);
	prof_thr_cnt_t *cnt JEMALLOC_CC_SILENCE_INIT(NULL);
	prof_ctx_t *old_ctx JEMALLOC_CC_SILENCE_INIT(NULL);

	if (size == 0) {
		if (ptr != NULL) {
			/* realloc(ptr, 0) is equivalent to free(p). */
			if (config_prof) {
				old_size = isalloc(ptr, true);
				if (config_valgrind && opt_valgrind)
					old_rzsize = p2rz(ptr);
			} else if (config_stats) {
				old_size = isalloc(ptr, false);
				if (config_valgrind && opt_valgrind)
					old_rzsize = u2rz(old_size);
			} else if (config_valgrind && opt_valgrind) {
				old_size = isalloc(ptr, false);
				old_rzsize = u2rz(old_size);
			}
			if (config_prof && opt_prof) {
				old_ctx = prof_ctx_get(ptr);
				cnt = NULL;
			}
			iqalloc(ptr);
			ret = NULL;
			goto label_return;
		} else
			size = 1;
	}

	if (ptr != NULL) {
		assert(malloc_initialized || IS_INITIALIZER);

		if (config_prof) {
			old_size = isalloc(ptr, true);
			if (config_valgrind && opt_valgrind)
				old_rzsize = p2rz(ptr);
		} else if (config_stats) {
			old_size = isalloc(ptr, false);
			if (config_valgrind && opt_valgrind)
				old_rzsize = u2rz(old_size);
		} else if (config_valgrind && opt_valgrind) {
			old_size = isalloc(ptr, false);
			old_rzsize = u2rz(old_size);
		}
		if (config_prof && opt_prof) {
			usize = s2u(size);
			old_ctx = prof_ctx_get(ptr);
			PROF_ALLOC_PREP(1, usize, cnt);
			if (cnt == NULL) {
				old_ctx = NULL;
				ret = NULL;
				goto label_oom;
			}
			if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U &&
			    usize <= SMALL_MAXCLASS) {
				ret = iralloc(ptr, SMALL_MAXCLASS+1, 0, 0,
				    false, false);
				if (ret != NULL)
					arena_prof_promoted(ret, usize);
				else
					old_ctx = NULL;
			} else {
				ret = iralloc(ptr, size, 0, 0, false, false);
				if (ret == NULL)
					old_ctx = NULL;
			}
		} else {
			if (config_stats || (config_valgrind && opt_valgrind))
				usize = s2u(size);
			ret = iralloc(ptr, size, 0, 0, false, false);
		}

label_oom:
		if (ret == NULL) {
			if (config_xmalloc && opt_xmalloc) {
				malloc_write("<jemalloc>: Error in realloc(): "
				    "out of memory\n");
				abort();
			}
			set_errno(ENOMEM);
		}
	} else {
		/* realloc(NULL, size) is equivalent to malloc(size). */
		if (config_prof && opt_prof)
			old_ctx = NULL;
		if (malloc_init()) {
			if (config_prof && opt_prof)
				cnt = NULL;
			ret = NULL;
		} else {
			if (config_prof && opt_prof) {
				usize = s2u(size);
				PROF_ALLOC_PREP(1, usize, cnt);
				if (cnt == NULL)
					ret = NULL;
				else {
					if (prof_promote && (uintptr_t)cnt !=
					    (uintptr_t)1U && usize <=
					    SMALL_MAXCLASS) {
						ret = imalloc(SMALL_MAXCLASS+1);
						if (ret != NULL) {
							arena_prof_promoted(ret,
							    usize);
						}
					} else
						ret = imalloc(size);
				}
			} else {
				if (config_stats || (config_valgrind &&
				    opt_valgrind))
					usize = s2u(size);
				ret = imalloc(size);
			}
		}

		if (ret == NULL) {
			if (config_xmalloc && opt_xmalloc) {
				malloc_write("<jemalloc>: Error in realloc(): "
				    "out of memory\n");
				abort();
			}
			set_errno(ENOMEM);
		}
	}

label_return:
	if (config_prof && opt_prof)
		prof_realloc(ret, usize, cnt, old_size, old_ctx);
	if (config_stats && ret != NULL) {
		thread_allocated_t *ta;
		assert(usize == isalloc(ret, config_prof));
		ta = thread_allocated_tsd_get();
		ta->allocated += usize;
		ta->deallocated += old_size;
	}
	UTRACE(ptr, size, ret);
	JEMALLOC_VALGRIND_REALLOC(ret, usize, ptr, old_size, old_rzsize, false);
	return (ret);
}

void
je_free(void *ptr)
{

	UTRACE(ptr, 0, 0);
	if (ptr != NULL) {
		size_t usize;
		size_t rzsize JEMALLOC_CC_SILENCE_INIT(0);

		assert(malloc_initialized || IS_INITIALIZER);

		if (config_prof && opt_prof) {
			usize = isalloc(ptr, config_prof);
			prof_free(ptr, usize);
		} else if (config_stats || config_valgrind)
			usize = isalloc(ptr, config_prof);
		if (config_stats)
			thread_allocated_tsd_get()->deallocated += usize;
		if (config_valgrind && opt_valgrind)
			rzsize = p2rz(ptr);
		iqalloc(ptr);
		JEMALLOC_VALGRIND_FREE(ptr, rzsize);
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
void *
je_memalign(size_t alignment, size_t size)
{
	void *ret JEMALLOC_CC_SILENCE_INIT(NULL);
	imemalign(&ret, alignment, size, 1);
	JEMALLOC_VALGRIND_MALLOC(ret != NULL, ret, size, false);
	return (ret);
}
#endif

#ifdef JEMALLOC_OVERRIDE_VALLOC
void *
je_valloc(size_t size)
{
	void *ret JEMALLOC_CC_SILENCE_INIT(NULL);
	imemalign(&ret, PAGE, size, 1);
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

#if ((is_malloc(je_malloc) == 1) && defined(__GLIBC__) && !defined(__UCLIBC__))
/*
 * glibc provides the RTLD_DEEPBIND flag for dlopen which can make it possible
 * to inconsistently reference libc's malloc(3)-compatible functions
 * (https://bugzilla.mozilla.org/show_bug.cgi?id=493541).
 *
 * These definitions interpose hooks in glibc.  The functions are actually
 * passed an extra argument for the caller return address, which will be
 * ignored.
 */
JEMALLOC_EXPORT void (* const __free_hook)(void *ptr) = je_free;
JEMALLOC_EXPORT void *(* const __malloc_hook)(size_t size) = je_malloc;
JEMALLOC_EXPORT void *(* const __realloc_hook)(void *ptr, size_t size) =
    je_realloc;
JEMALLOC_EXPORT void *(* const __memalign_hook)(size_t alignment, size_t size) =
    je_memalign;
#endif

/*
 * End non-standard override functions.
 */
/******************************************************************************/
/*
 * Begin non-standard functions.
 */

size_t
je_malloc_usable_size(const void *ptr)
{
	size_t ret;

	assert(malloc_initialized || IS_INITIALIZER);

	if (config_ivsalloc)
		ret = ivsalloc(ptr, config_prof);
	else
		ret = (ptr != NULL) ? isalloc(ptr, config_prof) : 0;

	return (ret);
}

void
je_malloc_stats_print(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *opts)
{

	stats_print(write_cb, cbopaque, opts);
}

int
je_mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen)
{

	if (malloc_init())
		return (EAGAIN);

	return (ctl_byname(name, oldp, oldlenp, newp, newlen));
}

int
je_mallctlnametomib(const char *name, size_t *mibp, size_t *miblenp)
{

	if (malloc_init())
		return (EAGAIN);

	return (ctl_nametomib(name, mibp, miblenp));
}

int
je_mallctlbymib(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
  void *newp, size_t newlen)
{

	if (malloc_init())
		return (EAGAIN);

	return (ctl_bymib(mib, miblen, oldp, oldlenp, newp, newlen));
}

/*
 * End non-standard functions.
 */
/******************************************************************************/
/*
 * Begin experimental functions.
 */
#ifdef JEMALLOC_EXPERIMENTAL

JEMALLOC_INLINE void *
iallocm(size_t usize, size_t alignment, bool zero)
{

	assert(usize == ((alignment == 0) ? s2u(usize) : sa2u(usize,
	    alignment)));

	if (alignment != 0)
		return (ipalloc(usize, alignment, zero));
	else if (zero)
		return (icalloc(usize));
	else
		return (imalloc(usize));
}

int
je_allocm(void **ptr, size_t *rsize, size_t size, int flags)
{
	void *p;
	size_t usize;
	size_t alignment = (ZU(1) << (flags & ALLOCM_LG_ALIGN_MASK)
	    & (SIZE_T_MAX-1));
	bool zero = flags & ALLOCM_ZERO;

	assert(ptr != NULL);
	assert(size != 0);

	if (malloc_init())
		goto label_oom;

	usize = (alignment == 0) ? s2u(size) : sa2u(size, alignment);
	if (usize == 0)
		goto label_oom;

	if (config_prof && opt_prof) {
		prof_thr_cnt_t *cnt;

		PROF_ALLOC_PREP(1, usize, cnt);
		if (cnt == NULL)
			goto label_oom;
		if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U && usize <=
		    SMALL_MAXCLASS) {
			size_t usize_promoted = (alignment == 0) ?
			    s2u(SMALL_MAXCLASS+1) : sa2u(SMALL_MAXCLASS+1,
			    alignment);
			assert(usize_promoted != 0);
			p = iallocm(usize_promoted, alignment, zero);
			if (p == NULL)
				goto label_oom;
			arena_prof_promoted(p, usize);
		} else {
			p = iallocm(usize, alignment, zero);
			if (p == NULL)
				goto label_oom;
		}
		prof_malloc(p, usize, cnt);
	} else {
		p = iallocm(usize, alignment, zero);
		if (p == NULL)
			goto label_oom;
	}
	if (rsize != NULL)
		*rsize = usize;

	*ptr = p;
	if (config_stats) {
		assert(usize == isalloc(p, config_prof));
		thread_allocated_tsd_get()->allocated += usize;
	}
	UTRACE(0, size, p);
	JEMALLOC_VALGRIND_MALLOC(true, p, usize, zero);
	return (ALLOCM_SUCCESS);
label_oom:
	if (config_xmalloc && opt_xmalloc) {
		malloc_write("<jemalloc>: Error in allocm(): "
		    "out of memory\n");
		abort();
	}
	*ptr = NULL;
	UTRACE(0, size, 0);
	return (ALLOCM_ERR_OOM);
}

int
je_rallocm(void **ptr, size_t *rsize, size_t size, size_t extra, int flags)
{
	void *p, *q;
	size_t usize;
	size_t old_size;
	size_t old_rzsize JEMALLOC_CC_SILENCE_INIT(0);
	size_t alignment = (ZU(1) << (flags & ALLOCM_LG_ALIGN_MASK)
	    & (SIZE_T_MAX-1));
	bool zero = flags & ALLOCM_ZERO;
	bool no_move = flags & ALLOCM_NO_MOVE;

	assert(ptr != NULL);
	assert(*ptr != NULL);
	assert(size != 0);
	assert(SIZE_T_MAX - size >= extra);
	assert(malloc_initialized || IS_INITIALIZER);

	p = *ptr;
	if (config_prof && opt_prof) {
		prof_thr_cnt_t *cnt;

		/*
		 * usize isn't knowable before iralloc() returns when extra is
		 * non-zero.  Therefore, compute its maximum possible value and
		 * use that in PROF_ALLOC_PREP() to decide whether to capture a
		 * backtrace.  prof_realloc() will use the actual usize to
		 * decide whether to sample.
		 */
		size_t max_usize = (alignment == 0) ? s2u(size+extra) :
		    sa2u(size+extra, alignment);
		prof_ctx_t *old_ctx = prof_ctx_get(p);
		old_size = isalloc(p, true);
		if (config_valgrind && opt_valgrind)
			old_rzsize = p2rz(p);
		PROF_ALLOC_PREP(1, max_usize, cnt);
		if (cnt == NULL)
			goto label_oom;
		/*
		 * Use minimum usize to determine whether promotion may happen.
		 */
		if (prof_promote && (uintptr_t)cnt != (uintptr_t)1U
		    && ((alignment == 0) ? s2u(size) : sa2u(size, alignment))
		    <= SMALL_MAXCLASS) {
			q = iralloc(p, SMALL_MAXCLASS+1, (SMALL_MAXCLASS+1 >=
			    size+extra) ? 0 : size+extra - (SMALL_MAXCLASS+1),
			    alignment, zero, no_move);
			if (q == NULL)
				goto label_err;
			if (max_usize < PAGE) {
				usize = max_usize;
				arena_prof_promoted(q, usize);
			} else
				usize = isalloc(q, config_prof);
		} else {
			q = iralloc(p, size, extra, alignment, zero, no_move);
			if (q == NULL)
				goto label_err;
			usize = isalloc(q, config_prof);
		}
		prof_realloc(q, usize, cnt, old_size, old_ctx);
		if (rsize != NULL)
			*rsize = usize;
	} else {
		if (config_stats) {
			old_size = isalloc(p, false);
			if (config_valgrind && opt_valgrind)
				old_rzsize = u2rz(old_size);
		} else if (config_valgrind && opt_valgrind) {
			old_size = isalloc(p, false);
			old_rzsize = u2rz(old_size);
		}
		q = iralloc(p, size, extra, alignment, zero, no_move);
		if (q == NULL)
			goto label_err;
		if (config_stats)
			usize = isalloc(q, config_prof);
		if (rsize != NULL) {
			if (config_stats == false)
				usize = isalloc(q, config_prof);
			*rsize = usize;
		}
	}

	*ptr = q;
	if (config_stats) {
		thread_allocated_t *ta;
		ta = thread_allocated_tsd_get();
		ta->allocated += usize;
		ta->deallocated += old_size;
	}
	UTRACE(p, size, q);
	JEMALLOC_VALGRIND_REALLOC(q, usize, p, old_size, old_rzsize, zero);
	return (ALLOCM_SUCCESS);
label_err:
	if (no_move) {
		UTRACE(p, size, q);
		return (ALLOCM_ERR_NOT_MOVED);
	}
label_oom:
	if (config_xmalloc && opt_xmalloc) {
		malloc_write("<jemalloc>: Error in rallocm(): "
		    "out of memory\n");
		abort();
	}
	UTRACE(p, size, 0);
	return (ALLOCM_ERR_OOM);
}

int
je_sallocm(const void *ptr, size_t *rsize, int flags)
{
	size_t sz;

	assert(malloc_initialized || IS_INITIALIZER);

	if (config_ivsalloc)
		sz = ivsalloc(ptr, config_prof);
	else {
		assert(ptr != NULL);
		sz = isalloc(ptr, config_prof);
	}
	assert(rsize != NULL);
	*rsize = sz;

	return (ALLOCM_SUCCESS);
}

int
je_dallocm(void *ptr, int flags)
{
	size_t usize;
	size_t rzsize JEMALLOC_CC_SILENCE_INIT(0);

	assert(ptr != NULL);
	assert(malloc_initialized || IS_INITIALIZER);

	UTRACE(ptr, 0, 0);
	if (config_stats || config_valgrind)
		usize = isalloc(ptr, config_prof);
	if (config_prof && opt_prof) {
		if (config_stats == false && config_valgrind == false)
			usize = isalloc(ptr, config_prof);
		prof_free(ptr, usize);
	}
	if (config_stats)
		thread_allocated_tsd_get()->deallocated += usize;
	if (config_valgrind && opt_valgrind)
		rzsize = p2rz(ptr);
	iqalloc(ptr);
	JEMALLOC_VALGRIND_FREE(ptr, rzsize);

	return (ALLOCM_SUCCESS);
}

int
je_nallocm(size_t *rsize, size_t size, int flags)
{
	size_t usize;
	size_t alignment = (ZU(1) << (flags & ALLOCM_LG_ALIGN_MASK)
	    & (SIZE_T_MAX-1));

	assert(size != 0);

	if (malloc_init())
		return (ALLOCM_ERR_OOM);

	usize = (alignment == 0) ? s2u(size) : sa2u(size, alignment);
	if (usize == 0)
		return (ALLOCM_ERR_OOM);

	if (rsize != NULL)
		*rsize = usize;
	return (ALLOCM_SUCCESS);
}

#endif
/*
 * End experimental functions.
 */
/******************************************************************************/
/*
 * The following functions are used by threading libraries for protection of
 * malloc during fork().
 */

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
	if (malloc_initialized == false)
		return;
#endif
	assert(malloc_initialized);

	/* Acquire all mutexes in a safe order. */
	malloc_mutex_prefork(&arenas_lock);
	for (i = 0; i < narenas; i++) {
		if (arenas[i] != NULL)
			arena_prefork(arenas[i]);
	}
	base_prefork();
	huge_prefork();
	chunk_dss_prefork();
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
	if (malloc_initialized == false)
		return;
#endif
	assert(malloc_initialized);

	/* Release all mutexes, now that fork() has completed. */
	chunk_dss_postfork_parent();
	huge_postfork_parent();
	base_postfork_parent();
	for (i = 0; i < narenas; i++) {
		if (arenas[i] != NULL)
			arena_postfork_parent(arenas[i]);
	}
	malloc_mutex_postfork_parent(&arenas_lock);
}

void
jemalloc_postfork_child(void)
{
	unsigned i;

	assert(malloc_initialized);

	/* Release all mutexes, now that fork() has completed. */
	chunk_dss_postfork_child();
	huge_postfork_child();
	base_postfork_child();
	for (i = 0; i < narenas; i++) {
		if (arenas[i] != NULL)
			arena_postfork_child(arenas[i]);
	}
	malloc_mutex_postfork_child(&arenas_lock);
}

/******************************************************************************/
/*
 * The following functions are used for TLS allocation/deallocation in static
 * binaries on FreeBSD.  The primary difference between these and i[mcd]alloc()
 * is that these avoid accessing TLS variables.
 */

static void *
a0alloc(size_t size, bool zero)
{

	if (malloc_init())
		return (NULL);

	if (size == 0)
		size = 1;

	if (size <= arena_maxclass)
		return (arena_malloc(arenas[0], size, zero, false));
	else
		return (huge_malloc(size, zero));
}

void *
a0malloc(size_t size)
{

	return (a0alloc(size, false));
}

void *
a0calloc(size_t num, size_t size)
{

	return (a0alloc(num * size, true));
}

void
a0free(void *ptr)
{
	arena_chunk_t *chunk;

	if (ptr == NULL)
		return;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	if (chunk != ptr)
		arena_dalloc(chunk->arena, chunk, ptr, false);
	else
		huge_dalloc(ptr, true);
}

/******************************************************************************/
