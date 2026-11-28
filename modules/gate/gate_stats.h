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
 *   3. The deterministic threshold grids.
 *   4. The heuristic drift window used to revoke a serving policy.
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

/* ---------------------------- Threshold grids ----------------------------
 * A grid is n strictly increasing thresholds fixed when the gate is created.
 * Region t accepts a score s iff s >= grid[t], so regions are nested and
 * grid[0] is the accept-everything region.
 *
 *   GATE_GRID_UNIFORM   SCORE mode. grid[t] = t / (n - 1): 0 ... 1.
 *   GATE_GRID_LOGIT     CLASSIFIER mode with K classes. The score is the
 *                       largest class probability, which lies in [1/K, 1].
 *                       The grid is uniform in log-odds from logit(1/K) to
 *                       logit(0.9999): grid[0] = 1/K exactly, grid[n-1] =
 *                       0.9999. This spends the resolution where confident
 *                       models actually put their scores (0.9, 0.99, 0.999).
 */
#define GATE_GRID_UNIFORM 0
#define GATE_GRID_LOGIT 1
#define GATE_GRID_LOGIT_TOP 0.9999

/* Fill grid[0..n-1]. Requires GATE_MIN_THRESHOLDS <= n <= GATE_MAX_THRESHOLDS
 * and, for the logit grid, nclasses >= 2. Returns GATE_OK / GATE_ERR. */
int gateGridBuild(double *grid, uint32_t n, int kind, uint32_t nclasses);

/* Number of thresholds t with grid[t] <= score, i.e. the score belongs to
 * regions 0 .. count-1. Returns 0 for a NaN score. O(log n). */
uint32_t gateGridCount(const double *grid, uint32_t n, double score);

/* ------------------------------ Drift window -----------------------------
 * Fixed window over the last GATE_DRIFT_WINDOW completed audits of accepted
 * traffic. This is an operational alarm, NOT a statistical guarantee. */

#define GATE_DRIFT_WINDOW 256
#define GATE_DRIFT_MIN 100

typedef struct gateDriftWindow {
    uint64_t bits[GATE_DRIFT_WINDOW / 64]; /* ring of error indicators */
    uint32_t n;            /* valid entries in the ring, <= GATE_DRIFT_WINDOW */
    uint32_t pos;          /* next slot to write */
    uint32_t errors;       /* errors among the valid entries */
    uint64_t total;        /* lifetime completed audits */
    uint64_t total_errors; /* lifetime audit errors */
} gateDriftWindow;

void gateDriftInit(gateDriftWindow *w);

/* Record one completed audit. Returns 1 when the alarm condition holds
 * after the push:  n >= GATE_DRIFT_MIN  and  errors > limit * n,
 * where 'limit' is the tolerated error fraction (alpha + margin). */
int gateDriftPush(gateDriftWindow *w, int error, double limit);

#endif /* GATE_STATS_H */
