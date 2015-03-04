#include "test/jemalloc_test.h"

TEST_BEGIN(test_rtree_get_empty)
{
	unsigned i;

	for (i = 1; i <= (sizeof(uintptr_t) << 3); i++) {
		rtree_t *rtree = rtree_new(i, imalloc, idalloc);
		assert_u_eq(rtree_get(rtree, 0), 0,
		    "rtree_get() should return NULL for empty tree");
		rtree_delete(rtree);
	}
}
TEST_END

TEST_BEGIN(test_rtree_extrema)
{
	unsigned i;

	for (i = 1; i <= (sizeof(uintptr_t) << 3); i++) {
		rtree_t *rtree = rtree_new(i, imalloc, idalloc);

		rtree_set(rtree, 0, 1);
		assert_u_eq(rtree_get(rtree, 0), 1,
		    "rtree_get() should return previously set value");

		rtree_set(rtree, ~((uintptr_t)0), 1);
		assert_u_eq(rtree_get(rtree, ~((uintptr_t)0)), 1,
		    "rtree_get() should return previously set value");

		rtree_delete(rtree);
	}
}
TEST_END

TEST_BEGIN(test_rtree_bits)
{
	unsigned i, j, k;

	for (i = 1; i < (sizeof(uintptr_t) << 3); i++) {
		uintptr_t keys[] = {0, 1,
		    (((uintptr_t)1) << (sizeof(uintptr_t)*8-i)) - 1};
		rtree_t *rtree = rtree_new(i, imalloc, idalloc);

		for (j = 0; j < sizeof(keys)/sizeof(uintptr_t); j++) {
			rtree_set(rtree, keys[j], 1);
			for (k = 0; k < sizeof(keys)/sizeof(uintptr_t); k++) {
				assert_u_eq(rtree_get(rtree, keys[k]), 1,
				    "rtree_get() should return previously set "
				    "value and ignore insignificant key bits; "
				    "i=%u, j=%u, k=%u, set key=%#"PRIxPTR", "
				    "get key=%#"PRIxPTR, i, j, k, keys[j],
				    keys[k]);
			}
			assert_u_eq(rtree_get(rtree,
			    (((uintptr_t)1) << (sizeof(uintptr_t)*8-i))), 0,
			    "Only leftmost rtree leaf should be set; "
			    "i=%u, j=%u", i, j);
			rtree_set(rtree, keys[j], 0);
		}

		rtree_delete(rtree);
	}
}
TEST_END

TEST_BEGIN(test_rtree_random)
{
	unsigned i;
	sfmt_t *sfmt;
#define	NSET 100
#define	SEED 42

	sfmt = init_gen_rand(SEED);
	for (i = 1; i <= (sizeof(uintptr_t) << 3); i++) {
		rtree_t *rtree = rtree_new(i, imalloc, idalloc);
		uintptr_t keys[NSET];
		unsigned j;

		for (j = 0; j < NSET; j++) {
			keys[j] = (uintptr_t)gen_rand64(sfmt);
			rtree_set(rtree, keys[j], 1);
			assert_u_eq(rtree_get(rtree, keys[j]), 1,
			    "rtree_get() should return previously set value");
		}
		for (j = 0; j < NSET; j++) {
			assert_u_eq(rtree_get(rtree, keys[j]), 1,
			    "rtree_get() should return previously set value");
		}

		for (j = 0; j < NSET; j++) {
			rtree_set(rtree, keys[j], 0);
			assert_u_eq(rtree_get(rtree, keys[j]), 0,
			    "rtree_get() should return previously set value");
		}
		for (j = 0; j < NSET; j++) {
			assert_u_eq(rtree_get(rtree, keys[j]), 0,
			    "rtree_get() should return previously set value");
		}

		rtree_delete(rtree);
	}
	fini_gen_rand(sfmt);
#undef NSET
#undef SEED
}
TEST_END

int
main(void)
{

	return (test(
	    test_rtree_get_empty,
	    test_rtree_extrema,
	    test_rtree_bits,
	    test_rtree_random));
}
