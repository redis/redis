#include "test/jemalloc_test.h"

static unsigned
get_nsizes_impl(const char *cmd) {
	unsigned ret;
	size_t z;

	z = sizeof(unsigned);
	expect_d_eq(mallctl(cmd, (void *)&ret, &z, NULL, 0), 0,
	    "Unexpected mallctl(\"%s\", ...) failure", cmd);

	return ret;
}

static unsigned
get_nlarge(void) {
	return get_nsizes_impl("arenas.nlextents");
}

static size_t
get_size_impl(const char *cmd, size_t ind) {
	size_t ret;
	size_t z;
	size_t mib[4];
	size_t miblen = 4;

	z = sizeof(size_t);
	expect_d_eq(mallctlnametomib(cmd, mib, &miblen),
	    0, "Unexpected mallctlnametomib(\"%s\", ...) failure", cmd);
	mib[2] = ind;
	z = sizeof(size_t);
	expect_d_eq(mallctlbymib(mib, miblen, (void *)&ret, &z, NULL, 0),
	    0, "Unexpected mallctlbymib([\"%s\", %zu], ...) failure", cmd, ind);

	return ret;
}

static size_t
get_large_size(size_t ind) {
	return get_size_impl("arenas.lextent.0.size", ind);
}

/*
 * On systems which can't merge extents, tests that call this function generate
 * a lot of dirty memory very quickly.  Purging between cycles mitigates
 * potential OOM on e.g. 32-bit Windows.
 */
static void
purge(void) {
	expect_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl error");
}

/*
 * GCC "-Walloc-size-larger-than" warning detects when one of the memory
 * allocation functions is called with a size larger than the maximum size that
 * they support. Here we want to explicitly test that the allocation functions
 * do indeed fail properly when this is the case, which triggers the warning.
 * Therefore we disable the warning for these tests.
 */
JEMALLOC_DIAGNOSTIC_PUSH
JEMALLOC_DIAGNOSTIC_IGNORE_ALLOC_SIZE_LARGER_THAN

TEST_BEGIN(test_overflow) {
	size_t largemax;

	largemax = get_large_size(get_nlarge()-1);

	expect_ptr_null(mallocx(largemax+1, 0),
	    "Expected OOM for mallocx(size=%#zx, 0)", largemax+1);

	expect_ptr_null(mallocx(ZU(PTRDIFF_MAX)+1, 0),
	    "Expected OOM for mallocx(size=%#zx, 0)", ZU(PTRDIFF_MAX)+1);

	expect_ptr_null(mallocx(SIZE_T_MAX, 0),
	    "Expected OOM for mallocx(size=%#zx, 0)", SIZE_T_MAX);

	expect_ptr_null(mallocx(1, MALLOCX_ALIGN(ZU(PTRDIFF_MAX)+1)),
	    "Expected OOM for mallocx(size=1, MALLOCX_ALIGN(%#zx))",
	    ZU(PTRDIFF_MAX)+1);
}
TEST_END

static void *
remote_alloc(void *arg) {
	unsigned arena;
	size_t sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.create", (void *)&arena, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	size_t large_sz;
	sz = sizeof(size_t);
	expect_d_eq(mallctl("arenas.lextent.0.size", (void *)&large_sz, &sz,
	    NULL, 0), 0, "Unexpected mallctl failure");

	void *ptr = mallocx(large_sz, MALLOCX_ARENA(arena)
	    | MALLOCX_TCACHE_NONE);
	void **ret = (void **)arg;
	*ret = ptr;

	return NULL;
}

TEST_BEGIN(test_remote_free) {
	thd_t thd;
	void *ret;
	thd_create(&thd, remote_alloc, (void *)&ret);
	thd_join(thd, NULL);
	expect_ptr_not_null(ret, "Unexpected mallocx failure");

	/* Avoid TCACHE_NONE to explicitly test tcache_flush(). */
	dallocx(ret, 0);
	mallctl("thread.tcache.flush", NULL, NULL, NULL, 0);
}
TEST_END

TEST_BEGIN(test_oom) {
	size_t largemax;
	bool oom;
	void *ptrs[3];
	unsigned i;

	/*
	 * It should be impossible to allocate three objects that each consume
	 * nearly half the virtual address space.
	 */
	largemax = get_large_size(get_nlarge()-1);
	oom = false;
	for (i = 0; i < sizeof(ptrs) / sizeof(void *); i++) {
		ptrs[i] = mallocx(largemax, MALLOCX_ARENA(0));
		if (ptrs[i] == NULL) {
			oom = true;
		}
	}
	expect_true(oom,
	    "Expected OOM during series of calls to mallocx(size=%zu, 0)",
	    largemax);
	for (i = 0; i < sizeof(ptrs) / sizeof(void *); i++) {
		if (ptrs[i] != NULL) {
			dallocx(ptrs[i], 0);
		}
	}
	purge();

#if LG_SIZEOF_PTR == 3
	expect_ptr_null(mallocx(0x8000000000000000ULL,
	    MALLOCX_ALIGN(0x8000000000000000ULL)),
	    "Expected OOM for mallocx()");
	expect_ptr_null(mallocx(0x8000000000000000ULL,
	    MALLOCX_ALIGN(0x80000000)),
	    "Expected OOM for mallocx()");
#else
	expect_ptr_null(mallocx(0x80000000UL, MALLOCX_ALIGN(0x80000000UL)),
	    "Expected OOM for mallocx()");
#endif
}
TEST_END

/* Re-enable the "-Walloc-size-larger-than=" warning */
JEMALLOC_DIAGNOSTIC_POP

TEST_BEGIN(test_basic) {
#define MAXSZ (((size_t)1) << 23)
	size_t sz;

	for (sz = 1; sz < MAXSZ; sz = nallocx(sz, 0) + 1) {
		size_t nsz, rsz;
		void *p;
		nsz = nallocx(sz, 0);
		expect_zu_ne(nsz, 0, "Unexpected nallocx() error");
		p = mallocx(sz, 0);
		expect_ptr_not_null(p,
		    "Unexpected mallocx(size=%zx, flags=0) error", sz);
		rsz = sallocx(p, 0);
		expect_zu_ge(rsz, sz, "Real size smaller than expected");
		expect_zu_eq(nsz, rsz, "nallocx()/sallocx() size mismatch");
		dallocx(p, 0);

		p = mallocx(sz, 0);
		expect_ptr_not_null(p,
		    "Unexpected mallocx(size=%zx, flags=0) error", sz);
		dallocx(p, 0);

		nsz = nallocx(sz, MALLOCX_ZERO);
		expect_zu_ne(nsz, 0, "Unexpected nallocx() error");
		p = mallocx(sz, MALLOCX_ZERO);
		expect_ptr_not_null(p,
		    "Unexpected mallocx(size=%zx, flags=MALLOCX_ZERO) error",
		    nsz);
		rsz = sallocx(p, 0);
		expect_zu_eq(nsz, rsz, "nallocx()/sallocx() rsize mismatch");
		dallocx(p, 0);
		purge();
	}
#undef MAXSZ
}
TEST_END

TEST_BEGIN(test_alignment_and_size) {
	const char *percpu_arena;
	size_t sz = sizeof(percpu_arena);

	if(mallctl("opt.percpu_arena", (void *)&percpu_arena, &sz, NULL, 0) ||
	    strcmp(percpu_arena, "disabled") != 0) {
		test_skip("test_alignment_and_size skipped: "
		    "not working with percpu arena.");
	};
#define MAXALIGN (((size_t)1) << 23)
#define NITER 4
	size_t nsz, rsz, alignment, total;
	unsigned i;
	void *ps[NITER];

	for (i = 0; i < NITER; i++) {
		ps[i] = NULL;
	}

	for (alignment = 8;
	    alignment <= MAXALIGN;
	    alignment <<= 1) {
		total = 0;
		for (sz = 1;
		    sz < 3 * alignment && sz < (1U << 31);
		    sz += (alignment >> (LG_SIZEOF_PTR-1)) - 1) {
			for (i = 0; i < NITER; i++) {
				nsz = nallocx(sz, MALLOCX_ALIGN(alignment) |
				    MALLOCX_ZERO | MALLOCX_ARENA(0));
				expect_zu_ne(nsz, 0,
				    "nallocx() error for alignment=%zu, "
				    "size=%zu (%#zx)", alignment, sz, sz);
				ps[i] = mallocx(sz, MALLOCX_ALIGN(alignment) |
				    MALLOCX_ZERO | MALLOCX_ARENA(0));
				expect_ptr_not_null(ps[i],
				    "mallocx() error for alignment=%zu, "
				    "size=%zu (%#zx)", alignment, sz, sz);
				rsz = sallocx(ps[i], 0);
				expect_zu_ge(rsz, sz,
				    "Real size smaller than expected for "
				    "alignment=%zu, size=%zu", alignment, sz);
				expect_zu_eq(nsz, rsz,
				    "nallocx()/sallocx() size mismatch for "
				    "alignment=%zu, size=%zu", alignment, sz);
				expect_ptr_null(
				    (void *)((uintptr_t)ps[i] & (alignment-1)),
				    "%p inadequately aligned for"
				    " alignment=%zu, size=%zu", ps[i],
				    alignment, sz);
				total += rsz;
				if (total >= (MAXALIGN << 1)) {
					break;
				}
			}
			for (i = 0; i < NITER; i++) {
				if (ps[i] != NULL) {
					dallocx(ps[i], 0);
					ps[i] = NULL;
				}
			}
		}
		purge();
	}
#undef MAXALIGN
#undef NITER
}
TEST_END

int
main(void) {
	return test(
	    test_overflow,
	    test_oom,
	    test_remote_free,
	    test_basic,
	    test_alignment_and_size);
}
