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

/* Turn one sample that was accounted as an error into a correct one: the
 * wealth becomes what it would be had the sample been correct from the
 * start (up to rounding). 'n' does not change. Does nothing if the region
 * holds no error. Does not set certified_at. */
void gateRegionFlip(gateRegion *r, const gateBets *b);

/* The number of correct answers in a row that take a fresh region to
 * 'log_threshold': the smallest n >= 1 with
 * log((1/4) * sum_j exp(n * inc_ok[j])) >= log_threshold. Returns
 * UINT64_MAX when that number does not fit (a tiny alpha) or the arguments
 * are not numbers. */
uint64_t gateBetsFlawless(const gateBets *b, double log_threshold);

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
 * Watches the spot checks of accepted traffic for an error rate above the
 * target. For a risk target alpha and an alternative p1 > alpha, the
 * likelihood ratio of one spot check is
 *
 *     e(E) = (p1 / alpha)^E * ((1 - p1) / (1 - alpha))^(1 - E),
 *
 * whose expectation is <= 1 whenever the true error rate is <= alpha. Two
 * alternatives are run side by side, p1 = 2 alpha and p1 = 4 alpha (each at
 * most halfway from alpha to 1), and averaged.
 *
 * Spot checks are numbered in the order they are ASKED and cut into blocks
 * of 64, of 512 and of 4,096 (GATE_DRIFT_SCALES block lengths at once). The
 * statistic of a block is the product of e() over the spot checks asked in
 * it so far, where a spot check counts as right until a disagreeing answer
 * arrives. The alarm fires when the statistic of a block reaches the alarm
 * level A.
 *
 * LOST ANSWERS. A spot check whose answer never arrives is not known to be
 * a disagreement, but it may be one (a teacher or a client that fails on
 * the hard cases loses exactly the answers that matter). A second
 * statistic per block counts every lost answer as a disagreement, next to
 * the real ones. It has its own, much higher alarm level, and its alarm
 * means "can no longer be checked", not "got worse".
 *
 * WHY BLOCKS. A product does not depend on the order of its factors, so
 * the statistic does not depend on the order in which answers arrive.
 * However answers are delayed, batched or reordered, its value is never
 * above the product over the same spot checks with their true answers
 * taken in the order they were asked, and that is a nonnegative
 * supermartingale while the error rate is at most alpha. Each block
 * therefore raises a false alarm with probability at most 1/A, whatever
 * the timing of the answers. (A running statistic that restarts when it
 * falls, like CUSUM, alarms on a handful of disagreements that happen to
 * arrive together.) Short blocks catch a gross failure within a few dozen
 * spot checks; long blocks gather enough evidence against a rate that is
 * only moderately too high. A disagreeing answer that arrives after its
 * block has ended counts only in the longer blocks that are still open.
 * Everything is kept as counts; the statistic is computed in log space. */

#define GATE_DRIFT_ALTS 2
#define GATE_DRIFT_SCALES 3
extern const uint32_t gateDriftBlocks[GATE_DRIFT_SCALES]; /* {64, 512, 4096} */

typedef struct gateDriftBets {
    double inc_ok[GATE_DRIFT_ALTS];  /* log e(0) per alternative (<= 0) */
    double inc_err[GATE_DRIFT_ALTS]; /* log e(1) per alternative (>= 0) */
} gateDriftBets;

typedef struct gateDrift {
    uint64_t asked;                    /* spot checks asked */
    uint64_t errors;                   /* disagreeing answers received */
    uint32_t wrong[GATE_DRIFT_SCALES]; /* ... of which belong to the block
                                        * now open, per block length */
    uint32_t lost[GATE_DRIFT_SCALES];  /* answers given up, of the block now
                                        * open, per block length */
} gateDrift;

#define GATE_DRIFT_QUIET 0
#define GATE_DRIFT_ALARM_WORSE 1    /* disagreements reached the alarm level */
#define GATE_DRIFT_ALARM_UNCHECKED 2 /* only together with lost answers */

/* 'alpha' must be in (0,1); otherwise the increments are poisoned so that
 * the first disagreement raises the alarm (fail closed). For an alpha so
 * close to 1 that no double lies between it and 1, an alternative has no
 * room above alpha and stays silent (both increments 0). */
void gateDriftBetsInit(gateDriftBets *b, double alpha);
void gateDriftInit(gateDrift *d);

/* A spot check is asked. Returns its number: 1, 2, 3, ... */
uint64_t gateDriftAsk(gateDrift *d);

/* The answer of spot check 'number' disagreed. Returns
 * GATE_DRIFT_ALARM_WORSE when the disagreements of a block reach the alarm
 * level 'log_alarm' (the natural log of A) or a statistic is not a finite
 * number; otherwise GATE_DRIFT_ALARM_UNCHECKED when disagreements and lost
 * answers together reach 'log_lost_alarm'; otherwise GATE_DRIFT_QUIET. A
 * number that was never handed out is ignored. Agreeing answers are not
 * reported at all. */
int gateDriftWrong(gateDrift *d, const gateDriftBets *b, uint64_t number, double log_alarm,
                   double log_lost_alarm);

/* The answer of spot check 'number' was given up. Returns
 * GATE_DRIFT_ALARM_UNCHECKED when disagreements and lost answers of a block
 * together reach 'log_lost_alarm', else GATE_DRIFT_QUIET. */
int gateDriftLost(gateDrift *d, const gateDriftBets *b, uint64_t number, double log_lost_alarm);

/* Spot checks asked so far in the block of length gateDriftBlocks[scale]
 * that is now open (0 before the first one). */
uint32_t gateDriftOpen(const gateDrift *d, int scale);

/* The largest block statistic now, as a log (0 for a fresh detector):
 * over the disagreements, or with 'with_lost' over disagreements and lost
 * answers together. */
double gateDriftLogStat(const gateDrift *d, const gateDriftBets *b, int with_lost);

#endif /* GATE_STATS_H */
