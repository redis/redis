#pragma once
#include <stdlib.h>

/**
 * Adaptive histogram based on something like streaming k-means crossed with Q-digest.
 * The implementation is a direct descendent of MergingDigest
 * https://github.com/tdunning/t-digest/
 *
 * Copyright (c) 2021 Redis, All rights reserved.
 * Copyright (c) 2018 Andrew Werner, All rights reserved.
 *
 * The special characteristics of this algorithm are:
 *
 * - smaller summaries than Q-digest
 *
 * - provides part per million accuracy for extreme quantiles and typically &lt;1000 ppm accuracy
 * for middle quantiles
 *
 * - fast
 *
 * - simple
 *
 * - easy to adapt for use with map-reduce
 */

#define MM_PI 3.14159265358979323846

struct td_histogram {
    // compression is a setting used to configure the size of centroids when merged.
    double compression;

    double min;
    double max;

    // cap is the total size of nodes
    int cap;
    // merged_nodes is the number of merged nodes at the front of nodes.
    int merged_nodes;
    // unmerged_nodes is the number of buffered nodes.
    int unmerged_nodes;

    // we run the merge in reverse every other merge to avoid left-to-right bias in merging
    long long total_compressions;

    long long merged_weight;
    long long unmerged_weight;

    double *nodes_mean;
    long long *nodes_weight;
};

typedef struct td_histogram td_histogram_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate the memory, initialise the t-digest, and return the histogram as output parameter.
 * @param compression The compression parameter.
 * 100 is a common value for normal uses.
 * 1000 is extremely large.
 * The number of centroids retained will be a smallish (usually less than 10) multiple of this
 * number.
 * @return the histogram on success, NULL if `compression` is invalid (non-finite, <= 0, or so
 * large that the centroid capacity would overflow) or if allocation failed. Because NULL is a
 * valid return, `td_free(td_new(...))` is a safe cleanup idiom (td_free() tolerates NULL).
 */
td_histogram_t *td_new(double compression);

/**
 * Allocate the memory and initialise the t-digest.
 *
 * @param compression The compression parameter.
 * 100 is a common value for normal uses.
 * 1000 is extremely large.
 * The number of centroids retained will be a smallish (usually less than 10) multiple of this
 * number.
 * @param result Output parameter to capture allocated histogram. On success `*result` is set to
 * the new histogram; on failure `*result` is left untouched (never written), so a caller may seed
 * it with a sentinel to distinguish "not written" from NULL.
 * @return 0 on success, 1 if `compression` is invalid (non-finite, <= 0, or so large that the
 * centroid capacity would overflow) or if allocation failed.
 */
int td_init(double compression, td_histogram_t **result);

/**
 * Frees the memory associated with the t-digest.
 *
 * @param h The histogram you want to free. Passing NULL is allowed and is a no-op, so the
 * `td_free(td_new(...))` idiom is safe even when td_new() returns NULL.
 */
void td_free(td_histogram_t *h);

/**
 * Reset a histogram to zero - empty out a histogram and re-initialise it
 *
 * If you want to re-use an existing histogram, but reset everything back to zero, this
 * is the routine to use.
 *
 * @param h The histogram you want to reset to empty.
 *
 */
void td_reset(td_histogram_t *h);

/**
 * Adds a sample to a histogram.
 *
 * @param val The value to add. Must be finite: NaN has no ordering (would break the centroid
 * sort) and +/-Inf is not closed under the centroid-merge arithmetic (Inf-Inf yields NaN), so
 * both are rejected.
 * @param weight The weight of this point.
 * @return 0 on success, EINVAL if val is not finite (NaN or +/-Inf), EDOM if overflow was
 * detected as a consequence of adding the provided weight.
 *
 */
int td_add(td_histogram_t *h, double val, long long weight);

/**
 * Re-examines a t-digest to determine whether some centroids are redundant.  If your data are
 * perversely ordered, this may be a good idea.  Even if not, this may save 20% or so in space.
 *
 * The cost is roughly the same as adding as many data points as there are centroids.  This
 * is typically &lt; 10 * compression, but could be as high as 100 * compression.
 * This is a destructive operation that is not thread-safe.
 *
 * @param h The histogram you want to compress.
 * @return 0 on success, EDOM if overflow was detected as a consequence of adding the provided
 * weight. If overflow is detected the histogram is not changed.
 *
 */
int td_compress(td_histogram_t *h);

/**
 * Merges all of the values from 'from' to 'this' histogram.
 *
 * @param h "This" pointer
 * @param from Histogram to copy values from.
 * * @return 0 on success, EDOM if overflow was detected as a consequence of merging the the
 * provided histogram. If overflow is detected the original histogram is not detected.
 */
int td_merge(td_histogram_t *h, td_histogram_t *from);

/**
 * Returns the fraction of all points added which are &le; x.
 *
 * @param x The cutoff for the cdf.
 * @return The fraction of all data which is less or equal to x.
 */
double td_cdf(td_histogram_t *h, double x);

/**
 * Returns an estimate of the cutoff such that a specified fraction of the data
 * added to this TDigest would be less than or equal to the cutoff.
 *
 * @param q The desired fraction
 * @return The value x such that cdf(x) == q;
 */
double td_quantile(td_histogram_t *h, double q);

/**
 * Returns an estimate of the cutoff such that a specified fraction of the data
 * added to this TDigest would be less than or equal to the cutoffs.
 *
 * @param quantiles The ordered percentiles array to get the values for.
 * @param values Destination array containing the values at the given quantiles.
 * The values array should be allocated by the caller.
 * @return 0 on success, ENOMEM if the provided destination array is null.
 */
int td_quantiles(td_histogram_t *h, const double *quantiles, double *values, size_t length);

/**
 * Returns the trimmed mean ignoring values outside given cutoff upper and lower limits.
 *
 * @param leftmost_cut Fraction to cut off of the left tail of the distribution.
 * @param rightmost_cut Fraction to cut off of the right tail of the distribution.
 * @return The trimmed mean ignoring values outside given cutoff upper and lower limits;
 */
double td_trimmed_mean(td_histogram_t *h, double leftmost_cut, double rightmost_cut);

/**
 * Returns the trimmed mean ignoring values outside given a symmetric cutoff limits.
 *
 * @param proportion_to_cut Fraction to cut off of the left and right tails of the distribution.
 * @return The trimmed mean ignoring values outside given cutoff upper and lower limits;
 */
double td_trimmed_mean_symmetric(td_histogram_t *h, double proportion_to_cut);

/**
 * Returns the current compression factor.
 *
 * @return The compression factor originally used to set up the TDigest.
 */
int td_compression(td_histogram_t *h);

/**
 * Returns the number of points that have been added to this TDigest.
 *
 * @return The sum of the weights on all centroids.
 */
long long td_size(td_histogram_t *h);

/**
 * Returns the number of centroids being used by this TDigest.
 *
 * @return The number of centroids being used.
 */
int td_centroid_count(td_histogram_t *h);

/**
 * Get minimum value from the histogram.  Will return __DBL_MAX__ if the histogram
 * is empty.
 *
 * @param h "This" pointer
 */
double td_min(td_histogram_t *h);

/**
 * Get maximum value from the histogram.  Will return - __DBL_MAX__ if the histogram
 * is empty.
 *
 * @param h "This" pointer
 */
double td_max(td_histogram_t *h);

/**
 * Get the full centroids weight array for 'this' histogram.
 *
 * @param h "This" pointer
 *
 * @return The full centroids weight array.
 */
const long long *td_centroids_weight(td_histogram_t *h);

/**
 * Get the full centroids mean array for 'this' histogram.
 *
 * @param h "This" pointer
 *
 * @return The full centroids mean array.
 */
const double *td_centroids_mean(td_histogram_t *h);

/**
 * Get the centroid weight for 'this' histogram and 'pos'.
 *
 * @param h "This" pointer
 * @param pos centroid position.
 *
 * @return The centroid weight.
 */
long long td_centroids_weight_at(td_histogram_t *h, int pos);

/**
 * Get the centroid mean for 'this' histogram and 'pos'.
 *
 * @param h "This" pointer
 * @param pos centroid position.
 *
 * @return The centroid mean.
 */
double td_centroids_mean_at(td_histogram_t *h, int pos);

#ifdef __cplusplus
}
#endif
