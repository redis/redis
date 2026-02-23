#include "test/jemalloc_test.h"

#include "jemalloc/internal/hook.h"

static void *arg_extra;
static int arg_type;
static void *arg_result;
static void *arg_address;
static size_t arg_old_usize;
static size_t arg_new_usize;
static uintptr_t arg_result_raw;
static uintptr_t arg_args_raw[4];

static int call_count = 0;

static void
reset_args() {
	arg_extra = NULL;
	arg_type = 12345;
	arg_result = NULL;
	arg_address = NULL;
	arg_old_usize = 0;
	arg_new_usize = 0;
	arg_result_raw = 0;
	memset(arg_args_raw, 77, sizeof(arg_args_raw));
}

static void
alloc_free_size(size_t sz) {
	void *ptr = mallocx(1, 0);
	free(ptr);
	ptr = mallocx(1, 0);
	free(ptr);
	ptr = mallocx(1, MALLOCX_TCACHE_NONE);
	dallocx(ptr, MALLOCX_TCACHE_NONE);
}

/*
 * We want to support a degree of user reentrancy.  This tests a variety of
 * allocation scenarios.
 */
static void
be_reentrant() {
	/* Let's make sure the tcache is non-empty if enabled. */
	alloc_free_size(1);
	alloc_free_size(1024);
	alloc_free_size(64 * 1024);
	alloc_free_size(256 * 1024);
	alloc_free_size(1024 * 1024);

	/* Some reallocation. */
	void *ptr = mallocx(129, 0);
	ptr = rallocx(ptr, 130, 0);
	free(ptr);

	ptr = mallocx(2 * 1024 * 1024, 0);
	free(ptr);
	ptr = mallocx(1 * 1024 * 1024, 0);
	ptr = rallocx(ptr, 2 * 1024 * 1024, 0);
	free(ptr);

	ptr = mallocx(1, 0);
	ptr = rallocx(ptr, 1000, 0);
	free(ptr);
}

static void
set_args_raw(uintptr_t *args_raw, int nargs) {
	memcpy(arg_args_raw, args_raw, sizeof(uintptr_t) * nargs);
}

static void
expect_args_raw(uintptr_t *args_raw_expected, int nargs) {
	int cmp = memcmp(args_raw_expected, arg_args_raw,
	    sizeof(uintptr_t) * nargs);
	expect_d_eq(cmp, 0, "Raw args mismatch");
}

static void
reset() {
	call_count = 0;
	reset_args();
}

static void
test_alloc_hook(void *extra, hook_alloc_t type, void *result,
    uintptr_t result_raw, uintptr_t args_raw[3]) {
	call_count++;
	arg_extra = extra;
	arg_type = (int)type;
	arg_result = result;
	arg_result_raw = result_raw;
	set_args_raw(args_raw, 3);
	be_reentrant();
}

static void
test_dalloc_hook(void *extra, hook_dalloc_t type, void *address,
    uintptr_t args_raw[3]) {
	call_count++;
	arg_extra = extra;
	arg_type = (int)type;
	arg_address = address;
	set_args_raw(args_raw, 3);
	be_reentrant();
}

static void
test_expand_hook(void *extra, hook_expand_t type, void *address,
    size_t old_usize, size_t new_usize, uintptr_t result_raw,
    uintptr_t args_raw[4]) {
	call_count++;
	arg_extra = extra;
	arg_type = (int)type;
	arg_address = address;
	arg_old_usize = old_usize;
	arg_new_usize = new_usize;
	arg_result_raw = result_raw;
	set_args_raw(args_raw, 4);
	be_reentrant();
}

TEST_BEGIN(test_hooks_basic) {
	/* Just verify that the record their arguments correctly. */
	hooks_t hooks = {
		&test_alloc_hook, &test_dalloc_hook, &test_expand_hook,
		(void *)111};
	void *handle = hook_install(TSDN_NULL, &hooks);
	uintptr_t args_raw[4] = {10, 20, 30, 40};

	/* Alloc */
	reset_args();
	hook_invoke_alloc(hook_alloc_posix_memalign, (void *)222, 333,
	    args_raw);
	expect_ptr_eq(arg_extra, (void *)111, "Passed wrong user pointer");
	expect_d_eq((int)hook_alloc_posix_memalign, arg_type,
	    "Passed wrong alloc type");
	expect_ptr_eq((void *)222, arg_result, "Passed wrong result address");
	expect_u64_eq(333, arg_result_raw, "Passed wrong result");
	expect_args_raw(args_raw, 3);

	/* Dalloc */
	reset_args();
	hook_invoke_dalloc(hook_dalloc_sdallocx, (void *)222, args_raw);
	expect_d_eq((int)hook_dalloc_sdallocx, arg_type,
	    "Passed wrong dalloc type");
	expect_ptr_eq((void *)111, arg_extra, "Passed wrong user pointer");
	expect_ptr_eq((void *)222, arg_address, "Passed wrong address");
	expect_args_raw(args_raw, 3);

	/* Expand */
	reset_args();
	hook_invoke_expand(hook_expand_xallocx, (void *)222, 333, 444, 555,
	    args_raw);
	expect_d_eq((int)hook_expand_xallocx, arg_type,
	    "Passed wrong expand type");
	expect_ptr_eq((void *)111, arg_extra, "Passed wrong user pointer");
	expect_ptr_eq((void *)222, arg_address, "Passed wrong address");
	expect_zu_eq(333, arg_old_usize, "Passed wrong old usize");
	expect_zu_eq(444, arg_new_usize, "Passed wrong new usize");
	expect_zu_eq(555, arg_result_raw, "Passed wrong result");
	expect_args_raw(args_raw, 4);

	hook_remove(TSDN_NULL, handle);
}
TEST_END

TEST_BEGIN(test_hooks_null) {
	/* Null hooks should be ignored, not crash. */
	hooks_t hooks1 = {NULL, NULL, NULL, NULL};
	hooks_t hooks2 = {&test_alloc_hook, NULL, NULL, NULL};
	hooks_t hooks3 = {NULL, &test_dalloc_hook, NULL, NULL};
	hooks_t hooks4 = {NULL, NULL, &test_expand_hook, NULL};

	void *handle1 = hook_install(TSDN_NULL, &hooks1);
	void *handle2 = hook_install(TSDN_NULL, &hooks2);
	void *handle3 = hook_install(TSDN_NULL, &hooks3);
	void *handle4 = hook_install(TSDN_NULL, &hooks4);

	expect_ptr_ne(handle1, NULL, "Hook installation failed");
	expect_ptr_ne(handle2, NULL, "Hook installation failed");
	expect_ptr_ne(handle3, NULL, "Hook installation failed");
	expect_ptr_ne(handle4, NULL, "Hook installation failed");

	uintptr_t args_raw[4] = {10, 20, 30, 40};

	call_count = 0;
	hook_invoke_alloc(hook_alloc_malloc, NULL, 0, args_raw);
	expect_d_eq(call_count, 1, "Called wrong number of times");

	call_count = 0;
	hook_invoke_dalloc(hook_dalloc_free, NULL, args_raw);
	expect_d_eq(call_count, 1, "Called wrong number of times");

	call_count = 0;
	hook_invoke_expand(hook_expand_realloc, NULL, 0, 0, 0, args_raw);
	expect_d_eq(call_count, 1, "Called wrong number of times");

	hook_remove(TSDN_NULL, handle1);
	hook_remove(TSDN_NULL, handle2);
	hook_remove(TSDN_NULL, handle3);
	hook_remove(TSDN_NULL, handle4);
}
TEST_END

TEST_BEGIN(test_hooks_remove) {
	hooks_t hooks = {&test_alloc_hook, NULL, NULL, NULL};
	void *handle = hook_install(TSDN_NULL, &hooks);
	expect_ptr_ne(handle, NULL, "Hook installation failed");
	call_count = 0;
	uintptr_t args_raw[4] = {10, 20, 30, 40};
	hook_invoke_alloc(hook_alloc_malloc, NULL, 0, args_raw);
	expect_d_eq(call_count, 1, "Hook not invoked");

	call_count = 0;
	hook_remove(TSDN_NULL, handle);
	hook_invoke_alloc(hook_alloc_malloc, NULL, 0, NULL);
	expect_d_eq(call_count, 0, "Hook invoked after removal");

}
TEST_END

TEST_BEGIN(test_hooks_alloc_simple) {
	/* "Simple" in the sense that we're not in a realloc variant. */
	hooks_t hooks = {&test_alloc_hook, NULL, NULL, (void *)123};
	void *handle = hook_install(TSDN_NULL, &hooks);
	expect_ptr_ne(handle, NULL, "Hook installation failed");

	/* Stop malloc from being optimized away. */
	volatile int err;
	void *volatile ptr;

	/* malloc */
	reset();
	ptr = malloc(1);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_alloc_malloc, "Wrong hook type");
	expect_ptr_eq(ptr, arg_result, "Wrong result");
	expect_u64_eq((uintptr_t)ptr, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)1, arg_args_raw[0], "Wrong argument");
	free(ptr);

	/* posix_memalign */
	reset();
	err = posix_memalign((void **)&ptr, 1024, 1);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_alloc_posix_memalign,
	    "Wrong hook type");
	expect_ptr_eq(ptr, arg_result, "Wrong result");
	expect_u64_eq((uintptr_t)err, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)&ptr, arg_args_raw[0], "Wrong argument");
	expect_u64_eq((uintptr_t)1024, arg_args_raw[1], "Wrong argument");
	expect_u64_eq((uintptr_t)1, arg_args_raw[2], "Wrong argument");
	free(ptr);

	/* aligned_alloc */
	reset();
	ptr = aligned_alloc(1024, 1);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_alloc_aligned_alloc,
	    "Wrong hook type");
	expect_ptr_eq(ptr, arg_result, "Wrong result");
	expect_u64_eq((uintptr_t)ptr, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)1024, arg_args_raw[0], "Wrong argument");
	expect_u64_eq((uintptr_t)1, arg_args_raw[1], "Wrong argument");
	free(ptr);

	/* calloc */
	reset();
	ptr = calloc(11, 13);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_alloc_calloc, "Wrong hook type");
	expect_ptr_eq(ptr, arg_result, "Wrong result");
	expect_u64_eq((uintptr_t)ptr, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)11, arg_args_raw[0], "Wrong argument");
	expect_u64_eq((uintptr_t)13, arg_args_raw[1], "Wrong argument");
	free(ptr);

	/* memalign */
#ifdef JEMALLOC_OVERRIDE_MEMALIGN
	reset();
	ptr = memalign(1024, 1);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_alloc_memalign, "Wrong hook type");
	expect_ptr_eq(ptr, arg_result, "Wrong result");
	expect_u64_eq((uintptr_t)ptr, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)1024, arg_args_raw[0], "Wrong argument");
	expect_u64_eq((uintptr_t)1, arg_args_raw[1], "Wrong argument");
	free(ptr);
#endif /* JEMALLOC_OVERRIDE_MEMALIGN */

	/* valloc */
#ifdef JEMALLOC_OVERRIDE_VALLOC
	reset();
	ptr = valloc(1);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_alloc_valloc, "Wrong hook type");
	expect_ptr_eq(ptr, arg_result, "Wrong result");
	expect_u64_eq((uintptr_t)ptr, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)1, arg_args_raw[0], "Wrong argument");
	free(ptr);
#endif /* JEMALLOC_OVERRIDE_VALLOC */

	/* mallocx */
	reset();
	ptr = mallocx(1, MALLOCX_LG_ALIGN(10));
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_alloc_mallocx, "Wrong hook type");
	expect_ptr_eq(ptr, arg_result, "Wrong result");
	expect_u64_eq((uintptr_t)ptr, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)1, arg_args_raw[0], "Wrong argument");
	expect_u64_eq((uintptr_t)MALLOCX_LG_ALIGN(10), arg_args_raw[1],
	    "Wrong flags");
	free(ptr);

	hook_remove(TSDN_NULL, handle);
}
TEST_END

TEST_BEGIN(test_hooks_dalloc_simple) {
	/* "Simple" in the sense that we're not in a realloc variant. */
	hooks_t hooks = {NULL, &test_dalloc_hook, NULL, (void *)123};
	void *handle = hook_install(TSDN_NULL, &hooks);
	expect_ptr_ne(handle, NULL, "Hook installation failed");

	void *volatile ptr;

	/* free() */
	reset();
	ptr = malloc(1);
	free(ptr);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_dalloc_free, "Wrong hook type");
	expect_ptr_eq(ptr, arg_address, "Wrong pointer freed");
	expect_u64_eq((uintptr_t)ptr, arg_args_raw[0], "Wrong raw arg");

	/* dallocx() */
	reset();
	ptr = malloc(1);
	dallocx(ptr, MALLOCX_TCACHE_NONE);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_dalloc_dallocx, "Wrong hook type");
	expect_ptr_eq(ptr, arg_address, "Wrong pointer freed");
	expect_u64_eq((uintptr_t)ptr, arg_args_raw[0], "Wrong raw arg");
	expect_u64_eq((uintptr_t)MALLOCX_TCACHE_NONE, arg_args_raw[1],
	    "Wrong raw arg");

	/* sdallocx() */
	reset();
	ptr = malloc(1);
	sdallocx(ptr, 1, MALLOCX_TCACHE_NONE);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_dalloc_sdallocx, "Wrong hook type");
	expect_ptr_eq(ptr, arg_address, "Wrong pointer freed");
	expect_u64_eq((uintptr_t)ptr, arg_args_raw[0], "Wrong raw arg");
	expect_u64_eq((uintptr_t)1, arg_args_raw[1], "Wrong raw arg");
	expect_u64_eq((uintptr_t)MALLOCX_TCACHE_NONE, arg_args_raw[2],
	    "Wrong raw arg");

	hook_remove(TSDN_NULL, handle);
}
TEST_END

TEST_BEGIN(test_hooks_expand_simple) {
	/* "Simple" in the sense that we're not in a realloc variant. */
	hooks_t hooks = {NULL, NULL, &test_expand_hook, (void *)123};
	void *handle = hook_install(TSDN_NULL, &hooks);
	expect_ptr_ne(handle, NULL, "Hook installation failed");

	void *volatile ptr;

	/* xallocx() */
	reset();
	ptr = malloc(1);
	size_t new_usize = xallocx(ptr, 100, 200, MALLOCX_TCACHE_NONE);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_expand_xallocx, "Wrong hook type");
	expect_ptr_eq(ptr, arg_address, "Wrong pointer expanded");
	expect_u64_eq(arg_old_usize, nallocx(1, 0), "Wrong old usize");
	expect_u64_eq(arg_new_usize, sallocx(ptr, 0), "Wrong new usize");
	expect_u64_eq(new_usize, arg_result_raw, "Wrong result");
	expect_u64_eq((uintptr_t)ptr, arg_args_raw[0], "Wrong arg");
	expect_u64_eq(100, arg_args_raw[1], "Wrong arg");
	expect_u64_eq(200, arg_args_raw[2], "Wrong arg");
	expect_u64_eq(MALLOCX_TCACHE_NONE, arg_args_raw[3], "Wrong arg");

	hook_remove(TSDN_NULL, handle);
}
TEST_END

TEST_BEGIN(test_hooks_realloc_as_malloc_or_free) {
	hooks_t hooks = {&test_alloc_hook, &test_dalloc_hook,
		&test_expand_hook, (void *)123};
	void *handle = hook_install(TSDN_NULL, &hooks);
	expect_ptr_ne(handle, NULL, "Hook installation failed");

	void *volatile ptr;

	/* realloc(NULL, size) as malloc */
	reset();
	ptr = realloc(NULL, 1);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_alloc_realloc, "Wrong hook type");
	expect_ptr_eq(ptr, arg_result, "Wrong result");
	expect_u64_eq((uintptr_t)ptr, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)NULL, arg_args_raw[0], "Wrong argument");
	expect_u64_eq((uintptr_t)1, arg_args_raw[1], "Wrong argument");
	free(ptr);

	/* realloc(ptr, 0) as free */
	if (opt_zero_realloc_action == zero_realloc_action_free) {
		ptr = malloc(1);
		reset();
		realloc(ptr, 0);
		expect_d_eq(call_count, 1, "Hook not called");
		expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
		expect_d_eq(arg_type, (int)hook_dalloc_realloc,
		    "Wrong hook type");
		expect_ptr_eq(ptr, arg_address,
		    "Wrong pointer freed");
		expect_u64_eq((uintptr_t)ptr, arg_args_raw[0],
		    "Wrong raw arg");
		expect_u64_eq((uintptr_t)0, arg_args_raw[1],
		    "Wrong raw arg");
	}

	/* realloc(NULL, 0) as malloc(0) */
	reset();
	ptr = realloc(NULL, 0);
	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, (int)hook_alloc_realloc, "Wrong hook type");
	expect_ptr_eq(ptr, arg_result, "Wrong result");
	expect_u64_eq((uintptr_t)ptr, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)NULL, arg_args_raw[0], "Wrong argument");
	expect_u64_eq((uintptr_t)0, arg_args_raw[1], "Wrong argument");
	free(ptr);

	hook_remove(TSDN_NULL, handle);
}
TEST_END

static void
do_realloc_test(void *(*ralloc)(void *, size_t, int), int flags,
    int expand_type, int dalloc_type) {
	hooks_t hooks = {&test_alloc_hook, &test_dalloc_hook,
		&test_expand_hook, (void *)123};
	void *handle = hook_install(TSDN_NULL, &hooks);
	expect_ptr_ne(handle, NULL, "Hook installation failed");

	void *volatile ptr;
	void *volatile ptr2;

	/* Realloc in-place, small. */
	ptr = malloc(129);
	reset();
	ptr2 = ralloc(ptr, 130, flags);
	expect_ptr_eq(ptr, ptr2, "Small realloc moved");

	expect_d_eq(call_count, 1, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, expand_type, "Wrong hook type");
	expect_ptr_eq(ptr, arg_address, "Wrong address");
	expect_u64_eq((uintptr_t)ptr, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)ptr, arg_args_raw[0], "Wrong argument");
	expect_u64_eq((uintptr_t)130, arg_args_raw[1], "Wrong argument");
	free(ptr);

	/*
	 * Realloc in-place, large.  Since we can't guarantee the large case
	 * across all platforms, we stay resilient to moving results.
	 */
	ptr = malloc(2 * 1024 * 1024);
	free(ptr);
	ptr2 = malloc(1 * 1024 * 1024);
	reset();
	ptr = ralloc(ptr2, 2 * 1024 * 1024, flags);
	/* ptr is the new address, ptr2 is the old address. */
	if (ptr == ptr2) {
		expect_d_eq(call_count, 1, "Hook not called");
		expect_d_eq(arg_type, expand_type, "Wrong hook type");
	} else {
		expect_d_eq(call_count, 2, "Wrong hooks called");
		expect_ptr_eq(ptr, arg_result, "Wrong address");
		expect_d_eq(arg_type, dalloc_type, "Wrong hook type");
	}
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_ptr_eq(ptr2, arg_address, "Wrong address");
	expect_u64_eq((uintptr_t)ptr, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)ptr2, arg_args_raw[0], "Wrong argument");
	expect_u64_eq((uintptr_t)2 * 1024 * 1024, arg_args_raw[1],
	    "Wrong argument");
	free(ptr);

	/* Realloc with move, small. */
	ptr = malloc(8);
	reset();
	ptr2 = ralloc(ptr, 128, flags);
	expect_ptr_ne(ptr, ptr2, "Small realloc didn't move");

	expect_d_eq(call_count, 2, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, dalloc_type, "Wrong hook type");
	expect_ptr_eq(ptr, arg_address, "Wrong address");
	expect_ptr_eq(ptr2, arg_result, "Wrong address");
	expect_u64_eq((uintptr_t)ptr2, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)ptr, arg_args_raw[0], "Wrong argument");
	expect_u64_eq((uintptr_t)128, arg_args_raw[1], "Wrong argument");
	free(ptr2);

	/* Realloc with move, large. */
	ptr = malloc(1);
	reset();
	ptr2 = ralloc(ptr, 2 * 1024 * 1024, flags);
	expect_ptr_ne(ptr, ptr2, "Large realloc didn't move");

	expect_d_eq(call_count, 2, "Hook not called");
	expect_ptr_eq(arg_extra, (void *)123, "Wrong extra");
	expect_d_eq(arg_type, dalloc_type, "Wrong hook type");
	expect_ptr_eq(ptr, arg_address, "Wrong address");
	expect_ptr_eq(ptr2, arg_result, "Wrong address");
	expect_u64_eq((uintptr_t)ptr2, (uintptr_t)arg_result_raw,
	    "Wrong raw result");
	expect_u64_eq((uintptr_t)ptr, arg_args_raw[0], "Wrong argument");
	expect_u64_eq((uintptr_t)2 * 1024 * 1024, arg_args_raw[1],
	    "Wrong argument");
	free(ptr2);

	hook_remove(TSDN_NULL, handle);
}

static void *
realloc_wrapper(void *ptr, size_t size, UNUSED int flags) {
	return realloc(ptr, size);
}

TEST_BEGIN(test_hooks_realloc) {
	do_realloc_test(&realloc_wrapper, 0, hook_expand_realloc,
	    hook_dalloc_realloc);
}
TEST_END

TEST_BEGIN(test_hooks_rallocx) {
	do_realloc_test(&rallocx, MALLOCX_TCACHE_NONE, hook_expand_rallocx,
	    hook_dalloc_rallocx);
}
TEST_END

int
main(void) {
	/* We assert on call counts. */
	return test_no_reentrancy(
	    test_hooks_basic,
	    test_hooks_null,
	    test_hooks_remove,
	    test_hooks_alloc_simple,
	    test_hooks_dalloc_simple,
	    test_hooks_expand_simple,
	    test_hooks_realloc_as_malloc_or_free,
	    test_hooks_realloc,
	    test_hooks_rallocx);
}
