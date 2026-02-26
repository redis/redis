#include "test/jemalloc_test.h"

/* Threshold: 2 << 20 = 2097152. */
const char *malloc_conf = "oversize_threshold:2097152";

#define HUGE_SZ (2 << 20)
#define SMALL_SZ (8)

TEST_BEGIN(huge_bind_thread) {
	unsigned arena1, arena2;
	size_t sz = sizeof(unsigned);

	/* Bind to a manual arena. */
	expect_d_eq(mallctl("arenas.create", &arena1, &sz, NULL, 0), 0,
	    "Failed to create arena");
	expect_d_eq(mallctl("thread.arena", NULL, NULL, &arena1,
	    sizeof(arena1)), 0, "Fail to bind thread");

	void *ptr = mallocx(HUGE_SZ, 0);
	expect_ptr_not_null(ptr, "Fail to allocate huge size");
	expect_d_eq(mallctl("arenas.lookup", &arena2, &sz, &ptr,
	    sizeof(ptr)), 0, "Unexpected mallctl() failure");
	expect_u_eq(arena1, arena2, "Wrong arena used after binding");
	dallocx(ptr, 0);

	/* Switch back to arena 0. */
	test_skip_if(have_percpu_arena &&
	    PERCPU_ARENA_ENABLED(opt_percpu_arena));
	arena2 = 0;
	expect_d_eq(mallctl("thread.arena", NULL, NULL, &arena2,
	    sizeof(arena2)), 0, "Fail to bind thread");
	ptr = mallocx(SMALL_SZ, MALLOCX_TCACHE_NONE);
	expect_d_eq(mallctl("arenas.lookup", &arena2, &sz, &ptr,
	    sizeof(ptr)), 0, "Unexpected mallctl() failure");
	expect_u_eq(arena2, 0, "Wrong arena used after binding");
	dallocx(ptr, MALLOCX_TCACHE_NONE);

	/* Then huge allocation should use the huge arena. */
	ptr = mallocx(HUGE_SZ, 0);
	expect_ptr_not_null(ptr, "Fail to allocate huge size");
	expect_d_eq(mallctl("arenas.lookup", &arena2, &sz, &ptr,
	    sizeof(ptr)), 0, "Unexpected mallctl() failure");
	expect_u_ne(arena2, 0, "Wrong arena used after binding");
	expect_u_ne(arena1, arena2, "Wrong arena used after binding");
	dallocx(ptr, 0);
}
TEST_END

TEST_BEGIN(huge_mallocx) {
	unsigned arena1, arena2;
	size_t sz = sizeof(unsigned);

	expect_d_eq(mallctl("arenas.create", &arena1, &sz, NULL, 0), 0,
	    "Failed to create arena");
	void *huge = mallocx(HUGE_SZ, MALLOCX_ARENA(arena1));
	expect_ptr_not_null(huge, "Fail to allocate huge size");
	expect_d_eq(mallctl("arenas.lookup", &arena2, &sz, &huge,
	    sizeof(huge)), 0, "Unexpected mallctl() failure");
	expect_u_eq(arena1, arena2, "Wrong arena used for mallocx");
	dallocx(huge, MALLOCX_ARENA(arena1));

	void *huge2 = mallocx(HUGE_SZ, 0);
	expect_ptr_not_null(huge, "Fail to allocate huge size");
	expect_d_eq(mallctl("arenas.lookup", &arena2, &sz, &huge2,
	    sizeof(huge2)), 0, "Unexpected mallctl() failure");
	expect_u_ne(arena1, arena2,
	    "Huge allocation should not come from the manual arena.");
	expect_u_ne(arena2, 0,
	    "Huge allocation should not come from the arena 0.");
	dallocx(huge2, 0);
}
TEST_END

TEST_BEGIN(huge_allocation) {
	unsigned arena1, arena2;

	void *ptr = mallocx(HUGE_SZ, 0);
	expect_ptr_not_null(ptr, "Fail to allocate huge size");
	size_t sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.lookup", &arena1, &sz, &ptr, sizeof(ptr)),
	    0, "Unexpected mallctl() failure");
	expect_u_gt(arena1, 0, "Huge allocation should not come from arena 0");
	dallocx(ptr, 0);

	ptr = mallocx(HUGE_SZ >> 1, 0);
	expect_ptr_not_null(ptr, "Fail to allocate half huge size");
	expect_d_eq(mallctl("arenas.lookup", &arena2, &sz, &ptr,
	    sizeof(ptr)), 0, "Unexpected mallctl() failure");
	expect_u_ne(arena1, arena2, "Wrong arena used for half huge");
	dallocx(ptr, 0);

	ptr = mallocx(SMALL_SZ, MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(ptr, "Fail to allocate small size");
	expect_d_eq(mallctl("arenas.lookup", &arena2, &sz, &ptr,
	    sizeof(ptr)), 0, "Unexpected mallctl() failure");
	expect_u_ne(arena1, arena2,
	    "Huge and small should be from different arenas");
	dallocx(ptr, 0);
}
TEST_END

int
main(void) {
	return test(
	    huge_allocation,
	    huge_mallocx,
	    huge_bind_thread);
}
