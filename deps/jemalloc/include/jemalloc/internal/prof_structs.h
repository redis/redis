#ifndef JEMALLOC_INTERNAL_PROF_STRUCTS_H
#define JEMALLOC_INTERNAL_PROF_STRUCTS_H

#include "jemalloc/internal/ckh.h"
#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/prng.h"
#include "jemalloc/internal/rb.h"

struct prof_bt_s {
	/* Backtrace, stored as len program counters. */
	void		**vec;
	unsigned	len;
};

#ifdef JEMALLOC_PROF_LIBGCC
/* Data structure passed to libgcc _Unwind_Backtrace() callback functions. */
typedef struct {
	void 		**vec;
	unsigned	*len;
	unsigned	max;
} prof_unwind_data_t;
#endif

struct prof_cnt_s {
	/* Profiling counters. */
	uint64_t	curobjs;
	uint64_t	curobjs_shifted_unbiased;
	uint64_t	curbytes;
	uint64_t	curbytes_unbiased;
	uint64_t	accumobjs;
	uint64_t	accumobjs_shifted_unbiased;
	uint64_t	accumbytes;
	uint64_t	accumbytes_unbiased;
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

	/*
	 * Reference count of how many times this tctx object is referenced in
	 * recent allocation / deallocation records, protected by tdata->lock.
	 */
	uint64_t		recent_count;

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

struct prof_info_s {
	/* Time when the allocation was made. */
	nstime_t		alloc_time;
	/* Points to the prof_tctx_t corresponding to the allocation. */
	prof_tctx_t		*alloc_tctx;
	/* Allocation request size. */
	size_t			alloc_size;
};

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

struct prof_recent_s {
	nstime_t alloc_time;
	nstime_t dalloc_time;

	ql_elm(prof_recent_t) link;
	size_t size;
	size_t usize;
	atomic_p_t alloc_edata; /* NULL means allocation has been freed. */
	prof_tctx_t *alloc_tctx;
	prof_tctx_t *dalloc_tctx;
};

#endif /* JEMALLOC_INTERNAL_PROF_STRUCTS_H */
