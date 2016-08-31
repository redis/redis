#include "test/jemalloc_test.h"

TEST_BEGIN(test_bitmap_size)
{
	size_t i, prev_size;

	prev_size = 0;
	for (i = 1; i <= BITMAP_MAXBITS; i++) {
		size_t size = bitmap_size(i);
		assert_true(size >= prev_size,
		    "Bitmap size is smaller than expected");
		prev_size = size;
	}
}
TEST_END

TEST_BEGIN(test_bitmap_init)
{
	size_t i;

	for (i = 1; i <= BITMAP_MAXBITS; i++) {
		bitmap_info_t binfo;
		bitmap_info_init(&binfo, i);
		{
			size_t j;
			bitmap_t *bitmap = (bitmap_t *)malloc(sizeof(bitmap_t) *
				bitmap_info_ngroups(&binfo));
			bitmap_init(bitmap, &binfo);

			for (j = 0; j < i; j++) {
				assert_false(bitmap_get(bitmap, &binfo, j),
				    "Bit should be unset");
			}
			free(bitmap);
		}
	}
}
TEST_END

TEST_BEGIN(test_bitmap_set)
{
	size_t i;

	for (i = 1; i <= BITMAP_MAXBITS; i++) {
		bitmap_info_t binfo;
		bitmap_info_init(&binfo, i);
		{
			size_t j;
			bitmap_t *bitmap = (bitmap_t *)malloc(sizeof(bitmap_t) *
				bitmap_info_ngroups(&binfo));
			bitmap_init(bitmap, &binfo);

			for (j = 0; j < i; j++)
				bitmap_set(bitmap, &binfo, j);
			assert_true(bitmap_full(bitmap, &binfo),
			    "All bits should be set");
			free(bitmap);
		}
	}
}
TEST_END

TEST_BEGIN(test_bitmap_unset)
{
	size_t i;

	for (i = 1; i <= BITMAP_MAXBITS; i++) {
		bitmap_info_t binfo;
		bitmap_info_init(&binfo, i);
		{
			size_t j;
			bitmap_t *bitmap = (bitmap_t *)malloc(sizeof(bitmap_t) *
				bitmap_info_ngroups(&binfo));
			bitmap_init(bitmap, &binfo);

			for (j = 0; j < i; j++)
				bitmap_set(bitmap, &binfo, j);
			assert_true(bitmap_full(bitmap, &binfo),
			    "All bits should be set");
			for (j = 0; j < i; j++)
				bitmap_unset(bitmap, &binfo, j);
			for (j = 0; j < i; j++)
				bitmap_set(bitmap, &binfo, j);
			assert_true(bitmap_full(bitmap, &binfo),
			    "All bits should be set");
			free(bitmap);
		}
	}
}
TEST_END

TEST_BEGIN(test_bitmap_sfu)
{
	size_t i;

	for (i = 1; i <= BITMAP_MAXBITS; i++) {
		bitmap_info_t binfo;
		bitmap_info_init(&binfo, i);
		{
			ssize_t j;
			bitmap_t *bitmap = (bitmap_t *)malloc(sizeof(bitmap_t) *
				bitmap_info_ngroups(&binfo));
			bitmap_init(bitmap, &binfo);

			/* Iteratively set bits starting at the beginning. */
			for (j = 0; j < i; j++) {
				assert_zd_eq(bitmap_sfu(bitmap, &binfo), j,
				    "First unset bit should be just after "
				    "previous first unset bit");
			}
			assert_true(bitmap_full(bitmap, &binfo),
			    "All bits should be set");

			/*
			 * Iteratively unset bits starting at the end, and
			 * verify that bitmap_sfu() reaches the unset bits.
			 */
			for (j = i - 1; j >= 0; j--) {
				bitmap_unset(bitmap, &binfo, j);
				assert_zd_eq(bitmap_sfu(bitmap, &binfo), j,
				    "First unset bit should the bit previously "
				    "unset");
				bitmap_unset(bitmap, &binfo, j);
			}
			assert_false(bitmap_get(bitmap, &binfo, 0),
			    "Bit should be unset");

			/*
			 * Iteratively set bits starting at the beginning, and
			 * verify that bitmap_sfu() looks past them.
			 */
			for (j = 1; j < i; j++) {
				bitmap_set(bitmap, &binfo, j - 1);
				assert_zd_eq(bitmap_sfu(bitmap, &binfo), j,
				    "First unset bit should be just after the "
				    "bit previously set");
				bitmap_unset(bitmap, &binfo, j);
			}
			assert_zd_eq(bitmap_sfu(bitmap, &binfo), i - 1,
			    "First unset bit should be the last bit");
			assert_true(bitmap_full(bitmap, &binfo),
			    "All bits should be set");
			free(bitmap);
		}
	}
}
TEST_END

int
main(void)
{

	return (test(
	    test_bitmap_size,
	    test_bitmap_init,
	    test_bitmap_set,
	    test_bitmap_unset,
	    test_bitmap_sfu));
}
