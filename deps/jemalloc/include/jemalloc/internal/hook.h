#ifndef JEMALLOC_INTERNAL_HOOK_H
#define JEMALLOC_INTERNAL_HOOK_H

#include "jemalloc/internal/tsd.h"

/*
 * This API is *extremely* experimental, and may get ripped out, changed in API-
 * and ABI-incompatible ways, be insufficiently or incorrectly documented, etc.
 *
 * It allows hooking the stateful parts of the API to see changes as they
 * happen.
 *
 * Allocation hooks are called after the allocation is done, free hooks are
 * called before the free is done, and expand hooks are called after the
 * allocation is expanded.
 *
 * For realloc and rallocx, if the expansion happens in place, the expansion
 * hook is called.  If it is moved, then the alloc hook is called on the new
 * location, and then the free hook is called on the old location (i.e. both
 * hooks are invoked in between the alloc and the dalloc).
 *
 * If we return NULL from OOM, then usize might not be trustworthy.  Calling
 * realloc(NULL, size) only calls the alloc hook, and calling realloc(ptr, 0)
 * only calls the free hook.  (Calling realloc(NULL, 0) is treated as malloc(0),
 * and only calls the alloc hook).
 *
 * Reentrancy:
 *   Reentrancy is guarded against from within the hook implementation.  If you
 *   call allocator functions from within a hook, the hooks will not be invoked
 *   again.
 * Threading:
 *   The installation of a hook synchronizes with all its uses.  If you can
 *   prove the installation of a hook happens-before a jemalloc entry point,
 *   then the hook will get invoked (unless there's a racing removal).
 *
 *   Hook insertion appears to be atomic at a per-thread level (i.e. if a thread
 *   allocates and has the alloc hook invoked, then a subsequent free on the
 *   same thread will also have the free hook invoked).
 *
 *   The *removal* of a hook does *not* block until all threads are done with
 *   the hook.  Hook authors have to be resilient to this, and need some
 *   out-of-band mechanism for cleaning up any dynamically allocated memory
 *   associated with their hook.
 * Ordering:
 *   Order of hook execution is unspecified, and may be different than insertion
 *   order.
 */

#define HOOK_MAX 4

enum hook_alloc_e {
	hook_alloc_malloc,
	hook_alloc_posix_memalign,
	hook_alloc_aligned_alloc,
	hook_alloc_calloc,
	hook_alloc_memalign,
	hook_alloc_valloc,
	hook_alloc_mallocx,

	/* The reallocating functions have both alloc and dalloc variants */
	hook_alloc_realloc,
	hook_alloc_rallocx,
};
/*
 * We put the enum typedef after the enum, since this file may get included by
 * jemalloc_cpp.cpp, and C++ disallows enum forward declarations.
 */
typedef enum hook_alloc_e hook_alloc_t;

enum hook_dalloc_e {
	hook_dalloc_free,
	hook_dalloc_dallocx,
	hook_dalloc_sdallocx,

	/*
	 * The dalloc halves of reallocation (not called if in-place expansion
	 * happens).
	 */
	hook_dalloc_realloc,
	hook_dalloc_rallocx,
};
typedef enum hook_dalloc_e hook_dalloc_t;


enum hook_expand_e {
	hook_expand_realloc,
	hook_expand_rallocx,
	hook_expand_xallocx,
};
typedef enum hook_expand_e hook_expand_t;

typedef void (*hook_alloc)(
    void *extra, hook_alloc_t type, void *result, uintptr_t result_raw,
    uintptr_t args_raw[3]);

typedef void (*hook_dalloc)(
    void *extra, hook_dalloc_t type, void *address, uintptr_t args_raw[3]);

typedef void (*hook_expand)(
    void *extra, hook_expand_t type, void *address, size_t old_usize,
    size_t new_usize, uintptr_t result_raw, uintptr_t args_raw[4]);

typedef struct hooks_s hooks_t;
struct hooks_s {
	hook_alloc alloc_hook;
	hook_dalloc dalloc_hook;
	hook_expand expand_hook;
	void *extra;
};

/*
 * Begin implementation details; everything above this point might one day live
 * in a public API.  Everything below this point never will.
 */

/*
 * The realloc pathways haven't gotten any refactoring love in a while, and it's
 * fairly difficult to pass information from the entry point to the hooks.  We
 * put the informaiton the hooks will need into a struct to encapsulate
 * everything.
 *
 * Much of these pathways are force-inlined, so that the compiler can avoid
 * materializing this struct until we hit an extern arena function.  For fairly
 * goofy reasons, *many* of the realloc paths hit an extern arena function.
 * These paths are cold enough that it doesn't matter; eventually, we should
 * rewrite the realloc code to make the expand-in-place and the
 * free-then-realloc paths more orthogonal, at which point we don't need to
 * spread the hook logic all over the place.
 */
typedef struct hook_ralloc_args_s hook_ralloc_args_t;
struct hook_ralloc_args_s {
	/* I.e. as opposed to rallocx. */
	bool is_realloc;
	/*
	 * The expand hook takes 4 arguments, even if only 3 are actually used;
	 * we add an extra one in case the user decides to memcpy without
	 * looking too closely at the hooked function.
	 */
	uintptr_t args[4];
};

/*
 * Returns an opaque handle to be used when removing the hook.  NULL means that
 * we couldn't install the hook.
 */
bool hook_boot();

void *hook_install(tsdn_t *tsdn, hooks_t *hooks);
/* Uninstalls the hook with the handle previously returned from hook_install. */
void hook_remove(tsdn_t *tsdn, void *opaque);

/* Hooks */

void hook_invoke_alloc(hook_alloc_t type, void *result, uintptr_t result_raw,
    uintptr_t args_raw[3]);

void hook_invoke_dalloc(hook_dalloc_t type, void *address,
    uintptr_t args_raw[3]);

void hook_invoke_expand(hook_expand_t type, void *address, size_t old_usize,
    size_t new_usize, uintptr_t result_raw, uintptr_t args_raw[4]);

#endif /* JEMALLOC_INTERNAL_HOOK_H */
