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
 * This file knows nothing about Redis, commands or classifiers. It provides:
 *
 *   1. The anytime-valid certification test: for one acceptance region and
 *      a risk target alpha it tests H0: E[error | request in region] >= alpha
 *      with a mixture of four fixed-fraction betting e-processes.
 *   2. The lifetime error budget: delta_g = delta_total / (g (g+1)) for the
 *      g-th frozen generation, split equally over the regions it tests.
 *   3. The deterministic threshold grid.
 *   4. The drift detector that withdraws a policy that got worse.
 *
 * THE TEST. For a region with risk target alpha and error indicators
 * E_1, E_2, ... in {0,1} (1 = the frozen prediction disagreed with the
 * teacher), each betting fraction lambda_j maintains the wealth
 *
 *     M_0 = 1,   M_t = M_(t-1) * (1 + lambda_j * (alpha - E_t))
 *
 * with lambda_j = c_j / (1 - alpha), c_j in {0.1, 0.25, 0.5, 0.9}. Because
 * lambda_j < 1/(1-alpha) every factor is positive, and under H0 its
 * conditional expectation is <= 1, so each M is a nonnegative
 * supermartingale and so is their equal weight average. By Ville's
 * inequality the average ever reaches 1/delta_region with probability at
 * most delta_region. The region is certified the first time
 *
 *     log( (1/4) * sum_j M_j )  >=  -log(delta_region).
 *
 * Note that the factor for an error is (1 - c_j) regardless of alpha, and
 * the factor for a correct sample is 1 + c_j * alpha / (1 - alpha).
 *
 * Everything is kept in log space in doubles: no overflow or underflow is
 * possible for any realistic stream length, and a NaN never certifies.
 */

#ifndef GATE_STATS_H
#define GATE_STATS_H

#include <stdint.h>
#include "gate_defs.h"

/* ------------------------------ E-process -------------------------------- */

#define GATE_NBETS 4
extern const double gateBetFractions[GATE_NBETS]; /* {0.1, 0.25, 0.5, 0.9} */

/* Precomputed log increments for one risk target. */
typedef struct gateBets {
    double alpha;
    double inc_ok[GATE_NBETS];  /* log1p(lambda_j * alpha)       : E = 0 */
    double inc_err[GATE_NBETS]; /* log1p(lambda_j * (alpha - 1)) : E = 1 */
} gateBets;

/* 'alpha' must be in (0,1): the caller validates. */
void gateBetsInit(gateBets *b, double alpha);

/* Evidence for one acceptance region (one class row x one threshold). */
typedef struct gateRegion {
    double logw[GATE_NBETS]; /* log wealth per betting fraction, starts at 0 */
    uint64_t n;              /* eligible samples that fell in the region */
    uint64_t errors;         /* of which disagreements */
    uint64_t certified_at;   /* value of 'n' when first certified; 0 = never */
} gateRegion;

void gateRegionInit(gateRegion *r);

/* Account one eligible sample ('error' is 0 or 1) and return the log mixture
 * wealth after the update. Does not set certified_at. */
double gateRegionUpdate(gateRegion *r, const gateBets *b, int error);

/* log((1/4) * sum_j exp(logw[j])), computed with log-sum-exp. */
double gateRegionLogWealth(const gateRegion *r);

/* Returns 1 if the region's log mixture wealth is a finite number that is
 * >= log_threshold, 0 otherwise (in particular 0 for NaN / infinities). */
int gateRegionCertifies(const gateRegion *r, double log_threshold);

/* ----------------------------- Error budget ------------------------------
 * Generations are numbered g = 1, 2, 3, ... in the order they are frozen and
 * the numbering is never reused, so the total spent is delta_total*g/(g+1)
 * and can never exceed delta_total. All functions require 0 < delta_total <
 * 1, g >= 1, nregions >= 1. */

/* delta_g = delta_total / (g * (g + 1)). */
double gateBudgetGenerationDelta(double delta_total, uint64_t g);
/* Budget consumed by generations 1..g: delta_total * g / (g + 1); 0 for g=0. */
double gateBudgetSpent(double delta_total, uint64_t g);
/* -log(delta_g / nregions), computed in log space (no underflow for huge g).
 * This is the log wealth a region of generation g must reach. */
double gateBudgetLogThreshold(double delta_total, uint64_t g, uint32_t nregions);

/* ---------------------------- Threshold grid -----------------------------
 * GATE_THRESHOLDS strictly increasing thresholds. Region t accepts a score
 * s iff s >= grid[t], so regions are nested: region 0 is the widest, region
 * GATE_THRESHOLDS-1 the most confident.
 *
 * The grid is evenly spaced in log(1 - threshold) between 'lo' and 'hi':
 *
 *     grid[t] = 1 - (1 - lo) * ((1 - hi) / (1 - lo)) ^ (t / (T - 1))
 *
 * which puts the resolution where confident scores live (0.9, 0.99, 0.999).
 * grid[0] is exactly 'lo' and grid[T-1] exactly 'hi'. Requires
 * 0 <= lo < hi < 1; returns GATE_OK / GATE_ERR. */
int gateGridBuild(double *grid, double lo, double hi);

/* Number of thresholds t with grid[t] <= score, i.e. the score belongs to
 * regions 0 .. count-1. Returns 0 for a NaN score. O(log n). */
uint32_t gateGridCount(const double *grid, uint32_t n, double score);

/* ----------------------------- Drift detector ----------------------------
 * A CUSUM e-detector over the spot checks of accepted traffic. For a risk
 * target alpha it watches for the error rate rising to an alternative p1 >
 * alpha with the likelihood ratio of one spot check,
 *
 *     e(E) = (p1 / alpha)^E * ((1 - p1) / (1 - alpha))^(1 - E),
 *
 * whose expectation is <= 1 whenever the true error rate is <= alpha. The
 * statistic restarts from 1 whenever it falls below 1:
 *
 *     W <- max(W, 1) * e(E)
 *
 * Two alternatives are run side by side, p1 = 2 alpha and p1 = 4 alpha
 * (each capped at (1 + alpha) / 2), and the alarm fires when their average
 * reaches the alarm level A. While the error rate is at most alpha the
 * expected number of spot checks before a false alarm is at least A; a
 * gross failure (half the answers wrong) is caught in about a dozen.
 * Everything is kept in log space. */

#define GATE_DRIFT_ALTS 2

typedef struct gateDriftBets {
    double inc_ok[GATE_DRIFT_ALTS];  /* log e(0) per alternative (negative) */
    double inc_err[GATE_DRIFT_ALTS]; /* log e(1) per alternative (positive) */
} gateDriftBets;

typedef struct gateDrift {
    double logw[GATE_DRIFT_ALTS]; /* log statistic per alternative, >= ... */
    uint64_t n;                   /* spot checks seen */
    uint64_t errors;              /* of which disagreements */
} gateDrift;

/* 'alpha' must be in (0,1); otherwise the increments are poisoned so that
 * the detector alarms on the first sample (fail closed). */
void gateDriftBetsInit(gateDriftBets *b, double alpha);
void gateDriftInit(gateDrift *d);

/* Record one spot check ('error' 0 or 1). Returns 1 when the alarm level
 * 'log_alarm' (the natural log of A) is reached, or when the statistic is
 * not a finite number. */
int gateDriftPush(gateDrift *d, const gateDriftBets *b, int error, double log_alarm);

/* Current value of the statistic, as the log of the average over the
 * alternatives. 0 for a fresh detector. */
double gateDriftLogStat(const gateDrift *d);

#endif /* GATE_STATS_H */
