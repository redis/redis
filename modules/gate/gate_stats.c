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
 * or, for the drift detector, the answer that raises the alarm.
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

/* Log mixture wealth of a region after n correct answers and no error. */
static double gateBetsFlawlessWealth(const gateBets *b, double n) {
    double max = -INFINITY, sum = 0.0;
    int j;

    for (j = 0; j < GATE_NBETS; j++)
        if (n * b->inc_ok[j] > max) max = n * b->inc_ok[j];
    for (j = 0; j < GATE_NBETS; j++) sum += exp(n * b->inc_ok[j] - max);
    return max + log(sum / GATE_NBETS);
}

uint64_t gateBetsFlawless(const gateBets *b, double log_threshold) {
    /* The boldest bet gains most per correct answer. The mixture is never
     * above it and never below a quarter of it, which brackets the answer
     * within a few steps; the wealth grows with n, so bisect. */
    double best = b->inc_ok[GATE_NBETS - 1], lo_d, hi_d;
    uint64_t lo, hi;

    if (!isfinite(log_threshold) || !(best > 0.0)) return UINT64_MAX;
    if (!(log_threshold > 0.0)) return 1;
    hi_d = ceil((log_threshold + log((double)GATE_NBETS)) / best) + 1.0;
    if (!(hi_d < 9e18)) return UINT64_MAX;
    lo_d = floor(log_threshold / best);
    lo = lo_d < 1.0 ? 1 : (uint64_t)lo_d;
    hi = (uint64_t)hi_d;
    if (hi < lo) hi = lo;
    while (gateBetsFlawlessWealth(b, (double)hi) < log_threshold) hi++; /* rounding only */
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;

        if (gateBetsFlawlessWealth(b, (double)mid) >= log_threshold) hi = mid; else lo = mid + 1;
    }
    return lo;
}

void gateRegionFlip(gateRegion *r, const gateBets *b) {
    int j;

    if (r->errors == 0) return; /* nothing to take back */
    for (j = 0; j < GATE_NBETS; j++) r->logw[j] += b->inc_ok[j] - b->inc_err[j];
    r->errors--;
}

double gateRegionLogWealth(const gateRegion *r) {
    double max = -INFINITY, sum = 0.0, tail;
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
    tail = log(0.25 * sum);
    /* An average is never above its largest term. That is tail <= 0, true
     * by itself with any exp() that does not exceed 1 for an argument <= 0;
     * the clamp makes it unconditional, because gateRegionCertifies()
     * builds on it. */
    if (tail > 0.0) tail = 0.0;
    return max + tail;
}

int gateRegionCertifies(const gateRegion *r, double log_threshold) {
    double max = -INFINITY, lw;
    int j;

    for (j = 0; j < GATE_NBETS; j++) {
        double v = r->logw[j];

        if (!isfinite(v)) return 0; /* see gateRegionLogWealth() */
        if (v > max) max = v;
    }
    /* The mixture cannot be above the best of the four bets (see above), so
     * a region whose best bet is still short of the threshold is not
     * certified, and saying so takes four comparisons instead of four
     * exp() and a log(). This is the answer for nearly every region on
     * nearly every sample, and this function runs for every region a
     * sample falls in. The negated form also rejects a NaN threshold. */
    if (!(max >= log_threshold)) return 0;

    lw = gateRegionLogWealth(r);
    /* The comparison alone already rejects a NaN: isfinite() states the
     * contract of the header explicitly and keeps it true even if the
     * comparison is ever rewritten in a form that a NaN would pass. */
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

/* ---------------------------- Threshold grid ----------------------------- */

int gateGridBuild(double *grid, double lo, double hi) {
    uint32_t t, n = GATE_THRESHOLDS;

    /* Validate everything before the first write: a rejected call leaves
     * the caller's buffer untouched. The comparisons also reject NaN. */
    if (grid == NULL) return GATE_ERR;
    if (!(lo >= 0.0 && lo < hi && hi < 1.0)) return GATE_ERR;

    /* Interpolate log(1 - threshold) linearly from log(1 - lo) down to
     * log(1 - hi). Both logs are finite because lo < hi < 1. */
    {
        double a = log1p(-lo), b = log1p(-hi);

        for (t = 0; t < n; t++) {
            double u = (double)t / (double)(n - 1);

            /* -expm1(x) = 1 - exp(x): keeps full precision when the
             * threshold is close to 0 (x close to 0). */
            grid[t] = -expm1(a + u * (b - a));
        }
    }
    /* The end points must be exact: grid[0] is the smallest score a region
     * can admit (one ulp more and the widest region would silently drop
     * the least confident requests) and grid[n-1] is the documented top. */
    grid[0] = lo;
    grid[n - 1] = hi;

    /* gateGridCount() and the nesting of the regions rely on a strictly
     * increasing grid; this also rejects a NaN produced by a broken libm. */
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

/* ----------------------------- Drift detector ---------------------------- */

static const double gateDriftMultiples[GATE_DRIFT_ALTS] = {2.0, 4.0};

void gateDriftBetsInit(gateDriftBets *b, double alpha) {
    int k;

    if (!(alpha > 0.0 && alpha < 1.0)) {
        /* Not a risk target (also catches NaN): poison the increments so
         * that the first sample raises the alarm. */
        for (k = 0; k < GATE_DRIFT_ALTS; k++) b->inc_ok[k] = b->inc_err[k] = NAN;
        return;
    }
    for (k = 0; k < GATE_DRIFT_ALTS; k++) {
        /* The alternative must stay a probability above alpha: halfway
         * between alpha and 1 is the cap for large alpha. */
        double cap = 0.5 * (1.0 + alpha);
        double p1 = gateDriftMultiples[k] * alpha;

        if (p1 > cap) p1 = cap;
        /* For an alpha one step below 1 the cap rounds to 1, which is not
         * an alternative (its log would be -inf and read as an alarm). No
         * room is left above alpha: this alternative stays silent. */
        if (!(p1 < 1.0)) p1 = alpha;
        b->inc_err[k] = log(p1 / alpha);                 /* >= 0 */
        b->inc_ok[k] = log1p(-p1) - log1p(-alpha);       /* <= 0 */
    }
}

const uint32_t gateDriftBlocks[GATE_DRIFT_SCALES] = {64, 512, 4096};

void gateDriftInit(gateDrift *d) {
    memset(d, 0, sizeof(*d));
}

uint32_t gateDriftOpen(const gateDrift *d, int scale) {
    if (d->counted == 0) return 0;
    return (uint32_t)((d->counted - 1) % gateDriftBlocks[scale]) + 1;
}

/* Log of the average over the alternatives of the block's product, with
 * 'w' of its spot checks counted as disagreements. */
static double gateDriftBlockStat(const gateDrift *d, const gateDriftBets *b, int s, double w) {
    double n = (double)gateDriftOpen(d, s);
    double max = -INFINITY, sum = 0.0, lw[GATE_DRIFT_ALTS];
    int k;

    for (k = 0; k < GATE_DRIFT_ALTS; k++) {
        lw[k] = w * b->inc_err[k] + (n - w) * b->inc_ok[k];
        if (!isfinite(lw[k])) return NAN;
        if (lw[k] > max) max = lw[k];
    }
    for (k = 0; k < GATE_DRIFT_ALTS; k++) sum += exp(lw[k] - max);
    return max + log(sum / GATE_DRIFT_ALTS);
}

double gateDriftLogStat(const gateDrift *d, const gateDriftBets *b, int with_late) {
    double best = 0.0;
    int s;

    if (d->counted == 0) return 0.0;
    for (s = 0; s < GATE_DRIFT_SCALES; s++) {
        double w = (double)d->wrong[s] + (with_late ? (double)d->late[s] : 0.0);
        double stat = gateDriftBlockStat(d, b, s, w);

        if (!isfinite(stat)) return NAN;
        if (s == 0 || stat > best) best = stat;
    }
    return best;
}

/* What the blocks now open say. 'worse' is 0 when only the second
 * statistic can have grown. Both tests are written as the negation of
 * "safely below the alarm", so that a NaN statistic or alarm level fires
 * instead of silently disabling the detector. */
static int gateDriftVerdict(const gateDrift *d, const gateDriftBets *b, int worse,
                            double log_alarm, double log_late_alarm)
{
    int s, verdict = GATE_DRIFT_QUIET;

    for (s = 0; s < GATE_DRIFT_SCALES; s++) {
        double stat;

        if (worse) {
            stat = gateDriftBlockStat(d, b, s, (double)d->wrong[s]);
            if (!(isfinite(stat) && stat < log_alarm)) return GATE_DRIFT_ALARM_WORSE;
        }
        stat = gateDriftBlockStat(d, b, s, (double)d->wrong[s] + (double)d->late[s]);
        if (!(isfinite(stat) && stat < log_late_alarm)) verdict = GATE_DRIFT_ALARM_UNCHECKED;
    }
    return verdict;
}

int gateDriftCount(gateDrift *d, const gateDriftBets *b, int outcome, double log_alarm,
                   double log_late_alarm, uint64_t *number)
{
    int s;

    if (number) *number = 0;
    if (d->counted >= GATE_COUNT_MAX) return GATE_DRIFT_QUIET; /* never in practice */
    /* The first spot check of a block opens it. */
    for (s = 0; s < GATE_DRIFT_SCALES; s++)
        if (d->counted % gateDriftBlocks[s] == 0) d->wrong[s] = d->late[s] = 0;
    d->counted++;
    if (number) *number = d->counted;

    if (outcome == GATE_DRIFT_RIGHT) return GATE_DRIFT_QUIET; /* both statistics fell */
    if (outcome == GATE_DRIFT_LATE) {
        for (s = 0; s < GATE_DRIFT_SCALES; s++) d->late[s]++;
        return gateDriftVerdict(d, b, 0, log_alarm, log_late_alarm);
    }
    d->errors++;
    for (s = 0; s < GATE_DRIFT_SCALES; s++) d->wrong[s]++;
    return gateDriftVerdict(d, b, 1, log_alarm, log_late_alarm);
}

int gateDriftAnswer(gateDrift *d, const gateDriftBets *b, uint64_t number, int wrong,
                    double log_alarm, double log_late_alarm)
{
    int s;

    if (number == 0 || number > d->counted) return GATE_DRIFT_QUIET;
    if (wrong && d->errors < d->counted) d->errors++;
    for (s = 0; s < GATE_DRIFT_SCALES; s++) {
        uint32_t len = gateDriftBlocks[s];

        if ((number - 1) / len != (d->counted - 1) / len) continue; /* its block has ended */
        if (d->late[s] == 0) continue; /* nothing to set right */
        d->late[s]--;
        if (wrong) d->wrong[s]++;
    }
    /* A right answer only lowers the second statistic. A wrong one is
     * judged even when every block it was counted in has ended: nothing
     * grew then, but broken increments or levels must still raise the
     * alarm (fail closed). */
    return wrong ? gateDriftVerdict(d, b, 1, log_alarm, log_late_alarm) : GATE_DRIFT_QUIET;
}
