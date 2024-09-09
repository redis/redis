#include "test/jemalloc_test.h"

#include "jemalloc/internal/rtree.h"

#define INVALID_ARENA_IND ((1U << MALLOCX_ARENA_BITS) - 1)

/* Potentially too large to safely place on the stack. */
rtree_t test_rtree;

TEST_BEGIN(test_rtree_read_empty) {
	tsdn_t *tsdn;

	tsdn = tsdn_fetch();

	base_t *base = base_new(tsdn, 0, &ehooks_default_extent_hooks,
	    /* metadata_use_hooks */ true);
	expect_ptr_not_null(base, "Unexpected base_new failure");

	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);
	expect_false(rtree_new(rtree, base, false),
	    "Unexpected rtree_new() failure");
	rtree_contents_t contents;
	expect_true(rtree_read_independent(tsdn, rtree, &rtree_ctx, PAGE,
	    &contents), "rtree_read_independent() should fail on empty rtree.");

	base_delete(tsdn, base);
}
TEST_END

#undef NTHREADS
#undef NITERS
#undef SEED

static edata_t *
alloc_edata(void) {
	void *ret = mallocx(sizeof(edata_t), MALLOCX_ALIGN(EDATA_ALIGNMENT));
	assert_ptr_not_null(ret, "Unexpected mallocx() failure");

	return ret;
}

TEST_BEGIN(test_rtree_extrema) {
	edata_t *edata_a, *edata_b;
	edata_a = alloc_edata();
	edata_b = alloc_edata();
	edata_init(edata_a, INVALID_ARENA_IND, NULL, SC_LARGE_MINCLASS,
	    false, sz_size2index(SC_LARGE_MINCLASS), 0,
	    extent_state_active, false, false, EXTENT_PAI_PAC, EXTENT_NOT_HEAD);
	edata_init(edata_b, INVALID_ARENA_IND, NULL, 0, false, SC_NSIZES, 0,
	    extent_state_active, false, false, EXTENT_PAI_PAC, EXTENT_NOT_HEAD);

	tsdn_t *tsdn = tsdn_fetch();

	base_t *base = base_new(tsdn, 0, &ehooks_default_extent_hooks,
	    /* metadata_use_hooks */ true);
	expect_ptr_not_null(base, "Unexpected base_new failure");

	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);
	expect_false(rtree_new(rtree, base, false),
	    "Unexpected rtree_new() failure");

	rtree_contents_t contents_a;
	contents_a.edata = edata_a;
	contents_a.metadata.szind = edata_szind_get(edata_a);
	contents_a.metadata.slab = edata_slab_get(edata_a);
	contents_a.metadata.is_head = edata_is_head_get(edata_a);
	contents_a.metadata.state = edata_state_get(edata_a);
	expect_false(rtree_write(tsdn, rtree, &rtree_ctx, PAGE, contents_a),
	    "Unexpected rtree_write() failure");
	expect_false(rtree_write(tsdn, rtree, &rtree_ctx, PAGE, contents_a),
	    "Unexpected rtree_write() failure");
	rtree_contents_t read_contents_a = rtree_read(tsdn, rtree, &rtree_ctx,
	    PAGE);
	expect_true(contents_a.edata == read_contents_a.edata
	    && contents_a.metadata.szind == read_contents_a.metadata.szind
	    && contents_a.metadata.slab == read_contents_a.metadata.slab
	    && contents_a.metadata.is_head == read_contents_a.metadata.is_head
	    && contents_a.metadata.state == read_contents_a.metadata.state,
	    "rtree_read() should return previously set value");

	rtree_contents_t contents_b;
	contents_b.edata = edata_b;
	contents_b.metadata.szind = edata_szind_get_maybe_invalid(edata_b);
	contents_b.metadata.slab = edata_slab_get(edata_b);
	contents_b.metadata.is_head = edata_is_head_get(edata_b);
	contents_b.metadata.state = edata_state_get(edata_b);
	expect_false(rtree_write(tsdn, rtree, &rtree_ctx, ~((uintptr_t)0),
	    contents_b), "Unexpected rtree_write() failure");
	rtree_contents_t read_contents_b = rtree_read(tsdn, rtree, &rtree_ctx,
	    ~((uintptr_t)0));
	assert_true(contents_b.edata == read_contents_b.edata
	    && contents_b.metadata.szind == read_contents_b.metadata.szind
	    && contents_b.metadata.slab == read_contents_b.metadata.slab
	    && contents_b.metadata.is_head == read_contents_b.metadata.is_head
	    && contents_b.metadata.state == read_contents_b.metadata.state,
	    "rtree_read() should return previously set value");

	base_delete(tsdn, base);
}
TEST_END

TEST_BEGIN(test_rtree_bits) {
	tsdn_t *tsdn = tsdn_fetch();
	base_t *base = base_new(tsdn, 0, &ehooks_default_extent_hooks,
	    /* metadata_use_hooks */ true);
	expect_ptr_not_null(base, "Unexpected base_new failure");

	uintptr_t keys[] = {PAGE, PAGE + 1,
	    PAGE + (((uintptr_t)1) << LG_PAGE) - 1};
	edata_t *edata_c = alloc_edata();
	edata_init(edata_c, INVALID_ARENA_IND, NULL, 0, false, SC_NSIZES, 0,
	    extent_state_active, false, false, EXTENT_PAI_PAC, EXTENT_NOT_HEAD);

	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);
	expect_false(rtree_new(rtree, base, false),
	    "Unexpected rtree_new() failure");

	for (unsigned i = 0; i < sizeof(keys)/sizeof(uintptr_t); i++) {
		rtree_contents_t contents;
		contents.edata = edata_c;
		contents.metadata.szind = SC_NSIZES;
		contents.metadata.slab = false;
		contents.metadata.is_head = false;
		contents.metadata.state = extent_state_active;

		expect_false(rtree_write(tsdn, rtree, &rtree_ctx, keys[i],
		    contents), "Unexpected rtree_write() failure");
		for (unsigned j = 0; j < sizeof(keys)/sizeof(uintptr_t); j++) {
			expect_ptr_eq(rtree_read(tsdn, rtree, &rtree_ctx,
			    keys[j]).edata, edata_c,
			    "rtree_edata_read() should return previously set "
			    "value and ignore insignificant key bits; i=%u, "
			    "j=%u, set key=%#"FMTxPTR", get key=%#"FMTxPTR, i,
			    j, keys[i], keys[j]);
		}
		expect_ptr_null(rtree_read(tsdn, rtree, &rtree_ctx,
		    (((uintptr_t)2) << LG_PAGE)).edata,
		    "Only leftmost rtree leaf should be set; i=%u", i);
		rtree_clear(tsdn, rtree, &rtree_ctx, keys[i]);
	}

	base_delete(tsdn, base);
}
TEST_END

TEST_BEGIN(test_rtree_random) {
#define NSET 16
#define SEED 42
	sfmt_t *sfmt = init_gen_rand(SEED);
	tsdn_t *tsdn = tsdn_fetch();

	base_t *base = base_new(tsdn, 0, &ehooks_default_extent_hooks,
	    /* metadata_use_hooks */ true);
	expect_ptr_not_null(base, "Unexpected base_new failure");

	uintptr_t keys[NSET];
	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);

	edata_t *edata_d = alloc_edata();
	edata_init(edata_d, INVALID_ARENA_IND, NULL, 0, false, SC_NSIZES, 0,
	    extent_state_active, false, false, EXTENT_PAI_PAC, EXTENT_NOT_HEAD);

	expect_false(rtree_new(rtree, base, false),
	    "Unexpected rtree_new() failure");

	for (unsigned i = 0; i < NSET; i++) {
		keys[i] = (uintptr_t)gen_rand64(sfmt);
		rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, rtree,
		    &rtree_ctx, keys[i], false, true);
		expect_ptr_not_null(elm,
		    "Unexpected rtree_leaf_elm_lookup() failure");
		rtree_contents_t contents;
		contents.edata = edata_d;
		contents.metadata.szind = SC_NSIZES;
		contents.metadata.slab = false;
		contents.metadata.is_head = false;
		contents.metadata.state = edata_state_get(edata_d);
		rtree_leaf_elm_write(tsdn, rtree, elm, contents);
		expect_ptr_eq(rtree_read(tsdn, rtree, &rtree_ctx,
		    keys[i]).edata, edata_d,
		    "rtree_edata_read() should return previously set value");
	}
	for (unsigned i = 0; i < NSET; i++) {
		expect_ptr_eq(rtree_read(tsdn, rtree, &rtree_ctx,
		    keys[i]).edata, edata_d,
		    "rtree_edata_read() should return previously set value, "
		    "i=%u", i);
	}

	for (unsigned i = 0; i < NSET; i++) {
		rtree_clear(tsdn, rtree, &rtree_ctx, keys[i]);
		expect_ptr_null(rtree_read(tsdn, rtree, &rtree_ctx,
		    keys[i]).edata,
		   "rtree_edata_read() should return previously set value");
	}
	for (unsigned i = 0; i < NSET; i++) {
		expect_ptr_null(rtree_read(tsdn, rtree, &rtree_ctx,
		    keys[i]).edata,
		    "rtree_edata_read() should return previously set value");
	}

	base_delete(tsdn, base);
	fini_gen_rand(sfmt);
#undef NSET
#undef SEED
}
TEST_END

static void
test_rtree_range_write(tsdn_t *tsdn, rtree_t *rtree, uintptr_t start,
    uintptr_t end) {
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);

	edata_t *edata_e = alloc_edata();
	edata_init(edata_e, INVALID_ARENA_IND, NULL, 0, false, SC_NSIZES, 0,
	    extent_state_active, false, false, EXTENT_PAI_PAC, EXTENT_NOT_HEAD);
	rtree_contents_t contents;
	contents.edata = edata_e;
	contents.metadata.szind = SC_NSIZES;
	contents.metadata.slab = false;
	contents.metadata.is_head = false;
	contents.metadata.state = extent_state_active;

	expect_false(rtree_write(tsdn, rtree, &rtree_ctx, start,
	    contents), "Unexpected rtree_write() failure");
	expect_false(rtree_write(tsdn, rtree, &rtree_ctx, end,
	    contents), "Unexpected rtree_write() failure");

	rtree_write_range(tsdn, rtree, &rtree_ctx, start, end, contents);
	for (uintptr_t i = 0; i < ((end - start) >> LG_PAGE); i++) {
		expect_ptr_eq(rtree_read(tsdn, rtree, &rtree_ctx,
		    start + (i << LG_PAGE)).edata, edata_e,
		    "rtree_edata_read() should return previously set value");
	}
	rtree_clear_range(tsdn, rtree, &rtree_ctx, start, end);
	rtree_leaf_elm_t *elm;
	for (uintptr_t i = 0; i < ((end - start) >> LG_PAGE); i++) {
		elm = rtree_leaf_elm_lookup(tsdn, rtree, &rtree_ctx,
		    start + (i << LG_PAGE), false, false);
		expect_ptr_not_null(elm, "Should have been initialized.");
		expect_ptr_null(rtree_leaf_elm_read(tsdn, rtree, elm,
		    false).edata, "Should have been cleared.");
	}
}

TEST_BEGIN(test_rtree_range) {
	tsdn_t *tsdn = tsdn_fetch();
	base_t *base = base_new(tsdn, 0, &ehooks_default_extent_hooks,
	    /* metadata_use_hooks */ true);
	expect_ptr_not_null(base, "Unexpected base_new failure");

	rtree_t *rtree = &test_rtree;
	expect_false(rtree_new(rtree, base, false),
	    "Unexpected rtree_new() failure");

	/* Not crossing rtree node boundary first. */
	uintptr_t start = ZU(1) << rtree_leaf_maskbits();
	uintptr_t end = start + (ZU(100) << LG_PAGE);
	test_rtree_range_write(tsdn, rtree, start, end);

	/* Crossing rtree node boundary. */
	start = (ZU(1) << rtree_leaf_maskbits()) - (ZU(10) << LG_PAGE);
	end = start + (ZU(100) << LG_PAGE);
	assert_ptr_ne((void *)rtree_leafkey(start), (void *)rtree_leafkey(end),
	    "The range should span across two rtree nodes");
	test_rtree_range_write(tsdn, rtree, start, end);

	base_delete(tsdn, base);
}
TEST_END

int
main(void) {
	return test(
	    test_rtree_read_empty,
	    test_rtree_extrema,
	    test_rtree_bits,
	    test_rtree_random,
	    test_rtree_range);
}
