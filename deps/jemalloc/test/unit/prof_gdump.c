#include "test/jemalloc_test.h"

#ifdef JEMALLOC_PROF
const char *malloc_conf = "prof:true,prof_active:false,prof_gdump:true";
#endif

static bool did_prof_dump_open;

static int
prof_dump_open_intercept(bool propagate_err, const char *filename)
{
	int fd;

	did_prof_dump_open = true;

	fd = open("/dev/null", O_WRONLY);
	assert_d_ne(fd, -1, "Unexpected open() failure");

	return (fd);
}

TEST_BEGIN(test_gdump)
{
	bool active;
	void *p, *q;

	test_skip_if(!config_prof);

	active = true;
	assert_d_eq(mallctl("prof.active", NULL, NULL, &active, sizeof(active)),
	    0, "Unexpected mallctl failure while activating profiling");

	prof_dump_open = prof_dump_open_intercept;

	did_prof_dump_open = false;
	p = mallocx(chunksize, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	assert_true(did_prof_dump_open, "Expected a profile dump");

	did_prof_dump_open = false;
	q = mallocx(chunksize, 0);
	assert_ptr_not_null(q, "Unexpected mallocx() failure");
	assert_true(did_prof_dump_open, "Expected a profile dump");

	dallocx(p, 0);
	dallocx(q, 0);
}
TEST_END

int
main(void)
{

	return (test(
	    test_gdump));
}
