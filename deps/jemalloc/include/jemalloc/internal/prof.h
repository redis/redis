/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct prof_bt_s prof_bt_t;
typedef struct prof_cnt_s prof_cnt_t;
typedef struct prof_tctx_s prof_tctx_t;
typedef struct prof_gctx_s prof_gctx_t;
typedef struct prof_tdata_s prof_tdata_t;

/* Option defaults. */
#ifdef JEMALLOC_PROF
#  define PROF_PREFIX_DEFAULT		"jeprof"
#else
#  define PROF_PREFIX_DEFAULT		""
#endif
#define	LG_PROF_SAMPLE_DEFAULT		19
#define	LG_PROF_INTERVAL_DEFAULT	-1

/*
 * Hard limit on stack backtrace depth.  The version of prof_backtrace() that
 * is based on __builtin_return_address() necessarily has a hard-coded number
 * of backtrace frame handlers, and should be kept in sync with this setting.
 */
#define	PROF_BT_MAX			128

/* Initial hash table size. */
#define	PROF_CKH_MINITEMS		64

/* Size of memory buffer to use when writing dump files. */
#define	PROF_DUMP_BUFSIZE		65536

/* Size of stack-allocated buffer used by prof_printf(). */
#define	PROF_PRINTF_BUFSIZE		128

/*
 * Number of mutexes shared among all gctx's.  No space is allocated for these
 * unless profiling is enabled, so it's okay to over-provision.
 */
#define	PROF_NCTX_LOCKS			1024

/*
 * Number of mutexes shared among all tdata's.  No space is allocated for these
 * unless profiling is enabled, so it's okay to over-provision.
 */
#define	PROF_NTDATA_LOCKS		256

/*
 * prof_tdata pointers close to NULL are used to encode state information that
 * is used for cleaning up during thread shutdown.
 */
#define	PROF_TDATA_STATE_REINCARNATED	((prof_tdata_t *)(uintptr_t)1)
#define	PROF_TDATA_STATE_PURGATORY	((prof_tdata_t *)(uintptr_t)2)
#define	PROF_TDATA_STATE_MAX		PROF_TDATA_STATE_PURGATORY

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct prof_bt_s {
	/* Backtrace, stored as len program counters. */
	void		**vec;
	unsigned	len;
};

#ifdef JEMALLOC_PROF_LIBGCC
/* Data structure passed to libgcc _Unwind_Backtrace() callback functions. */
typedef struct {
	prof_bt_t	*bt;
	unsigned	max;
} prof_unwind_data_t;
#endif

struct prof_cnt_s {
	/* Profiling counters. */
	uint64_t	curobjs;
	uint64_t	curbytes;
	uint64_t	accumobjs;
	uint64_t	accumbytes;
};

typedef enum {
	prof_tctx_state_initializing,
	prof_tctx_state_nominal,
	prof_tctx_state_dumping,
	prof_tctx_state_purgatory /* Dumper must finish destroying. */
} prof_tctx_state_t;

struct prof_tctx_s {
	/* Thread data for thread that performed the allocation. */
	prof_tdata_t		*tdata;

	/*
	 * Copy of tdata->thr_{uid,discrim}, necessary because tdata may be
	 * defunct during teardown.
	 */
	uint64_t		thr_uid;
	uint64_t		thr_discrim;

	/* Profiling counters, protected by tdata->lock. */
	prof_cnt_t		cnts;

	/* Associated global context. */
	prof_gctx_t		*gctx;

	/*
	 * UID that distinguishes multiple tctx's created by the same thread,
	 * but coexisting in gctx->tctxs.  There are two ways that such
	 * coexistence can occur:
	 * - A dumper thread can cause a tctx to be retained in the purgatory
	 *   state.
	 * - Although a single "producer" thread must create all tctx's which
	 *   share the same thr_uid, multiple "consumers" can each concurrently
	 *   execute portions of prof_tctx_destroy().  prof_tctx_destroy() only
	 *   gets called once each time cnts.cur{objs,bytes} drop to 0, but this
	 *   threshold can be hit again before the first consumer finishes
	 *   executing prof_tctx_destroy().
	 */
	uint64_t		tctx_uid;

	/* Linkage into gctx's tctxs. */
	rb_node(prof_tctx_t)	tctx_link;

	/*
	 * True during prof_alloc_prep()..prof_malloc_sample_object(), prevents
	 * sample vs destroy race.
	 */
	bool			prepared;

	/* Current dump-related state, protected by gctx->lock. */
	prof_tctx_state_t	state;

	/*
	 * Copy of cnts snapshotted during early dump phase, protected by
	 * dump_mtx.
	 */
	prof_cnt_t		dump_cnts;
};
typedef rb_tree(prof_tctx_t) prof_tctx_tree_t;

struct prof_gctx_s {
	/* Protects nlimbo, cnt_summed, and tctxs. */
	malloc_mutex_t		*lock;

	/*
	 * Number of threads that currently cause this gctx to be in a state of
	 * limbo due to one of:
	 *   - Initializing this gctx.
	 *   - Initializing per thread counters associated with this gctx.
	 *   - Preparing to destroy this gctx.
	 *   - Dumping a heap profile that includes this gctx.
	 * nlimbo must be 1 (single destroyer) in order to safely destroy the
	 * gctx.
	 */
	unsigned		nlimbo;

	/*
	 * Tree of profile counters, one for each thread that has allocated in
	 * this context.
	 */
	prof_tctx_tree_t	tctxs;

	/* Linkage for tree of contexts to be dumped. */
	rb_node(prof_gctx_t)	dump_link;

	/* Temporary storage for summation during dump. */
	prof_cnt_t		cnt_summed;

	/* Associated backtrace. */
	prof_bt_t		bt;

	/* Backtrace vector, variable size, referred to by bt. */
	void			*vec[1];
};
typedef rb_tree(prof_gctx_t) prof_gctx_tree_t;

struct prof_tdata_s {
	malloc_mutex_t		*lock;

	/* Monotonically increasing unique thread identifier. */
	uint64_t		thr_uid;

	/*
	 * Monotonically increasing discriminator among tdata structures
	 * associated with the same thr_uid.
	 */
	uint64_t		thr_discrim;

	/* Included in heap profile dumps if non-NULL. */
	char			*thread_name;

	bool			attached;
	bool			expired;

	rb_node(prof_tdata_t)	tdata_link;

	/*
	 * Counter used to initialize prof_tctx_t's tctx_uid.  No locking is
	 * necessary when incrementing this field, because only one thread ever
	 * does so.
	 */
	uint64_t		tctx_uid_next;

	/*
	 * Hash of (prof_bt_t *)-->(prof_tctx_t *).  Each thread tracks
	 * backtraces for which it has non-zero allocation/deallocation counters
	 * associated with thread-specific prof_tctx_t objects.  Other threads
	 * may write to prof_tctx_t contents when freeing associated objects.
	 */
	ckh_t			bt2tctx;

	/* Sampling state. */
	uint64_t		prng_state;
	uint64_t		bytes_until_sample;

	/* State used to avoid dumping while operating on prof internals. */
	bool			enq;
	bool			enq_idump;
	bool			enq_gdump;

	/*
	 * Set to true during an early dump phase for tdata's which are
	 * currently being dumped.  New threads' tdata's have this initialized
	 * to false so that they aren't accidentally included in later dump
	 * phases.
	 */
	bool			dumping;

	/*
	 * True if profiling is active for this tdata's thread
	 * (thread.prof.active mallctl).
	 */
	bool			active;

	/* Temporary storage for summation during dump. */
	prof_cnt_t		cnt_summed;

	/* Backtrace vector, used for calls to prof_backtrace(). */
	void			*vec[PROF_BT_MAX];
};
typedef rb_tree(prof_tdata_t) prof_tdata_tree_t;

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern bool	opt_prof;
extern bool	opt_prof_active;
extern bool	opt_prof_thread_active_init;
extern size_t	opt_lg_prof_sample;   /* Mean bytes between samples. */
extern ssize_t	opt_lg_prof_interval; /* lg(prof_interval). */
extern bool	opt_prof_gdump;       /* High-water memory dumping. */
extern bool	opt_prof_final;       /* Final profile dumping. */
extern bool	opt_prof_leak;        /* Dump leak summary at exit. */
extern bool	opt_prof_accum;       /* Report cumulative bytes. */
extern char	opt_prof_prefix[
    /* Minimize memory bloat for non-prof builds. */
#ifdef JEMALLOC_PROF
    PATH_MAX +
#endif
    1];

/* Accessed via prof_active_[gs]et{_unlocked,}(). */
extern bool	prof_active;

/* Accessed via prof_gdump_[gs]et{_unlocked,}(). */
extern bool	prof_gdump_val;

/*
 * Profile dump interval, measured in bytes allocated.  Each arena triggers a
 * profile dump when it reaches this threshold.  The effect is that the
 * interval between profile dumps averages prof_interval, though the actual
 * interval between dumps will tend to be sporadic, and the interval will be a
 * maximum of approximately (prof_interval * narenas).
 */
extern uint64_t	prof_interval;

/*
 * Initialized as opt_lg_prof_sample, and potentially modified during profiling
 * resets.
 */
extern size_t	lg_prof_sample;

void	prof_alloc_rollback(tsd_t *tsd, prof_tctx_t *tctx, bool updated);
void	prof_malloc_sample_object(const void *ptr, size_t usize,
    prof_tctx_t *tctx);
void	prof_free_sampled_object(tsd_t *tsd, size_t usize, prof_tctx_t *tctx);
void	bt_init(prof_bt_t *bt, void **vec);
void	prof_backtrace(prof_bt_t *bt);
prof_tctx_t	*prof_lookup(tsd_t *tsd, prof_bt_t *bt);
#ifdef JEMALLOC_JET
size_t	prof_tdata_count(void);
size_t	prof_bt_count(void);
const prof_cnt_t *prof_cnt_all(void);
typedef int (prof_dump_open_t)(bool, const char *);
extern prof_dump_open_t *prof_dump_open;
typedef bool (prof_dump_header_t)(bool, const prof_cnt_t *);
extern prof_dump_header_t *prof_dump_header;
#endif
void	prof_idump(void);
bool	prof_mdump(const char *filename);
void	prof_gdump(void);
prof_tdata_t	*prof_tdata_init(tsd_t *tsd);
prof_tdata_t	*prof_tdata_reinit(tsd_t *tsd, prof_tdata_t *tdata);
void	prof_reset(tsd_t *tsd, size_t lg_sample);
void	prof_tdata_cleanup(tsd_t *tsd);
const char	*prof_thread_name_get(void);
bool	prof_active_get(void);
bool	prof_active_set(bool active);
int	prof_thread_name_set(tsd_t *tsd, const char *thread_name);
bool	prof_thread_active_get(void);
bool	prof_thread_active_set(bool active);
bool	prof_thread_active_init_get(void);
bool	prof_thread_active_init_set(bool active_init);
bool	prof_gdump_get(void);
bool	prof_gdump_set(bool active);
void	prof_boot0(void);
void	prof_boot1(void);
bool	prof_boot2(void);
void	prof_prefork(void);
void	prof_postfork_parent(void);
void	prof_postfork_child(void);
void	prof_sample_threshold_update(prof_tdata_t *tdata);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#ifndef JEMALLOC_ENABLE_INLINE
bool	prof_active_get_unlocked(void);
bool	prof_gdump_get_unlocked(void);
prof_tdata_t	*prof_tdata_get(tsd_t *tsd, bool create);
bool	prof_sample_accum_update(tsd_t *tsd, size_t usize, bool commit,
    prof_tdata_t **tdata_out);
prof_tctx_t	*prof_alloc_prep(tsd_t *tsd, size_t usize, bool prof_active,
    bool update);
prof_tctx_t	*prof_tctx_get(const void *ptr);
void	prof_tctx_set(const void *ptr, size_t usize, prof_tctx_t *tctx);
void	prof_tctx_reset(const void *ptr, size_t usize, const void *old_ptr,
    prof_tctx_t *tctx);
void	prof_malloc_sample_object(const void *ptr, size_t usize,
    prof_tctx_t *tctx);
void	prof_malloc(const void *ptr, size_t usize, prof_tctx_t *tctx);
void	prof_realloc(tsd_t *tsd, const void *ptr, size_t usize,
    prof_tctx_t *tctx, bool prof_active, bool updated, const void *old_ptr,
    size_t old_usize, prof_tctx_t *old_tctx);
void	prof_free(tsd_t *tsd, const void *ptr, size_t usize);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_PROF_C_))
JEMALLOC_ALWAYS_INLINE bool
prof_active_get_unlocked(void)
{

	/*
	 * Even if opt_prof is true, sampling can be temporarily disabled by
	 * setting prof_active to false.  No locking is used when reading
	 * prof_active in the fast path, so there are no guarantees regarding
	 * how long it will take for all threads to notice state changes.
	 */
	return (prof_active);
}

JEMALLOC_ALWAYS_INLINE bool
prof_gdump_get_unlocked(void)
{

	/*
	 * No locking is used when reading prof_gdump_val in the fast path, so
	 * there are no guarantees regarding how long it will take for all
	 * threads to notice state changes.
	 */
	return (prof_gdump_val);
}

JEMALLOC_ALWAYS_INLINE prof_tdata_t *
prof_tdata_get(tsd_t *tsd, bool create)
{
	prof_tdata_t *tdata;

	cassert(config_prof);

	tdata = tsd_prof_tdata_get(tsd);
	if (create) {
		if (unlikely(tdata == NULL)) {
			if (tsd_nominal(tsd)) {
				tdata = prof_tdata_init(tsd);
				tsd_prof_tdata_set(tsd, tdata);
			}
		} else if (unlikely(tdata->expired)) {
			tdata = prof_tdata_reinit(tsd, tdata);
			tsd_prof_tdata_set(tsd, tdata);
		}
		assert(tdata == NULL || tdata->attached);
	}

	return (tdata);
}

JEMALLOC_ALWAYS_INLINE prof_tctx_t *
prof_tctx_get(const void *ptr)
{

	cassert(config_prof);
	assert(ptr != NULL);

	return (arena_prof_tctx_get(ptr));
}

JEMALLOC_ALWAYS_INLINE void
prof_tctx_set(const void *ptr, size_t usize, prof_tctx_t *tctx)
{

	cassert(config_prof);
	assert(ptr != NULL);

	arena_prof_tctx_set(ptr, usize, tctx);
}

JEMALLOC_ALWAYS_INLINE void
prof_tctx_reset(const void *ptr, size_t usize, const void *old_ptr,
    prof_tctx_t *old_tctx)
{

	cassert(config_prof);
	assert(ptr != NULL);

	arena_prof_tctx_reset(ptr, usize, old_ptr, old_tctx);
}

JEMALLOC_ALWAYS_INLINE bool
prof_sample_accum_update(tsd_t *tsd, size_t usize, bool update,
    prof_tdata_t **tdata_out)
{
	prof_tdata_t *tdata;

	cassert(config_prof);

	tdata = prof_tdata_get(tsd, true);
	if ((uintptr_t)tdata <= (uintptr_t)PROF_TDATA_STATE_MAX)
		tdata = NULL;

	if (tdata_out != NULL)
		*tdata_out = tdata;

	if (tdata == NULL)
		return (true);

	if (tdata->bytes_until_sample >= usize) {
		if (update)
			tdata->bytes_until_sample -= usize;
		return (true);
	} else {
		/* Compute new sample threshold. */
		if (update)
			prof_sample_threshold_update(tdata);
		return (!tdata->active);
	}
}

JEMALLOC_ALWAYS_INLINE prof_tctx_t *
prof_alloc_prep(tsd_t *tsd, size_t usize, bool prof_active, bool update)
{
	prof_tctx_t *ret;
	prof_tdata_t *tdata;
	prof_bt_t bt;

	assert(usize == s2u(usize));

	if (!prof_active || likely(prof_sample_accum_update(tsd, usize, update,
	    &tdata)))
		ret = (prof_tctx_t *)(uintptr_t)1U;
	else {
		bt_init(&bt, tdata->vec);
		prof_backtrace(&bt);
		ret = prof_lookup(tsd, &bt);
	}

	return (ret);
}

JEMALLOC_ALWAYS_INLINE void
prof_malloc(const void *ptr, size_t usize, prof_tctx_t *tctx)
{

	cassert(config_prof);
	assert(ptr != NULL);
	assert(usize == isalloc(ptr, true));

	if (unlikely((uintptr_t)tctx > (uintptr_t)1U))
		prof_malloc_sample_object(ptr, usize, tctx);
	else
		prof_tctx_set(ptr, usize, (prof_tctx_t *)(uintptr_t)1U);
}

JEMALLOC_ALWAYS_INLINE void
prof_realloc(tsd_t *tsd, const void *ptr, size_t usize, prof_tctx_t *tctx,
    bool prof_active, bool updated, const void *old_ptr, size_t old_usize,
    prof_tctx_t *old_tctx)
{
	bool sampled, old_sampled;

	cassert(config_prof);
	assert(ptr != NULL || (uintptr_t)tctx <= (uintptr_t)1U);

	if (prof_active && !updated && ptr != NULL) {
		assert(usize == isalloc(ptr, true));
		if (prof_sample_accum_update(tsd, usize, true, NULL)) {
			/*
			 * Don't sample.  The usize passed to prof_alloc_prep()
			 * was larger than what actually got allocated, so a
			 * backtrace was captured for this allocation, even
			 * though its actual usize was insufficient to cross the
			 * sample threshold.
			 */
			tctx = (prof_tctx_t *)(uintptr_t)1U;
		}
	}

	sampled = ((uintptr_t)tctx > (uintptr_t)1U);
	old_sampled = ((uintptr_t)old_tctx > (uintptr_t)1U);

	if (unlikely(sampled))
		prof_malloc_sample_object(ptr, usize, tctx);
	else
		prof_tctx_reset(ptr, usize, old_ptr, old_tctx);

	if (unlikely(old_sampled))
		prof_free_sampled_object(tsd, old_usize, old_tctx);
}

JEMALLOC_ALWAYS_INLINE void
prof_free(tsd_t *tsd, const void *ptr, size_t usize)
{
	prof_tctx_t *tctx = prof_tctx_get(ptr);

	cassert(config_prof);
	assert(usize == isalloc(ptr, true));

	if (unlikely((uintptr_t)tctx > (uintptr_t)1U))
		prof_free_sampled_object(tsd, usize, tctx);
}
#endif

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/
