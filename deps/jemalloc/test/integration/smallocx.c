#include "test/jemalloc_test.h"
#include "jemalloc/jemalloc_macros.h"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#ifndef JEMALLOC_VERSION_GID_IDENT
  #error "JEMALLOC_VERSION_GID_IDENT not defined"
#endif

#define JOIN(x, y) x ## y
#define JOIN2(x, y) JOIN(x, y)
#define smallocx JOIN2(smallocx_, JEMALLOC_VERSION_GID_IDENT)

typedef struct {
	void *ptr;
	size_t size;
} smallocx_return_t;

extern smallocx_return_t
smallocx(size_t size, int flags);

static unsigned
get_nsizes_impl(const char *cmd) {
	unsigned ret;
	size_t z;

	z = sizeof(unsigned);
	assert_d_eq(mallctl(cmd, (void *)&ret, &z, NULL, 0), 0,
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
	assert_d_eq(mallctlnametomib(cmd, mib, &miblen),
	    0, "Unexpected mallctlnametomib(\"%s\", ...) failure", cmd);
	mib[2] = ind;
	z = sizeof(size_t);
	assert_d_eq(mallctlbymib(mib, miblen, (void *)&ret, &z, NULL, 0),
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
	assert_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
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

	assert_ptr_null(smallocx(largemax+1, 0).ptr,
	    "Expected OOM for smallocx(size=%#zx, 0)", largemax+1);

	assert_ptr_null(smallocx(ZU(PTRDIFF_MAX)+1, 0).ptr,
	    "Expected OOM for smallocx(size=%#zx, 0)", ZU(PTRDIFF_MAX)+1);

	assert_ptr_null(smallocx(SIZE_T_MAX, 0).ptr,
	    "Expected OOM for smallocx(size=%#zx, 0)", SIZE_T_MAX);

	assert_ptr_null(smallocx(1, MALLOCX_ALIGN(ZU(PTRDIFF_MAX)+1)).ptr,
	    "Expected OOM for smallocx(size=1, MALLOCX_ALIGN(%#zx))",
	    ZU(PTRDIFF_MAX)+1);
}
TEST_END

static void *
remote_alloc(void *arg) {
	unsigned arena;
	size_t sz = sizeof(unsigned);
	assert_d_eq(mallctl("arenas.create", (void *)&arena, &sz, NULL, 0), 0,
	    "Unexpected mallctl() failure");
	size_t large_sz;
	sz = sizeof(size_t);
	assert_d_eq(mallctl("arenas.lextent.0.size", (void *)&large_sz, &sz,
	    NULL, 0), 0, "Unexpected mallctl failure");

	smallocx_return_t r
	    = smallocx(large_sz, MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE);
	void *ptr = r.ptr;
	assert_zu_eq(r.size,
	    nallocx(large_sz, MALLOCX_ARENA(arena) | MALLOCX_TCACHE_NONE),
	    "Expected smalloc(size,flags).size == nallocx(size,flags)");
	void **ret = (void **)arg;
	*ret = ptr;

	return NULL;
}

TEST_BEGIN(test_remote_free) {
	thd_t thd;
	void *ret;
	thd_create(&thd, remote_alloc, (void *)&ret);
	thd_join(thd, NULL);
	assert_ptr_not_null(ret, "Unexpected smallocx failure");

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
		ptrs[i] = smallocx(largemax, 0).ptr;
		if (ptrs[i] == NULL) {
			oom = true;
		}
	}
	assert_true(oom,
	    "Expected OOM during series of calls to smallocx(size=%zu, 0)",
	    largemax);
	for (i = 0; i < sizeof(ptrs) / sizeof(void *); i++) {
		if (ptrs[i] != NULL) {
			dallocx(ptrs[i], 0);
		}
	}
	purge();

#if LG_SIZEOF_PTR == 3
	assert_ptr_null(smallocx(0x8000000000000000ULL,
	    MALLOCX_ALIGN(0x8000000000000000ULL)).ptr,
	    "Expected OOM for smallocx()");
	assert_ptr_null(smallocx(0x8000000000000000ULL,
	    MALLOCX_ALIGN(0x80000000)).ptr,
	    "Expected OOM for smallocx()");
#else
	assert_ptr_null(smallocx(0x80000000UL, MALLOCX_ALIGN(0x80000000UL)).ptr,
	    "Expected OOM for smallocx()");
#endif
}
TEST_END

/* Re-enable the "-Walloc-size-larger-than=" warning */
JEMALLOC_DIAGNOSTIC_POP

TEST_BEGIN(test_basic) {
#define MAXSZ (((size_t)1) << 23)
	size_t sz;

	for (sz = 1; sz < MAXSZ; sz = nallocx(sz, 0) + 1) {
		smallocx_return_t ret;
		size_t nsz, rsz, smz;
		void *p;
		nsz = nallocx(sz, 0);
		assert_zu_ne(nsz, 0, "Unexpected nallocx() error");
		ret = smallocx(sz, 0);
		p = ret.ptr;
		smz = ret.size;
		assert_ptr_not_null(p,
		    "Unexpected smallocx(size=%zx, flags=0) error", sz);
		rsz = sallocx(p, 0);
		assert_zu_ge(rsz, sz, "Real size smaller than expected");
		assert_zu_eq(nsz, rsz, "nallocx()/sallocx() size mismatch");
		assert_zu_eq(nsz, smz, "nallocx()/smallocx() size mismatch");
		dallocx(p, 0);

		ret = smallocx(sz, 0);
		p = ret.ptr;
		smz = ret.size;
		assert_ptr_not_null(p,
		    "Unexpected smallocx(size=%zx, flags=0) error", sz);
		dallocx(p, 0);

		nsz = nallocx(sz, MALLOCX_ZERO);
		assert_zu_ne(nsz, 0, "Unexpected nallocx() error");
		assert_zu_ne(smz, 0, "Unexpected smallocx() error");
		ret = smallocx(sz, MALLOCX_ZERO);
		p = ret.ptr;
		assert_ptr_not_null(p,
		    "Unexpected smallocx(size=%zx, flags=MALLOCX_ZERO) error",
		    nsz);
		rsz = sallocx(p, 0);
		assert_zu_eq(nsz, rsz, "nallocx()/sallocx() rsize mismatch");
		assert_zu_eq(nsz, smz, "nallocx()/smallocx() size mismatch");
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
	size_t nsz, rsz, smz, alignment, total;
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
				    MALLOCX_ZERO);
				assert_zu_ne(nsz, 0,
				    "nallocx() error for alignment=%zu, "
				    "size=%zu (%#zx)", alignment, sz, sz);
				smallocx_return_t ret
				    = smallocx(sz, MALLOCX_ALIGN(alignment) | MALLOCX_ZERO);
				ps[i] = ret.ptr;
				assert_ptr_not_null(ps[i],
				    "smallocx() error for alignment=%zu, "
				    "size=%zu (%#zx)", alignment, sz, sz);
				rsz = sallocx(ps[i], 0);
				smz = ret.size;
				assert_zu_ge(rsz, sz,
				    "Real size smaller than expected for "
				    "alignment=%zu, size=%zu", alignment, sz);
				assert_zu_eq(nsz, rsz,
				    "nallocx()/sallocx() size mismatch for "
				    "alignment=%zu, size=%zu", alignment, sz);
				assert_zu_eq(nsz, smz,
				    "nallocx()/smallocx() size mismatch for "
				    "alignment=%zu, size=%zu", alignment, sz);
				assert_ptr_null(
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
