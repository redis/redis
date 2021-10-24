#include "test/jemalloc_test.h"

static bool did_prof_dump_open;

static int
prof_dump_open_intercept(bool propagate_err, const char *filename) {
	int fd;

	did_prof_dump_open = true;

	fd = open("/dev/null", O_WRONLY);
	assert_d_ne(fd, -1, "Unexpected open() failure");

	return fd;
}

TEST_BEGIN(test_gdump) {
	bool active, gdump, gdump_old;
	void *p, *q, *r, *s;
	size_t sz;

	test_skip_if(!config_prof);

	active = true;
	assert_d_eq(mallctl("prof.active", NULL, NULL, (void *)&active,
	    sizeof(active)), 0,
	    "Unexpected mallctl failure while activating profiling");

	prof_dump_open = prof_dump_open_intercept;

	did_prof_dump_open = false;
	p = mallocx((1U << SC_LG_LARGE_MINCLASS), 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	assert_true(did_prof_dump_open, "Expected a profile dump");

	did_prof_dump_open = false;
	q = mallocx((1U << SC_LG_LARGE_MINCLASS), 0);
	assert_ptr_not_null(q, "Unexpected mallocx() failure");
	assert_true(did_prof_dump_open, "Expected a profile dump");

	gdump = false;
	sz = sizeof(gdump_old);
	assert_d_eq(mallctl("prof.gdump", (void *)&gdump_old, &sz,
	    (void *)&gdump, sizeof(gdump)), 0,
	    "Unexpected mallctl failure while disabling prof.gdump");
	assert(gdump_old);
	did_prof_dump_open = false;
	r = mallocx((1U << SC_LG_LARGE_MINCLASS), 0);
	assert_ptr_not_null(q, "Unexpected mallocx() failure");
	assert_false(did_prof_dump_open, "Unexpected profile dump");

	gdump = true;
	sz = sizeof(gdump_old);
	assert_d_eq(mallctl("prof.gdump", (void *)&gdump_old, &sz,
	    (void *)&gdump, sizeof(gdump)), 0,
	    "Unexpected mallctl failure while enabling prof.gdump");
	assert(!gdump_old);
	did_prof_dump_open = false;
	s = mallocx((1U << SC_LG_LARGE_MINCLASS), 0);
	assert_ptr_not_null(q, "Unexpected mallocx() failure");
	assert_true(did_prof_dump_open, "Expected a profile dump");

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
