/*
 * Gate data type: statistical core.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * The statistics are described in gate_stats.h. Two rules shape the code:
 *
 * FAIL CLOSED. Everything here sits on the path that decides whether a cheap
 * prediction may replace the teacher. The header states the valid domain of
 * every argument and the caller is expected to respect it, but a caller bug
 * or a corrupted value must never turn into a certification. So for inputs
 * outside the documented domain each function returns the answer that
 * cannot certify (a NaN increment, an unreachable threshold, a zero budget)
 * or, for the drift window, the answer that raises the alarm.
 *
 * NO STATE, NO HEAP. Every function works on caller owned structures and
 * nothing here allocates, so there is no failure path to handle.
 */

#include <math.h>
#include <string.h>
#include "gate_stats.h"

/* The fail closed paths are comparisons with NaN and infinity. Under
 * -ffast-math / -ffinite-math-only the compiler is allowed to assume those
 * values do not exist and to delete the checks. */
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "gate_stats.c needs IEEE NaN and infinity semantics: do not build it with -ffast-math"
#endif

/* ------------------------------ E-process -------------------------------- */

const double gateBetFractions[GATE_NBETS] = {0.1, 0.25, 0.5, 0.9};

void gateBetsInit(gateBets *b, double alpha) {
    int j;

    b->alpha = alpha;
    if (!(alpha > 0.0 && alpha < 1.0)) {
        /* Not a risk target (this also catches NaN). Poison the increments:
         * a NaN log wealth can never certify, whatever the stream is. */
        for (j = 0; j < GATE_NBETS; j++) b->inc_ok[j] = b->inc_err[j] = NAN;
        return;
    }
    for (j = 0; j < GATE_NBETS; j++) {
        double c = gateBetFractions[j];
        double lambda = c / (1.0 - alpha);

        /* A correct sample multiplies the wealth by 1 + lambda * alpha. For
         * small alpha this is 1 + tiny, hence log1p: log(1 + x) would lose
         * most of the digits of x and the loss would be multiplied by the
         * thousands of samples a certification needs. */
        b->inc_ok[j] = log1p(lambda * alpha);
        /* An error multiplies it by 1 + lambda * (alpha - 1), which is
         * 1 - c exactly. Evaluating it as log1p(-c) instead of through
         * lambda keeps the rounding of the division out of it, so the price
         * of an error really is the same for every alpha. It is finite
         * because every c is < 1. */
        b->inc_err[j] = log1p(-c);
    }
}

void gateRegionInit(gateRegion *r) {
    int j;

    for (j = 0; j < GATE_NBETS; j++) r->logw[j] = 0.0; /* wealth 1 */
    r->n = 0;
    r->errors = 0;
    r->certified_at = 0;
}

/* The log wealths are plain sums of the precomputed increments. Each
 * increment is bounded (|inc| < 37 for any double alpha in (0,1)), so even
 * 2^64 samples keep |logw| below 1e21: overflow is impossible and the only
 * way to see a non finite value here is corrupted state. The rounding error
 * of the running sum is about 1e-16 relative per addition, far below
 * anything that could matter against a threshold of a few units. */
double gateRegionUpdate(gateRegion *r, const gateBets *b, int error) {
    /* Anything that is not a clean 0 is accounted as an error: the caller
     * promises 0 or 1, and reading garbage as "correct" would be the unsafe
     * direction. */
    const double *inc = error ? b->inc_err : b->inc_ok;
    int j;

    for (j = 0; j < GATE_NBETS; j++) r->logw[j] += inc[j];
    r->n++;
    if (error) r->errors++;
    return gateRegionLogWealth(r);
}

double gateRegionLogWealth(const gateRegion *r) {
    double max = -INFINITY, sum = 0.0;
    int j;

    for (j = 0; j < GATE_NBETS; j++) {
        double v = r->logw[j];

        /* The updates can only produce finite log wealths (see above), so a
         * NaN or an infinity here is corrupted state, in any of the four
         * slots and of either sign: -inf would be a bet that lost
         * everything, which a factor of 1 - c > 0 never does. There is no
         * evidence to report then, and NaN is the one answer that no
         * comparison downstream can mistake for some. */
        if (!isfinite(v)) return NAN;
        if (v > max) max = v;
    }

    /* Subtracting the maximum makes every exponent <= 0 and one of them
     * exactly 0: the sum is in [1, 4] whatever the magnitude of the log
     * wealths, so exp() can neither overflow nor underflow to an all zero
     * sum. Scaling by 1/4 is exact (a power of two), which also makes four
     * equal log wealths return that same value with no rounding at all, so
     * a fresh region reports a log wealth of exactly 0. */
    for (j = 0; j < GATE_NBETS; j++) sum += exp(r->logw[j] - max);
    return max + log(0.25 * sum);
}

int gateRegionCertifies(const gateRegion *r, double log_threshold) {
    double lw = gateRegionLogWealth(r);

    /* The comparison alone already rejects a NaN wealth (and a NaN
     * threshold): a comparison with NaN is false. isfinite() states the
     * contract of the header explicitly and keeps it true even if the
     * comparison is ever rewritten in a form that NaN would pass. */
    return isfinite(lw) && lw >= log_threshold;
}

/* ----------------------------- Error budget ------------------------------ */

/* Shared domain check. A total outside (0,1) is not a probability budget and
 * generation 0 does not exist: both get no budget at all. */
static int gateBudgetValid(double delta_total, uint64_t g) {
    return delta_total > 0.0 && delta_total < 1.0 && g >= 1;
}

double gateBudgetGenerationDelta(double delta_total, uint64_t g) {
    double dg = (double)g;

    if (!gateBudgetValid(delta_total, g)) return 0.0;
    /* g * (g + 1) is formed in floating point because the integer product
     * wraps around for g >= 2^32, which would hand a late generation a
     * large budget. In double it is exact up to g = 2^26 (so the result is
     * the correctly rounded quotient) and at most 3.4e38 afterwards. */
    return delta_total / (dg * (dg + 1.0));
}

double gateBudgetSpent(double delta_total, uint64_t g) {
    double dg = (double)g;

    if (!gateBudgetValid(delta_total, g)) return 0.0;
    /* The ratio is rounded first on purpose: it is <= 1 in floating point
     * too, so the product can never come out above delta_total, not even by
     * one ulp and not even when g + 1 is absorbed (g >= 2^53). */
    return delta_total * (dg / (dg + 1.0));
}

double gateBudgetLogThreshold(double delta_total, uint64_t g, uint32_t nregions) {
    double dg = (double)g;

    /* +inf is a threshold no finite wealth can reach. Without this check
     * g = 0 or nregions = 0 would produce log(0) = -inf below, that is a
     * threshold every region passes. */
    if (!gateBudgetValid(delta_total, g) || nregions == 0) return INFINITY;

    /* -log(delta_total / (g (g+1) nregions)) as a sum of logarithms. The
     * quotient itself underflows to zero for a small delta_total and a late
     * generation, the logarithms never do. All four terms are >= 0, so
     * there is no cancellation, and each is non decreasing in g, so the
     * threshold never gets easier for a later generation. The smallest
     * possible value is log(2) - log(delta_total) > 0: a region with no
     * evidence (log wealth 0) is never certified. */
    return log(dg) + log(dg + 1.0) + log((double)nregions) - log(delta_total);
}

/* ---------------------------- Threshold grids ---------------------------- */

/* Two branches so that exp() only ever sees a non positive argument. */
static double gateGridSigmoid(double x) {
    if (x >= 0.0) {
        return 1.0 / (1.0 + exp(-x));
    } else {
        double e = exp(x);
        return e / (1.0 + e);
    }
}

int gateGridBuild(double *grid, uint32_t n, int kind, uint32_t nclasses) {
    uint32_t t;

    /* Validate everything before the first write: a rejected call leaves
     * the caller's buffer untouched. */
    if (grid == NULL) return GATE_ERR;
    if (n < GATE_MIN_THRESHOLDS || n > GATE_MAX_THRESHOLDS) return GATE_ERR;
    if (kind != GATE_GRID_UNIFORM && kind != GATE_GRID_LOGIT) return GATE_ERR;
    if (kind == GATE_GRID_LOGIT && nclasses < 2) return GATE_ERR;

    if (kind == GATE_GRID_UNIFORM) {
        for (t = 0; t < n; t++) grid[t] = (double)t / (double)(n - 1);
        grid[n - 1] = 1.0;
    } else {
        /* logit(1/K) = log((1/K) / (1 - 1/K)) = -log(K - 1), exact in K.
         * For K = 2 this is 0, i.e. the grid starts at 0.5. */
        double lo = -log((double)nclasses - 1.0);
        double hi = log(GATE_GRID_LOGIT_TOP) - log1p(-GATE_GRID_LOGIT_TOP);

        for (t = 0; t < n; t++)
            grid[t] = gateGridSigmoid(lo + (double)t * (hi - lo) / (double)(n - 1));
        /* The interpolation reproduces the end points only up to rounding,
         * and they have to be exact. grid[0] must be the correctly rounded
         * 1/K, because that is the smallest value the largest of K
         * probabilities can take: one ulp more and the accept-everything
         * region would silently drop the least confident requests. */
        grid[0] = 1.0 / (double)nclasses;
        grid[n - 1] = GATE_GRID_LOGIT_TOP;
    }

    /* gateGridCount() and the nesting of the regions rely on a strictly
     * increasing grid. The steps are orders of magnitude above the rounding
     * (the closest pair, at the top of a 256 point two class grid, is still
     * 3.7e-6 apart) so this cannot fail with a working libm; it also
     * rejects a NaN. */
    for (t = 1; t < n; t++) {
        if (!(grid[t] > grid[t - 1])) return GATE_ERR;
    }
    return GATE_OK;
}

uint32_t gateGridCount(const double *grid, uint32_t n, double score) {
    uint32_t lo = 0, hi = n;

    /* A NaN score belongs to no region. The search below would also return
     * 0, since every comparison with NaN is false, but only as long as the
     * comparison keeps its current direction: this must not depend on it. */
    if (isnan(score)) return 0;

    /* Upper bound search. Invariant: grid[t] <= score for every t < lo and
     * grid[t] > score for every t >= hi. */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        if (grid[mid] <= score) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* ------------------------------ Drift window ----------------------------- */

_Static_assert(GATE_DRIFT_WINDOW % 64 == 0, "the ring must fill whole 64 bit words");
_Static_assert(GATE_DRIFT_MIN <= GATE_DRIFT_WINDOW, "the alarm could never be evaluated");

void gateDriftInit(gateDriftWindow *w) {
    /* memset rather than field assignments so that the padding bytes are
     * defined too and two windows with the same history are byte identical. */
    memset(w, 0, sizeof(*w));
}

int gateDriftPush(gateDriftWindow *w, int error, double limit) {
    /* 'pos' is always < GATE_DRIFT_WINDOW; the modulo only makes sure that a
     * corrupted value cannot index outside the ring. */
    uint32_t pos = w->pos % GATE_DRIFT_WINDOW;
    uint64_t *word = &w->bits[pos / 64];
    uint64_t mask = (uint64_t)1 << (pos % 64);

    if (w->n >= GATE_DRIFT_WINDOW) {
        /* The ring is full, so the slot about to be written holds the oldest
         * sample: it leaves the window. */
        if ((*word & mask) && w->errors > 0) w->errors--;
    } else {
        w->n++;
    }
    if (error) {
        *word |= mask;
        w->errors++;
        w->total_errors++;
    } else {
        *word &= ~mask;
    }
    w->pos = (pos + 1) % GATE_DRIFT_WINDOW;
    w->total++;

    /* With fewer samples the empirical fraction is too noisy to act on: a
     * single early error would be a rate of 100%. */
    if (w->n < GATE_DRIFT_MIN) return 0;
    /* errors > limit * n, written as the negation of "within the limit" so
     * that a NaN limit raises the alarm instead of disabling the guard. For
     * every other limit the two forms are the same comparison. */
    return !((double)w->errors <= limit * (double)w->n);
}
