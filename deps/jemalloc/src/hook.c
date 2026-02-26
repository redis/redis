#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/hook.h"

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/seq.h"

typedef struct hooks_internal_s hooks_internal_t;
struct hooks_internal_s {
	hooks_t hooks;
	bool in_use;
};

seq_define(hooks_internal_t, hooks)

static atomic_u_t nhooks = ATOMIC_INIT(0);
static seq_hooks_t hooks[HOOK_MAX];
static malloc_mutex_t hooks_mu;

bool
hook_boot() {
	return malloc_mutex_init(&hooks_mu, "hooks", WITNESS_RANK_HOOK,
	    malloc_mutex_rank_exclusive);
}

static void *
hook_install_locked(hooks_t *to_install) {
	hooks_internal_t hooks_internal;
	for (int i = 0; i < HOOK_MAX; i++) {
		bool success = seq_try_load_hooks(&hooks_internal, &hooks[i]);
		/* We hold mu; no concurrent access. */
		assert(success);
		if (!hooks_internal.in_use) {
			hooks_internal.hooks = *to_install;
			hooks_internal.in_use = true;
			seq_store_hooks(&hooks[i], &hooks_internal);
			atomic_store_u(&nhooks,
			    atomic_load_u(&nhooks, ATOMIC_RELAXED) + 1,
			    ATOMIC_RELAXED);
			return &hooks[i];
		}
	}
	return NULL;
}

void *
hook_install(tsdn_t *tsdn, hooks_t *to_install) {
	malloc_mutex_lock(tsdn, &hooks_mu);
	void *ret = hook_install_locked(to_install);
	if (ret != NULL) {
		tsd_global_slow_inc(tsdn);
	}
	malloc_mutex_unlock(tsdn, &hooks_mu);
	return ret;
}

static void
hook_remove_locked(seq_hooks_t *to_remove) {
	hooks_internal_t hooks_internal;
	bool success = seq_try_load_hooks(&hooks_internal, to_remove);
	/* We hold mu; no concurrent access. */
	assert(success);
	/* Should only remove hooks that were added. */
	assert(hooks_internal.in_use);
	hooks_internal.in_use = false;
	seq_store_hooks(to_remove, &hooks_internal);
	atomic_store_u(&nhooks, atomic_load_u(&nhooks, ATOMIC_RELAXED) - 1,
	    ATOMIC_RELAXED);
}

void
hook_remove(tsdn_t *tsdn, void *opaque) {
	if (config_debug) {
		char *hooks_begin = (char *)&hooks[0];
		char *hooks_end = (char *)&hooks[HOOK_MAX];
		char *hook = (char *)opaque;
		assert(hooks_begin <= hook && hook < hooks_end
		    && (hook - hooks_begin) % sizeof(seq_hooks_t) == 0);
	}
	malloc_mutex_lock(tsdn, &hooks_mu);
	hook_remove_locked((seq_hooks_t *)opaque);
	tsd_global_slow_dec(tsdn);
	malloc_mutex_unlock(tsdn, &hooks_mu);
}

#define FOR_EACH_HOOK_BEGIN(hooks_internal_ptr)				\
for (int for_each_hook_counter = 0;					\
    for_each_hook_counter < HOOK_MAX;					\
    for_each_hook_counter++) {						\
	bool for_each_hook_success = seq_try_load_hooks(		\
	    (hooks_internal_ptr), &hooks[for_each_hook_counter]);	\
	if (!for_each_hook_success) {					\
		continue;						\
	}								\
	if (!(hooks_internal_ptr)->in_use) {				\
		continue;						\
	}
#define FOR_EACH_HOOK_END						\
}

static bool *
hook_reentrantp() {
	/*
	 * We prevent user reentrancy within hooks.  This is basically just a
	 * thread-local bool that triggers an early-exit.
	 *
	 * We don't fold in_hook into reentrancy.  There are two reasons for
	 * this:
	 * - Right now, we turn on reentrancy during things like extent hook
	 *   execution.  Allocating during extent hooks is not officially
	 *   supported, but we don't want to break it for the time being.  These
	 *   sorts of allocations should probably still be hooked, though.
	 * - If a hook allocates, we may want it to be relatively fast (after
	 *   all, it executes on every allocator operation).  Turning on
	 *   reentrancy is a fairly heavyweight mode (disabling tcache,
	 *   redirecting to arena 0, etc.).  It's possible we may one day want
	 *   to turn on reentrant mode here, if it proves too difficult to keep
	 *   this working.  But that's fairly easy for us to see; OTOH, people
	 *   not using hooks because they're too slow is easy for us to miss.
	 *
	 * The tricky part is
	 * that this code might get invoked even if we don't have access to tsd.
	 * This function mimics getting a pointer to thread-local data, except
	 * that it might secretly return a pointer to some global data if we
	 * know that the caller will take the early-exit path.
	 * If we return a bool that indicates that we are reentrant, then the
	 * caller will go down the early exit path, leaving the global
	 * untouched.
	 */
	static bool in_hook_global = true;
	tsdn_t *tsdn = tsdn_fetch();
	bool *in_hook = tsdn_in_hookp_get(tsdn);
	if (in_hook!= NULL) {
		return in_hook;
	}
	return &in_hook_global;
}

#define HOOK_PROLOGUE							\
	if (likely(atomic_load_u(&nhooks, ATOMIC_RELAXED) == 0)) {	\
		return;							\
	}								\
	bool *in_hook = hook_reentrantp();				\
	if (*in_hook) {							\
		return;							\
	}								\
	*in_hook = true;

#define HOOK_EPILOGUE							\
	*in_hook = false;

void
hook_invoke_alloc(hook_alloc_t type, void *result, uintptr_t result_raw,
    uintptr_t args_raw[3]) {
	HOOK_PROLOGUE

	hooks_internal_t hook;
	FOR_EACH_HOOK_BEGIN(&hook)
		hook_alloc h = hook.hooks.alloc_hook;
		if (h != NULL) {
			h(hook.hooks.extra, type, result, result_raw, args_raw);
		}
	FOR_EACH_HOOK_END

	HOOK_EPILOGUE
}

void
hook_invoke_dalloc(hook_dalloc_t type, void *address, uintptr_t args_raw[3]) {
	HOOK_PROLOGUE
	hooks_internal_t hook;
	FOR_EACH_HOOK_BEGIN(&hook)
		hook_dalloc h = hook.hooks.dalloc_hook;
		if (h != NULL) {
			h(hook.hooks.extra, type, address, args_raw);
		}
	FOR_EACH_HOOK_END
	HOOK_EPILOGUE
}

void
hook_invoke_expand(hook_expand_t type, void *address, size_t old_usize,
    size_t new_usize, uintptr_t result_raw, uintptr_t args_raw[4]) {
	HOOK_PROLOGUE
	hooks_internal_t hook;
	FOR_EACH_HOOK_BEGIN(&hook)
		hook_expand h = hook.hooks.expand_hook;
		if (h != NULL) {
			h(hook.hooks.extra, type, address, old_usize, new_usize,
			    result_raw, args_raw);
		}
	FOR_EACH_HOOK_END
	HOOK_EPILOGUE
}
