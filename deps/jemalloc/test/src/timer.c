#include "test/jemalloc_test.h"

void
timer_start(timedelta_t *timer) {
	nstime_init_update(&timer->t0);
}

void
timer_stop(timedelta_t *timer) {
	nstime_copy(&timer->t1, &timer->t0);
	nstime_update(&timer->t1);
}

uint64_t
timer_usec(const timedelta_t *timer) {
	nstime_t delta;

	nstime_copy(&delta, &timer->t1);
	nstime_subtract(&delta, &timer->t0);
	return nstime_ns(&delta) / 1000;
}

void
timer_ratio(timedelta_t *a, timedelta_t *b, char *buf, size_t buflen) {
	uint64_t t0 = timer_usec(a);
	uint64_t t1 = timer_usec(b);
	uint64_t mult;
	size_t i = 0;
	size_t j, n;

	/* Whole. */
	n = malloc_snprintf(&buf[i], buflen-i, "%"FMTu64, t0 / t1);
	i += n;
	if (i >= buflen) {
		return;
	}
	mult = 1;
	for (j = 0; j < n; j++) {
		mult *= 10;
	}

	/* Decimal. */
	n = malloc_snprintf(&buf[i], buflen-i, ".");
	i += n;

	/* Fraction. */
	while (i < buflen-1) {
		uint64_t round = (i+1 == buflen-1 && ((t0 * mult * 10 / t1) % 10
		    >= 5)) ? 1 : 0;
		n = malloc_snprintf(&buf[i], buflen-i,
		    "%"FMTu64, (t0 * mult / t1) % 10 + round);
		i += n;
		mult *= 10;
	}
}
