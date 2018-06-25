#include "test/jemalloc_test.h"

static int
prof_dump_open_intercept(bool propagate_err, const char *filename)
{
	int fd;

	fd = open("/dev/null", O_WRONLY);
	assert_d_ne(fd, -1, "Unexpected open() failure");

	return (fd);
}

static void
set_prof_active(bool active)
{

	assert_d_eq(mallctl("prof.active", NULL, NULL, (void *)&active,
	    sizeof(active)), 0, "Unexpected mallctl failure");
}

static size_t
get_lg_prof_sample(void)
{
	size_t lg_prof_sample;
	size_t sz = sizeof(size_t);

	assert_d_eq(mallctl("prof.lg_sample", (void *)&lg_prof_sample, &sz,
	    NULL, 0), 0,
	    "Unexpected mallctl failure while reading profiling sample rate");
	return (lg_prof_sample);
}

static void
do_prof_reset(size_t lg_prof_sample)
{
	assert_d_eq(mallctl("prof.reset", NULL, NULL,
	    (void *)&lg_prof_sample, sizeof(size_t)), 0,
	    "Unexpected mallctl failure while resetting profile data");
	assert_zu_eq(lg_prof_sample, get_lg_prof_sample(),
	    "Expected profile sample rate change");
}

TEST_BEGIN(test_prof_reset_basic)
{
	size_t lg_prof_sample_orig, lg_prof_sample, lg_prof_sample_next;
	size_t sz;
	unsigned i;

	test_skip_if(!config_prof);

	sz = sizeof(size_t);
	assert_d_eq(mallctl("opt.lg_prof_sample", (void *)&lg_prof_sample_orig,
	    &sz, NULL, 0), 0,
	    "Unexpected mallctl failure while reading profiling sample rate");
	assert_zu_eq(lg_prof_sample_orig, 0,
	    "Unexpected profiling sample rate");
	lg_prof_sample = get_lg_prof_sample();
	assert_zu_eq(lg_prof_sample_orig, lg_prof_sample,
	    "Unexpected disagreement between \"opt.lg_prof_sample\" and "
	    "\"prof.lg_sample\"");

	/* Test simple resets. */
	for (i = 0; i < 2; i++) {
		assert_d_eq(mallctl("prof.reset", NULL, NULL, NULL, 0), 0,
		    "Unexpected mallctl failure while resetting profile data");
		lg_prof_sample = get_lg_prof_sample();
		assert_zu_eq(lg_prof_sample_orig, lg_prof_sample,
		    "Unexpected profile sample rate change");
	}

	/* Test resets with prof.lg_sample changes. */
	lg_prof_sample_next = 1;
	for (i = 0; i < 2; i++) {
		do_prof_reset(lg_prof_sample_next);
		lg_prof_sample = get_lg_prof_sample();
		assert_zu_eq(lg_prof_sample, lg_prof_sample_next,
		    "Expected profile sample rate change");
		lg_prof_sample_next = lg_prof_sample_orig;
	}

	/* Make sure the test code restored prof.lg_sample. */
	lg_prof_sample = get_lg_prof_sample();
	assert_zu_eq(lg_prof_sample_orig, lg_prof_sample,
	    "Unexpected disagreement between \"opt.lg_prof_sample\" and "
	    "\"prof.lg_sample\"");
}
TEST_END

bool prof_dump_header_intercepted = false;
prof_cnt_t cnt_all_copy = {0, 0, 0, 0};
static bool
prof_dump_header_intercept(tsdn_t *tsdn, bool propagate_err,
    const prof_cnt_t *cnt_all)
{

	prof_dump_header_intercepted = true;
	memcpy(&cnt_all_copy, cnt_all, sizeof(prof_cnt_t));

	return (false);
}

TEST_BEGIN(test_prof_reset_cleanup)
{
	void *p;
	prof_dump_header_t *prof_dump_header_orig;

	test_skip_if(!config_prof);

	set_prof_active(true);

	assert_zu_eq(prof_bt_count(), 0, "Expected 0 backtraces");
	p = mallocx(1, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");
	assert_zu_eq(prof_bt_count(), 1, "Expected 1 backtrace");

	prof_dump_header_orig = prof_dump_header;
	prof_dump_header = prof_dump_header_intercept;
	assert_false(prof_dump_header_intercepted, "Unexpected intercept");

	assert_d_eq(mallctl("prof.dump", NULL, NULL, NULL, 0),
	    0, "Unexpected error while dumping heap profile");
	assert_true(prof_dump_header_intercepted, "Expected intercept");
	assert_u64_eq(cnt_all_copy.curobjs, 1, "Expected 1 allocation");

	assert_d_eq(mallctl("prof.reset", NULL, NULL, NULL, 0), 0,
	    "Unexpected error while resetting heap profile data");
	assert_d_eq(mallctl("prof.dump", NULL, NULL, NULL, 0),
	    0, "Unexpected error while dumping heap profile");
	assert_u64_eq(cnt_all_copy.curobjs, 0, "Expected 0 allocations");
	assert_zu_eq(prof_bt_count(), 1, "Expected 1 backtrace");

	prof_dump_header = prof_dump_header_orig;

	dallocx(p, 0);
	assert_zu_eq(prof_bt_count(), 0, "Expected 0 backtraces");

	set_prof_active(false);
}
TEST_END

#define	NTHREADS		4
#define	NALLOCS_PER_THREAD	(1U << 13)
#define	OBJ_RING_BUF_COUNT	1531
#define	RESET_INTERVAL		(1U << 10)
#define	DUMP_INTERVAL		3677
static void *
thd_start(void *varg)
{
	unsigned thd_ind = *(unsigned *)varg;
	unsigned i;
	void *objs[OBJ_RING_BUF_COUNT];

	memset(objs, 0, sizeof(objs));

	for (i = 0; i < NALLOCS_PER_THREAD; i++) {
		if (i % RESET_INTERVAL == 0) {
			assert_d_eq(mallctl("prof.reset", NULL, NULL, NULL, 0),
			    0, "Unexpected error while resetting heap profile "
			    "data");
		}

		if (i % DUMP_INTERVAL == 0) {
			assert_d_eq(mallctl("prof.dump", NULL, NULL, NULL, 0),
			    0, "Unexpected error while dumping heap profile");
		}

		{
			void **pp = &objs[i % OBJ_RING_BUF_COUNT];
			if (*pp != NULL) {
				dallocx(*pp, 0);
				*pp = NULL;
			}
			*pp = btalloc(1, thd_ind*NALLOCS_PER_THREAD + i);
			assert_ptr_not_null(*pp,
			    "Unexpected btalloc() failure");
		}
	}

	/* Clean up any remaining objects. */
	for (i = 0; i < OBJ_RING_BUF_COUNT; i++) {
		void **pp = &objs[i % OBJ_RING_BUF_COUNT];
		if (*pp != NULL) {
			dallocx(*pp, 0);
			*pp = NULL;
		}
	}

	return (NULL);
}

TEST_BEGIN(test_prof_reset)
{
	size_t lg_prof_sample_orig;
	thd_t thds[NTHREADS];
	unsigned thd_args[NTHREADS];
	unsigned i;
	size_t bt_count, tdata_count;

	test_skip_if(!config_prof);

	bt_count = prof_bt_count();
	assert_zu_eq(bt_count, 0,
	    "Unexpected pre-existing tdata structures");
	tdata_count = prof_tdata_count();

	lg_prof_sample_orig = get_lg_prof_sample();
	do_prof_reset(5);

	set_prof_active(true);

	for (i = 0; i < NTHREADS; i++) {
		thd_args[i] = i;
		thd_create(&thds[i], thd_start, (void *)&thd_args[i]);
	}
	for (i = 0; i < NTHREADS; i++)
		thd_join(thds[i], NULL);

	assert_zu_eq(prof_bt_count(), bt_count,
	    "Unexpected bactrace count change");
	assert_zu_eq(prof_tdata_count(), tdata_count,
	    "Unexpected remaining tdata structures");

	set_prof_active(false);

	do_prof_reset(lg_prof_sample_orig);
}
TEST_END
#undef NTHREADS
#undef NALLOCS_PER_THREAD
#undef OBJ_RING_BUF_COUNT
#undef RESET_INTERVAL
#undef DUMP_INTERVAL

/* Test sampling at the same allocation site across resets. */
#define	NITER 10
TEST_BEGIN(test_xallocx)
{
	size_t lg_prof_sample_orig;
	unsigned i;
	void *ptrs[NITER];

	test_skip_if(!config_prof);

	lg_prof_sample_orig = get_lg_prof_sample();
	set_prof_active(true);

	/* Reset profiling. */
	do_prof_reset(0);

	for (i = 0; i < NITER; i++) {
		void *p;
		size_t sz, nsz;

		/* Reset profiling. */
		do_prof_reset(0);

		/* Allocate small object (which will be promoted). */
		p = ptrs[i] = mallocx(1, 0);
		assert_ptr_not_null(p, "Unexpected mallocx() failure");

		/* Reset profiling. */
		do_prof_reset(0);

		/* Perform successful xallocx(). */
		sz = sallocx(p, 0);
		assert_zu_eq(xallocx(p, sz, 0, 0), sz,
		    "Unexpected xallocx() failure");

		/* Perform unsuccessful xallocx(). */
		nsz = nallocx(sz+1, 0);
		assert_zu_eq(xallocx(p, nsz, 0, 0), sz,
		    "Unexpected xallocx() success");
	}

	for (i = 0; i < NITER; i++) {
		/* dallocx. */
		dallocx(ptrs[i], 0);
	}

	set_prof_active(false);
	do_prof_reset(lg_prof_sample_orig);
}
TEST_END
#undef NITER

int
main(void)
{

	/* Intercept dumping prior to running any tests. */
	prof_dump_open = prof_dump_open_intercept;

	return (test(
	    test_prof_reset_basic,
	    test_prof_reset_cleanup,
	    test_prof_reset,
	    test_xallocx));
}
