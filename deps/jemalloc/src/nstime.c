#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/nstime.h"

#include "jemalloc/internal/assert.h"

#define BILLION	UINT64_C(1000000000)
#define MILLION	UINT64_C(1000000)

static void
nstime_set_initialized(nstime_t *time) {
#ifdef JEMALLOC_DEBUG
	time->magic = NSTIME_MAGIC;
#endif
}

static void
nstime_assert_initialized(const nstime_t *time) {
#ifdef JEMALLOC_DEBUG
	/*
	 * Some parts (e.g. stats) rely on memset to zero initialize.  Treat
	 * these as valid initialization.
	 */
	assert(time->magic == NSTIME_MAGIC ||
	    (time->magic == 0 && time->ns == 0));
#endif
}

static void
nstime_pair_assert_initialized(const nstime_t *t1, const nstime_t *t2) {
	nstime_assert_initialized(t1);
	nstime_assert_initialized(t2);
}

static void
nstime_initialize_operand(nstime_t *time) {
	/*
	 * Operations like nstime_add may have the initial operand being zero
	 * initialized (covered by the assert below).  Full-initialize needed
	 * before changing it to non-zero.
	 */
	nstime_assert_initialized(time);
	nstime_set_initialized(time);
}

void
nstime_init(nstime_t *time, uint64_t ns) {
	nstime_set_initialized(time);
	time->ns = ns;
}

void
nstime_init2(nstime_t *time, uint64_t sec, uint64_t nsec) {
	nstime_set_initialized(time);
	time->ns = sec * BILLION + nsec;
}

uint64_t
nstime_ns(const nstime_t *time) {
	nstime_assert_initialized(time);
	return time->ns;
}

uint64_t
nstime_msec(const nstime_t *time) {
	nstime_assert_initialized(time);
	return time->ns / MILLION;
}

uint64_t
nstime_sec(const nstime_t *time) {
	nstime_assert_initialized(time);
	return time->ns / BILLION;
}

uint64_t
nstime_nsec(const nstime_t *time) {
	nstime_assert_initialized(time);
	return time->ns % BILLION;
}

void
nstime_copy(nstime_t *time, const nstime_t *source) {
	/* Source is required to be initialized. */
	nstime_assert_initialized(source);
	*time = *source;
	nstime_assert_initialized(time);
}

int
nstime_compare(const nstime_t *a, const nstime_t *b) {
	nstime_pair_assert_initialized(a, b);
	return (a->ns > b->ns) - (a->ns < b->ns);
}

void
nstime_add(nstime_t *time, const nstime_t *addend) {
	nstime_pair_assert_initialized(time, addend);
	assert(UINT64_MAX - time->ns >= addend->ns);

	nstime_initialize_operand(time);
	time->ns += addend->ns;
}

void
nstime_iadd(nstime_t *time, uint64_t addend) {
	nstime_assert_initialized(time);
	assert(UINT64_MAX - time->ns >= addend);

	nstime_initialize_operand(time);
	time->ns += addend;
}

void
nstime_subtract(nstime_t *time, const nstime_t *subtrahend) {
	nstime_pair_assert_initialized(time, subtrahend);
	assert(nstime_compare(time, subtrahend) >= 0);

	/* No initialize operand -- subtraction must be initialized. */
	time->ns -= subtrahend->ns;
}

void
nstime_isubtract(nstime_t *time, uint64_t subtrahend) {
	nstime_assert_initialized(time);
	assert(time->ns >= subtrahend);

	/* No initialize operand -- subtraction must be initialized. */
	time->ns -= subtrahend;
}

void
nstime_imultiply(nstime_t *time, uint64_t multiplier) {
	nstime_assert_initialized(time);
	assert((((time->ns | multiplier) & (UINT64_MAX << (sizeof(uint64_t) <<
	    2))) == 0) || ((time->ns * multiplier) / multiplier == time->ns));

	nstime_initialize_operand(time);
	time->ns *= multiplier;
}

void
nstime_idivide(nstime_t *time, uint64_t divisor) {
	nstime_assert_initialized(time);
	assert(divisor != 0);

	nstime_initialize_operand(time);
	time->ns /= divisor;
}

uint64_t
nstime_divide(const nstime_t *time, const nstime_t *divisor) {
	nstime_pair_assert_initialized(time, divisor);
	assert(divisor->ns != 0);

	/* No initialize operand -- *time itself remains unchanged. */
	return time->ns / divisor->ns;
}

/* Returns time since *past, w/o updating *past. */
uint64_t
nstime_ns_since(const nstime_t *past) {
	nstime_assert_initialized(past);

	nstime_t now;
	nstime_copy(&now, past);
	nstime_update(&now);

	assert(nstime_compare(&now, past) >= 0);
	return now.ns - past->ns;
}

#ifdef _WIN32
#  define NSTIME_MONOTONIC true
static void
nstime_get(nstime_t *time) {
	FILETIME ft;
	uint64_t ticks_100ns;

	GetSystemTimeAsFileTime(&ft);
	ticks_100ns = (((uint64_t)ft.dwHighDateTime) << 32) | ft.dwLowDateTime;

	nstime_init(time, ticks_100ns * 100);
}
#elif defined(JEMALLOC_HAVE_CLOCK_MONOTONIC_COARSE)
#  define NSTIME_MONOTONIC true
static void
nstime_get(nstime_t *time) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	nstime_init2(time, ts.tv_sec, ts.tv_nsec);
}
#elif defined(JEMALLOC_HAVE_CLOCK_MONOTONIC)
#  define NSTIME_MONOTONIC true
static void
nstime_get(nstime_t *time) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	nstime_init2(time, ts.tv_sec, ts.tv_nsec);
}
#elif defined(JEMALLOC_HAVE_MACH_ABSOLUTE_TIME)
#  define NSTIME_MONOTONIC true
static void
nstime_get(nstime_t *time) {
	nstime_init(time, mach_absolute_time());
}
#else
#  define NSTIME_MONOTONIC false
static void
nstime_get(nstime_t *time) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	nstime_init2(time, tv.tv_sec, tv.tv_usec * 1000);
}
#endif

static bool
nstime_monotonic_impl(void) {
	return NSTIME_MONOTONIC;
#undef NSTIME_MONOTONIC
}
nstime_monotonic_t *JET_MUTABLE nstime_monotonic = nstime_monotonic_impl;

prof_time_res_t opt_prof_time_res =
	prof_time_res_default;

const char *prof_time_res_mode_names[] = {
	"default",
	"high",
};


static void
nstime_get_realtime(nstime_t *time) {
#if defined(JEMALLOC_HAVE_CLOCK_REALTIME) && !defined(_WIN32)
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	nstime_init2(time, ts.tv_sec, ts.tv_nsec);
#else
	unreachable();
#endif
}

static void
nstime_prof_update_impl(nstime_t *time) {
	nstime_t old_time;

	nstime_copy(&old_time, time);

	if (opt_prof_time_res == prof_time_res_high) {
		nstime_get_realtime(time);
	} else {
		nstime_get(time);
	}
}
nstime_prof_update_t *JET_MUTABLE nstime_prof_update = nstime_prof_update_impl;

static void
nstime_update_impl(nstime_t *time) {
	nstime_t old_time;

	nstime_copy(&old_time, time);
	nstime_get(time);

	/* Handle non-monotonic clocks. */
	if (unlikely(nstime_compare(&old_time, time) > 0)) {
		nstime_copy(time, &old_time);
	}
}
nstime_update_t *JET_MUTABLE nstime_update = nstime_update_impl;

void
nstime_init_update(nstime_t *time) {
	nstime_init_zero(time);
	nstime_update(time);
}

void
nstime_prof_init_update(nstime_t *time) {
	nstime_init_zero(time);
	nstime_prof_update(time);
}


