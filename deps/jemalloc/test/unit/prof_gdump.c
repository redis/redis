#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_sys.h"

static bool did_prof_dump_open;

static int
prof_dump_open_file_intercept(const char *filename, int mode) {
	int fd;

	did_prof_dump_open = true;

	fd = open("/dev/null", O_WRONLY);
	assert_d_ne(fd, -1, "Unexpected open() failure");

	return fd;
}

TEST_BEGIN(test_gdump) {
	test_skip_if(opt_hpa);
	bool active, gdump, gdump_old;
	void *p, *q, *r, *s;
	size_t sz;

	test_skip_if(!config_prof);

	active = true;
	expect_d_eq(mallctl("prof.active", NULL, NULL, (void *)&active,
	    sizeof(active)), 0,
	    "Unexpected mallctl failure while activating profiling");

	prof_dump_open_file = prof_dump_open_file_intercept;

	did_prof_dump_open = false;
	p = mallocx((1U << SC_LG_LARGE_MINCLASS), 0);
	expect_ptr_not_null(p, "Unexpected mallocx() failure");
	expect_true(did_prof_dump_open, "Expected a profile dump");

	did_prof_dump_open = false;
	q = mallocx((1U << SC_LG_LARGE_MINCLASS), 0);
	expect_ptr_not_null(q, "Unexpected mallocx() failure");
	expect_true(did_prof_dump_open, "Expected a profile dump");

	gdump = false;
	sz = sizeof(gdump_old);
	expect_d_eq(mallctl("prof.gdump", (void *)&gdump_old, &sz,
	    (void *)&gdump, sizeof(gdump)), 0,
	    "Unexpected mallctl failure while disabling prof.gdump");
	assert(gdump_old);
	did_prof_dump_open = false;
	r = mallocx((1U << SC_LG_LARGE_MINCLASS), 0);
	expect_ptr_not_null(q, "Unexpected mallocx() failure");
	expect_false(did_prof_dump_open, "Unexpected profile dump");

	gdump = true;
	sz = sizeof(gdump_old);
	expect_d_eq(mallctl("prof.gdump", (void *)&gdump_old, &sz,
	    (void *)&gdump, sizeof(gdump)), 0,
	    "Unexpected mallctl failure while enabling prof.gdump");
	assert(!gdump_old);
	did_prof_dump_open = false;
	s = mallocx((1U << SC_LG_LARGE_MINCLASS), 0);
	expect_ptr_not_null(q, "Unexpected mallocx() failure");
	expect_true(did_prof_dump_open, "Expected a profile dump");

	dallocx(p, 0);
	dallocx(q, 0);
	dallocx(r, 0);
	dallocx(s, 0);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_gdump);
}
