#include "test/jemalloc_test.h"

#define NBITS_TAB \
    NB( 1) \
    NB( 2) \
    NB( 3) \
    NB( 4) \
    NB( 5) \
    NB( 6) \
    NB( 7) \
    NB( 8) \
    NB( 9) \
    NB(10) \
    NB(11) \
    NB(12) \
    NB(13) \
    NB(14) \
    NB(15) \
    NB(16) \
    NB(17) \
    NB(18) \
    NB(19) \
    NB(20) \
    NB(21) \
    NB(22) \
    NB(23) \
    NB(24) \
    NB(25) \
    NB(26) \
    NB(27) \
    NB(28) \
    NB(29) \
    NB(30) \
    NB(31) \
    NB(32) \
    \
    NB(33) \
    NB(34) \
    NB(35) \
    NB(36) \
    NB(37) \
    NB(38) \
    NB(39) \
    NB(40) \
    NB(41) \
    NB(42) \
    NB(43) \
    NB(44) \
    NB(45) \
    NB(46) \
    NB(47) \
    NB(48) \
    NB(49) \
    NB(50) \
    NB(51) \
    NB(52) \
    NB(53) \
    NB(54) \
    NB(55) \
    NB(56) \
    NB(57) \
    NB(58) \
    NB(59) \
    NB(60) \
    NB(61) \
    NB(62) \
    NB(63) \
    NB(64) \
    NB(65) \
    \
    NB(126) \
    NB(127) \
    NB(128) \
    NB(129) \
    NB(130) \
    \
    NB(254) \
    NB(255) \
    NB(256) \
    NB(257) \
    NB(258) \
    \
    NB(510) \
    NB(511) \
    NB(512) \
    NB(513) \
    NB(514) \
    \
    NB(1024) \
    NB(2048) \
    NB(4096) \
    NB(8192) \
    NB(16384) \

static void
test_bitmap_initializer_body(const bitmap_info_t *binfo, size_t nbits) {
	bitmap_info_t binfo_dyn;
	bitmap_info_init(&binfo_dyn, nbits);

	assert_zu_eq(bitmap_size(binfo), bitmap_size(&binfo_dyn),
	    "Unexpected difference between static and dynamic initialization, "
	    "nbits=%zu", nbits);
	assert_zu_eq(binfo->nbits, binfo_dyn.nbits,
	    "Unexpected difference between static and dynamic initialization, "
	    "nbits=%zu", nbits);
#ifdef BITMAP_USE_TREE
	assert_u_eq(binfo->nlevels, binfo_dyn.nlevels,
	    "Unexpected difference between static and dynamic initialization, "
	    "nbits=%zu", nbits);
	{
		unsigned i;

		for (i = 0; i < binfo->nlevels; i++) {
			assert_zu_eq(binfo->levels[i].group_offset,
			    binfo_dyn.levels[i].group_offset,
			    "Unexpected difference between static and dynamic "
			    "initialization, nbits=%zu, level=%u", nbits, i);
		}
	}
#else
	assert_zu_eq(binfo->ngroups, binfo_dyn.ngroups,
	    "Unexpected difference between static and dynamic initialization");
#endif
}

TEST_BEGIN(test_bitmap_initializer) {
#define NB(nbits) {							\
		if (nbits <= BITMAP_MAXBITS) {				\
			bitmap_info_t binfo =				\
			    BITMAP_INFO_INITIALIZER(nbits);		\
			test_bitmap_initializer_body(&binfo, nbits);	\
		}							\
	}
	NBITS_TAB
#undef NB
}
TEST_END

static size_t
test_bitmap_size_body(const bitmap_info_t *binfo, size_t nbits,
    size_t prev_size) {
	size_t size = bitmap_size(binfo);
	assert_zu_ge(size, (nbits >> 3),
	    "Bitmap size is smaller than expected");
	assert_zu_ge(size, prev_size, "Bitmap size is smaller than expected");
	return size;
}

TEST_BEGIN(test_bitmap_size) {
	size_t nbits, prev_size;

	prev_size = 0;
	for (nbits = 1; nbits <= BITMAP_MAXBITS; nbits++) {
		bitmap_info_t binfo;
		bitmap_info_init(&binfo, nbits);
		prev_size = test_bitmap_size_body(&binfo, nbits, prev_size);
	}
#define NB(nbits) {							\
		bitmap_info_t binfo = BITMAP_INFO_INITIALIZER(nbits);	\
		prev_size = test_bitmap_size_body(&binfo, nbits,	\
		    prev_size);						\
	}
	prev_size = 0;
	NBITS_TAB
#undef NB
}
TEST_END

static void
test_bitmap_init_body(const bitmap_info_t *binfo, size_t nbits) {
	size_t i;
	bitmap_t *bitmap = (bitmap_t *)malloc(bitmap_size(binfo));
	assert_ptr_not_null(bitmap, "Unexpected malloc() failure");

	bitmap_init(bitmap, binfo, false);
	for (i = 0; i < nbits; i++) {
		assert_false(bitmap_get(bitmap, binfo, i),
		    "Bit should be unset");
	}

	bitmap_init(bitmap, binfo, true);
	for (i = 0; i < nbits; i++) {
		assert_true(bitmap_get(bitmap, binfo, i), "Bit should be set");
	}

	free(bitmap);
}

TEST_BEGIN(test_bitmap_init) {
	size_t nbits;

	for (nbits = 1; nbits <= BITMAP_MAXBITS; nbits++) {
		bitmap_info_t binfo;
		bitmap_info_init(&binfo, nbits);
		test_bitmap_init_body(&binfo, nbits);
	}
#define NB(nbits) {							\
		bitmap_info_t binfo = BITMAP_INFO_INITIALIZER(nbits);	\
		test_bitmap_init_body(&binfo, nbits);			\
	}
	NBITS_TAB
#undef NB
}
TEST_END

static void
test_bitmap_set_body(const bitmap_info_t *binfo, size_t nbits) {
	size_t i;
	bitmap_t *bitmap = (bitmap_t *)malloc(bitmap_size(binfo));
	assert_ptr_not_null(bitmap, "Unexpected malloc() failure");
	bitmap_init(bitmap, binfo, false);

	for (i = 0; i < nbits; i++) {
		bitmap_set(bitmap, binfo, i);
	}
	assert_true(bitmap_full(bitmap, binfo), "All bits should be set");
	free(bitmap);
}

TEST_BEGIN(test_bitmap_set) {
	size_t nbits;

	for (nbits = 1; nbits <= BITMAP_MAXBITS; nbits++) {
		bitmap_info_t binfo;
		bitmap_info_init(&binfo, nbits);
		test_bitmap_set_body(&binfo, nbits);
	}
#define NB(nbits) {							\
		bitmap_info_t binfo = BITMAP_INFO_INITIALIZER(nbits);	\
		test_bitmap_set_body(&binfo, nbits);			\
	}
	NBITS_TAB
#undef NB
}
TEST_END

static void
test_bitmap_unset_body(const bitmap_info_t *binfo, size_t nbits) {
	size_t i;
	bitmap_t *bitmap = (bitmap_t *)malloc(bitmap_size(binfo));
	assert_ptr_not_null(bitmap, "Unexpected malloc() failure");
	bitmap_init(bitmap, binfo, false);

	for (i = 0; i < nbits; i++) {
		bitmap_set(bitmap, binfo, i);
	}
	assert_true(bitmap_full(bitmap, binfo), "All bits should be set");
	for (i = 0; i < nbits; i++) {
		bitmap_unset(bitmap, binfo, i);
	}
	for (i = 0; i < nbits; i++) {
		bitmap_set(bitmap, binfo, i);
	}
	assert_true(bitmap_full(bitmap, binfo), "All bits should be set");
	free(bitmap);
}

TEST_BEGIN(test_bitmap_unset) {
	size_t nbits;

	for (nbits = 1; nbits <= BITMAP_MAXBITS; nbits++) {
		bitmap_info_t binfo;
		bitmap_info_init(&binfo, nbits);
		test_bitmap_unset_body(&binfo, nbits);
	}
#define NB(nbits) {							\
		bitmap_info_t binfo = BITMAP_INFO_INITIALIZER(nbits);	\
		test_bitmap_unset_body(&binfo, nbits);			\
	}
	NBITS_TAB
#undef NB
}
TEST_END

static void
test_bitmap_xfu_body(const bitmap_info_t *binfo, size_t nbits) {
	bitmap_t *bitmap = (bitmap_t *)malloc(bitmap_size(binfo));
	assert_ptr_not_null(bitmap, "Unexpected malloc() failure");
	bitmap_init(bitmap, binfo, false);

	/* Iteratively set bits starting at the beginning. */
	for (size_t i = 0; i < nbits; i++) {
		assert_zu_eq(bitmap_ffu(bitmap, binfo, 0), i,
		    "First unset bit should be just after previous first unset "
		    "bit");
		assert_zu_eq(bitmap_ffu(bitmap, binfo, (i > 0) ? i-1 : i), i,
		    "First unset bit should be just after previous first unset "
		    "bit");
		assert_zu_eq(bitmap_ffu(bitmap, binfo, i), i,
		    "First unset bit should be just after previous first unset "
		    "bit");
		assert_zu_eq(bitmap_sfu(bitmap, binfo), i,
		    "First unset bit should be just after previous first unset "
		    "bit");
	}
	assert_true(bitmap_full(bitmap, binfo), "All bits should be set");

	/*
	 * Iteratively unset bits starting at the end, and verify that
	 * bitmap_sfu() reaches the unset bits.
	 */
	for (size_t i = nbits - 1; i < nbits; i--) { /* (nbits..0] */
		bitmap_unset(bitmap, binfo, i);
		assert_zu_eq(bitmap_ffu(bitmap, binfo, 0), i,
		    "First unset bit should the bit previously unset");
		assert_zu_eq(bitmap_ffu(bitmap, binfo, (i > 0) ? i-1 : i), i,
		    "First unset bit should the bit previously unset");
		assert_zu_eq(bitmap_ffu(bitmap, binfo, i), i,
		    "First unset bit should the bit previously unset");
		assert_zu_eq(bitmap_sfu(bitmap, binfo), i,
		    "First unset bit should the bit previously unset");
		bitmap_unset(bitmap, binfo, i);
	}
	assert_false(bitmap_get(bitmap, binfo, 0), "Bit should be unset");

	/*
	 * Iteratively set bits starting at the beginning, and verify that
	 * bitmap_sfu() looks past them.
	 */
	for (size_t i = 1; i < nbits; i++) {
		bitmap_set(bitmap, binfo, i - 1);
		assert_zu_eq(bitmap_ffu(bitmap, binfo, 0), i,
		    "First unset bit should be just after the bit previously "
		    "set");
		assert_zu_eq(bitmap_ffu(bitmap, binfo, (i > 0) ? i-1 : i), i,
		    "First unset bit should be just after the bit previously "
		    "set");
		assert_zu_eq(bitmap_ffu(bitmap, binfo, i), i,
		    "First unset bit should be just after the bit previously "
		    "set");
		assert_zu_eq(bitmap_sfu(bitmap, binfo), i,
		    "First unset bit should be just after the bit previously "
		    "set");
		bitmap_unset(bitmap, binfo, i);
	}
	assert_zu_eq(bitmap_ffu(bitmap, binfo, 0), nbits - 1,
	    "First unset bit should be the last bit");
	assert_zu_eq(bitmap_ffu(bitmap, binfo, (nbits > 1) ? nbits-2 : nbits-1),
	    nbits - 1, "First unset bit should be the last bit");
	assert_zu_eq(bitmap_ffu(bitmap, binfo, nbits - 1), nbits - 1,
	    "First unset bit should be the last bit");
	assert_zu_eq(bitmap_sfu(bitmap, binfo), nbits - 1,
	    "First unset bit should be the last bit");
	assert_true(bitmap_full(bitmap, binfo), "All bits should be set");

	/*
	 * Bubble a "usu" pattern through the bitmap and verify that
	 * bitmap_ffu() finds the correct bit for all five min_bit cases.
	 */
	if (nbits >= 3) {
		for (size_t i = 0; i < nbits-2; i++) {
			bitmap_unset(bitmap, binfo, i);
			bitmap_unset(bitmap, binfo, i+2);
			if (i > 0) {
				assert_zu_eq(bitmap_ffu(bitmap, binfo, i-1), i,
				    "Unexpected first unset bit");
			}
			assert_zu_eq(bitmap_ffu(bitmap, binfo, i), i,
			    "Unexpected first unset bit");
			assert_zu_eq(bitmap_ffu(bitmap, binfo, i+1), i+2,
			    "Unexpected first unset bit");
			assert_zu_eq(bitmap_ffu(bitmap, binfo, i+2), i+2,
			    "Unexpected first unset bit");
			if (i + 3 < nbits) {
				assert_zu_eq(bitmap_ffu(bitmap, binfo, i+3),
				    nbits, "Unexpected first unset bit");
			}
			assert_zu_eq(bitmap_sfu(bitmap, binfo), i,
			    "Unexpected first unset bit");
			assert_zu_eq(bitmap_sfu(bitmap, binfo), i+2,
			    "Unexpected first unset bit");
		}
	}

	/*
	 * Unset the last bit, bubble another unset bit through the bitmap, and
	 * verify that bitmap_ffu() finds the correct bit for all four min_bit
	 * cases.
	 */
	if (nbits >= 3) {
		bitmap_unset(bitmap, binfo, nbits-1);
		for (size_t i = 0; i < nbits-1; i++) {
			bitmap_unset(bitmap, binfo, i);
			if (i > 0) {
				assert_zu_eq(bitmap_ffu(bitmap, binfo, i-1), i,
				    "Unexpected first unset bit");
			}
			assert_zu_eq(bitmap_ffu(bitmap, binfo, i), i,
			    "Unexpected first unset bit");
			assert_zu_eq(bitmap_ffu(bitmap, binfo, i+1), nbits-1,
			    "Unexpected first unset bit");
			assert_zu_eq(bitmap_ffu(bitmap, binfo, nbits-1),
			    nbits-1, "Unexpected first unset bit");

			assert_zu_eq(bitmap_sfu(bitmap, binfo), i,
			    "Unexpected first unset bit");
		}
		assert_zu_eq(bitmap_sfu(bitmap, binfo), nbits-1,
		    "Unexpected first unset bit");
	}

	free(bitmap);
}

TEST_BEGIN(test_bitmap_xfu) {
	size_t nbits;

	for (nbits = 1; nbits <= BITMAP_MAXBITS; nbits++) {
		bitmap_info_t binfo;
		bitmap_info_init(&binfo, nbits);
		test_bitmap_xfu_body(&binfo, nbits);
	}
#define NB(nbits) {							\
		bitmap_info_t binfo = BITMAP_INFO_INITIALIZER(nbits);	\
		test_bitmap_xfu_body(&binfo, nbits);			\
	}
	NBITS_TAB
#undef NB
}
TEST_END

int
main(void) {
	return test(
	    test_bitmap_initializer,
	    test_bitmap_size,
	    test_bitmap_init,
	    test_bitmap_set,
	    test_bitmap_unset,
	    test_bitmap_xfu);
}
