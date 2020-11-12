#include "test/jemalloc_test.h"

#include "jemalloc/internal/rtree.h"

rtree_node_alloc_t *rtree_node_alloc_orig;
rtree_node_dalloc_t *rtree_node_dalloc_orig;
rtree_leaf_alloc_t *rtree_leaf_alloc_orig;
rtree_leaf_dalloc_t *rtree_leaf_dalloc_orig;

/* Potentially too large to safely place on the stack. */
rtree_t test_rtree;

static rtree_node_elm_t *
rtree_node_alloc_intercept(tsdn_t *tsdn, rtree_t *rtree, size_t nelms) {
	rtree_node_elm_t *node;

	if (rtree != &test_rtree) {
		return rtree_node_alloc_orig(tsdn, rtree, nelms);
	}

	malloc_mutex_unlock(tsdn, &rtree->init_lock);
	node = (rtree_node_elm_t *)calloc(nelms, sizeof(rtree_node_elm_t));
	assert_ptr_not_null(node, "Unexpected calloc() failure");
	malloc_mutex_lock(tsdn, &rtree->init_lock);

	return node;
}

static void
rtree_node_dalloc_intercept(tsdn_t *tsdn, rtree_t *rtree,
    rtree_node_elm_t *node) {
	if (rtree != &test_rtree) {
		rtree_node_dalloc_orig(tsdn, rtree, node);
		return;
	}

	free(node);
}

static rtree_leaf_elm_t *
rtree_leaf_alloc_intercept(tsdn_t *tsdn, rtree_t *rtree, size_t nelms) {
	rtree_leaf_elm_t *leaf;

	if (rtree != &test_rtree) {
		return rtree_leaf_alloc_orig(tsdn, rtree, nelms);
	}

	malloc_mutex_unlock(tsdn, &rtree->init_lock);
	leaf = (rtree_leaf_elm_t *)calloc(nelms, sizeof(rtree_leaf_elm_t));
	assert_ptr_not_null(leaf, "Unexpected calloc() failure");
	malloc_mutex_lock(tsdn, &rtree->init_lock);

	return leaf;
}

static void
rtree_leaf_dalloc_intercept(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *leaf) {
	if (rtree != &test_rtree) {
		rtree_leaf_dalloc_orig(tsdn, rtree, leaf);
		return;
	}

	free(leaf);
}

TEST_BEGIN(test_rtree_read_empty) {
	tsdn_t *tsdn;

	tsdn = tsdn_fetch();

	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);
	assert_false(rtree_new(rtree, false), "Unexpected rtree_new() failure");
	assert_ptr_null(rtree_extent_read(tsdn, rtree, &rtree_ctx, PAGE,
	    false), "rtree_extent_read() should return NULL for empty tree");
	rtree_delete(tsdn, rtree);
}
TEST_END

#undef NTHREADS
#undef NITERS
#undef SEED

TEST_BEGIN(test_rtree_extrema) {
	extent_t extent_a, extent_b;
	extent_init(&extent_a, NULL, NULL, LARGE_MINCLASS, false,
	    sz_size2index(LARGE_MINCLASS), 0, extent_state_active, false,
	    false, true);
	extent_init(&extent_b, NULL, NULL, 0, false, NSIZES, 0,
	    extent_state_active, false, false, true);

	tsdn_t *tsdn = tsdn_fetch();

	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);
	assert_false(rtree_new(rtree, false), "Unexpected rtree_new() failure");

	assert_false(rtree_write(tsdn, rtree, &rtree_ctx, PAGE, &extent_a,
	    extent_szind_get(&extent_a), extent_slab_get(&extent_a)),
	    "Unexpected rtree_write() failure");
	rtree_szind_slab_update(tsdn, rtree, &rtree_ctx, PAGE,
	    extent_szind_get(&extent_a), extent_slab_get(&extent_a));
	assert_ptr_eq(rtree_extent_read(tsdn, rtree, &rtree_ctx, PAGE, true),
	    &extent_a,
	    "rtree_extent_read() should return previously set value");

	assert_false(rtree_write(tsdn, rtree, &rtree_ctx, ~((uintptr_t)0),
	    &extent_b, extent_szind_get_maybe_invalid(&extent_b),
	    extent_slab_get(&extent_b)), "Unexpected rtree_write() failure");
	assert_ptr_eq(rtree_extent_read(tsdn, rtree, &rtree_ctx,
	    ~((uintptr_t)0), true), &extent_b,
	    "rtree_extent_read() should return previously set value");

	rtree_delete(tsdn, rtree);
}
TEST_END

TEST_BEGIN(test_rtree_bits) {
	tsdn_t *tsdn = tsdn_fetch();

	uintptr_t keys[] = {PAGE, PAGE + 1,
	    PAGE + (((uintptr_t)1) << LG_PAGE) - 1};

	extent_t extent;
	extent_init(&extent, NULL, NULL, 0, false, NSIZES, 0,
	    extent_state_active, false, false, true);

	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);
	assert_false(rtree_new(rtree, false), "Unexpected rtree_new() failure");

	for (unsigned i = 0; i < sizeof(keys)/sizeof(uintptr_t); i++) {
		assert_false(rtree_write(tsdn, rtree, &rtree_ctx, keys[i],
		    &extent, NSIZES, false),
		    "Unexpected rtree_write() failure");
		for (unsigned j = 0; j < sizeof(keys)/sizeof(uintptr_t); j++) {
			assert_ptr_eq(rtree_extent_read(tsdn, rtree, &rtree_ctx,
			    keys[j], true), &extent,
			    "rtree_extent_read() should return previously set "
			    "value and ignore insignificant key bits; i=%u, "
			    "j=%u, set key=%#"FMTxPTR", get key=%#"FMTxPTR, i,
			    j, keys[i], keys[j]);
		}
		assert_ptr_null(rtree_extent_read(tsdn, rtree, &rtree_ctx,
		    (((uintptr_t)2) << LG_PAGE), false),
		    "Only leftmost rtree leaf should be set; i=%u", i);
		rtree_clear(tsdn, rtree, &rtree_ctx, keys[i]);
	}

	rtree_delete(tsdn, rtree);
}
TEST_END

TEST_BEGIN(test_rtree_random) {
#define NSET 16
#define SEED 42
	sfmt_t *sfmt = init_gen_rand(SEED);
	tsdn_t *tsdn = tsdn_fetch();
	uintptr_t keys[NSET];
	rtree_t *rtree = &test_rtree;
	rtree_ctx_t rtree_ctx;
	rtree_ctx_data_init(&rtree_ctx);

	extent_t extent;
	extent_init(&extent, NULL, NULL, 0, false, NSIZES, 0,
	    extent_state_active, false, false, true);

	assert_false(rtree_new(rtree, false), "Unexpected rtree_new() failure");

	for (unsigned i = 0; i < NSET; i++) {
		keys[i] = (uintptr_t)gen_rand64(sfmt);
		rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, rtree,
		    &rtree_ctx, keys[i], false, true);
		assert_ptr_not_null(elm,
		    "Unexpected rtree_leaf_elm_lookup() failure");
		rtree_leaf_elm_write(tsdn, rtree, elm, &extent, NSIZES, false);
		assert_ptr_eq(rtree_extent_read(tsdn, rtree, &rtree_ctx,
		    keys[i], true), &extent,
		    "rtree_extent_read() should return previously set value");
	}
	for (unsigned i = 0; i < NSET; i++) {
		assert_ptr_eq(rtree_extent_read(tsdn, rtree, &rtree_ctx,
		    keys[i], true), &extent,
		    "rtree_extent_read() should return previously set value, "
		    "i=%u", i);
	}

	for (unsigned i = 0; i < NSET; i++) {
		rtree_clear(tsdn, rtree, &rtree_ctx, keys[i]);
		assert_ptr_null(rtree_extent_read(tsdn, rtree, &rtree_ctx,
		    keys[i], true),
		   "rtree_extent_read() should return previously set value");
	}
	for (unsigned i = 0; i < NSET; i++) {
		assert_ptr_null(rtree_extent_read(tsdn, rtree, &rtree_ctx,
		    keys[i], true),
		    "rtree_extent_read() should return previously set value");
	}

	rtree_delete(tsdn, rtree);
	fini_gen_rand(sfmt);
#undef NSET
#undef SEED
}
TEST_END

int
main(void) {
	rtree_node_alloc_orig = rtree_node_alloc;
	rtree_node_alloc = rtree_node_alloc_intercept;
	rtree_node_dalloc_orig = rtree_node_dalloc;
	rtree_node_dalloc = rtree_node_dalloc_intercept;
	rtree_leaf_alloc_orig = rtree_leaf_alloc;
	rtree_leaf_alloc = rtree_leaf_alloc_intercept;
	rtree_leaf_dalloc_orig = rtree_leaf_dalloc;
	rtree_leaf_dalloc = rtree_leaf_dalloc_intercept;

	return test(
	    test_rtree_read_empty,
	    test_rtree_extrema,
	    test_rtree_bits,
	    test_rtree_random);
}
