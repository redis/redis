#include "test/jemalloc_test.h"

TEST_BEGIN(test_new_delete)
{
	tsd_t *tsd;
	ckh_t ckh;

	tsd = tsd_fetch();

	assert_false(ckh_new(tsd, &ckh, 2, ckh_string_hash, ckh_string_keycomp),
	    "Unexpected ckh_new() error");
	ckh_delete(tsd, &ckh);

	assert_false(ckh_new(tsd, &ckh, 3, ckh_pointer_hash,
	    ckh_pointer_keycomp), "Unexpected ckh_new() error");
	ckh_delete(tsd, &ckh);
}
TEST_END

TEST_BEGIN(test_count_insert_search_remove)
{
	tsd_t *tsd;
	ckh_t ckh;
	const char *strs[] = {
	    "a string",
	    "A string",
	    "a string.",
	    "A string."
	};
	const char *missing = "A string not in the hash table.";
	size_t i;

	tsd = tsd_fetch();

	assert_false(ckh_new(tsd, &ckh, 2, ckh_string_hash, ckh_string_keycomp),
	    "Unexpected ckh_new() error");
	assert_zu_eq(ckh_count(&ckh), 0,
	    "ckh_count() should return %zu, but it returned %zu", ZU(0),
	    ckh_count(&ckh));

	/* Insert. */
	for (i = 0; i < sizeof(strs)/sizeof(const char *); i++) {
		ckh_insert(tsd, &ckh, strs[i], strs[i]);
		assert_zu_eq(ckh_count(&ckh), i+1,
		    "ckh_count() should return %zu, but it returned %zu", i+1,
		    ckh_count(&ckh));
	}

	/* Search. */
	for (i = 0; i < sizeof(strs)/sizeof(const char *); i++) {
		union {
			void *p;
			const char *s;
		} k, v;
		void **kp, **vp;
		const char *ks, *vs;

		kp = (i & 1) ? &k.p : NULL;
		vp = (i & 2) ? &v.p : NULL;
		k.p = NULL;
		v.p = NULL;
		assert_false(ckh_search(&ckh, strs[i], kp, vp),
		    "Unexpected ckh_search() error");

		ks = (i & 1) ? strs[i] : (const char *)NULL;
		vs = (i & 2) ? strs[i] : (const char *)NULL;
		assert_ptr_eq((void *)ks, (void *)k.s, "Key mismatch, i=%zu",
		    i);
		assert_ptr_eq((void *)vs, (void *)v.s, "Value mismatch, i=%zu",
		    i);
	}
	assert_true(ckh_search(&ckh, missing, NULL, NULL),
	    "Unexpected ckh_search() success");

	/* Remove. */
	for (i = 0; i < sizeof(strs)/sizeof(const char *); i++) {
		union {
			void *p;
			const char *s;
		} k, v;
		void **kp, **vp;
		const char *ks, *vs;

		kp = (i & 1) ? &k.p : NULL;
		vp = (i & 2) ? &v.p : NULL;
		k.p = NULL;
		v.p = NULL;
		assert_false(ckh_remove(tsd, &ckh, strs[i], kp, vp),
		    "Unexpected ckh_remove() error");

		ks = (i & 1) ? strs[i] : (const char *)NULL;
		vs = (i & 2) ? strs[i] : (const char *)NULL;
		assert_ptr_eq((void *)ks, (void *)k.s, "Key mismatch, i=%zu",
		    i);
		assert_ptr_eq((void *)vs, (void *)v.s, "Value mismatch, i=%zu",
		    i);
		assert_zu_eq(ckh_count(&ckh),
		    sizeof(strs)/sizeof(const char *) - i - 1,
		    "ckh_count() should return %zu, but it returned %zu",
		        sizeof(strs)/sizeof(const char *) - i - 1,
		    ckh_count(&ckh));
	}

	ckh_delete(tsd, &ckh);
}
TEST_END

TEST_BEGIN(test_insert_iter_remove)
{
#define	NITEMS ZU(1000)
	tsd_t *tsd;
	ckh_t ckh;
	void **p[NITEMS];
	void *q, *r;
	size_t i;

	tsd = tsd_fetch();

	assert_false(ckh_new(tsd, &ckh, 2, ckh_pointer_hash,
	    ckh_pointer_keycomp), "Unexpected ckh_new() error");

	for (i = 0; i < NITEMS; i++) {
		p[i] = mallocx(i+1, 0);
		assert_ptr_not_null(p[i], "Unexpected mallocx() failure");
	}

	for (i = 0; i < NITEMS; i++) {
		size_t j;

		for (j = i; j < NITEMS; j++) {
			assert_false(ckh_insert(tsd, &ckh, p[j], p[j]),
			    "Unexpected ckh_insert() failure");
			assert_false(ckh_search(&ckh, p[j], &q, &r),
			    "Unexpected ckh_search() failure");
			assert_ptr_eq(p[j], q, "Key pointer mismatch");
			assert_ptr_eq(p[j], r, "Value pointer mismatch");
		}

		assert_zu_eq(ckh_count(&ckh), NITEMS,
		    "ckh_count() should return %zu, but it returned %zu",
		    NITEMS, ckh_count(&ckh));

		for (j = i + 1; j < NITEMS; j++) {
			assert_false(ckh_search(&ckh, p[j], NULL, NULL),
			    "Unexpected ckh_search() failure");
			assert_false(ckh_remove(tsd, &ckh, p[j], &q, &r),
			    "Unexpected ckh_remove() failure");
			assert_ptr_eq(p[j], q, "Key pointer mismatch");
			assert_ptr_eq(p[j], r, "Value pointer mismatch");
			assert_true(ckh_search(&ckh, p[j], NULL, NULL),
			    "Unexpected ckh_search() success");
			assert_true(ckh_remove(tsd, &ckh, p[j], &q, &r),
			    "Unexpected ckh_remove() success");
		}

		{
			bool seen[NITEMS];
			size_t tabind;

			memset(seen, 0, sizeof(seen));

			for (tabind = 0; !ckh_iter(&ckh, &tabind, &q, &r);) {
				size_t k;

				assert_ptr_eq(q, r, "Key and val not equal");

				for (k = 0; k < NITEMS; k++) {
					if (p[k] == q) {
						assert_false(seen[k],
						    "Item %zu already seen", k);
						seen[k] = true;
						break;
					}
				}
			}

			for (j = 0; j < i + 1; j++)
				assert_true(seen[j], "Item %zu not seen", j);
			for (; j < NITEMS; j++)
				assert_false(seen[j], "Item %zu seen", j);
		}
	}

	for (i = 0; i < NITEMS; i++) {
		assert_false(ckh_search(&ckh, p[i], NULL, NULL),
		    "Unexpected ckh_search() failure");
		assert_false(ckh_remove(tsd, &ckh, p[i], &q, &r),
		    "Unexpected ckh_remove() failure");
		assert_ptr_eq(p[i], q, "Key pointer mismatch");
		assert_ptr_eq(p[i], r, "Value pointer mismatch");
		assert_true(ckh_search(&ckh, p[i], NULL, NULL),
		    "Unexpected ckh_search() success");
		assert_true(ckh_remove(tsd, &ckh, p[i], &q, &r),
		    "Unexpected ckh_remove() success");
		dallocx(p[i], 0);
	}

	assert_zu_eq(ckh_count(&ckh), 0,
	    "ckh_count() should return %zu, but it returned %zu",
	    ZU(0), ckh_count(&ckh));
	ckh_delete(tsd, &ckh);
#undef NITEMS
}
TEST_END

int
main(void)
{

	return (test(
	    test_new_delete,
	    test_count_insert_search_remove,
	    test_insert_iter_remove));
}
