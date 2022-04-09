#include "test/jemalloc_test.h"

static void
noop_alloc_hook(void *extra, hook_alloc_t type, void *result,
    uintptr_t result_raw, uintptr_t args_raw[3]) {
}

static void
noop_dalloc_hook(void *extra, hook_dalloc_t type, void *address,
    uintptr_t args_raw[3]) {
}

static void
noop_expand_hook(void *extra, hook_expand_t type, void *address,
    size_t old_usize, size_t new_usize, uintptr_t result_raw,
    uintptr_t args_raw[4]) {
}

static void
malloc_free_loop(int iters) {
	for (int i = 0; i < iters; i++) {
		void *p = mallocx(1, 0);
		free(p);
	}
}

static void
test_hooked(int iters) {
	hooks_t hooks = {&noop_alloc_hook, &noop_dalloc_hook, &noop_expand_hook,
		NULL};

	int err;
	void *handles[HOOK_MAX];
	size_t sz = sizeof(handles[0]);

	for (int i = 0; i < HOOK_MAX; i++) {
		err = mallctl("experimental.hooks.install", &handles[i],
		    &sz, &hooks, sizeof(hooks));
		assert(err == 0);

		timedelta_t timer;
		timer_start(&timer);
		malloc_free_loop(iters);
		timer_stop(&timer);
		malloc_printf("With %d hook%s: %"FMTu64"us\n", i + 1,
		    i + 1 == 1 ? "" : "s", timer_usec(&timer));
	}
	for (int i = 0; i < HOOK_MAX; i++) {
		err = mallctl("experimental.hooks.remove", NULL, NULL,
		    &handles[i], sizeof(handles[i]));
		assert(err == 0);
	}
}

static void
test_unhooked(int iters) {
	timedelta_t timer;
	timer_start(&timer);
	malloc_free_loop(iters);
	timer_stop(&timer);

	malloc_printf("Without hooks: %"FMTu64"us\n", timer_usec(&timer));
}

int
main(void) {
	/* Initialize */
	free(mallocx(1, 0));
	int iters = 10 * 1000 * 1000;
	malloc_printf("Benchmarking hooks with %d iterations:\n", iters);
	test_hooked(iters);
	test_unhooked(iters);
}
