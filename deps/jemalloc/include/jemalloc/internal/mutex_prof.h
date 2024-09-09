#ifndef JEMALLOC_INTERNAL_MUTEX_PROF_H
#define JEMALLOC_INTERNAL_MUTEX_PROF_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/nstime.h"
#include "jemalloc/internal/tsd_types.h"

#define MUTEX_PROF_GLOBAL_MUTEXES					\
    OP(background_thread)						\
    OP(max_per_bg_thd)							\
    OP(ctl)								\
    OP(prof)								\
    OP(prof_thds_data)							\
    OP(prof_dump)							\
    OP(prof_recent_alloc)						\
    OP(prof_recent_dump)						\
    OP(prof_stats)

typedef enum {
#define OP(mtx) global_prof_mutex_##mtx,
	MUTEX_PROF_GLOBAL_MUTEXES
#undef OP
	mutex_prof_num_global_mutexes
} mutex_prof_global_ind_t;

#define MUTEX_PROF_ARENA_MUTEXES					\
    OP(large)								\
    OP(extent_avail)							\
    OP(extents_dirty)							\
    OP(extents_muzzy)							\
    OP(extents_retained)						\
    OP(decay_dirty)							\
    OP(decay_muzzy)							\
    OP(base)								\
    OP(tcache_list)							\
    OP(hpa_shard)							\
    OP(hpa_shard_grow)							\
    OP(hpa_sec)

typedef enum {
#define OP(mtx) arena_prof_mutex_##mtx,
	MUTEX_PROF_ARENA_MUTEXES
#undef OP
	mutex_prof_num_arena_mutexes
} mutex_prof_arena_ind_t;

/*
 * The forth parameter is a boolean value that is true for derived rate counters
 * and false for real ones.
 */
#define MUTEX_PROF_UINT64_COUNTERS					\
    OP(num_ops, uint64_t, "n_lock_ops", false, num_ops)					\
    OP(num_ops_ps, uint64_t, "(#/sec)", true, num_ops)				\
    OP(num_wait, uint64_t, "n_waiting", false, num_wait)				\
    OP(num_wait_ps, uint64_t, "(#/sec)", true, num_wait)				\
    OP(num_spin_acq, uint64_t, "n_spin_acq", false, num_spin_acq)			\
    OP(num_spin_acq_ps, uint64_t, "(#/sec)", true, num_spin_acq)			\
    OP(num_owner_switch, uint64_t, "n_owner_switch", false, num_owner_switch)		\
    OP(num_owner_switch_ps, uint64_t, "(#/sec)", true, num_owner_switch)	\
    OP(total_wait_time, uint64_t, "total_wait_ns", false, total_wait_time)		\
    OP(total_wait_time_ps, uint64_t, "(#/sec)", true, total_wait_time)		\
    OP(max_wait_time, uint64_t, "max_wait_ns", false, max_wait_time)

#define MUTEX_PROF_UINT32_COUNTERS					\
    OP(max_num_thds, uint32_t, "max_n_thds", false, max_num_thds)

#define MUTEX_PROF_COUNTERS						\
		MUTEX_PROF_UINT64_COUNTERS				\
		MUTEX_PROF_UINT32_COUNTERS

#define OP(counter, type, human, derived, base_counter) mutex_counter_##counter,

#define COUNTER_ENUM(counter_list, t)					\
		typedef enum {						\
			counter_list					\
			mutex_prof_num_##t##_counters			\
		} mutex_prof_##t##_counter_ind_t;

COUNTER_ENUM(MUTEX_PROF_UINT64_COUNTERS, uint64_t)
COUNTER_ENUM(MUTEX_PROF_UINT32_COUNTERS, uint32_t)

#undef COUNTER_ENUM
#undef OP

typedef struct {
	/*
	 * Counters touched on the slow path, i.e. when there is lock
	 * contention.  We update them once we have the lock.
	 */
	/* Total time (in nano seconds) spent waiting on this mutex. */
	nstime_t		tot_wait_time;
	/* Max time (in nano seconds) spent on a single lock operation. */
	nstime_t		max_wait_time;
	/* # of times have to wait for this mutex (after spinning). */
	uint64_t		n_wait_times;
	/* # of times acquired the mutex through local spinning. */
	uint64_t		n_spin_acquired;
	/* Max # of threads waiting for the mutex at the same time. */
	uint32_t		max_n_thds;
	/* Current # of threads waiting on the lock.  Atomic synced. */
	atomic_u32_t		n_waiting_thds;

	/*
	 * Data touched on the fast path.  These are modified right after we
	 * grab the lock, so it's placed closest to the end (i.e. right before
	 * the lock) so that we have a higher chance of them being on the same
	 * cacheline.
	 */
	/* # of times the mutex holder is different than the previous one. */
	uint64_t		n_owner_switches;
	/* Previous mutex holder, to facilitate n_owner_switches. */
	tsdn_t			*prev_owner;
	/* # of lock() operations in total. */
	uint64_t		n_lock_ops;
} mutex_prof_data_t;

#endif /* JEMALLOC_INTERNAL_MUTEX_PROF_H */
