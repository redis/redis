#include "test/jemalloc_test.h"

/******************************************************************************/

/*
 * General purpose tool for examining random number distributions.
 *
 * Input -
 * (a) a random number generator, and
 * (b) the buckets:
 *     (1) number of buckets,
 *     (2) width of each bucket, in log scale,
 *     (3) expected mean and stddev of the count of random numbers in each
 *         bucket, and
 * (c) number of iterations to invoke the generator.
 *
 * The program generates the specified amount of random numbers, and assess how
 * well they conform to the expectations: for each bucket, output -
 * (a) the (given) expected mean and stddev,
 * (b) the actual count and any interesting level of deviation:
 *     (1) ~68% buckets should show no interesting deviation, meaning a
 *         deviation less than stddev from the expectation;
 *     (2) ~27% buckets should show '+' / '-', meaning a deviation in the range
 *         of [stddev, 2 * stddev) from the expectation;
 *     (3) ~4% buckets should show '++' / '--', meaning a deviation in the
 *         range of [2 * stddev, 3 * stddev) from the expectation; and
 *     (4) less than 0.3% buckets should show more than two '+'s / '-'s.
 *
 * Technical remarks:
 * (a) The generator is expected to output uint64_t numbers, so you might need
 *     to define a wrapper.
 * (b) The buckets must be of equal width and the lowest bucket starts at
 *     [0, 2^lg_bucket_width - 1).
 * (c) Any generated number >= n_bucket * 2^lg_bucket_width will be counted
 *     towards the last bucket; the expected mean and stddev provided should
 *     also reflect that.
 * (d) The number of iterations is advised to be determined so that the bucket
 *     with the minimal expected proportion gets a sufficient count.
 */

static void
fill(size_t a[], const size_t n, const size_t k) {
	for (size_t i = 0; i < n; ++i) {
		a[i] = k;
	}
}

static void
collect_buckets(uint64_t (*gen)(void *), void *opaque, size_t buckets[],
    const size_t n_bucket, const size_t lg_bucket_width, const size_t n_iter) {
	for (size_t i = 0; i < n_iter; ++i) {
		uint64_t num = gen(opaque);
		uint64_t bucket_id = num >> lg_bucket_width;
		if (bucket_id >= n_bucket) {
			bucket_id = n_bucket - 1;
		}
		++buckets[bucket_id];
	}
}

static void
print_buckets(const size_t buckets[], const size_t means[],
    const size_t stddevs[], const size_t n_bucket) {
	for (size_t i = 0; i < n_bucket; ++i) {
		malloc_printf("%zu:\tmean = %zu,\tstddev = %zu,\tbucket = %zu",
		    i, means[i], stddevs[i], buckets[i]);

		/* Make sure there's no overflow. */
		assert(buckets[i] + stddevs[i] >= stddevs[i]);
		assert(means[i] + stddevs[i] >= stddevs[i]);

		if (buckets[i] + stddevs[i] <= means[i]) {
			malloc_write(" ");
			for (size_t t = means[i] - buckets[i]; t >= stddevs[i];
			    t -= stddevs[i]) {
				malloc_write("-");
			}
		} else if (buckets[i] >= means[i] + stddevs[i]) {
			malloc_write(" ");
			for (size_t t = buckets[i] - means[i]; t >= stddevs[i];
			    t -= stddevs[i]) {
				malloc_write("+");
			}
		}
		malloc_write("\n");
	}
}

static void
bucket_analysis(uint64_t (*gen)(void *), void *opaque, size_t buckets[],
    const size_t means[], const size_t stddevs[], const size_t n_bucket,
    const size_t lg_bucket_width, const size_t n_iter) {
	for (size_t i = 1; i <= 3; ++i) {
		malloc_printf("round %zu\n", i);
		fill(buckets, n_bucket, 0);
		collect_buckets(gen, opaque, buckets, n_bucket,
		    lg_bucket_width, n_iter);
		print_buckets(buckets, means, stddevs, n_bucket);
	}
}

/* (Recommended) minimal bucket mean. */
#define MIN_BUCKET_MEAN 10000

/******************************************************************************/

/* Uniform random number generator. */

typedef struct uniform_gen_arg_s uniform_gen_arg_t;
struct uniform_gen_arg_s {
	uint64_t state;
	const unsigned lg_range;
};

static uint64_t
uniform_gen(void *opaque) {
	uniform_gen_arg_t *arg = (uniform_gen_arg_t *)opaque;
	return prng_lg_range_u64(&arg->state, arg->lg_range);
}

TEST_BEGIN(test_uniform) {
#define LG_N_BUCKET 5
#define N_BUCKET (1 << LG_N_BUCKET)

#define QUOTIENT_CEIL(n, d) (((n) - 1) / (d) + 1)

	const unsigned lg_range_test = 25;

	/*
	 * Mathematical tricks to guarantee that both mean and stddev are
	 * integers, and that the minimal bucket mean is at least
	 * MIN_BUCKET_MEAN.
	 */
	const size_t q = 1 << QUOTIENT_CEIL(LG_CEIL(QUOTIENT_CEIL(
	    MIN_BUCKET_MEAN, N_BUCKET * (N_BUCKET - 1))), 2);
	const size_t stddev = (N_BUCKET - 1) * q;
	const size_t mean = N_BUCKET * stddev * q;
	const size_t n_iter = N_BUCKET * mean;

	size_t means[N_BUCKET];
	fill(means, N_BUCKET, mean);
	size_t stddevs[N_BUCKET];
	fill(stddevs, N_BUCKET, stddev);

	uniform_gen_arg_t arg = {(uint64_t)(uintptr_t)&lg_range_test,
	    lg_range_test};
	size_t buckets[N_BUCKET];
	assert_zu_ge(lg_range_test, LG_N_BUCKET, "");
	const size_t lg_bucket_width = lg_range_test - LG_N_BUCKET;

	bucket_analysis(uniform_gen, &arg, buckets, means, stddevs,
	    N_BUCKET, lg_bucket_width, n_iter);

#undef LG_N_BUCKET
#undef N_BUCKET
#undef QUOTIENT_CEIL
}
TEST_END

/******************************************************************************/

/* Geometric random number generator; compiled only when prof is on. */

#ifdef JEMALLOC_PROF

/*
 * Fills geometric proportions and returns the minimal proportion.  See
 * comments in test_prof_sample for explanations for n_divide.
 */
static double
fill_geometric_proportions(double proportions[], const size_t n_bucket,
    const size_t n_divide) {
	assert(n_bucket > 0);
	assert(n_divide > 0);
	double x = 1.;
	for (size_t i = 0; i < n_bucket; ++i) {
		if (i == n_bucket - 1) {
			proportions[i] = x;
		} else {
			double y = x * exp(-1. / n_divide);
			proportions[i] = x - y;
			x = y;
		}
	}
	/*
	 * The minimal proportion is the smaller one of the last two
	 * proportions for geometric distribution.
	 */
	double min_proportion = proportions[n_bucket - 1];
	if (n_bucket >= 2 && proportions[n_bucket - 2] < min_proportion) {
		min_proportion = proportions[n_bucket - 2];
	}
	return min_proportion;
}

static size_t
round_to_nearest(const double x) {
	return (size_t)(x + .5);
}

static void
fill_references(size_t means[], size_t stddevs[], const double proportions[],
    const size_t n_bucket, const size_t n_iter) {
	for (size_t i = 0; i < n_bucket; ++i) {
		double x = n_iter * proportions[i];
		means[i] = round_to_nearest(x);
		stddevs[i] = round_to_nearest(sqrt(x * (1. - proportions[i])));
	}
}

static uint64_t
prof_sample_gen(void *opaque) {
	return prof_sample_new_event_wait((tsd_t *)opaque) - 1;
}

#endif /* JEMALLOC_PROF */

TEST_BEGIN(test_prof_sample) {
	test_skip_if(!config_prof);
#ifdef JEMALLOC_PROF

/* Number of divisions within [0, mean). */
#define LG_N_DIVIDE 3
#define N_DIVIDE (1 << LG_N_DIVIDE)

/* Coverage of buckets in terms of multiples of mean. */
#define LG_N_MULTIPLY 2
#define N_GEO_BUCKET (N_DIVIDE << LG_N_MULTIPLY)

	test_skip_if(!opt_prof);

	size_t lg_prof_sample_test = 25;

	size_t lg_prof_sample_orig = lg_prof_sample;
	assert_d_eq(mallctl("prof.reset", NULL, NULL, &lg_prof_sample_test,
	    sizeof(size_t)), 0, "");
	malloc_printf("lg_prof_sample = %zu\n", lg_prof_sample_test);

	double proportions[N_GEO_BUCKET + 1];
	const double min_proportion = fill_geometric_proportions(proportions,
	    N_GEO_BUCKET + 1, N_DIVIDE);
	const size_t n_iter = round_to_nearest(MIN_BUCKET_MEAN /
	    min_proportion);
	size_t means[N_GEO_BUCKET + 1];
	size_t stddevs[N_GEO_BUCKET + 1];
	fill_references(means, stddevs, proportions, N_GEO_BUCKET + 1, n_iter);

	tsd_t *tsd = tsd_fetch();
	assert_ptr_not_null(tsd, "");
	size_t buckets[N_GEO_BUCKET + 1];
	assert_zu_ge(lg_prof_sample, LG_N_DIVIDE, "");
	const size_t lg_bucket_width = lg_prof_sample - LG_N_DIVIDE;

	bucket_analysis(prof_sample_gen, tsd, buckets, means, stddevs,
	    N_GEO_BUCKET + 1, lg_bucket_width, n_iter);

	assert_d_eq(mallctl("prof.reset", NULL, NULL, &lg_prof_sample_orig,
	    sizeof(size_t)), 0, "");

#undef LG_N_DIVIDE
#undef N_DIVIDE
#undef LG_N_MULTIPLY
#undef N_GEO_BUCKET

#endif /* JEMALLOC_PROF */
}
TEST_END

/******************************************************************************/

int
main(void) {
	return test_no_reentrancy(
	    test_uniform,
	    test_prof_sample);
}
