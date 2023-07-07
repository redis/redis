#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"

JEMALLOC_DIAGNOSTIC_DISABLE_SPURIOUS

/******************************************************************************/
/* Data. */

/* This option should be opt-in only. */
#define BACKGROUND_THREAD_DEFAULT false
/* Read-only after initialization. */
bool opt_background_thread = BACKGROUND_THREAD_DEFAULT;
size_t opt_max_background_threads = MAX_BACKGROUND_THREAD_LIMIT + 1;

/* Used for thread creation, termination and stats. */
malloc_mutex_t background_thread_lock;
/* Indicates global state.  Atomic because decay reads this w/o locking. */
atomic_b_t background_thread_enabled_state;
size_t n_background_threads;
size_t max_background_threads;
/* Thread info per-index. */
background_thread_info_t *background_thread_info;

/******************************************************************************/

#ifdef JEMALLOC_PTHREAD_CREATE_WRAPPER

static int (*pthread_create_fptr)(pthread_t *__restrict, const pthread_attr_t *,
    void *(*)(void *), void *__restrict);

static void
pthread_create_wrapper_init(void) {
#ifdef JEMALLOC_LAZY_LOCK
	if (!isthreaded) {
		isthreaded = true;
	}
#endif
}

int
pthread_create_wrapper(pthread_t *__restrict thread, const pthread_attr_t *attr,
    void *(*start_routine)(void *), void *__restrict arg) {
	pthread_create_wrapper_init();

	return pthread_create_fptr(thread, attr, start_routine, arg);
}
#endif /* JEMALLOC_PTHREAD_CREATE_WRAPPER */

#ifndef JEMALLOC_BACKGROUND_THREAD
#define NOT_REACHED { not_reached(); }
bool background_thread_create(tsd_t *tsd, unsigned arena_ind) NOT_REACHED
bool background_threads_enable(tsd_t *tsd) NOT_REACHED
bool background_threads_disable(tsd_t *tsd) NOT_REACHED
bool background_thread_is_started(background_thread_info_t *info) NOT_REACHED
void background_thread_wakeup_early(background_thread_info_t *info,
    nstime_t *remaining_sleep) NOT_REACHED
void background_thread_prefork0(tsdn_t *tsdn) NOT_REACHED
void background_thread_prefork1(tsdn_t *tsdn) NOT_REACHED
void background_thread_postfork_parent(tsdn_t *tsdn) NOT_REACHED
void background_thread_postfork_child(tsdn_t *tsdn) NOT_REACHED
bool background_thread_stats_read(tsdn_t *tsdn,
    background_thread_stats_t *stats) NOT_REACHED
void background_thread_ctl_init(tsdn_t *tsdn) NOT_REACHED
#undef NOT_REACHED
#else

static bool background_thread_enabled_at_fork;

static void
background_thread_info_init(tsdn_t *tsdn, background_thread_info_t *info) {
	background_thread_wakeup_time_set(tsdn, info, 0);
	info->npages_to_purge_new = 0;
	if (config_stats) {
		info->tot_n_runs = 0;
		nstime_init_zero(&info->tot_sleep_time);
	}
}

static inline bool
set_current_thread_affinity(int cpu) {
#if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
	cpu_set_t cpuset;
#else
#  ifndef __NetBSD__
	cpuset_t cpuset;
#  else
	cpuset_t *cpuset;
#  endif
#endif

#ifndef __NetBSD__
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
#else
	cpuset = cpuset_create();
#endif

#if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
	return (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0);
#else
#  ifndef __NetBSD__
	int ret = pthread_setaffinity_np(pthread_self(), sizeof(cpuset_t),
	    &cpuset);
#  else
	int ret = pthread_setaffinity_np(pthread_self(), cpuset_size(cpuset),
	    cpuset);
	cpuset_destroy(cpuset);
#  endif
	return ret != 0;
#endif
}

#define BILLION UINT64_C(1000000000)
/* Minimal sleep interval 100 ms. */
#define BACKGROUND_THREAD_MIN_INTERVAL_NS (BILLION / 10)

static void
background_thread_sleep(tsdn_t *tsdn, background_thread_info_t *info,
    uint64_t interval) {
	if (config_stats) {
		info->tot_n_runs++;
	}
	info->npages_to_purge_new = 0;

	struct timeval tv;
	/* Specific clock required by timedwait. */
	gettimeofday(&tv, NULL);
	nstime_t before_sleep;
	nstime_init2(&before_sleep, tv.tv_sec, tv.tv_usec * 1000);

	int ret;
	if (interval == BACKGROUND_THREAD_INDEFINITE_SLEEP) {
		background_thread_wakeup_time_set(tsdn, info,
		    BACKGROUND_THREAD_INDEFINITE_SLEEP);
		ret = pthread_cond_wait(&info->cond, &info->mtx.lock);
		assert(ret == 0);
	} else {
		assert(interval >= BACKGROUND_THREAD_MIN_INTERVAL_NS &&
		    interval <= BACKGROUND_THREAD_INDEFINITE_SLEEP);
		/* We need malloc clock (can be different from tv). */
		nstime_t next_wakeup;
		nstime_init_update(&next_wakeup);
		nstime_iadd(&next_wakeup, interval);
		assert(nstime_ns(&next_wakeup) <
		    BACKGROUND_THREAD_INDEFINITE_SLEEP);
		background_thread_wakeup_time_set(tsdn, info,
		    nstime_ns(&next_wakeup));

		nstime_t ts_wakeup;
		nstime_copy(&ts_wakeup, &before_sleep);
		nstime_iadd(&ts_wakeup, interval);
		struct timespec ts;
		ts.tv_sec = (size_t)nstime_sec(&ts_wakeup);
		ts.tv_nsec = (size_t)nstime_nsec(&ts_wakeup);

		assert(!background_thread_indefinite_sleep(info));
		ret = pthread_cond_timedwait(&info->cond, &info->mtx.lock, &ts);
		assert(ret == ETIMEDOUT || ret == 0);
	}
	if (config_stats) {
		gettimeofday(&tv, NULL);
		nstime_t after_sleep;
		nstime_init2(&after_sleep, tv.tv_sec, tv.tv_usec * 1000);
		if (nstime_compare(&after_sleep, &before_sleep) > 0) {
			nstime_subtract(&after_sleep, &before_sleep);
			nstime_add(&info->tot_sleep_time, &after_sleep);
		}
	}
}

static bool
background_thread_pause_check(tsdn_t *tsdn, background_thread_info_t *info) {
	if (unlikely(info->state == background_thread_paused)) {
		malloc_mutex_unlock(tsdn, &info->mtx);
		/* Wait on global lock to update status. */
		malloc_mutex_lock(tsdn, &background_thread_lock);
		malloc_mutex_unlock(tsdn, &background_thread_lock);
		malloc_mutex_lock(tsdn, &info->mtx);
		return true;
	}

	return false;
}

static inline void
background_work_sleep_once(tsdn_t *tsdn, background_thread_info_t *info,
    unsigned ind) {
	uint64_t ns_until_deferred = BACKGROUND_THREAD_DEFERRED_MAX;
	unsigned narenas = narenas_total_get();
	bool slept_indefinitely = background_thread_indefinite_sleep(info);

	for (unsigned i = ind; i < narenas; i += max_background_threads) {
		arena_t *arena = arena_get(tsdn, i, false);
		if (!arena) {
			continue;
		}
		/*
		 * If thread was woken up from the indefinite sleep, don't
		 * do the work instantly, but rather check when the deferred
		 * work that caused this thread to wake up is scheduled for.
		 */
		if (!slept_indefinitely) {
			arena_do_deferred_work(tsdn, arena);
		}
		if (ns_until_deferred <= BACKGROUND_THREAD_MIN_INTERVAL_NS) {
			/* Min interval will be used. */
			continue;
		}
		uint64_t ns_arena_deferred = pa_shard_time_until_deferred_work(
		    tsdn, &arena->pa_shard);
		if (ns_arena_deferred < ns_until_deferred) {
			ns_until_deferred = ns_arena_deferred;
		}
	}

	uint64_t sleep_ns;
	if (ns_until_deferred == BACKGROUND_THREAD_DEFERRED_MAX) {
		sleep_ns = BACKGROUND_THREAD_INDEFINITE_SLEEP;
	} else {
		sleep_ns =
		    (ns_until_deferred < BACKGROUND_THREAD_MIN_INTERVAL_NS)
		    ? BACKGROUND_THREAD_MIN_INTERVAL_NS
		    : ns_until_deferred;

	}

	background_thread_sleep(tsdn, info, sleep_ns);
}

static bool
background_threads_disable_single(tsd_t *tsd, background_thread_info_t *info) {
	if (info == &background_thread_info[0]) {
		malloc_mutex_assert_owner(tsd_tsdn(tsd),
		    &background_thread_lock);
	} else {
		malloc_mutex_assert_not_owner(tsd_tsdn(tsd),
		    &background_thread_lock);
	}

	pre_reentrancy(tsd, NULL);
	malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
	bool has_thread;
	assert(info->state != background_thread_paused);
	if (info->state == background_thread_started) {
		has_thread = true;
		info->state = background_thread_stopped;
		pthread_cond_signal(&info->cond);
	} else {
		has_thread = false;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);

	if (!has_thread) {
		post_reentrancy(tsd);
		return false;
	}
	void *ret;
	if (pthread_join(info->thread, &ret)) {
		post_reentrancy(tsd);
		return true;
	}
	assert(ret == NULL);
	n_background_threads--;
	post_reentrancy(tsd);

	return false;
}

static void *background_thread_entry(void *ind_arg);

static int
background_thread_create_signals_masked(pthread_t *thread,
    const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg) {
	/*
	 * Mask signals during thread creation so that the thread inherits
	 * an empty signal set.
	 */
	sigset_t set;
	sigfillset(&set);
	sigset_t oldset;
	int mask_err = pthread_sigmask(SIG_SETMASK, &set, &oldset);
	if (mask_err != 0) {
		return mask_err;
	}
	int create_err = pthread_create_wrapper(thread, attr, start_routine,
	    arg);
	/*
	 * Restore the signal mask.  Failure to restore the signal mask here
	 * changes program behavior.
	 */
	int restore_err = pthread_sigmask(SIG_SETMASK, &oldset, NULL);
	if (restore_err != 0) {
		malloc_printf("<jemalloc>: background thread creation "
		    "failed (%d), and signal mask restoration failed "
		    "(%d)\n", create_err, restore_err);
		if (opt_abort) {
			abort();
		}
	}
	return create_err;
}

static bool
check_background_thread_creation(tsd_t *tsd, unsigned *n_created,
    bool *created_threads) {
	bool ret = false;
	if (likely(*n_created == n_background_threads)) {
		return ret;
	}

	tsdn_t *tsdn = tsd_tsdn(tsd);
	malloc_mutex_unlock(tsdn, &background_thread_info[0].mtx);
	for (unsigned i = 1; i < max_background_threads; i++) {
		if (created_threads[i]) {
			continue;
		}
		background_thread_info_t *info = &background_thread_info[i];
		malloc_mutex_lock(tsdn, &info->mtx);
		/*
		 * In case of the background_thread_paused state because of
		 * arena reset, delay the creation.
		 */
		bool create = (info->state == background_thread_started);
		malloc_mutex_unlock(tsdn, &info->mtx);
		if (!create) {
			continue;
		}

		pre_reentrancy(tsd, NULL);
		int err = background_thread_create_signals_masked(&info->thread,
		    NULL, background_thread_entry, (void *)(uintptr_t)i);
		post_reentrancy(tsd);

		if (err == 0) {
			(*n_created)++;
			created_threads[i] = true;
		} else {
			malloc_printf("<jemalloc>: background thread "
			    "creation failed (%d)\n", err);
			if (opt_abort) {
				abort();
			}
		}
		/* Return to restart the loop since we unlocked. */
		ret = true;
		break;
	}
	malloc_mutex_lock(tsdn, &background_thread_info[0].mtx);

	return ret;
}

static void
background_thread0_work(tsd_t *tsd) {
	/* Thread0 is also responsible for launching / terminating threads. */
	VARIABLE_ARRAY(bool, created_threads, max_background_threads);
	unsigned i;
	for (i = 1; i < max_background_threads; i++) {
		created_threads[i] = false;
	}
	/* Start working, and create more threads when asked. */
	unsigned n_created = 1;
	while (background_thread_info[0].state != background_thread_stopped) {
		if (background_thread_pause_check(tsd_tsdn(tsd),
		    &background_thread_info[0])) {
			continue;
		}
		if (check_background_thread_creation(tsd, &n_created,
		    (bool *)&created_threads)) {
			continue;
		}
		background_work_sleep_once(tsd_tsdn(tsd),
		    &background_thread_info[0], 0);
	}

	/*
	 * Shut down other threads at exit.  Note that the ctl thread is holding
	 * the global background_thread mutex (and is waiting) for us.
	 */
	assert(!background_thread_enabled());
	for (i = 1; i < max_background_threads; i++) {
		background_thread_info_t *info = &background_thread_info[i];
		assert(info->state != background_thread_paused);
		if (created_threads[i]) {
			background_threads_disable_single(tsd, info);
		} else {
			malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
			if (info->state != background_thread_stopped) {
				/* The thread was not created. */
				assert(info->state ==
				    background_thread_started);
				n_background_threads--;
				info->state = background_thread_stopped;
			}
			malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
		}
	}
	background_thread_info[0].state = background_thread_stopped;
	assert(n_background_threads == 1);
}

static void
background_work(tsd_t *tsd, unsigned ind) {
	background_thread_info_t *info = &background_thread_info[ind];

	malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
	background_thread_wakeup_time_set(tsd_tsdn(tsd), info,
	    BACKGROUND_THREAD_INDEFINITE_SLEEP);
	if (ind == 0) {
		background_thread0_work(tsd);
	} else {
		while (info->state != background_thread_stopped) {
			if (background_thread_pause_check(tsd_tsdn(tsd),
			    info)) {
				continue;
			}
			background_work_sleep_once(tsd_tsdn(tsd), info, ind);
		}
	}
	assert(info->state == background_thread_stopped);
	background_thread_wakeup_time_set(tsd_tsdn(tsd), info, 0);
	malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
}

static void *
background_thread_entry(void *ind_arg) {
	unsigned thread_ind = (unsigned)(uintptr_t)ind_arg;
	assert(thread_ind < max_background_threads);
#ifdef JEMALLOC_HAVE_PTHREAD_SETNAME_NP
	pthread_setname_np(pthread_self(), "jemalloc_bg_thd");
#elif defined(__FreeBSD__) || defined(__DragonFly__)
	pthread_set_name_np(pthread_self(), "jemalloc_bg_thd");
#endif
	if (opt_percpu_arena != percpu_arena_disabled) {
		set_current_thread_affinity((int)thread_ind);
	}
	/*
	 * Start periodic background work.  We use internal tsd which avoids
	 * side effects, for example triggering new arena creation (which in
	 * turn triggers another background thread creation).
	 */
	background_work(tsd_internal_fetch(), thread_ind);
	assert(pthread_equal(pthread_self(),
	    background_thread_info[thread_ind].thread));

	return NULL;
}

static void
background_thread_init(tsd_t *tsd, background_thread_info_t *info) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &background_thread_lock);
	info->state = background_thread_started;
	background_thread_info_init(tsd_tsdn(tsd), info);
	n_background_threads++;
}

static bool
background_thread_create_locked(tsd_t *tsd, unsigned arena_ind) {
	assert(have_background_thread);
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &background_thread_lock);

	/* We create at most NCPUs threads. */
	size_t thread_ind = arena_ind % max_background_threads;
	background_thread_info_t *info = &background_thread_info[thread_ind];

	bool need_new_thread;
	malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
	need_new_thread = background_thread_enabled() &&
	    (info->state == background_thread_stopped);
	if (need_new_thread) {
		background_thread_init(tsd, info);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
	if (!need_new_thread) {
		return false;
	}
	if (arena_ind != 0) {
		/* Threads are created asynchronously by Thread 0. */
		background_thread_info_t *t0 = &background_thread_info[0];
		malloc_mutex_lock(tsd_tsdn(tsd), &t0->mtx);
		assert(t0->state == background_thread_started);
		pthread_cond_signal(&t0->cond);
		malloc_mutex_unlock(tsd_tsdn(tsd), &t0->mtx);

		return false;
	}

	pre_reentrancy(tsd, NULL);
	/*
	 * To avoid complications (besides reentrancy), create internal
	 * background threads with the underlying pthread_create.
	 */
	int err = background_thread_create_signals_masked(&info->thread, NULL,
	    background_thread_entry, (void *)thread_ind);
	post_reentrancy(tsd);

	if (err != 0) {
		malloc_printf("<jemalloc>: arena 0 background thread creation "
		    "failed (%d)\n", err);
		malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
		info->state = background_thread_stopped;
		n_background_threads--;
		malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);

		return true;
	}

	return false;
}

/* Create a new background thread if needed. */
bool
background_thread_create(tsd_t *tsd, unsigned arena_ind) {
	assert(have_background_thread);

	bool ret;
	malloc_mutex_lock(tsd_tsdn(tsd), &background_thread_lock);
	ret = background_thread_create_locked(tsd, arena_ind);
	malloc_mutex_unlock(tsd_tsdn(tsd), &background_thread_lock);

	return ret;
}

bool
background_threads_enable(tsd_t *tsd) {
	assert(n_background_threads == 0);
	assert(background_thread_enabled());
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &background_thread_lock);

	VARIABLE_ARRAY(bool, marked, max_background_threads);
	unsigned nmarked;
	for (unsigned i = 0; i < max_background_threads; i++) {
		marked[i] = false;
	}
	nmarked = 0;
	/* Thread 0 is required and created at the end. */
	marked[0] = true;
	/* Mark the threads we need to create for thread 0. */
	unsigned narenas = narenas_total_get();
	for (unsigned i = 1; i < narenas; i++) {
		if (marked[i % max_background_threads] ||
		    arena_get(tsd_tsdn(tsd), i, false) == NULL) {
			continue;
		}
		background_thread_info_t *info = &background_thread_info[
		    i % max_background_threads];
		malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
		assert(info->state == background_thread_stopped);
		background_thread_init(tsd, info);
		malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
		marked[i % max_background_threads] = true;
		if (++nmarked == max_background_threads) {
			break;
		}
	}

	bool err = background_thread_create_locked(tsd, 0);
	if (err) {
		return true;
	}
	for (unsigned i = 0; i < narenas; i++) {
		arena_t *arena = arena_get(tsd_tsdn(tsd), i, false);
		if (arena != NULL) {
			pa_shard_set_deferral_allowed(tsd_tsdn(tsd),
			    &arena->pa_shard, true);
		}
	}
	return false;
}

bool
background_threads_disable(tsd_t *tsd) {
	assert(!background_thread_enabled());
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &background_thread_lock);

	/* Thread 0 will be responsible for terminating other threads. */
	if (background_threads_disable_single(tsd,
	    &background_thread_info[0])) {
		return true;
	}
	assert(n_background_threads == 0);
	unsigned narenas = narenas_total_get();
	for (unsigned i = 0; i < narenas; i++) {
		arena_t *arena = arena_get(tsd_tsdn(tsd), i, false);
		if (arena != NULL) {
			pa_shard_set_deferral_allowed(tsd_tsdn(tsd),
			    &arena->pa_shard, false);
		}
	}

	return false;
}

bool
background_thread_is_started(background_thread_info_t *info) {
	return info->state == background_thread_started;
}

void
background_thread_wakeup_early(background_thread_info_t *info,
    nstime_t *remaining_sleep) {
	/*
	 * This is an optimization to increase batching. At this point
	 * we know that background thread wakes up soon, so the time to cache
	 * the just freed memory is bounded and low.
	 */
	if (remaining_sleep != NULL && nstime_ns(remaining_sleep) <
	    BACKGROUND_THREAD_MIN_INTERVAL_NS) {
		return;
	}
	pthread_cond_signal(&info->cond);
}

void
background_thread_prefork0(tsdn_t *tsdn) {
	malloc_mutex_prefork(tsdn, &background_thread_lock);
	background_thread_enabled_at_fork = background_thread_enabled();
}

void
background_thread_prefork1(tsdn_t *tsdn) {
	for (unsigned i = 0; i < max_background_threads; i++) {
		malloc_mutex_prefork(tsdn, &background_thread_info[i].mtx);
	}
}

void
background_thread_postfork_parent(tsdn_t *tsdn) {
	for (unsigned i = 0; i < max_background_threads; i++) {
		malloc_mutex_postfork_parent(tsdn,
		    &background_thread_info[i].mtx);
	}
	malloc_mutex_postfork_parent(tsdn, &background_thread_lock);
}

void
background_thread_postfork_child(tsdn_t *tsdn) {
	for (unsigned i = 0; i < max_background_threads; i++) {
		malloc_mutex_postfork_child(tsdn,
		    &background_thread_info[i].mtx);
	}
	malloc_mutex_postfork_child(tsdn, &background_thread_lock);
	if (!background_thread_enabled_at_fork) {
		return;
	}

	/* Clear background_thread state (reset to disabled for child). */
	malloc_mutex_lock(tsdn, &background_thread_lock);
	n_background_threads = 0;
	background_thread_enabled_set(tsdn, false);
	for (unsigned i = 0; i < max_background_threads; i++) {
		background_thread_info_t *info = &background_thread_info[i];
		malloc_mutex_lock(tsdn, &info->mtx);
		info->state = background_thread_stopped;
		int ret = pthread_cond_init(&info->cond, NULL);
		assert(ret == 0);
		background_thread_info_init(tsdn, info);
		malloc_mutex_unlock(tsdn, &info->mtx);
	}
	malloc_mutex_unlock(tsdn, &background_thread_lock);
}

bool
background_thread_stats_read(tsdn_t *tsdn, background_thread_stats_t *stats) {
	assert(config_stats);
	malloc_mutex_lock(tsdn, &background_thread_lock);
	if (!background_thread_enabled()) {
		malloc_mutex_unlock(tsdn, &background_thread_lock);
		return true;
	}

	nstime_init_zero(&stats->run_interval);
	memset(&stats->max_counter_per_bg_thd, 0, sizeof(mutex_prof_data_t));

	uint64_t num_runs = 0;
	stats->num_threads = n_background_threads;
	for (unsigned i = 0; i < max_background_threads; i++) {
		background_thread_info_t *info = &background_thread_info[i];
		if (malloc_mutex_trylock(tsdn, &info->mtx)) {
			/*
			 * Each background thread run may take a long time;
			 * avoid waiting on the stats if the thread is active.
			 */
			continue;
		}
		if (info->state != background_thread_stopped) {
			num_runs += info->tot_n_runs;
			nstime_add(&stats->run_interval, &info->tot_sleep_time);
			malloc_mutex_prof_max_update(tsdn,
			    &stats->max_counter_per_bg_thd, &info->mtx);
		}
		malloc_mutex_unlock(tsdn, &info->mtx);
	}
	stats->num_runs = num_runs;
	if (num_runs > 0) {
		nstime_idivide(&stats->run_interval, num_runs);
	}
	malloc_mutex_unlock(tsdn, &background_thread_lock);

	return false;
}

#undef BACKGROUND_THREAD_NPAGES_THRESHOLD
#undef BILLION
#undef BACKGROUND_THREAD_MIN_INTERVAL_NS

#ifdef JEMALLOC_HAVE_DLSYM
#include <dlfcn.h>
#endif

static bool
pthread_create_fptr_init(void) {
	if (pthread_create_fptr != NULL) {
		return false;
	}
	/*
	 * Try the next symbol first, because 1) when use lazy_lock we have a
	 * wrapper for pthread_create; and 2) application may define its own
	 * wrapper as well (and can call malloc within the wrapper).
	 */
#ifdef JEMALLOC_HAVE_DLSYM
	pthread_create_fptr = dlsym(RTLD_NEXT, "pthread_create");
#else
	pthread_create_fptr = NULL;
#endif
	if (pthread_create_fptr == NULL) {
		if (config_lazy_lock) {
			malloc_write("<jemalloc>: Error in dlsym(RTLD_NEXT, "
			    "\"pthread_create\")\n");
			abort();
		} else {
			/* Fall back to the default symbol. */
			pthread_create_fptr = pthread_create;
		}
	}

	return false;
}

/*
 * When lazy lock is enabled, we need to make sure setting isthreaded before
 * taking any background_thread locks.  This is called early in ctl (instead of
 * wait for the pthread_create calls to trigger) because the mutex is required
 * before creating background threads.
 */
void
background_thread_ctl_init(tsdn_t *tsdn) {
	malloc_mutex_assert_not_owner(tsdn, &background_thread_lock);
#ifdef JEMALLOC_PTHREAD_CREATE_WRAPPER
	pthread_create_fptr_init();
	pthread_create_wrapper_init();
#endif
}

#endif /* defined(JEMALLOC_BACKGROUND_THREAD) */

bool
background_thread_boot0(void) {
	if (!have_background_thread && opt_background_thread) {
		malloc_printf("<jemalloc>: option background_thread currently "
		    "supports pthread only\n");
		return true;
	}
#ifdef JEMALLOC_PTHREAD_CREATE_WRAPPER
	if ((config_lazy_lock || opt_background_thread) &&
	    pthread_create_fptr_init()) {
		return true;
	}
#endif
	return false;
}

bool
background_thread_boot1(tsdn_t *tsdn, base_t *base) {
#ifdef JEMALLOC_BACKGROUND_THREAD
	assert(have_background_thread);
	assert(narenas_total_get() > 0);

	if (opt_max_background_threads > MAX_BACKGROUND_THREAD_LIMIT) {
		opt_max_background_threads = DEFAULT_NUM_BACKGROUND_THREAD;
	}
	max_background_threads = opt_max_background_threads;

	background_thread_enabled_set(tsdn, opt_background_thread);
	if (malloc_mutex_init(&background_thread_lock,
	    "background_thread_global",
	    WITNESS_RANK_BACKGROUND_THREAD_GLOBAL,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}

	background_thread_info = (background_thread_info_t *)base_alloc(tsdn,
	    base, opt_max_background_threads *
	    sizeof(background_thread_info_t), CACHELINE);
	if (background_thread_info == NULL) {
		return true;
	}

	for (unsigned i = 0; i < max_background_threads; i++) {
		background_thread_info_t *info = &background_thread_info[i];
		/* Thread mutex is rank_inclusive because of thread0. */
		if (malloc_mutex_init(&info->mtx, "background_thread",
		    WITNESS_RANK_BACKGROUND_THREAD,
		    malloc_mutex_address_ordered)) {
			return true;
		}
		if (pthread_cond_init(&info->cond, NULL)) {
			return true;
		}
		malloc_mutex_lock(tsdn, &info->mtx);
		info->state = background_thread_stopped;
		background_thread_info_init(tsdn, info);
		malloc_mutex_unlock(tsdn, &info->mtx);
	}
#endif

	return false;
}
