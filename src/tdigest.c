#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include "tdigest.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>

#include "zmalloc.h"
#define td_malloc_ ztrymalloc
#define td_calloc_(n, size) ztrycalloc((n) * (size))
#define td_free_ zfree

#define __td_max(x, y) (((x) > (y)) ? (x) : (y))
#define __td_min(x, y) (((x) < (y)) ? (x) : (y))

static inline double weighted_average_sorted(double x1, double w1, double x2, double w2) {
    const double x = (x1 * w1 + x2 * w2) / (w1 + w2);
    return __td_max(x1, __td_min(x, x2));
}

static inline bool _tdigest_long_long_add_safe(long long a, long long b) {
    if (b < 0) {
        return (a >= LLONG_MIN - b);
    } else {
        return (a <= __LONG_LONG_MAX__ - b);
    }
}

static inline double weighted_average(double x1, double w1, double x2, double w2) {
    if (x1 <= x2) {
        return weighted_average_sorted(x1, w1, x2, w2);
    } else {
        return weighted_average_sorted(x2, w2, x1, w1);
    }
}

static inline void swap(double *arr, int i, int j) {
    const double temp = arr[i];
    arr[i] = arr[j];
    arr[j] = temp;
}

static inline void swap_l(long long *arr, int i, int j) {
    const long long temp = arr[i];
    arr[i] = arr[j];
    arr[j] = temp;
}

// Optional key-comparison counter, used only by the complexity regression test.
// Zero-cost unless TD_INSTRUMENT_SORT is defined at build time.
#ifdef TD_INSTRUMENT_SORT
unsigned long long td_sort_comparisons = 0;
unsigned long long td_sort_heap_fallbacks = 0;
#define TD_SORT_CMP() (++td_sort_comparisons)
#define TD_SORT_FALLBACK() (++td_sort_heap_fallbacks)
#else
#define TD_SORT_CMP() ((void)0)
#define TD_SORT_FALLBACK() ((void)0)
#endif

// Counted key comparison `a < b`. Keeping the counter inside a dedicated helper (instead of a
// comma expression in the && operands) leaves the sort's boolean conditions side-effect free;
// it inlines to a plain `a < b` when TD_INSTRUMENT_SORT is off.
static inline bool td_key_lt(double a, double b) {
    TD_SORT_CMP();
    return a < b;
}

#define TD_INSORT_THRESHOLD 16

// Insertion sort of the inclusive range [lo, hi] (used for small ranges).
static void td_insertion_sort(double *means, long long *weights, int lo, int hi) {
    for (int i = lo + 1; i <= hi; i++) {
        const double m = means[i];
        const long long w = weights[i];
        int j = i - 1;
        while (j >= lo && td_key_lt(m, means[j])) {
            means[j + 1] = means[j];
            weights[j + 1] = weights[j];
            j--;
        }
        means[j + 1] = m;
        weights[j + 1] = w;
    }
}

// Max-heap sift-down over the range starting at `lo` (heap size `n`, root heap
// index `i`), keyed on means with weights moved in lock-step.
static void td_sift_down(double *means, long long *weights, int lo, int i, int n) {
    // Internal nodes are [0, n/2); index >= n/2 is a leaf. Testing leaf-ness BEFORE forming
    // 2*i+1 keeps the child index from overflowing signed int when n approaches INT_MAX (a
    // capacity #41 permits): for i < n/2, 2*i < n so 2*i+1 <= n and cannot overflow.
    while (i < n / 2) {
        int child = 2 * i + 1;
        if (child + 1 < n && td_key_lt(means[lo + child], means[lo + child + 1])) {
            child++;
        }
        if (!td_key_lt(means[lo + i], means[lo + child])) {
            break;
        }
        swap(means, lo + i, lo + child);
        swap_l(weights, lo + i, lo + child);
        i = child;
    }
}

// Heapsort of the inclusive range [lo, hi]: the O(n log n) worst-case fallback.
static void td_heap_sort(double *means, long long *weights, int lo, int hi) {
    const int n = hi - lo + 1;
    for (int i = n / 2 - 1; i >= 0; i--) {
        td_sift_down(means, weights, lo, i, n);
    }
    for (int end = n - 1; end > 0; end--) {
        swap(means, lo, lo + end);
        swap_l(weights, lo, lo + end);
        td_sift_down(means, weights, lo, 0, end);
    }
}

// Move the median of means[lo], means[mid], means[hi] into `mid` (weights follow)
// and return it, so an already-sorted / organ-pipe input does not pick a bad pivot.
static double td_median3(double *means, long long *weights, int lo, int mid, int hi) {
    if (td_key_lt(means[mid], means[lo])) {
        swap(means, lo, mid);
        swap_l(weights, lo, mid);
    }
    if (td_key_lt(means[hi], means[lo])) {
        swap(means, lo, hi);
        swap_l(weights, lo, hi);
    }
    if (td_key_lt(means[hi], means[mid])) {
        swap(means, mid, hi);
        swap_l(weights, mid, hi);
    }
    return means[mid];
}

/**
 * Introsort over two parallel arrays keyed on `means` (weights permuted in
 * lock-step). Guaranteed O(n log n) time and O(log n) stack:
 *   - 3-way (Dutch-flag) partitioning collapses runs of equal keys in a single
 *     pass -- t-digest's common duplicate-heavy input;
 *   - a median-of-three pivot avoids the trivial already-sorted worst case;
 *   - a recursion-depth limit falls back to heapsort, so an adversarial
 *     distinct-value permutation crafted to defeat the pivot (which a
 *     fixed-position pivot cannot resist) cannot force quadratic time;
 *   - small ranges finish with insertion sort;
 *   - recursing only the smaller side bounds the stack to O(log n).
 *
 * Note: this is NOT output-identical to a naive single-pivot sort for runs of
 * equal keys -- the order within an equal-key run differs, which can change how
 * weighted duplicate centroids are subsequently merged. Results stay within
 * t-digest's accuracy guarantee; exact centroid-for-centroid reproduction of the
 * old sorter is not preserved.
 */
static void td_introsort(double *means, long long *weights, int lo, int hi, int depth_limit) {
    while (hi - lo > TD_INSORT_THRESHOLD) {
        if (depth_limit == 0) {
            TD_SORT_FALLBACK();
            td_heap_sort(means, weights, lo, hi);
            return;
        }
        depth_limit--;
        const int mid = lo + (hi - lo) / 2;
        const double pivot = td_median3(means, weights, lo, mid, hi);
        // While scanning: [lo, lt) < pivot, [lt, i) == pivot, (gt, hi] > pivot.
        int lt = lo;
        int i = lo;
        int gt = hi;
        while (i <= gt) {
            const double v = means[i];
            if (td_key_lt(v, pivot)) {
                swap(means, i, lt);
                swap_l(weights, i, lt);
                lt++;
                i++;
            } else if (td_key_lt(pivot, v)) {
                swap(means, i, gt);
                swap_l(weights, i, gt);
                gt--;
            } else {
                i++;
            }
        }
        const int left_size = lt - lo;  // count of elements < pivot
        const int right_size = hi - gt; // count of elements > pivot
        // Recurse the smaller side, loop on the larger (bounds the stack).
        if (left_size < right_size) {
            if (left_size > 1) {
                td_introsort(means, weights, lo, lt - 1, depth_limit);
            }
            lo = gt + 1;
        } else {
            if (right_size > 1) {
                td_introsort(means, weights, gt + 1, hi, depth_limit);
            }
            hi = lt - 1;
        }
    }
    td_insertion_sort(means, weights, lo, hi);
}

static void td_qsort(double *means, long long *weights, unsigned int lo_u, unsigned int hi_u) {
    // Indices fit an int: the node arrays are sized by `cap` (an int field), so
    // the sorted range is always within [0, node_count).
    const int lo = (int)lo_u;
    const int hi = (int)hi_u;
    if (lo >= hi) {
        return;
    }
    // Depth limit = 2*floor(log2(n)); exceeding it hands the range to heapsort,
    // which is what guarantees the O(n log n) worst case.
    int depth_limit = 0;
    for (int t = hi - lo + 1; t > 1; t >>= 1) {
        depth_limit += 2;
    }
    td_introsort(means, weights, lo, hi, depth_limit);
}

static inline uint64_t cap_from_compression(uint64_t compression) {
    return (UINT64_C(6) * compression) + UINT64_C(10);
}

// Validate `compression` and compute the node-array capacity (cap = 6*compression + 10)
// entirely in 64-bit width, WITHOUT allocating. Factored out of td_init() so the
// accepted/rejected boundary can be probed in a test without committing tens of GiB of
// backing storage (at cap ~ INT_MAX the two 8-byte node arrays total ~34 GB / ~32 GiB).
// Returns 0 and writes *capacity on success; returns 1 on rejection and leaves *capacity
// untouched. Rejections: non-finite, <= 0, > INT_MAX, or a capacity that would overflow int
// or a size_t element count for either node array.
static inline int capacity_from_compression(double compression, size_t *capacity) {
    if (!isfinite(compression) || compression <= 0 || compression > INT_MAX) {
        return 1;
    }
    const uint64_t capacity64 = cap_from_compression((uint64_t)compression);
    if (capacity64 > INT_MAX || capacity64 > SIZE_MAX / sizeof(double) ||
        capacity64 > SIZE_MAX / sizeof(long long)) {
        return 1;
    }
    *capacity = (size_t)capacity64;
    return 0;
}

static inline bool should_td_compress(td_histogram_t *h) {
    return ((h->merged_nodes + h->unmerged_nodes) >= (h->cap - 1));
}

static inline int next_node(td_histogram_t *h) { return h->merged_nodes + h->unmerged_nodes; }

int td_compress(td_histogram_t *h);

static inline int _check_overflow(const double v) {
    // double-precision overflow detected on h->unmerged_weight
    if (v == INFINITY) {
        return EDOM;
    }
    return 0;
}

static inline int _check_td_overflow(const double new_unmerged_weight,
                                     const double new_total_weight) {
    // double-precision overflow detected on h->unmerged_weight
    if (new_unmerged_weight == INFINITY) {
        return EDOM;
    }
    if (new_total_weight == INFINITY) {
        return EDOM;
    }
    const double denom = 2 * MM_PI * new_total_weight * log(new_total_weight);
    if (denom == INFINITY) {
        return EDOM;
    }

    return 0;
}

int td_centroid_count(td_histogram_t *h) { return next_node(h); }

void td_reset(td_histogram_t *h) {
    if (!h) {
        return;
    }
    h->min = __DBL_MAX__;
    h->max = -h->min;
    h->merged_nodes = 0;
    h->merged_weight = 0;
    h->unmerged_nodes = 0;
    h->unmerged_weight = 0;
    h->total_compressions = 0;
}

int td_init(double compression, td_histogram_t **result) {

    // Validate compression and size the node arrays in 64-bit width before narrowing to int
    // (see capacity_from_compression). On rejection *result is left untouched.
    size_t capacity;
    if (capacity_from_compression(compression, &capacity) != 0) {
        return 1;
    }
    td_histogram_t *histogram;
    histogram = (td_histogram_t *)td_malloc_(sizeof(td_histogram_t));
    if (!histogram) {
        return 1;
    }
    histogram->nodes_mean = NULL;
    histogram->nodes_weight = NULL;
    histogram->cap = (int)capacity;
    histogram->compression = (double)compression;
    td_reset(histogram);
    histogram->nodes_mean = (double *)td_calloc_(capacity, sizeof(double));
    if (!histogram->nodes_mean) {
        td_free(histogram);
        return 1;
    }
    histogram->nodes_weight = (long long *)td_calloc_(capacity, sizeof(long long));
    if (!histogram->nodes_weight) {
        td_free(histogram);
        return 1;
    }
    *result = histogram;

    return 0;
}

td_histogram_t *td_new(double compression) {
    td_histogram_t *mdigest = NULL;
    td_init(compression, &mdigest);
    return mdigest;
}

void td_free(td_histogram_t *histogram) {
    // NULL guard: td_new() returns NULL for invalid compression (non-finite / <= 0 /
    // cap > INT_MAX) or allocation failure, so the idiomatic td_free(td_new(bad)) cleanup
    // would otherwise dereference NULL. (td_new()'s validation landed in #41.)
    if (!histogram) {
        return;
    }
    if (histogram->nodes_mean) {
        td_free_((void *)(histogram->nodes_mean));
    }
    if (histogram->nodes_weight) {
        td_free_((void *)(histogram->nodes_weight));
    }
    td_free_((void *)(histogram));
}

int td_merge(td_histogram_t *into, td_histogram_t *from) {
    if (td_compress(into) != 0)
        return EDOM;
    if (td_compress(from) != 0)
        return EDOM;
    const int pos = from->merged_nodes + from->unmerged_nodes;
    for (int i = 0; i < pos; i++) {
        const double mean = from->nodes_mean[i];
        const long long weight = from->nodes_weight[i];
        if (td_add(into, mean, weight) != 0) {
            return EDOM;
        }
    }
    return 0;
}

long long td_size(td_histogram_t *h) { return h->merged_weight + h->unmerged_weight; }

double td_cdf(td_histogram_t *h, double val) {
    td_compress(h);
    // no data to examine
    if (h->merged_nodes == 0) {
        return NAN;
    }
    // bellow lower bound
    if (val < h->min) {
        return 0;
    }
    // above upper bound
    if (val > h->max) {
        return 1;
    }
    if (h->merged_nodes == 1) {
        // exactly one centroid, should have max==min
        const double width = h->max - h->min;
        if (val - h->min <= width) {
            // min and max are too close together to do any viable interpolation
            return 0.5;
        } else {
            // interpolate if somehow we have weight > 0 and max != min
            return (val - h->min) / width;
        }
    }
    const int n = h->merged_nodes;
    // check for the left tail
    const double left_centroid_mean = h->nodes_mean[0];
    const double left_centroid_weight = (double)h->nodes_weight[0];
    const double merged_weight_d = (double)h->merged_weight;
    if (val < left_centroid_mean) {
        // note that this is different than h->nodes_mean[0] > min
        // ... this guarantees we divide by non-zero number and interpolation works
        const double width = left_centroid_mean - h->min;
        if (width > 0) {
            // must be a sample exactly at min
            if (val == h->min) {
                return 0.5 / merged_weight_d;
            } else {
                return (1 + (val - h->min) / width * (left_centroid_weight / 2 - 1)) /
                       merged_weight_d;
            }
        } else {
            // this should be redundant with the check val < h->min
            return 0;
        }
    }
    // and the right tail
    const double right_centroid_mean = h->nodes_mean[n - 1];
    const double right_centroid_weight = (double)h->nodes_weight[n - 1];
    if (val > right_centroid_mean) {
        const double width = h->max - right_centroid_mean;
        if (width > 0) {
            if (val == h->max) {
                return 1 - 0.5 / merged_weight_d;
            } else {
                // there has to be a single sample exactly at max
                const double dq = (1 + (h->max - val) / width * (right_centroid_weight / 2 - 1)) /
                                  merged_weight_d;
                return 1 - dq;
            }
        } else {
            return 1;
        }
    }
    // we know that there are at least two centroids and mean[0] < x < mean[n-1]
    // that means that there are either one or more consecutive centroids all at exactly x
    // or there are consecutive centroids, c0 < x < c1
    double weightSoFar = 0;
    for (int it = 0; it < n - 1; it++) {
        // weightSoFar does not include weight[it] yet
        if (h->nodes_mean[it] == val) {
            // we have one or more centroids == x, treat them as one
            // dw will accumulate the weight of all of the centroids at x
            double dw = 0;
            while (it < n && h->nodes_mean[it] == val) {
                dw += (double)h->nodes_weight[it];
                it++;
            }
            return (weightSoFar + dw / 2) / (double)h->merged_weight;
        } else if (h->nodes_mean[it] <= val && val < h->nodes_mean[it + 1]) {
            const double node_weight = (double)h->nodes_weight[it];
            const double node_weight_next = (double)h->nodes_weight[it + 1];
            const double node_mean = h->nodes_mean[it];
            const double node_mean_next = h->nodes_mean[it + 1];
            // landed between centroids ... check for floating point madness
            if (node_mean_next - node_mean > 0) {
                // note how we handle singleton centroids here
                // the point is that for singleton centroids, we know that their entire
                // weight is exactly at the centroid and thus shouldn't be involved in
                // interpolation
                double leftExcludedW = 0;
                double rightExcludedW = 0;
                if (node_weight == 1) {
                    if (node_weight_next == 1) {
                        // two singletons means no interpolation
                        // left singleton is in, right is out
                        return (weightSoFar + 1) / merged_weight_d;
                    } else {
                        leftExcludedW = 0.5;
                    }
                } else if (node_weight_next == 1) {
                    rightExcludedW = 0.5;
                }
                double dw = (node_weight + node_weight_next) / 2;

                // adjust endpoints for any singleton
                double dwNoSingleton = dw - leftExcludedW - rightExcludedW;

                double base = weightSoFar + node_weight / 2 + leftExcludedW;
                return (base + dwNoSingleton * (val - node_mean) / (node_mean_next - node_mean)) /
                       merged_weight_d;
            } else {
                // this is simply caution against floating point madness
                // it is conceivable that the centroids will be different
                // but too near to allow safe interpolation
                double dw = (node_weight + node_weight_next) / 2;
                return (weightSoFar + dw) / merged_weight_d;
            }
        } else {
            weightSoFar += (double)h->nodes_weight[it];
        }
    }
    return 1 - 0.5 / merged_weight_d;
}

static double td_internal_iterate_centroids_to_index(const td_histogram_t *h, const double index,
                                                     const double left_centroid_weight,
                                                     const int total_centroids, double *weightSoFar,
                                                     int *node_pos) {
    if (left_centroid_weight > 1 && index < left_centroid_weight / 2) {
        // there is a single sample at min so we interpolate with less weight
        return h->min + (index - 1) / (left_centroid_weight / 2 - 1) * (h->nodes_mean[0] - h->min);
    }

    // usually the last centroid will have unit weight so this test will make it moot
    if (index > h->merged_weight - 1) {
        return h->max;
    }

    // if the right-most centroid has more than one sample, we still know
    // that one sample occurred at max so we can do some interpolation
    const double right_centroid_weight = (double)h->nodes_weight[total_centroids - 1];
    const double right_centroid_mean = h->nodes_mean[total_centroids - 1];
    if (right_centroid_weight > 1 &&
        (double)h->merged_weight - index <= right_centroid_weight / 2) {
        return h->max - ((double)h->merged_weight - index - 1) / (right_centroid_weight / 2 - 1) *
                            (h->max - right_centroid_mean);
    }

    for (; *node_pos < total_centroids - 1; (*node_pos)++) {
        const int i = *node_pos;
        const double node_weight = (double)h->nodes_weight[i];
        const double node_weight_next = (double)h->nodes_weight[i + 1];
        const double node_mean = h->nodes_mean[i];
        const double node_mean_next = h->nodes_mean[i + 1];
        const double dw = (node_weight + node_weight_next) / 2;
        if (*weightSoFar + dw > index) {
            // centroids i and i+1 bracket our current point
            // check for unit weight
            double leftUnit = 0;
            if (node_weight == 1) {
                if (index - *weightSoFar < 0.5) {
                    // within the singleton's sphere
                    return node_mean;
                } else {
                    leftUnit = 0.5;
                }
            }
            double rightUnit = 0;
            if (node_weight_next == 1) {
                if (*weightSoFar + dw - index <= 0.5) {
                    // no interpolation needed near singleton
                    return node_mean_next;
                }
                rightUnit = 0.5;
            }
            const double z1 = index - *weightSoFar - leftUnit;
            const double z2 = *weightSoFar + dw - index - rightUnit;
            return weighted_average(node_mean, z2, node_mean_next, z1);
        }
        *weightSoFar += dw;
    }

    // weightSoFar = totalWeight - weight[total_centroids-1]/2 (very nearly)
    // so we interpolate out to max value ever seen
    const double z1 = index - h->merged_weight - right_centroid_weight / 2.0;
    const double z2 = right_centroid_weight / 2 - z1;
    return weighted_average(right_centroid_mean, z1, h->max, z2);
}

double td_quantile(td_histogram_t *h, double q) {
    td_compress(h);
    // q should be in [0,1]
    if (q < 0.0 || q > 1.0 || h->merged_nodes == 0) {
        return NAN;
    }
    // with one data point, all quantiles lead to Rome
    if (h->merged_nodes == 1) {
        return h->nodes_mean[0];
    }

    // if values were stored in a sorted array, index would be the offset we are interested in
    const double index = q * (double)h->merged_weight;

    // beyond the boundaries, we return min or max
    // usually, the first centroid will have unit weight so this will make it moot
    if (index < 1) {
        return h->min;
    }

    // we know that there are at least two centroids now
    const int n = h->merged_nodes;

    // if the left centroid has more than one sample, we still know
    // that one sample occurred at min so we can do some interpolation
    const double left_centroid_weight = (double)h->nodes_weight[0];

    // in between extremes we interpolate between centroids
    double weightSoFar = left_centroid_weight / 2;
    int i = 0;
    return td_internal_iterate_centroids_to_index(h, index, left_centroid_weight, n, &weightSoFar,
                                                  &i);
}

int td_quantiles(td_histogram_t *h, const double *quantiles, double *values, size_t length) {
    td_compress(h);

    if (NULL == quantiles || NULL == values) {
        return EINVAL;
    }

    const int n = h->merged_nodes;
    if (n == 0) {
        for (size_t i = 0; i < length; i++) {
            values[i] = NAN;
        }
        return 0;
    }
    if (n == 1) {
        for (size_t i = 0; i < length; i++) {
            const double requested_quantile = quantiles[i];

            // q should be in [0,1]
            if (requested_quantile < 0.0 || requested_quantile > 1.0) {
                values[i] = NAN;
            } else {
                // with one data point, all quantiles lead to Rome
                values[i] = h->nodes_mean[0];
            }
        }
        return 0;
    }

    // we know that there are at least two centroids now
    // if the left centroid has more than one sample, we still know
    // that one sample occurred at min so we can do some interpolation
    const double left_centroid_weight = (double)h->nodes_weight[0];

    // in between extremes we interpolate between centroids
    double weightSoFar = left_centroid_weight / 2;
    int node_pos = 0;

    // to avoid allocations we use the values array for intermediate computation
    // i.e. to store the expected cumulative count at each percentile
    for (size_t qpos = 0; qpos < length; qpos++) {
        const double index = quantiles[qpos] * (double)h->merged_weight;
        values[qpos] = td_internal_iterate_centroids_to_index(h, index, left_centroid_weight, n,
                                                              &weightSoFar, &node_pos);
    }
    return 0;
}

static double td_internal_trimmed_mean(const td_histogram_t *h, const double leftmost_weight,
                                       const double rightmost_weight) {
    double count_done = 0;
    double trimmed_sum = 0;
    double trimmed_count = 0;
    for (int i = 0; i < h->merged_nodes; i++) {

        const double n_weight = (double)h->nodes_weight[i];
        // Assume the whole centroid falls into the range
        double count_add = n_weight;

        // If we haven't reached the low threshold yet, skip appropriate part of the centroid.
        count_add -= __td_min(__td_max(0, leftmost_weight - count_done), count_add);

        // If we have reached the upper threshold, ignore the overflowing part of the centroid.

        count_add = __td_min(__td_max(0, rightmost_weight - count_done), count_add);

        // consider the whole centroid processed
        count_done += n_weight;

        // increment the sum / count
        trimmed_sum += h->nodes_mean[i] * count_add;
        trimmed_count += count_add;

        // break once we cross the high threshold
        if (count_done >= rightmost_weight)
            break;
    }

    return trimmed_sum / trimmed_count;
}

double td_trimmed_mean_symmetric(td_histogram_t *h, double proportion_to_cut) {
    td_compress(h);
    // proportion_to_cut should be in [0,1]
    if (h->merged_nodes == 0 || proportion_to_cut < 0.0 || proportion_to_cut > 1.0) {
        return NAN;
    }
    // with one data point, all values lead to Rome
    if (h->merged_nodes == 1) {
        return h->nodes_mean[0];
    }

    /* translate the percentiles to counts */
    const double leftmost_weight = floor((double)h->merged_weight * proportion_to_cut);
    const double rightmost_weight = ceil((double)h->merged_weight * (1.0 - proportion_to_cut));

    return td_internal_trimmed_mean(h, leftmost_weight, rightmost_weight);
}

double td_trimmed_mean(td_histogram_t *h, double leftmost_cut, double rightmost_cut) {
    td_compress(h);
    // leftmost_cut and rightmost_cut should be in [0,1]
    if (h->merged_nodes == 0 || leftmost_cut < 0.0 || leftmost_cut > 1.0 || rightmost_cut < 0.0 ||
        rightmost_cut > 1.0) {
        return NAN;
    }
    // with one data point, all values lead to Rome
    if (h->merged_nodes == 1) {
        return h->nodes_mean[0];
    }

    /* translate the percentiles to counts */
    const double leftmost_weight = floor((double)h->merged_weight * leftmost_cut);
    const double rightmost_weight = ceil((double)h->merged_weight * rightmost_cut);

    return td_internal_trimmed_mean(h, leftmost_weight, rightmost_weight);
}

int td_add(td_histogram_t *h, double mean, long long weight) {
    // Reject non-finite means before any mutation. NaN has no ordering, so it would leave a
    // partition unsorted and violate td_compress()'s sorted invariant. +/-Inf sorts fine but is
    // not closed under the centroid-merge arithmetic: merging two equal infinities computes
    // `delta = Inf - Inf = NaN`, poisoning a centroid mean (which then breaks a later sort).
    // Rejecting all non-finite input at ingest keeps every stored mean finite, matching the
    // reference t-digest (which rejects NaN in add()).
    if (!isfinite(mean)) {
        return EINVAL;
    }
    if (should_td_compress(h)) {
        const int overflow_res = td_compress(h);
        if (overflow_res != 0)
            return overflow_res;
    }
    const int pos = next_node(h);
    if (pos >= h->cap)
        return EDOM;
    if (_tdigest_long_long_add_safe(h->unmerged_weight, weight) == false)
        return EDOM;
    const long long new_unmerged_weight = h->unmerged_weight + weight;
    if (_tdigest_long_long_add_safe(new_unmerged_weight, h->merged_weight) == false)
        return EDOM;
    const long long new_total_weight = new_unmerged_weight + h->merged_weight;
    // double-precision overflow detected
    const int overflow_res =
        _check_td_overflow((double)new_unmerged_weight, (double)new_total_weight);
    if (overflow_res != 0)
        return overflow_res;

    if (mean < h->min) {
        h->min = mean;
    }
    if (mean > h->max) {
        h->max = mean;
    }
    h->nodes_mean[pos] = mean;
    h->nodes_weight[pos] = weight;
    h->unmerged_nodes++;
    h->unmerged_weight = new_unmerged_weight;
    return 0;
}

int td_compress(td_histogram_t *h) {
    if (h->unmerged_nodes == 0) {
        return 0;
    }
    int N = h->merged_nodes + h->unmerged_nodes;
    td_qsort(h->nodes_mean, h->nodes_weight, 0, N - 1);
    const double total_weight = (double)h->merged_weight + (double)h->unmerged_weight;
    // double-precision overflow detected
    const int overflow_res = _check_td_overflow((double)h->unmerged_weight, (double)total_weight);
    if (overflow_res != 0)
        return overflow_res;
    if (total_weight <= 1) {
        // Only move data if there are unmerged nodes to move
        if (h->unmerged_nodes > 0) {
            h->merged_nodes = h->merged_nodes + h->unmerged_nodes;
            h->merged_weight = total_weight;
            h->unmerged_nodes = 0;
            h->unmerged_weight = 0;
            h->total_compressions++;
        }
        return 0;
    }
    const double denom = 2 * MM_PI * total_weight * log(total_weight);
    if (_check_overflow(denom) != 0)
        return EDOM;

    // Compute the normalizer given compression and number of points.
    const double normalizer = h->compression / denom;
    if (_check_overflow(normalizer) != 0)
        return EDOM;
    int cur = 0;
    double weight_so_far = 0;

    for (int i = 1; i < N; i++) {
        const double proposed_weight = (double)h->nodes_weight[cur] + (double)h->nodes_weight[i];
        const double z = proposed_weight * normalizer;
        // quantile up to cur
        const double q0 = weight_so_far / total_weight;
        // quantile up to cur + i
        const double q2 = (weight_so_far + proposed_weight) / total_weight;
        // Convert  a quantile to the k-scale
        const bool should_add = (z <= (q0 * (1 - q0))) && (z <= (q2 * (1 - q2)));
        // next point will fit
        // so merge into existing centroid
        if (should_add) {
            h->nodes_weight[cur] += h->nodes_weight[i];
            const double delta = h->nodes_mean[i] - h->nodes_mean[cur];
            const double weighted_delta = (delta * h->nodes_weight[i]) / h->nodes_weight[cur];
            h->nodes_mean[cur] += weighted_delta;
        } else {
            weight_so_far += h->nodes_weight[cur];
            cur++;
            h->nodes_weight[cur] = h->nodes_weight[i];
            h->nodes_mean[cur] = h->nodes_mean[i];
        }
        if (cur != i) {
            h->nodes_weight[i] = 0;
            h->nodes_mean[i] = 0.0;
        }
    }
    h->merged_nodes = cur + 1;
    h->merged_weight = total_weight;
    h->unmerged_nodes = 0;
    h->unmerged_weight = 0;
    h->total_compressions++;
    return 0;
}

double td_min(td_histogram_t *h) { return h->min; }

double td_max(td_histogram_t *h) { return h->max; }

int td_compression(td_histogram_t *h) { return h->compression; }

const long long *td_centroids_weight(td_histogram_t *h) { return h->nodes_weight; }

const double *td_centroids_mean(td_histogram_t *h) { return h->nodes_mean; }

long long td_centroids_weight_at(td_histogram_t *h, int pos) { return h->nodes_weight[pos]; }

double td_centroids_mean_at(td_histogram_t *h, int pos) {
    if (pos < 0 || pos > h->merged_nodes) {
        return NAN;
    }
    return h->nodes_mean[pos];
}
