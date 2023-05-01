#include "test/jemalloc_test.h"

#define arraylen(arr) (sizeof(arr)/sizeof(arr[0]))
static size_t ptr_ind;
static void *volatile ptrs[100];
static void *last_junked_ptr;
static size_t last_junked_usize;

static void
reset() {
	ptr_ind = 0;
	last_junked_ptr = NULL;
	last_junked_usize = 0;
}

static void
test_junk(void *ptr, size_t usize) {
	last_junked_ptr = ptr;
	last_junked_usize = usize;
}

static void
do_allocs(size_t size, bool zero, size_t lg_align) {
#define JUNK_ALLOC(...)							\
	do {								\
		assert(ptr_ind + 1 < arraylen(ptrs));			\
		void *ptr = __VA_ARGS__;				\
		assert_ptr_not_null(ptr, "");				\
		ptrs[ptr_ind++] = ptr;					\
		if (opt_junk_alloc && !zero) {				\
			expect_ptr_eq(ptr, last_junked_ptr, "");	\
			expect_zu_eq(last_junked_usize,			\
			    TEST_MALLOC_SIZE(ptr), "");			\
		}							\
	} while (0)
	if (!zero && lg_align == 0) {
		JUNK_ALLOC(malloc(size));
	}
	if (!zero) {
		JUNK_ALLOC(aligned_alloc(1 << lg_align, size));
	}
#ifdef JEMALLOC_OVERRIDE_MEMALIGN
	if (!zero) {
		JUNK_ALLOC(je_memalign(1 << lg_align, size));
	}
#endif
#ifdef JEMALLOC_OVERRIDE_VALLOC
	if (!zero && lg_align == LG_PAGE) {
		JUNK_ALLOC(je_valloc(size));
	}
#endif
	int zero_flag = zero ? MALLOCX_ZERO : 0;
	JUNK_ALLOC(mallocx(size, zero_flag | MALLOCX_LG_ALIGN(lg_align)));
	JUNK_ALLOC(mallocx(size, zero_flag | MALLOCX_LG_ALIGN(lg_align)
	    | MALLOCX_TCACHE_NONE));
	if (lg_align >= LG_SIZEOF_PTR) {
		void *memalign_result;
		int err = posix_memalign(&memalign_result, (1 << lg_align),
		    size);
		assert_d_eq(err, 0, "");
		JUNK_ALLOC(memalign_result);
	}
}

TEST_BEGIN(test_junk_alloc_free) {
	bool zerovals[] = {false, true};
	size_t sizevals[] = {
		1, 8, 100, 1000, 100*1000
	/*
	 * Memory allocation failure is a real possibility in 32-bit mode.
	 * Rather than try to check in the face of resource exhaustion, we just
	 * rely more on the 64-bit tests.  This is a little bit white-box-y in
	 * the sense that this is only a good test strategy if we know that the
	 * junk pathways don't touch interact with the allocation selection
	 * mechanisms; but this is in fact the case.
	 */
#if LG_SIZEOF_PTR == 3
		    , 10 * 1000 * 1000
#endif
	};
	size_t lg_alignvals[] = {
		0, 4, 10, 15, 16, LG_PAGE
#if LG_SIZEOF_PTR == 3
		    , 20, 24
#endif
	};

#define JUNK_FREE(...)							\
	do {								\
		do_allocs(size, zero, lg_align);			\
		for (size_t n = 0; n < ptr_ind; n++) {			\
			void *ptr = ptrs[n];				\
			__VA_ARGS__;					\
			if (opt_junk_free) {				\
				assert_ptr_eq(ptr, last_junked_ptr,	\
				    "");				\
				assert_zu_eq(usize, last_junked_usize,	\
				    "");				\
			}						\
			reset();					\
		}							\
	} while (0)
	for (size_t i = 0; i < arraylen(zerovals); i++) {
		for (size_t j = 0; j < arraylen(sizevals); j++) {
			for (size_t k = 0; k < arraylen(lg_alignvals); k++) {
				bool zero = zerovals[i];
				size_t size = sizevals[j];
				size_t lg_align = lg_alignvals[k];
				size_t usize = nallocx(size,
				    MALLOCX_LG_ALIGN(lg_align));

				JUNK_FREE(free(ptr));
				JUNK_FREE(dallocx(ptr, 0));
				JUNK_FREE(dallocx(ptr, MALLOCX_TCACHE_NONE));
				JUNK_FREE(dallocx(ptr, MALLOCX_LG_ALIGN(
				    lg_align)));
				JUNK_FREE(sdallocx(ptr, usize, MALLOCX_LG_ALIGN(
				    lg_align)));
				JUNK_FREE(sdallocx(ptr, usize,
				    MALLOCX_TCACHE_NONE | MALLOCX_LG_ALIGN(lg_align)));
				if (opt_zero_realloc_action
				    == zero_realloc_action_free) {
					JUNK_FREE(realloc(ptr, 0));
				}
			}
		}
	}
}
TEST_END

TEST_BEGIN(test_realloc_expand) {
	char *volatile ptr;
	char *volatile expanded;

	test_skip_if(!opt_junk_alloc);

	/* Realloc */
	ptr = malloc(SC_SMALL_MAXCLASS);
	expanded = realloc(ptr, SC_LARGE_MINCLASS);
	expect_ptr_eq(last_junked_ptr, &expanded[SC_SMALL_MAXCLASS], "");
	expect_zu_eq(last_junked_usize,
	    SC_LARGE_MINCLASS - SC_SMALL_MAXCLASS, "");
	free(expanded);

	/* rallocx(..., 0) */
	ptr = malloc(SC_SMALL_MAXCLASS);
	expanded = rallocx(ptr, SC_LARGE_MINCLASS, 0);
	expect_ptr_eq(last_junked_ptr, &expanded[SC_SMALL_MAXCLASS], "");
	expect_zu_eq(last_junked_usize,
	    SC_LARGE_MINCLASS - SC_SMALL_MAXCLASS, "");
	free(expanded);

	/* rallocx(..., nonzero) */
	ptr = malloc(SC_SMALL_MAXCLASS);
	expanded = rallocx(ptr, SC_LARGE_MINCLASS, MALLOCX_TCACHE_NONE);
	expect_ptr_eq(last_junked_ptr, &expanded[SC_SMALL_MAXCLASS], "");
	expect_zu_eq(last_junked_usize,
	    SC_LARGE_MINCLASS - SC_SMALL_MAXCLASS, "");
	free(expanded);

	/* rallocx(..., MALLOCX_ZERO) */
	ptr = malloc(SC_SMALL_MAXCLASS);
	last_junked_ptr = (void *)-1;
	last_junked_usize = (size_t)-1;
	expanded = rallocx(ptr, SC_LARGE_MINCLASS, MALLOCX_ZERO);
	expect_ptr_eq(last_junked_ptr, (void *)-1, "");
	expect_zu_eq(last_junked_usize, (size_t)-1, "");
	free(expanded);

	/*
	 * Unfortunately, testing xallocx reliably is difficult to do portably
	 * (since allocations can be expanded / not expanded differently on
	 * different platforms.  We rely on manual inspection there -- the
	 * xallocx pathway is easy to inspect, though.
	 *
	 * Likewise, we don't test the shrinking pathways.  It's difficult to do
	 * so consistently (because of the risk of split failure or memory
	 * exhaustion, in which case no junking should happen).  This is fine
	 * -- junking is a best-effort debug mechanism in the first place.
	 */
}
TEST_END

int
main(void) {
	junk_alloc_callback = &test_junk;
	junk_free_callback = &test_junk;
	/*
	 * We check the last pointer junked.  If a reentrant call happens, that
	 * might be an internal allocation.
	 */
	return test_no_reentrancy(
	    test_junk_alloc_free,
	    test_realloc_expand);
}
