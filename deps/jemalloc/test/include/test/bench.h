static inline void
time_func(timedelta_t *timer, uint64_t nwarmup, uint64_t niter,
    void (*func)(void)) {
	uint64_t i;

	for (i = 0; i < nwarmup; i++) {
		func();
	}
	timer_start(timer);
	for (i = 0; i < niter; i++) {
		func();
	}
	timer_stop(timer);
}

#define FMT_NSECS_BUF_SIZE 100
/* Print nanoseconds / iter into the buffer "buf". */
static inline void
fmt_nsecs(uint64_t usec, uint64_t iters, char *buf) {
	uint64_t nsec = usec * 1000;
	/* We'll display 3 digits after the decimal point. */
	uint64_t nsec1000 = nsec * 1000;
	uint64_t nsecs_per_iter1000 = nsec1000 / iters;
	uint64_t intpart = nsecs_per_iter1000 / 1000;
	uint64_t fracpart = nsecs_per_iter1000 % 1000;
	malloc_snprintf(buf, FMT_NSECS_BUF_SIZE, "%"FMTu64".%03"FMTu64, intpart,
	    fracpart);
}

static inline void
compare_funcs(uint64_t nwarmup, uint64_t niter, const char *name_a,
    void (*func_a), const char *name_b, void (*func_b)) {
	timedelta_t timer_a, timer_b;
	char ratio_buf[6];
	void *p;

	p = mallocx(1, 0);
	if (p == NULL) {
		test_fail("Unexpected mallocx() failure");
		return;
	}

	time_func(&timer_a, nwarmup, niter, func_a);
	time_func(&timer_b, nwarmup, niter, func_b);

	uint64_t usec_a = timer_usec(&timer_a);
	char buf_a[FMT_NSECS_BUF_SIZE];
	fmt_nsecs(usec_a, niter, buf_a);

	uint64_t usec_b = timer_usec(&timer_b);
	char buf_b[FMT_NSECS_BUF_SIZE];
	fmt_nsecs(usec_b, niter, buf_b);

	timer_ratio(&timer_a, &timer_b, ratio_buf, sizeof(ratio_buf));
	malloc_printf("%"FMTu64" iterations, %s=%"FMTu64"us (%s ns/iter), "
	    "%s=%"FMTu64"us (%s ns/iter), ratio=1:%s\n",
	    niter, name_a, usec_a, buf_a, name_b, usec_b, buf_b, ratio_buf);

	dallocx(p, 0);
}
