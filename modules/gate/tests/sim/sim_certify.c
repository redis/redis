/*
 * Gate data type: Monte Carlo study of the proof (betting test, bar, late
 * answers) and of the drift detector implemented in gate_stats.c.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Usage: sim_certify [trials [seed]]
 *
 * 'trials' (default 30000, about a minute and a quarter on one core) is
 * the number of races per scorer of table (B) and of streams per row of
 * tables (C) and (E2). Tables (A) and (E1) estimate much smaller
 * probabilities and use 5 x trials streams per row. Table (D) pushes every
 * spot check through the detector, up to 400,000 per stream when no alarm
 * comes, and uses trials / 100 streams per row for the false alarms and
 * trials / 10 for the detection delays (at least 10). Everything is
 * deterministic for a given (trials, seed).
 *
 * THE RULES (gate_core.h). Threshold t of a row stands for the region
 * "score >= grid[t]" and has its own betting test, fed with the teacher
 * answers that fall in the region. Every threshold is a test of its own: it
 * is PROVEN as soon as its log wealth has reached the bar of its snapshot,
 *
 *     bar = log(g (g + 1)) + log(tested rows * 32) - log(DELTA)
 *
 * (32 = GATE_THRESHOLDS; g = 1 and DELTA = 0.01 throughout), for both kinds
 * of gate, and the gate answers at the lowest proven threshold of the row.
 * A threshold whose risk is alpha or more reaches the bar with probability
 * at most exp(-bar) and a row has 32 thresholds, so a row proves a bad
 * threshold with probability at most DELTA / (g (g + 1) * tested rows) and
 * a snapshot with probability at most DELTA / (g (g + 1)).
 *
 * A request whose answer is late counts as a wrong guess until the answer
 * arrives (gateRegionFlip() then takes the error back if the guess was
 * right) and for good if it never does.
 *
 * It prints five tables:
 *
 *   (A) how often one threshold whose true risk is at or above alpha reaches
 *       its bar anyway (must stay below exp(-bar));
 *   (B) a DESIGN TABLE: one row of a classifier with two labels, 32 nested
 *       thresholds fed by one stream, under three rules for the order in
 *       which thresholds may be proven. The gate uses the third, "every
 *       threshold"; the other two are the alternatives it was chosen over
 *       (the fixed walk of the reference design, and the "entry points" of
 *       the first version of this module, which the core no longer has).
 *       How long until the first threshold is proven, how much the row
 *       answers later, and how often a bad threshold is ever proven (must
 *       stay below the row's share of DELTA);
 *   (C) how many samples a good threshold needs to reach the bar;
 *   (D) false alarms and detection delay of the drift detector;
 *   (E) late and lost answers: that counting an outstanding answer as a
 *       wrong guess keeps (A) true whatever the teacher does, that leaving
 *       it out does not, and how many samples an outstanding answer costs.
 *
 * Simulation is evidence, not proof: the guarantee is Ville's inequality,
 * these numbers only show that the code does what the inequality promises
 * and what it costs. All the streams are IID, which is the assumption the
 * guarantee itself makes; nothing here speaks for correlated traffic.
 *
 * HOW A CROSSING TIME IS COMPUTED. The wealth of a region depends on the
 * number of samples n and of errors k it has seen, not on their order. So
 * "crossed" is a region of the (n, k) plane and its border is a table
 * nmin[k]: the smallest n at which k errors still reach the bar. The table
 * is built by asking gateRegionCertifies() about a region holding exactly
 * those counts, and a stream has then crossed at the first n with
 * n >= nmin[errors so far]. This turns a trial into a walk over its errors
 * only (the gaps between errors are drawn from the geometric distribution)
 * and makes millions of streams affordable, which the rare events of (A)
 * need. The race of (B) uses one table per bar and keeps per band counts,
 * recounting the 32 regions only at the samples where one of them could be
 * due. The late answers of (E) turn into counts as well: a region in which
 * outstanding answers count as errors holds every request asked so far and
 * as many errors as there are wrong and outstanding answers. None of this
 * is a second implementation of the test: before any table is printed the
 * program replays a few hundred streams, and a dozen complete races, sample
 * by sample through gateRegionUpdate() / gateRegionCertifies() /
 * gateGridCount(), and several hundred streams with late answers request by
 * request and answer by answer through gateRegionUpdate() /
 * gateRegionFlip(), and requires identical crossing times. It also draws
 * the scorers of (B) from their definition and compares them with the
 * bands the race samples from. The detector of (D) is always driven spot
 * check by spot check through gateDriftPush().
 *
 * The exit status is non zero if a self check fails, if a simulated false
 * proof frequency is significantly above its bound, if the unsound rule of
 * (E) fails to exceed that bound against the teacher built to break it, if
 * a sample count of (E) differs from its exact value, or if the detector's
 * mean time to a false alarm is below the alarm level.
 */

#include <time.h>
#include "testutil.h"
#include "gate_stats.h"

#if GATE_THRESHOLDS != 32
#error "the band lookup of simDrawSample() is written for 32 thresholds"
#endif

enum { T = GATE_THRESHOLDS };   /* as an int: many loops here count down */

#define SIM_NEVER UINT64_MAX
#define SIM_Z95 1.959963984540054
#define SIM_DELTA 0.01          /* DELTA of every gate simulated here */
#define SIM_ROWS 2              /* tested rows of a classifier with two labels */

/* ------------------------------- utilities ------------------------------- */

static void *simAlloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb, size);

    if (p == NULL) {
        fprintf(stderr, "sim_certify: out of memory\n");
        exit(2);
    }
    return p;
}

/* An independent generator state for stream 'id' of this run. The splitmix64
 * output function is a bijection of its counter, so distinct ids give
 * distinct, unrelated starting points. */
static uint64_t simStream(uint64_t seed, uint64_t id) {
    uint64_t s = seed ^ (id * 0x9e3779b97f4a7c15ull);

    return gate_test_rand64(&s);
}

/* Number of correct samples before the next error of an IID stream with
 * error rate p, given log1mp = log(1 - p): geometric, by inversion.
 * P(gap >= m) = P(u <= (1-p)^m) = (1-p)^m for u uniform in (0,1]. */
static uint64_t simGap(double log1mp, uint64_t *rng) {
    double u = 1.0 - gate_test_uniform(rng);
    double gap;

    if (!(log1mp < 0.0)) return SIM_NEVER / 4; /* p = 0: no error, ever */
    gap = log(u) / log1mp;
    return gap < 1e18 ? (uint64_t)gap : (uint64_t)1e18;
}

/* 95% Wilson score interval for x successes out of n. */
static void simWilson(uint64_t x, uint64_t n, double *lo, double *hi) {
    double z2 = SIM_Z95 * SIM_Z95, dn = (double)n, ph = (double)x / dn;
    double den = 1.0 + z2 / dn;
    double center = (ph + z2 / (2.0 * dn)) / den;
    double half = SIM_Z95 * sqrt(ph * (1.0 - ph) / dn + z2 / (4.0 * dn * dn)) / den;

    *lo = center - half > 0.0 ? center - half : 0.0;
    *hi = center + half < 1.0 ? center + half : 1.0;
    if (x == 0) *lo = 0.0;
}

static int simCmpU64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

/* The q quantile of an ascending array: the smallest value that has at
 * least a fraction q of the sample at or below it. Runs that never finished
 * are stored as SIM_NEVER and sort last, so a quantile that falls among
 * them comes out as SIM_NEVER ("beyond the cap"). */
static uint64_t simQuantile(const uint64_t *sorted, uint64_t n, double q) {
    double rank = ceil(q * (double)n);
    uint64_t idx = (uint64_t)rank;

    if (idx < 1) idx = 1;
    if (idx > n) idx = n;
    return sorted[idx - 1];
}

/* "1234" or ">cap" for a run that did not finish. */
static const char *simFmt(char *buf, size_t len, uint64_t v, uint64_t cap) {
    if (v == SIM_NEVER) {
        snprintf(buf, len, ">%llu", (unsigned long long)cap);
    } else {
        snprintf(buf, len, "%llu", (unsigned long long)v);
    }
    return buf;
}

static double simSeconds(clock_t since) {
    return (double)(clock() - since) / CLOCKS_PER_SEC;
}

/* The bar of a first snapshot that splits its share of DELTA over
 * 'regions'. What gateBar() does in the gate: regions = tested rows x
 * GATE_THRESHOLDS, every threshold of every tested row. Table (B) also
 * computes the bars the rules it compares with would have. */
static double simBar(uint32_t regions) {
    return gateBudgetLogThreshold(SIM_DELTA, 1, regions);
}

/* --------------------------- crossing boundary --------------------------- */

typedef struct simBoundary {
    double alpha;
    double bar;         /* log wealth to reach */
    uint64_t horizon;   /* streams are followed for this many samples */
    uint32_t len;       /* nmin[0..len-1] are <= horizon; more errors than
                         * len-1 cannot cross within the horizon */
    uint32_t *nmin;
    gateBets bets;
} simBoundary;

/* Would a region that has seen n samples, k of them errors, be at the bar?
 * The log wealths are the closed form of what n updates add up to. */
static int simCountsCross(const gateBets *b, double bar, uint64_t n, uint64_t k) {
    gateRegion r;
    int j;

    gateRegionInit(&r);
    for (j = 0; j < GATE_NBETS; j++)
        r.logw[j] = (double)(n - k) * b->inc_ok[j] + (double)k * b->inc_err[j];
    r.n = n;
    r.errors = k;
    return gateRegionCertifies(&r, bar);
}

/* Smallest n in [from, horizon] at which k errors cross, 0 if there is
 * none. For a fixed k the wealth grows with n, so this is a search for the
 * first true of a monotone predicate: gallop, then bisect. */
static uint64_t simFirstCrossing(const gateBets *b, double bar, uint64_t k,
                                 uint64_t from, uint64_t horizon) {
    uint64_t lo, hi, step = 1;

    if (from > horizon || !simCountsCross(b, bar, horizon, k)) return 0;
    if (simCountsCross(b, bar, from, k)) return from;
    lo = from;      /* does not cross */
    hi = horizon;   /* crosses */
    while (lo + step < hi) {
        if (simCountsCross(b, bar, lo + step, k)) {
            hi = lo + step;
            break;
        }
        lo += step;
        step *= 2;
    }
    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;

        if (simCountsCross(b, bar, mid, k)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return hi;
}

static void simBoundaryBuild(simBoundary *bd, double alpha, double bar, uint64_t horizon) {
    uint32_t cap = 256;
    uint64_t prev = 0;

    if (horizon >= UINT32_MAX) {    /* the table stores 32 bit sample counts */
        fprintf(stderr, "sim_certify: horizon too long\n");
        exit(2);
    }
    bd->alpha = alpha;
    bd->bar = bar;
    bd->horizon = horizon;
    bd->len = 0;
    bd->nmin = simAlloc(cap, sizeof(uint32_t));
    gateBetsInit(&bd->bets, alpha);
    for (;;) {
        /* One more error can only be afforded with more samples: if (n, k)
         * crosses so does (n - 1, k - 1), which had the same correct
         * samples and one error less. Hence nmin[k] > nmin[k - 1]. */
        uint64_t n = simFirstCrossing(&bd->bets, bar, bd->len, prev + 1, horizon);

        if (n == 0) break;
        if (bd->len == cap) {
            cap *= 2;
            bd->nmin = realloc(bd->nmin, cap * sizeof(uint32_t));
            if (bd->nmin == NULL) {
                fprintf(stderr, "sim_certify: out of memory\n");
                exit(2);
            }
        }
        bd->nmin[bd->len++] = (uint32_t)n;
        prev = n;
    }
}

static void simBoundaryFree(simBoundary *bd) {
    free(bd->nmin);
    bd->nmin = NULL;
    bd->len = 0;
}

/* Sample at which an IID stream with error rate p crosses, 0 if not within
 * the horizon. 'at' is the position of the latest error (0 = none) and
 * 'next' the position of the following one: every sample in between has
 * exactly k errors behind it, so the stream crosses there as soon as n
 * reaches nmin[k]. */
static uint64_t simCrossTime(const simBoundary *bd, double log1mp, uint64_t *rng) {
    uint64_t at = 0;
    uint32_t k = 0;

    while (k < bd->len) {
        uint64_t next = at + simGap(log1mp, rng) + 1;
        uint64_t n = bd->nmin[k] > at ? bd->nmin[k] : at;

        if (n < next) return n;
        if (next > bd->horizon) return 0;
        at = next;
        k++;
    }
    return 0;
}

/* The same stream (same draws, same order) fed sample by sample to the
 * library. Slow; used to validate simCrossTime(). */
static uint64_t simCrossTimeLibrary(const simBoundary *bd, double log1mp, uint64_t *rng) {
    gateRegion r;
    uint64_t n, next = simGap(log1mp, rng) + 1;

    gateRegionInit(&r);
    for (n = 1; n <= bd->horizon; n++) {
        int error = n == next;

        if (error) next = n + simGap(log1mp, rng) + 1;
        gateRegionUpdate(&r, &bd->bets, error);
        if (gateRegionCertifies(&r, bd->bar)) return n;
    }
    return 0;
}

/* Exact probability that an IID stream with error rate p crosses within
 * 'horizon' samples (at most the horizon of the boundary): forward
 * recursion over the number of errors of the streams that have not crossed
 * yet. Mass that moves beyond len-1 errors can never come back and is
 * dropped. No randomness involved: this is the number the Monte Carlo
 * frequencies of tables (A) and (E) estimate. */
static double simExactCrossProbWithin(const simBoundary *bd, double p, uint64_t horizon) {
    double *alive, absorbed = 0.0;
    uint32_t low = 0, k;
    uint64_t n;

    if (bd->len == 0) return 0.0;
    if (horizon > bd->horizon) horizon = bd->horizon;
    alive = simAlloc(bd->len, sizeof(double));
    alive[0] = 1.0;
    for (n = 1; n <= horizon; n++) {
        for (k = bd->len - 1; k > 0; k--) alive[k] = alive[k] * (1.0 - p) + alive[k - 1] * p;
        alive[0] *= 1.0 - p;
        /* States with nmin[k] <= n cross at this sample. */
        while (low < bd->len && bd->nmin[low] <= n) low++;
        for (k = 0; k < low; k++) {
            absorbed += alive[k];
            alive[k] = 0.0;
        }
    }
    free(alive);
    return absorbed;
}

static double simExactCrossProb(const simBoundary *bd, double p) {
    return simExactCrossProbWithin(bd, p, bd->horizon);
}

/* ------------------------------ self check 1 ----------------------------- */

static void simSelfCheckStream(uint64_t seed) {
    static const double alphas[] = {0.05, 0.01};
    static const double mults[] = {0.5, 0.8, 1.0};
    const uint64_t horizon = 20000;
    uint64_t streams = 0, crossed = 0, different = 0;
    size_t a, m;
    int i;

    for (a = 0; a < 2; a++) {
        simBoundary bd;

        simBoundaryBuild(&bd, alphas[a], simBar(SIM_ROWS * T), horizon);
        for (m = 0; m < 3; m++) {
            double log1mp = log1p(-alphas[a] * mults[m]);

            for (i = 0; i < 60; i++) {
                uint64_t id = 0x5c000 + a * 1000 + m * 100 + (uint64_t)i;
                uint64_t r1 = simStream(seed, id), r2 = simStream(seed, id);
                uint64_t fast = simCrossTime(&bd, log1mp, &r1);
                uint64_t slow = simCrossTimeLibrary(&bd, log1mp, &r2);

                streams++;
                crossed += slow != 0;
                different += fast != slow;
            }
        }
        simBoundaryFree(&bd);
    }
    printf("self check 1: %llu streams replayed sample by sample through gateRegionUpdate(): "
           "%llu crossed the bar,\n              %llu crossing times differ from the boundary walk\n",
           (unsigned long long)streams, (unsigned long long)crossed, (unsigned long long)different);
    CHECK_EQ_INT(different, 0);
    CHECK(crossed > streams / 4 && crossed < streams); /* both outcomes exercised */
}

/* ------------------------------- table (A) ------------------------------- */

static void simTableA(uint64_t trials, uint64_t seed) {
    static const double alphas[] = {0.05, 0.02};
    static const double mults[] = {1.0, 1.25};
    /* The regions a snapshot splits its share of DELTA over: tested rows
     * x thresholds. The first is no gate's (see the text printed below). */
    static const struct {
        uint32_t rows, thresholds;
    } bars[] = {{1, 1}, {1, T}, {SIM_ROWS, T}};
    const uint64_t horizon = 20000, n = trials * 5;
    clock_t start = clock();
    /* Range of exact/bound over the rows at risk = alpha and 1.25 alpha. */
    double ratio_lo[2] = {INFINITY, INFINITY}, ratio_hi[2] = {0.0, 0.0};
    size_t a, d, m;
    uint64_t row = 0;

    gate_test_current = "table A";
    printf("\n(A) ONE BAD THRESHOLD REACHING ITS BAR\n"
           "    A threshold whose true risk is alpha or more must not be proven. Each trial is one IID stream\n"
           "    of %llu samples in its region and counts if the log wealth reaches the bar after ANY of them;\n"
           "    %llu trials per row. Bound: exp(-bar), the share of DELTA the threshold was given (Ville's\n"
           "    inequality). The bars are those of a first snapshot, which splits its share over every\n"
           "    threshold of every tested row: 1 row x 32 thresholds is a score gate, 2 x 32 a classifier\n"
           "    with two labels. No gate has a bar as low as 1 x 1 (it would be the 'fixed walk' rule of\n"
           "    table (B) with a single row): it is here because its crossings are frequent enough to\n"
           "    compare the simulation with the exact value sharply.\n"
           "    'exact' is the same probability computed without sampling (recursion over the error count);\n"
           "    the simulated frequency must agree with it within Monte Carlo error.\n\n",
           (unsigned long long)horizon, (unsigned long long)n);
    printf("    %-5s  %-9s  %-17s  %7s  %-10s  %7s  %-9s  %-22s  %-9s  %s\n", "alpha", "true risk",
           "rows x thresholds", "log bar", "bound", "crossed", "frequency", "95% Wilson interval", "exact",
           "exact/bound");
    for (a = 0; a < 2; a++) {
        for (d = 0; d < 3; d++) {
            for (m = 0; m < 2; m++) {
                uint32_t regions = bars[d].rows * bars[d].thresholds;
                double alpha = alphas[a], p = alpha * mults[m], bar = simBar(regions);
                double bound = gateBudgetGenerationDelta(SIM_DELTA, 1) / regions;
                double log1mp = log1p(-p), lo, hi, exact;
                uint64_t rng = simStream(seed, 0xa000 + row++), x = 0, i;
                simBoundary bd;

                simBoundaryBuild(&bd, alpha, bar, horizon);
                for (i = 0; i < n; i++) x += simCrossTime(&bd, log1mp, &rng) != 0;
                exact = simExactCrossProb(&bd, p);
                simWilson(x, n, &lo, &hi);
                printf("    %.3f  %.5f    %u x %-13u  %7.4f  %.4e  %7llu  %.3e  [%.3e, %.3e]  %.3e  %5.3f  %s\n",
                       alpha, p, bars[d].rows, bars[d].thresholds, bar, bound, (unsigned long long)x,
                       (double)x / (double)n, lo, hi, exact, exact / bound,
                       exact <= bound && lo <= bound ? "ok" : "VIOLATION");
                /* The guarantee, and the simulator against the exact value
                 * (4.5 standard deviations, +3 for tiny expected counts). */
                CHECK(exact <= bound);
                CHECK(lo <= bound);
                CHECK(fabs((double)x - (double)n * exact) <= 4.5 * sqrt((double)n * exact) + 3.0);
                if (exact / bound < ratio_lo[m]) ratio_lo[m] = exact / bound;
                if (exact / bound > ratio_hi[m]) ratio_hi[m] = exact / bound;
                simBoundaryFree(&bd);
            }
        }
    }
    printf("\n    At true risk = alpha the exact probability is %.2f - %.2f of the bound: the test is valid without\n"
           "    wasting much of its budget. At 1.25 alpha it is already down to %.3f - %.3f of the bound.\n"
           "    The table took %.1f s.\n", ratio_lo[0], ratio_hi[0], ratio_lo[1], ratio_hi[1], simSeconds(start));
}

/* ------------------------------- table (B) ------------------------------- */

/* The traffic of one row, cut at the thresholds: band b holds the scores in
 * [grid[b], grid[b+1]), the last one everything from grid[31] up. */
typedef struct simPopulation {
    double grid[T];
    double mass[T];         /* P(score in band b) */
    double cum[T];          /* P(band <= b) */
    double err[T];          /* P(the guess is wrong | band b) */
    double coverage[T];     /* P(score >= grid[t]) */
    double risk[T];         /* P(the guess is wrong | score >= grid[t]) */
    int bad[T];             /* risk[t] is alpha or more: must not be proven */
} simPopulation;

/* Region t is the union of the bands t..31, so its coverage and risk are
 * suffix sums: the truth the simulation is judged against. 'at_alpha' says
 * that every band errs at exactly alpha, which makes every region bad and
 * which a comparison of rounded sums could not tell. */
static void simPopulationFinish(simPopulation *pop, double alpha, int at_alpha) {
    double c = 0.0, e = 0.0, total = 0.0;
    int b;

    for (b = 0; b < T; b++) {
        total += pop->mass[b];
        pop->cum[b] = total;
    }
    pop->cum[T - 1] = 1.0;
    for (b = T - 1; b >= 0; b--) {
        c += pop->mass[b];
        e += pop->mass[b] * pop->err[b];
        pop->coverage[b] = c;
        pop->risk[b] = e / c;
        pop->bad[b] = at_alpha || pop->risk[b] > alpha;
    }
}

/* Share of the requests whose |logit| is in [a, b] and whose guess is wrong:
 * the integral of the half normal density times 1 - sigmoid(x), by
 * Simpson's rule. */
static double simWrongMass(double sd, double a, double b) {
    const int n = 2000;
    double h = (b - a) / n, sum = 0.0;
    int i;

    for (i = 0; i <= n; i++) {
        double x = a + i * h;
        double f = exp(-x * x / (2.0 * sd * sd)) / (1.0 + exp(x));

        sum += (i == 0 || i == n) ? f : (i % 2 ? 4.0 * f : 2.0 * f);
    }
    return sqrt(2.0 / M_PI) / sd * sum * h / 3.0;
}

/* A calibrated binary scorer: the logit z of a request is N(0, sd^2), the
 * guess is the sign of z, its score is sigmoid(|z|) and it is wrong with
 * probability 1 - score. A small sd is a model that is rarely sure. */
static void simPopulationScorer(simPopulation *pop, double sd, double alpha) {
    int b;

    if (gateGridBuild(pop->grid, 0.5, 0.999) != GATE_OK) {
        fprintf(stderr, "sim_certify: cannot build the grid\n");
        exit(2);
    }
    for (b = 0; b < T; b++) {
        double lo = log(pop->grid[b] / (1.0 - pop->grid[b]));
        /* The last band has no end: 40 further up the integrand is e^-40
         * times smaller and the rest of it does not matter. */
        double hi = b + 1 < T ? log(pop->grid[b + 1] / (1.0 - pop->grid[b + 1])) : lo + 40.0;

        /* P(|z| >= x) = erfc(x / (sd sqrt(2))). */
        pop->mass[b] = erfc(lo / (sd * sqrt(2.0))) - (b + 1 < T ? erfc(hi / (sd * sqrt(2.0))) : 0.0);
        pop->err[b] = simWrongMass(sd, lo, hi) / pop->mass[b];
    }
    simPopulationFinish(pop, alpha, 0);
}

/* One request of the row: the band of its score and whether the guess was
 * wrong. */
static int simDrawSample(const simPopulation *pop, uint64_t *rng, int *error) {
    double u = gate_test_uniform(rng);
    int b = 0;

    /* Number of cum[0..30] at or below u, i.e. the band: a binary search
     * written without branches, because its outcome is random by design
     * and every branch would be mispredicted half of the time. */
    b += (u >= pop->cum[b + 15]) << 4;
    b += (u >= pop->cum[b + 7]) << 3;
    b += (u >= pop->cum[b + 3]) << 2;
    b += (u >= pop->cum[b + 1]) << 1;
    b += (u >= pop->cum[b]);

    *error = gate_test_uniform(rng) < pop->err[b];
    return b;
}

/* The same request drawn the long way, from the definition of the scorer:
 * z by Box-Muller, then its score. Used to validate the bands. */
static double simDrawScore(double sd, uint64_t *rng, int *error) {
    double u1 = 1.0 - gate_test_uniform(rng), u2 = gate_test_uniform(rng);
    double z = sd * sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
    double score = 1.0 / (1.0 + exp(-fabs(z)));

    *error = gate_test_uniform(rng) < 1.0 - score;
    return score;
}

/* THE RULES THAT TABLE (B) COMPARES. A rule says in which order the
 * thresholds of a row may be proven. It is given by its starting points,
 * every 'step'-th threshold counting from the most confident one: under the
 * rule a threshold is proven once it has reached the rule's bar and it is a
 * starting point or the next more confident threshold is proven. A bad
 * threshold can then only be proven if the highest bad threshold at or
 * below some starting point reaches the bar, which is one test per starting
 * point: the rule's bar splits the row's share of DELTA over its starting
 * points only.
 *
 *   step 32  one starting point, the most confident threshold: the fixed
 *            walk of the reference design;
 *   step 8   four starting points: the "entry points" of the first version
 *            of this module (gateIsEntry() / gateEntryPoints(), which the
 *            core no longer has);
 *   step 1   every threshold is a starting point, that is a test of its own
 *            that waits for nothing: THE RULE THE GATE USES, for both kinds
 *            of gate (gateBar(): tested rows x 32).
 *
 * Nothing here depends on the core: the three rules are applied by
 * simProvenTimes() to the crossing times of one and the same race. Ordered
 * by bar, lowest first, which simRace() relies on. */
#define SIM_RULES 3
#define SIM_RULE_IN_USE 2
static const struct {
    int step;
    const char *name, *origin;
} simRules[SIM_RULES] = {
    {T, "fixed walk from the top", "the reference design"},
    {8, "every eighth (old rule)", "the 'entry points' of the first version"},
    {1, "every threshold (IN USE)", "gateBar(): tested rows x 32"}
};

static int simIsStart(int step, int t) {
    return (T - 1 - t) % step == 0;
}

static uint32_t simStarts(int step) {
    return (uint32_t)((T - 1) / step + 1);
}

/* What table (B) and its self checks simulate. */
#define SIM_RACE_ALPHA 0.05
#define SIM_RACE_HORIZON 20000
static const double simScorerSd[2] = {2.7, 5.0};

/* The boundary of each rule's bar, for the row of a classifier with two
 * labels. */
static void simRuleBoundaries(simBoundary *bd) {
    int r;

    for (r = 0; r < SIM_RULES; r++) {
        simBoundaryBuild(&bd[r], SIM_RACE_ALPHA, simBar(SIM_ROWS * simStarts(simRules[r].step)),
                         SIM_RACE_HORIZON);
    }
}

/* One race: 'horizon' requests of the row, every region fed by the requests
 * that fall in it. cross[r][t] receives the request (counted from 1) at
 * which threshold t crossed the bar of rule r, 0 if it did not.
 *
 * Region t holds the bands t..31, so its sample and error counts are suffix
 * sums of per band counters, and it has crossed a bar when its samples
 * reach nmin[its errors]. A threshold meets the bars in ascending order, so
 * it only ever looks at the lowest one it has not crossed: stage[t]. Call
 * the samples it still needs for that one its slack. A correct sample
 * lowers the slack of the regions it falls in by one; an error never lowers
 * it (nmin[k+1] >= nmin[k] + 1). So nothing can cross before 'check' = now
 * + the smallest slack, and the regions only need to be recounted then: a
 * region found at slack 0 crossed with exactly that sample. */
static void simRace(const simPopulation *pop, const simBoundary *bd, uint64_t *rng,
                    uint32_t cross[SIM_RULES][T]) {
    uint32_t seen[T], wrong[T];
    int stage[T];
    uint32_t i, check, horizon = (uint32_t)bd[0].horizon;
    int t;

    for (t = 0; t < T; t++) {
        seen[t] = wrong[t] = 0;
        stage[t] = 0;
    }
    memset(cross, 0, sizeof(uint32_t) * SIM_RULES * T);
    check = bd[0].len ? bd[0].nmin[0] : UINT32_MAX;
    for (i = 1; i <= horizon; i++) {
        int error, band = simDrawSample(pop, rng, &error);
        uint32_t n = 0, k = 0, slack = UINT32_MAX;

        seen[band]++;
        wrong[band] += (uint32_t)error;
        if (i < check) continue;

        for (t = T - 1; t >= 0; t--) {
            n += seen[t];
            k += wrong[t];
            for (; stage[t] < SIM_RULES; stage[t]++) {
                const simBoundary *b = &bd[stage[t]];

                /* Too many errors to make it within the horizon, at this
                 * bar and at every higher one. */
                if (k >= b->len) break;
                if (n < b->nmin[k]) {
                    if (b->nmin[k] - n < slack) slack = b->nmin[k] - n;
                    break;
                }
                cross[stage[t]][t] = i;
            }
        }
        check = slack == UINT32_MAX ? UINT32_MAX : i + slack;
    }
}

/* The same race through the library: every request gets a score inside its
 * band (one in sixteen exactly on the threshold that opens it), the score
 * goes through the real grid lookup and each region is a real gateRegion
 * updated sample by sample. 'aux' only places the score inside the band,
 * which no region can tell. */
static void simRaceLibrary(const simPopulation *pop, const simBoundary *bd, uint64_t *rng,
                           uint32_t cross[SIM_RULES][T], uint64_t *misplaced) {
    gateRegion region[T];
    uint64_t aux = *rng ^ 0x5eed;
    uint32_t i, t, horizon = (uint32_t)bd[0].horizon;
    int r;

    for (t = 0; t < T; t++) gateRegionInit(&region[t]);
    memset(cross, 0, sizeof(uint32_t) * SIM_RULES * T);
    for (i = 1; i <= horizon; i++) {
        int error, band = simDrawSample(pop, rng, &error);
        double lo = pop->grid[band], hi = band + 1 < T ? pop->grid[band + 1] : 1.0;
        double v = gate_test_uniform(&aux), score = v < 0.0625 ? lo : lo + v * (hi - lo);
        uint32_t count;

        if (!(score < hi)) score = lo;      /* rounding must not leave the band */
        count = gateGridCount(pop->grid, T, score);
        /* A score of band b belongs to the regions 0..b and to no other. */
        if (count != (uint32_t)band + 1) (*misplaced)++;
        for (t = 0; t < count; t++) {
            gateRegionUpdate(&region[t], &bd[0].bets, error);
            for (r = 0; r < SIM_RULES; r++) {
                if (cross[r][t] == 0 && gateRegionCertifies(&region[t], bd[r].bar)) cross[r][t] = i;
            }
        }
    }
}

/* ---------------------------- self checks 2, 3 --------------------------- */

static void simSelfCheckRace(uint64_t seed) {
    const int races = 12;
    simPopulation pop;
    simBoundary bd[SIM_RULES];
    uint64_t different = 0, crossed = 0, misplaced = 0;
    int i, r, t;

    simPopulationScorer(&pop, simScorerSd[0], SIM_RACE_ALPHA);
    simRuleBoundaries(bd);
    for (i = 0; i < races; i++) {
        uint64_t r1 = simStream(seed, 0x5d000 + (uint64_t)i), r2 = r1;
        uint32_t fast[SIM_RULES][T], slow[SIM_RULES][T];

        simRace(&pop, bd, &r1, fast);
        simRaceLibrary(&pop, bd, &r2, slow, &misplaced);
        for (r = 0; r < SIM_RULES; r++) {
            for (t = 0; t < T; t++) {
                different += fast[r][t] != slow[r][t];
                crossed += slow[r][t] != 0;
            }
        }
    }
    printf("self check 2: %d races of %llu samples replayed through gateGridCount() and 32 gateRegion "
           "structures:\n              %llu bar crossings, %llu differ from the count based race, "
           "%llu scores in the wrong regions\n",
           races, (unsigned long long)bd[0].horizon, (unsigned long long)crossed,
           (unsigned long long)different, (unsigned long long)misplaced);
    for (r = 0; r < SIM_RULES; r++) simBoundaryFree(&bd[r]);
    CHECK_EQ_INT(different, 0);
    CHECK_EQ_INT(misplaced, 0);
    CHECK(crossed > (uint64_t)races * 5 * SIM_RULES);
}

/* The band masses and error rates come from a numerical integral. Draw the
 * scorer from its definition instead and compare, region by region, how
 * many requests and how many wrong guesses land in each. */
static void simSelfCheckPopulation(uint64_t seed) {
    const uint64_t n = 1000000;
    int p, t;

    for (p = 0; p < 2; p++) {
        uint64_t seen[T] = {0}, wrong[T] = {0}, requests = 0, errors = 0, i;
        uint64_t rng = simStream(seed, 0x5e000 + (uint64_t)p);
        double worst = 0.0;
        int within = 1;
        simPopulation pop;

        simPopulationScorer(&pop, simScorerSd[p], SIM_RACE_ALPHA);
        for (i = 0; i < n; i++) {
            int error;
            uint32_t count = gateGridCount(pop.grid, T, simDrawScore(simScorerSd[p], &rng, &error));

            if (count == 0) {   /* a score is never below grid[0] = 0.5 */
                within = 0;
                continue;
            }
            seen[count - 1]++;
            wrong[count - 1] += (uint64_t)error;
        }
        for (t = T - 1; t >= 0; t--) {
            double expect_n = (double)n * pop.coverage[t];
            double expect_k = expect_n * pop.risk[t];
            double dn, dk;

            requests += seen[t];
            errors += wrong[t];
            dn = fabs((double)requests - expect_n);
            dk = fabs((double)errors - expect_k);
            /* Six standard deviations, +3 for the regions with a handful
             * of expected errors. */
            if (dn > 6.0 * sqrt(expect_n * (1.0 - pop.coverage[t])) + 3.0) within = 0;
            if (dk > 6.0 * sqrt(expect_k) + 3.0) within = 0;
            if (dk / sqrt(expect_k) > worst) worst = dk / sqrt(expect_k);
        }
        printf("self check 3: %llu requests of the sd %.1f scorer drawn from its definition: %.3f%% wrong "
               "(the bands say %.3f%%),\n              largest deviation of a region's error count from "
               "the bands: %.1f standard deviations\n", (unsigned long long)n, simScorerSd[p],
               100.0 * (double)errors / (double)n, 100.0 * pop.risk[0], worst);
        CHECK(within);
    }
}

/* --------------------------- table (B), continued ------------------------ */

#define SIM_NCHECK 3
static const uint32_t simCheckpoints[SIM_NCHECK] = {2000, 5000, 20000};

typedef struct simRuleStats {
    uint64_t *first;                /* per trial: request that proved the first
                                     * threshold, SIM_NEVER if none; sorted */
    double coverage[SIM_NCHECK];    /* summed over the trials: coverage of the
                                     * lowest threshold proven by a checkpoint */
    uint64_t bad;                   /* trials that proved a bad threshold */
    uint64_t crossed;               /* trials in which a threshold crossed */
} simRuleStats;

/* When each threshold became proven (0 = never) under the rule with the
 * starting points of 'step', from when it reached that rule's bar. With
 * step 1, the rule of gate_core.h, the two are the same. */
static void simProvenTimes(int step, const uint32_t *cross, uint32_t *proven) {
    uint32_t above = 0;     /* when the next more confident one was proven */
    int t;

    for (t = T - 1; t >= 0; t--) {
        uint32_t at = 0;

        if (cross[t] != 0) {
            if (simIsStart(step, t)) {
                at = cross[t];
            } else if (above != 0) {
                at = cross[t] > above ? cross[t] : above;
            }
        }
        proven[t] = at;
        above = at;
    }
}

static void simRunRaces(const simPopulation *pop, const simBoundary *bd, uint64_t trials,
                        uint64_t rng, simRuleStats *st) {
    uint64_t i;
    int r, t, c;

    for (r = 0; r < SIM_RULES; r++) {
        memset(&st[r], 0, sizeof(st[r]));
        st[r].first = simAlloc(trials, sizeof(uint64_t));
    }
    for (i = 0; i < trials; i++) {
        uint32_t cross[SIM_RULES][T], proven[T];

        simRace(pop, bd, &rng, cross);
        for (r = 0; r < SIM_RULES; r++) {
            uint32_t first = 0;
            int bad = 0, crossed = 0;

            simProvenTimes(simRules[r].step, cross[r], proven);
            for (t = 0; t < T; t++) {
                crossed |= cross[r][t] != 0;
                if (proven[t] == 0) continue;
                bad |= pop->bad[t];
                if (first == 0 || proven[t] < first) first = proven[t];
            }
            st[r].first[i] = first ? first : SIM_NEVER;
            st[r].bad += (uint64_t)bad;
            st[r].crossed += (uint64_t)crossed;
            for (c = 0; c < SIM_NCHECK; c++) {
                /* The row answers at the lowest threshold proven so far. */
                for (t = 0; t < T; t++) {
                    if (proven[t] != 0 && proven[t] <= simCheckpoints[c]) break;
                }
                if (t < T) st[r].coverage[c] += pop->coverage[t];
            }
        }
    }
    for (r = 0; r < SIM_RULES; r++) qsort(st[r].first, trials, sizeof(uint64_t), simCmpU64);
}

/* Results of one calibrated scorer under the three rules. */
static void simPrintScorer(const simBoundary *bd, const simRuleStats *st, uint64_t trials, double bound) {
    char b1[32], b2[32];
    uint64_t never, med[SIM_RULES];
    double lo, hi;
    int r, c;

    printf("    %-26s  %-23s  %s\n    %-26s  %7s %7s  %6s", "", "first proven threshold",
           "mean coverage of the lowest proven threshold", "rule", "median", "p90", "never");
    for (c = 0; c < SIM_NCHECK; c++) printf("  after %5u", simCheckpoints[c]);
    printf(" samples\n");
    for (r = 0; r < SIM_RULES; r++) {
        for (never = 0; never < trials && st[r].first[trials - 1 - never] == SIM_NEVER; never++) {}
        med[r] = simQuantile(st[r].first, trials, 0.5);
        printf("    %-26s  %7s %7s  %5.1f%%", simRules[r].name,
               simFmt(b1, sizeof(b1), med[r], SIM_RACE_HORIZON),
               simFmt(b2, sizeof(b2), simQuantile(st[r].first, trials, 0.9), SIM_RACE_HORIZON),
               100.0 * (double)never / (double)trials);
        for (c = 0; c < SIM_NCHECK; c++) printf("  %11.4f", st[r].coverage[c] / (double)trials);
        printf("\n");
        /* No proof can come before a threshold could have crossed this
         * bar with no error at all. */
        CHECK(st[r].first[0] >= bd[r].nmin[0]);
    }
    /* What the rule in use pays, or gains, for being one concept. */
    printf("    'every threshold' against the other two, median samples to the first proven threshold:\n       ");
    for (r = 0; r < SIM_RULES; r++) {
        if (r == SIM_RULE_IN_USE) continue;
        if (med[r] == SIM_NEVER || med[SIM_RULE_IN_USE] == SIM_NEVER) {
            printf(" n/a");
        } else {
            printf(" %+.1f%%", 100.0 * ((double)med[SIM_RULE_IN_USE] / (double)med[r] - 1.0));
        }
        printf(" vs '%s'%s", simRules[r].name, r == 0 ? "," : "\n");
    }
    printf("\n    %-26s  %-25s  %-9s  %-22s  %s\n", "rule", "bad threshold ever proven", "frequency",
           "95% Wilson interval", "bound");
    for (r = 0; r < SIM_RULES; r++) {
        simWilson(st[r].bad, trials, &lo, &hi);
        printf("    %-26s  %7llu of %-14llu  %.3e  [%.3e, %.3e]  %.1e  %s\n", simRules[r].name,
               (unsigned long long)st[r].bad, (unsigned long long)trials, (double)st[r].bad / (double)trials,
               lo, hi, bound, lo <= bound ? "ok" : "VIOLATION");
        CHECK(lo <= bound);
    }
}

static void simTableB(uint64_t trials, uint64_t seed) {
    static const char *what[2] = {"a model that is rarely sure", "a model that is often sure"};
    const double alpha = SIM_RACE_ALPHA, *sds = simScorerSd;
    const double bound = gateBudgetGenerationDelta(SIM_DELTA, 1) / SIM_ROWS;
    clock_t start = clock();
    simPopulation pop[2], flat;
    simBoundary bd[SIM_RULES];
    simRuleStats st[SIM_RULES];
    double lo, hi;
    int p, r, t;

    gate_test_current = "table B";
    simRuleBoundaries(bd);
    for (p = 0; p < 2; p++) simPopulationScorer(&pop[p], sds[p], alpha);

    printf("\n(B) DESIGN TABLE: THREE RULES FOR ONE ROW OF A CLASSIFIER WITH TWO LABELS\n"
           "    32 nested thresholds fed by one stream. Grid 0.5 .. 0.999, alpha = %.2f. Each trial is %d\n"
           "    teacher answers to guesses of this label; an answer whose guess had score s feeds every\n"
           "    threshold <= s, so one answer moves many tests.\n"
           "    THE GATE USES THE RULE 'every threshold': each threshold is a test of its own, PROVEN as soon\n"
           "    as it has reached the bar, and the row answers at the lowest proven threshold. This table\n"
           "    keeps, on the same streams, the two rules it was chosen over. Under those a threshold that\n"
           "    has reached the bar is proven only if it is a starting point or the next more confident\n"
           "    threshold is proven. Fewer starting points make the bar,\n"
           "    log(g (g + 1) x rows x starting points / DELTA), lower but leave more thresholds waiting\n"
           "    for the ones above them:\n",
           alpha, SIM_RACE_HORIZON);
    for (r = 0; r < SIM_RULES; r++) {
        printf("        %-26s  %2u starting point%s  bar log(2 x %d x %2u / %.2f) = %.4f   %s\n", simRules[r].name,
               simStarts(simRules[r].step), simStarts(simRules[r].step) == 1 ? " " : "s",
               SIM_ROWS, simStarts(simRules[r].step), SIM_DELTA, bd[r].bar, simRules[r].origin);
    }
    printf("    Whatever the rule, the row may prove a bad threshold with probability at most\n"
           "    DELTA / (g (g + 1) x rows) = %.1e.\n\n"
           "    The scorers: the logit z of a request is N(0, sd^2), its score is sigmoid(|z|) and the guess\n"
           "    is wrong with probability 1 - score (calibrated). 'coverage' = share of the row's requests\n"
           "    at or above the threshold, 'risk' = share of those that are wrong, BAD = risk above alpha.\n"
           "    '8th' marks the four starting points of the old rule; the gate itself has no such thing.\n\n"
           "    %-20s     sd %-31.1fsd %.1f\n    %2s  %-9s  %-5s", bound, "", sds[0], sds[1], "t", "threshold",
           "8th");
    for (p = 0; p < 2; p++) printf("     %-8s  %-7s  %s", "coverage", "risk", "risk/alpha");
    printf("\n");
    for (t = 0; t < T; t++) {
        printf("    %2d  %-9.6f  %-5s", t, pop[0].grid[t], simIsStart(8, t) ? "8th" : "");
        for (p = 0; p < 2; p++) {
            printf("     %.4f    %.5f  %6.3f%s", pop[p].coverage[t], pop[p].risk[t], pop[p].risk[t] / alpha,
                   pop[p].bad[t] ? " BAD" : p == 0 ? "    " : "");
        }
        printf("\n");
    }

    for (p = 0; p < 2; p++) {
        for (t = 0; t < T - 1 && pop[p].bad[t]; t++) {}     /* the lowest good threshold */
        printf("\n    (B%d) sd %.1f, %s: %.1f%% of its guesses are wrong, and the lowest good threshold\n"
               "    is %d (coverage %.4f). %llu trials.\n\n", p + 1, sds[p], what[p], 100.0 * pop[p].risk[0],
               t, pop[p].coverage[t], (unsigned long long)trials);
        simRunRaces(&pop[p], bd, trials, simStream(seed, 0xb100 + (uint64_t)p), st);
        simPrintScorer(bd, st, trials, bound);
        for (r = 0; r < SIM_RULES; r++) free(st[r].first);
    }

    flat = pop[1];
    for (t = 0; t < T; t++) flat.err[t] = alpha;
    simPopulationFinish(&flat, alpha, 1);
    printf("\n    (B3) Least favourable for the bound: the traffic of the sd %.1f scorer, but every band errs at\n"
           "    exactly alpha, so every threshold has risk = alpha and any proof is a false one. 'reached'\n"
           "    counts the trials in which some threshold reached its bar. Under the rule in use that is a\n"
           "    proof; under the other two it only becomes one through a starting point. %llu trials.\n\n",
           sds[1], (unsigned long long)trials);
    printf("    %-26s  %-22s  %-22s  %-22s  %s\n", "rule", "some threshold reached",
           "some threshold proven", "95% Wilson interval", "bound");
    simRunRaces(&flat, bd, trials, simStream(seed, 0xb300), st);
    for (r = 0; r < SIM_RULES; r++) {
        simWilson(st[r].bad, trials, &lo, &hi);
        printf("    %-26s  %7llu  %.3e      %7llu  %.3e      [%.3e, %.3e]  %.1e  %s\n", simRules[r].name,
               (unsigned long long)st[r].crossed, (double)st[r].crossed / (double)trials,
               (unsigned long long)st[r].bad, (double)st[r].bad / (double)trials, lo, hi, bound,
               lo <= bound ? "ok" : "VIOLATION");
        CHECK(lo <= bound);
        free(st[r].first);
    }
    /* "Proven as soon as it has reached the bar". */
    CHECK_EQ_INT(st[SIM_RULE_IN_USE].bad, st[SIM_RULE_IN_USE].crossed);

    for (r = 0; r < SIM_RULES; r++) simBoundaryFree(&bd[r]);
    printf("\n    The table took %.1f s.\n", simSeconds(start));
}

/* ------------------------------- table (C) ------------------------------- */

/* Crossing time of a stream without errors, straight from the library. */
static uint64_t simZeroErrorCount(double alpha, double bar, uint64_t cap) {
    gateBets b;
    gateRegion r;

    gateBetsInit(&b, alpha);
    gateRegionInit(&r);
    while (r.n < cap) {
        gateRegionUpdate(&r, &b, 0);
        if (gateRegionCertifies(&r, bar)) return r.n;
    }
    return 0;
}

static void simTableC(uint64_t trials, uint64_t seed) {
    static const double alphas[] = {0.05, 0.02, 0.01};
    static const struct {
        uint32_t regions;
        const char *name;
    } bars[] = {
        {T, "score gate, 1 row x 32 thresholds"},
        {SIM_ROWS * T, "classifier, 2 labels x 32 thresholds"}
    };
    static const double mults[] = {0.25, 0.5, 0.8};
    static const char *names[] = {"alpha/4", "alpha/2", "0.8 alpha"};
    const uint64_t horizon = 100000;
    clock_t start = clock();
    uint64_t *times = simAlloc(trials, sizeof(uint64_t));
    uint64_t row = 0, i;
    double scaled[3][4];    /* median x alpha of the score gate rows */
    char b1[32], b2[32];
    size_t a, d, m;

    gate_test_current = "table C";
    printf("\n(C) SAMPLES A GOOD THRESHOLD NEEDS TO REACH THE BAR   (DELTA = %.2f, first snapshot)\n"
           "    'samples' are teacher answers to guesses that fall in the threshold's region: every request\n"
           "    of the label while nothing is proven for it, only its spot checks afterwards. With no\n"
           "    errors the count is exact; at a true risk > 0 it is the median and the 90th percentile of\n"
           "    %llu IID streams, each followed for at most %llu samples ('never' = share that had not\n"
           "    crossed by then). A stream with k errors behind it crosses when it holds the samples that\n"
           "    k errors need, so the crossing times pile up on a few values and a median can move by a\n"
           "    whole step from one seed to the next.\n\n",
           SIM_DELTA, (unsigned long long)trials, (unsigned long long)horizon);
    printf("    %-5s  %-37s  %7s   %-9s %-7s  %8s %8s  %6s\n", "alpha", "bar of", "log bar", "true risk", "",
           "median", "p90", "never");
    for (a = 0; a < 3; a++) {
        for (d = 0; d < 2; d++) {
            double alpha = alphas[a], bar = simBar(bars[d].regions);
            uint64_t n0 = simZeroErrorCount(alpha, bar, horizon);
            simBoundary bd;

            simBoundaryBuild(&bd, alpha, bar, horizon);
            /* The boundary table and the library agree on the one point
             * that needs no simulation. */
            CHECK(bd.len > 0 && bd.nmin[0] == n0);
            if (d == 0) scaled[a][0] = (double)n0 * alpha;
            printf("    %.3f  %-37s  %7.4f   %-9s %.5f  %8llu %8llu  %5.1f%%\n", alpha, bars[d].name, bar,
                   "0 (exact)", 0.0, (unsigned long long)n0, (unsigned long long)n0, 0.0);
            for (m = 0; m < 3; m++) {
                double p = alpha * mults[m], log1mp = log1p(-p);
                uint64_t rng = simStream(seed, 0xc000 + row++), never = 0, med, p90;

                for (i = 0; i < trials; i++) {
                    uint64_t t = simCrossTime(&bd, log1mp, &rng);

                    times[i] = t ? t : SIM_NEVER;
                    never += t == 0;
                }
                qsort(times, trials, sizeof(uint64_t), simCmpU64);
                med = simQuantile(times, trials, 0.5);
                p90 = simQuantile(times, trials, 0.9);
                printf("    %.3f  %-37s  %7.4f   %-9s %.5f  %8s %8s  %5.1f%%\n", alpha, bars[d].name, bar,
                       names[m], p, simFmt(b1, sizeof(b1), med, horizon), simFmt(b2, sizeof(b2), p90, horizon),
                       100.0 * (double)never / (double)trials);
                /* No stream can beat the zero error count. */
                CHECK(times[0] >= n0);
                if (d == 0) scaled[a][m + 1] = med == SIM_NEVER ? NAN : (double)med * alpha;
            }
            simBoundaryFree(&bd);
            printf("\n");
        }
    }
    free(times);
    printf("    Scaling. The sample count is close to a constant divided by alpha. Median samples x alpha at\n"
           "    the score gate bar, for alpha = 0.05 / 0.02 / 0.01:\n"
           "        no errors %.1f / %.1f / %.1f    alpha/4 %.1f / %.1f / %.1f    alpha/2 %.1f / %.1f / %.1f"
           "    0.8 alpha %.0f / %.0f / %.0f\n",
           scaled[0][0], scaled[1][0], scaled[2][0], scaled[0][1], scaled[1][1], scaled[2][1],
           scaled[0][2], scaled[1][2], scaled[2][2], scaled[0][3], scaled[1][3], scaled[2][3]);
    printf("    The table took %.1f s.\n", simSeconds(start));
}

/* ------------------------------- table (D) ------------------------------- */

#define SIM_DRIFT_CAP 400000    /* spot checks a stream is followed for */

/* Spot check at which a fresh detector alarms on an IID stream with error
 * rate p, SIM_NEVER if not within the cap. */
static uint64_t simAlarmTime(const gateDriftBets *b, double p, uint64_t *rng) {
    const double log_alarm = log(GATE_DRIFT_ALARM);
    gateDrift d;

    gateDriftInit(&d);
    while (d.n < SIM_DRIFT_CAP) {
        if (gateDriftPush(&d, b, gate_test_uniform(rng) < p, log_alarm)) return d.n;
    }
    return SIM_NEVER;
}

/* One row of table (D): n streams at error rate p. Fills 'times' (sorted)
 * and returns the mean, with the streams that never alarmed counted at the
 * cap; '*never' receives how many those were. */
static double simAlarmTimes(double alpha, double p, uint64_t n, uint64_t rng,
                            uint64_t *times, uint64_t *never) {
    gateDriftBets b;
    double sum = 0.0;
    uint64_t i;

    gateDriftBetsInit(&b, alpha);
    *never = 0;
    for (i = 0; i < n; i++) {
        times[i] = simAlarmTime(&b, p, &rng);
        *never += times[i] == SIM_NEVER;
        sum += times[i] == SIM_NEVER ? (double)SIM_DRIFT_CAP : (double)times[i];
    }
    qsort(times, n, sizeof(uint64_t), simCmpU64);
    return sum / (double)n;
}

static void simTableD(uint64_t trials, uint64_t seed) {
    static const double alphas[] = {0.05, 0.02, 0.01};
    static const double calm[] = {0.5, 0.75, 1.0};
    static const char *calm_names[] = {"alpha/2", "0.75 alpha", "alpha"};
    static const double jump[] = {1.5, 2.0, 5.0, 0.0};  /* 0: half of the answers wrong */
    static const char *jump_names[] = {"1.5 alpha", "2 alpha", "5 alpha", "0.5"};
    const uint64_t n1 = trials / 100 < 10 ? 10 : trials / 100;
    const uint64_t n2 = trials / 10 < 10 ? 10 : trials / 10;
    clock_t start = clock();
    uint64_t *times = simAlloc(n1 > n2 ? n1 : n2, sizeof(uint64_t));
    uint64_t row = 0, never;
    double mean, lowest = INFINITY, slowest = 0.0;
    char b1[32], b2[32];
    size_t a, m;

    gate_test_current = "table D";
    printf("\n(D) DRIFT DETECTOR   likelihood ratios for 2 alpha and 4 alpha, alarm at %.0f\n"
           "    Every stream starts from a fresh detector and is pushed spot check by spot check.\n\n"
           "    (D1) False alarms: spot checks until the alarm on IID traffic whose risk is at or below the\n"
           "    target. %llu streams per row, each followed for at most %d spot checks. Where some streams\n"
           "    never alarm the mean counts them as %d and is a lower bound. The detector promises a\n"
           "    mean of at least %.0f.\n\n",
           GATE_DRIFT_ALARM, (unsigned long long)n1, SIM_DRIFT_CAP, SIM_DRIFT_CAP, GATE_DRIFT_ALARM);
    printf("    %-5s  %-18s  %10s  %11s  %s %d\n", "alpha", "true risk", "median", "mean", "no alarm in",
           SIM_DRIFT_CAP);
    for (a = 0; a < 3; a++) {
        for (m = 0; m < 3; m++) {
            double p = alphas[a] * calm[m];

            mean = simAlarmTimes(alphas[a], p, n1, simStream(seed, 0xd000 + row++), times, &never);
            printf("    %.3f  %-10s %.5f  %10s  %s%9.0f  %5.1f%%\n", alphas[a], calm_names[m], p,
                   simFmt(b1, sizeof(b1), simQuantile(times, n1, 0.5), SIM_DRIFT_CAP), never ? ">=" : "  ",
                   mean, 100.0 * (double)never / (double)n1);
            /* What the detector promises, and what a run of errors can do
             * at best: five in a row reach (2^5 + 4^5) / 2 = 528. */
            CHECK(mean >= GATE_DRIFT_ALARM);
            CHECK(times[0] >= 6);
            if (m == 2 && mean < lowest) lowest = mean;
        }
        printf("\n");
    }
    printf("    At risk = alpha, the worst case the promise covers, the mean is at least %.0f times the\n"
           "    alarm level.\n\n", floor(lowest / GATE_DRIFT_ALARM));

    printf("    (D2) Detection delay: spot checks from a jump of the risk to the alarm, %llu streams per\n"
           "    row. The detector is at its restart value when the risk jumps. That is the worst case:\n"
           "    whatever it saw before can only bring the alarm forward.\n\n", (unsigned long long)n2);
    printf("    %-5s  %-19s %10s  %11s  %10s\n", "alpha", "risk after the jump", "median", "mean", "p90");
    for (a = 0; a < 3; a++) {
        for (m = 0; m < 4; m++) {
            double p = jump[m] > 0.0 ? alphas[a] * jump[m] : 0.5;

            mean = simAlarmTimes(alphas[a], p, n2, simStream(seed, 0xd800 + row++), times, &never);
            printf("    %.3f  %-10s %.5f  %10s  %11.1f  %10s\n", alphas[a], jump_names[m], p,
                   simFmt(b1, sizeof(b1), simQuantile(times, n2, 0.5), SIM_DRIFT_CAP), mean,
                   simFmt(b2, sizeof(b2), simQuantile(times, n2, 0.9), SIM_DRIFT_CAP));
            /* A risk this far above the target is always caught. */
            CHECK_EQ_INT(never, 0);
            CHECK(times[0] >= 6);
            if (m == 3 && mean > slowest) slowest = mean;
        }
        printf("\n");
    }
    printf("    A policy that gets half of its answers wrong is stopped after %.0f spot checks on average.\n"
           "    The table took %.1f s.\n", ceil(slowest), simSeconds(start));
    free(times);
}

/* ------------------------------- table (E) ------------------------------- */

#define SIM_LOST UINT32_MAX     /* a lag: the answer never arrives */

/* How a teacher answers the requests of one region. The requests are the
 * clock: request i is asked at time i and its answer arrives at time
 * i + lag, when 'lag' more requests have been asked (0: at once), or never.
 * Answers that arrive at the same time arrive in the order of their
 * requests, and the region is looked at after every one of them.
 *
 * The lag of a request is a number drawn uniformly from 0..any_lag whatever
 * the answer is, plus wrong_lag if the guess was wrong. So any_lag is a
 * teacher that is simply slow, and wrong_lag one that is slow, or silent,
 * exactly where the gate is wrong: the worst case for a rule that would
 * rather not count what it has not seen. */
typedef struct simTeacher {
    char name[40];
    uint32_t wrong_lag;     /* SIM_LOST: wrong answers never arrive */
    uint32_t any_lag;
} simTeacher;

/* The teachers of table (E). What a lag does depends on how it compares
 * with the requests a threshold needs, and those go like 1 / alpha (an
 * error free threshold reaches the bar of a score gate after about
 * 11 / alpha requests), so the lags are given in units of 1 / alpha
 * requests: 5 / alpha is 100 requests at alpha = 0.05 and 250 at 0.02. */
#define SIM_TEACHERS 4
#define SIM_TEACHER_LOST 0      /* wrong answers never arrive */
#define SIM_TEACHER_LATE 1      /* wrong answers late by nearly half of what a proof needs */
static const struct {
    double wrong_per_alpha;     /* < 0: wrong answers never arrive */
    double any_per_alpha;
} simTeacherKinds[SIM_TEACHERS] = {{-1.0, 0.0}, {5.0, 0.0}, {2.0, 0.0}, {0.0, 1.0}};

static void simTeacherFor(int kind, double alpha, simTeacher *t) {
    double wrong = simTeacherKinds[kind].wrong_per_alpha, any = simTeacherKinds[kind].any_per_alpha;

    t->wrong_lag = wrong < 0.0 ? SIM_LOST : (uint32_t)floor(wrong / alpha + 0.5);
    t->any_lag = (uint32_t)floor(any / alpha + 0.5);
    if (t->any_lag != 0) {
        snprintf(t->name, sizeof(t->name), "every answer 0-%u requests late", t->any_lag);
    } else if (t->wrong_lag == SIM_LOST) {
        snprintf(t->name, sizeof(t->name), "wrong answers never arrive");
    } else {
        snprintf(t->name, sizeof(t->name), "wrong answers %u requests late", t->wrong_lag);
    }
}

/* THE TWO RULES OF TABLE (E), for one region and the requests that fall in
 * it.
 *
 *   COUNTED  the rule of the gate: a request is an error of the region from
 *            the moment it is asked until its answer arrives, and for good
 *            if it never does. The region holds every request asked so
 *            far; its errors are the wrong answers and the outstanding
 *            ones. Through the library: gateRegionUpdate(1) when the
 *            request is asked, gateRegionFlip() when a correct answer
 *            arrives.
 *   IGNORED  what the gate does NOT do: a request is left out until its
 *            answer arrives. The region holds the requests whose answers
 *            are in: gateRegionUpdate() with the answer when it arrives.
 *
 * COUNTED never holds more wealth than the same requests with their true
 * answers (an outstanding answer is an error factor where the true answer
 * is an error factor or better), so it reaches the bar no more often and no
 * earlier than table (A) says, whatever the teacher does. IGNORED holds
 * the wealth of whatever the teacher has let through so far. */

/* The wrong guesses of one IID stream with error rate p: fills err[] with
 * the requests (counted from 1, ascending) that are wrong, up to the
 * horizon, and returns how many there are. */
static uint32_t simDrawErrors(double log1mp, uint64_t horizon, uint64_t *rng, uint32_t *err) {
    uint64_t at = 0;
    uint32_t n = 0;

    for (;;) {
        at += simGap(log1mp, rng) + 1;
        if (at > horizon) return n;
        err[n++] = (uint32_t)at;
    }
}

/* Request at which the IGNORED region reaches the bar when correct answers
 * arrive at once and wrong ones 'lag' requests late (SIM_LOST: never); 0 if
 * it does not within the horizon. After request n the region holds the
 * correct requests up to n and the wrong ones up to n - lag. Between two
 * events (a wrong request is asked, a wrong answer arrives) every request
 * is a correct one that adds a sample, so the region reaches the bar at the
 * first of them at which its samples reach nmin[its errors].
 *
 * With lag 0 this is the stream with its true answers, the walk of
 * simCrossTime() over a stored stream. It is also the COUNTED region under
 * any teacher that delays wrong answers only: those are errors of the
 * region from the start, and their arrival changes nothing. */
static uint64_t simLagWalk(const simBoundary *bd, const uint32_t *err, uint32_t nerr, uint32_t lag) {
    uint32_t asked = 0, in = 0;     /* wrong requests asked, wrong answers in */
    uint64_t start = 1;             /* first request that may reach the bar */

    for (;;) {
        uint64_t next_err = asked < nerr ? err[asked] : SIM_NEVER;
        uint64_t next_in = (lag != SIM_LOST && in < nerr) ? (uint64_t)err[in] + lag : SIM_NEVER;
        uint64_t t = next_err < next_in ? next_err : next_in;
        uint64_t end = t - 1 < bd->horizon ? t - 1 : bd->horizon, need;

        /* Too many errors are in to make it within the horizon. */
        if (in >= bd->len) return 0;
        /* samples = n - asked + in must reach nmin[in]. */
        need = (uint64_t)bd->nmin[in] + asked - in;
        if (need < start) need = start;
        if (need <= end) return need;
        if (t > bd->horizon) return 0;
        if (next_in == t) in++;
        if (next_err == t) {
            asked++;
            start = t + 1;  /* request t is wrong: it adds nothing now */
        } else {
            start = t;      /* request t is correct and comes after the answer that just arrived */
        }
    }
}

/* Could the IGNORED region of this stream reach the bar if no answer is more
 * than 'late' requests late? At time n it holds at most n requests, and
 * every wrong answer up to request n - late is in, so it needs
 * n >= nmin[errors among the first n - late requests]: the stream with its
 * true answers must reach a boundary lowered by 'late'. Necessary, not
 * sufficient: table (E) replays the streams that pass, request by request,
 * and no other. */
static int simIgnoredCandidate(const simBoundary *bd, const uint32_t *err, uint32_t nerr, uint64_t late) {
    uint32_t k;

    for (k = 0; k < bd->len && k <= nerr; k++) {
        /* The last request that has exactly k errors behind it. */
        uint64_t last = k < nerr ? (uint64_t)err[k] - 1 : bd->horizon;
        uint64_t n = last + late < bd->horizon ? last + late : bd->horizon;

        if (n >= bd->nmin[k]) return 1;
    }
    return 0;
}

/* Working memory of simLateReplay(), for streams of at most 'horizon'
 * requests. */
typedef struct simLateScratch {
    uint32_t *err;          /* the wrong requests of the stream, ascending */
    uint8_t *wrong;         /* [request] */
    uint32_t *head, *tail;  /* [time] first and last request whose answer
                             * arrives then, 0 = none */
    uint32_t *next;         /* [request] the next one arriving at that time */
} simLateScratch;

static void simLateScratchInit(simLateScratch *ws, uint64_t horizon) {
    ws->err = simAlloc(horizon + 1, sizeof(uint32_t));
    ws->wrong = simAlloc(horizon + 1, sizeof(uint8_t));
    ws->head = simAlloc(horizon + 1, sizeof(uint32_t));
    ws->tail = simAlloc(horizon + 1, sizeof(uint32_t));
    ws->next = simAlloc(horizon + 1, sizeof(uint32_t));
}

static void simLateScratchFree(simLateScratch *ws) {
    free(ws->err);
    free(ws->wrong);
    free(ws->head);
    free(ws->tail);
    free(ws->next);
}

/* The stream ws->err[0..nerr-1] under teacher 't', request by request and
 * answer by answer. '*counted' and '*ignored' receive the request at which
 * the region of each rule first reached the bar, 0 if it did not within the
 * horizon. With 'library' every request and every answer goes through
 * gateRegionUpdate() / gateRegionFlip() and the bar is tested by
 * gateRegionCertifies(); without, the two regions are kept as counts and
 * tested against the boundary. 'lag_rng' draws the lags: one draw per
 * request if the teacher has an any_lag, none otherwise.
 *
 * Returns 0, or 1 if at the end the counters of the library's regions are
 * not the counts kept here (or a certification was latched, which nothing
 * here asks for). */
static int simLateReplay(const simBoundary *bd, const simTeacher *t, simLateScratch *ws, uint32_t nerr,
                         uint64_t lag_rng, int library, uint64_t *counted, uint64_t *ignored) {
    gateRegion rc, ri;                  /* library: the two regions */
    uint64_t in_ok = 0;                 /* correct answers in */
    uint64_t ign_n = 0, ign_k = 0;      /* the IGNORED region as counts */
    uint64_t n, horizon = bd->horizon;
    uint32_t e = 0, i;

    *counted = *ignored = 0;
    gateRegionInit(&rc);
    gateRegionInit(&ri);
    memset(ws->head, 0, (horizon + 1) * sizeof(uint32_t));
    for (n = 1; n <= horizon && !(*counted && *ignored); n++) {
        int is_wrong = e < nerr && ws->err[e] == n;
        uint64_t due = n;

        if (t->any_lag) due += gate_test_rand64(&lag_rng) % ((uint64_t)t->any_lag + 1);
        if (is_wrong) {
            e++;
            due = t->wrong_lag == SIM_LOST ? SIM_NEVER : due + t->wrong_lag;
        }
        ws->wrong[n] = (uint8_t)is_wrong;
        ws->next[n] = 0;
        if (due <= horizon) {
            if (ws->head[due]) ws->next[ws->tail[due]] = (uint32_t)n; else ws->head[due] = (uint32_t)n;
            ws->tail[due] = (uint32_t)n;
        }
        /* Asked: an error of the COUNTED region until it is answered. */
        if (library) gateRegionUpdate(&rc, &bd->bets, 1);

        for (i = ws->head[n]; i != 0; i = ws->next[i]) {
            ign_n++;
            if (ws->wrong[i]) {
                /* A wrong answer: COUNTED has it as an error already. */
                ign_k++;
                if (library) gateRegionUpdate(&ri, &bd->bets, 1);
                continue;
            }
            in_ok++;
            if (library) {
                gateRegionFlip(&rc, &bd->bets);
                gateRegionUpdate(&ri, &bd->bets, 0);
                if (!*ignored && gateRegionCertifies(&ri, bd->bar)) *ignored = n;
            } else if (!*ignored && ign_k < bd->len && ign_n >= bd->nmin[ign_k]) {
                *ignored = n;
            }
        }
        /* COUNTED only gains while answers arrive: once per request is
         * enough to see it reach the bar. */
        if (*counted) continue;
        if (library) {
            if (gateRegionCertifies(&rc, bd->bar)) *counted = n;
        } else {
            uint64_t k = n - in_ok;     /* wrong or outstanding */

            if (k < bd->len && n >= bd->nmin[k]) *counted = n;
        }
    }
    n--;    /* requests asked */
    return library && (rc.n != n || rc.errors != n - in_ok || ri.n != ign_n || ri.errors != ign_k ||
                       rc.certified_at != 0 || ri.certified_at != 0);
}

/* ------------------------------ self check 4 ----------------------------- */

static void simSelfCheckLate(uint64_t seed) {
    static const double mults[] = {0.5, 1.0};
    const uint64_t horizon = 4000;
    const double alpha = 0.05;
    const int per = 40;
    uint64_t streams = 0, reached_c = 0, reached_i = 0, differ = 0, shortcut = 0, early = 0, walk = 0;
    uint64_t skipped = 0, counters = 0;
    simBoundary bd;
    simLateScratch ws;
    size_t m;
    int t, i;

    /* A bar no gate has, so that both outcomes are frequent. */
    simBoundaryBuild(&bd, alpha, simBar(1), horizon);
    simLateScratchInit(&ws, horizon);
    for (t = 0; t < SIM_TEACHERS; t++) {
        simTeacher teacher, *tch = &teacher;

        simTeacherFor(t, alpha, tch);
        for (m = 0; m < 2; m++) {
            double log1mp = log1p(-alpha * mults[m]);

            for (i = 0; i < per; i++) {
                uint64_t id = 0x5f000 + (uint64_t)t * 1000 + m * 100 + (uint64_t)i;
                uint64_t r1 = simStream(seed, id), r2 = r1, lag_rng = simStream(seed ^ 0x1a6, id);
                uint32_t nerr = simDrawErrors(log1mp, horizon, &r1, ws.err);
                uint64_t truth = simLagWalk(&bd, ws.err, nerr, 0), c1, i1, c2, i2;

                (void)simLateReplay(&bd, tch, &ws, nerr, lag_rng, 0, &c1, &i1);
                counters += (uint64_t)simLateReplay(&bd, tch, &ws, nerr, lag_rng, 1, &c2, &i2);
                streams++;
                reached_c += c2 != 0;
                reached_i += i2 != 0;
                differ += (c1 != c2) + (i1 != i2);
                /* What table (E) computes instead of a replay. */
                if (tch->any_lag == 0) {
                    shortcut += (c2 != truth) + (i2 != simLagWalk(&bd, ws.err, nerr, tch->wrong_lag));
                } else {
                    shortcut += c2 != 0 && truth == 0;
                    shortcut += i2 != 0 && !simIgnoredCandidate(&bd, ws.err, nerr, tch->any_lag);
                    /* ... and the shortcut is not "replay everything". */
                    skipped += truth == 0 && !simIgnoredCandidate(&bd, ws.err, nerr, tch->any_lag);
                }
                /* COUNTED never reaches the bar before the true answers. */
                early += c2 != 0 && (truth == 0 || truth > c2);
                /* The stored stream is the stream of tables (A) and (C). */
                walk += truth != simCrossTime(&bd, log1mp, &r2);
            }
        }
    }
    printf("self check 4: %llu streams with late answers replayed request by request and answer by answer through\n"
           "              gateRegionUpdate() / gateRegionFlip(): the region that counts outstanding answers as\n"
           "              errors reached the bar in %llu, the one that ignores them in %llu; %llu crossing times\n"
           "              differ from the count based replay, %llu from the shortcuts of table (E), %llu came\n"
           "              before the crossing of the true answers, %llu regions ended with other counters than\n"
           "              the requests and answers they were given\n",
           (unsigned long long)streams, (unsigned long long)reached_c, (unsigned long long)reached_i,
           (unsigned long long)differ, (unsigned long long)(shortcut + walk), (unsigned long long)early,
           (unsigned long long)counters);
    simLateScratchFree(&ws);
    simBoundaryFree(&bd);
    CHECK_EQ_INT(differ, 0);
    CHECK_EQ_INT(counters, 0);
    CHECK_EQ_INT(shortcut, 0);
    CHECK_EQ_INT(walk, 0);
    CHECK_EQ_INT(early, 0);
    /* Both outcomes exercised, the two rules do differ, and some streams
     * of the slow teacher are of the kind table (E) does not replay. */
    CHECK(reached_c > streams / 8 && reached_c < streams);
    CHECK(reached_i > reached_c && reached_i < streams);
    CHECK(skipped > 0);
}

/* --------------------------- table (E), continued ------------------------ */

/* The boundary of a region in which 'k' answers are outstanding at any
 * moment: after m answered requests, and k more that were asked, it holds
 * m + k samples and k errors more than the answered ones contain, so it
 * reaches the bar when m reaches nmin[errors + k] - k. */
static void simBoundaryShift(const simBoundary *bd, uint32_t k, simBoundary *out) {
    uint32_t j;

    *out = *bd;
    out->len = bd->len > k ? bd->len - k : 0;
    out->horizon = bd->horizon - k;
    out->nmin = simAlloc(out->len ? out->len : 1, sizeof(uint32_t));
    for (j = 0; j < out->len; j++) out->nmin[j] = bd->nmin[j + k] - k;
}

/* Requests until the bar is reached when 'k' answers are outstanding at any
 * moment and every guess is right, straight from the library: request n is
 * asked and counts as wrong, then the answer of request n - k arrives and
 * the error is taken back. */
static uint64_t simOutstandingCount(double alpha, double bar, uint32_t k, uint64_t cap, uint64_t *errors) {
    gateBets b;
    gateRegion r;
    uint64_t n;

    gateBetsInit(&b, alpha);
    gateRegionInit(&r);
    for (n = 1; n <= cap; n++) {
        gateRegionUpdate(&r, &b, 1);
        if (n > k) gateRegionFlip(&r, &b);
        if (gateRegionCertifies(&r, bar)) break;
    }
    *errors = r.errors;     /* the k answers that are still out */
    return n <= cap && r.n == n ? n : 0;
}

/* The exact values of table (E3): the smallest n with
 *
 *     (1/4) sum_j (1 - c_j)^k (1 + c_j alpha / (1 - alpha))^(n - k) >= 2 x regions / DELTA
 *
 * for k = 0, 1, 3, 10, 30, 100, computed outside this program in 80 digit
 * decimal arithmetic from the exact binary values of alpha, c_j and DELTA.
 * Every one clears the bar by more than 8e-5 at n and misses it by more
 * than 4e-4 at n - 1 (in log wealth), far beyond any rounding here. */
#define SIM_COST_KS 6
static const uint32_t simCostK[SIM_COST_KS] = {0, 1, 3, 10, 30, 100};
static const struct {
    double alpha;
    uint32_t regions;
    uint32_t n[SIM_COST_KS];
} simCostExact[] = {
    {0.05, 32, {220, 270, 371, 665, 1220, 3069}},
    {0.05, 64, {235, 285, 386, 691, 1247, 3117}},
    {0.02, 32, {558, 685, 937, 1685, 3075, 7721}},
    {0.02, 64, {596, 723, 976, 1749, 3144, 7839}},
    {0.01, 32, {1121, 1375, 1881, 3384, 6165, 15473}},
    {0.01, 64, {1198, 1452, 1960, 3513, 6305, 15708}}
};

static void simTableE(uint64_t trials, uint64_t seed) {
    static const double alphas[] = {0.05, 0.02};
    static const double mults[] = {1.0, 1.25};
    const uint64_t horizon = 20000, n = trials * 5, cost_horizon = 100000;
    const double bar = simBar(T), bound = gateBudgetGenerationDelta(SIM_DELTA, 1) / T;
    clock_t start = clock();
    /* Results of the 2 x 2 blocks (alpha, true risk), kept for the second
     * sub-table. */
    simTeacher teachers[4][SIM_TEACHERS];
    uint64_t x_true[4], x_cnt[4][SIM_TEACHERS], x_ign[4][SIM_TEACHERS], replays = 0;
    double exact[4], exact_short[4][SIM_TEACHERS], excess_lo = INFINITY, excess_hi = 0.0;
    uint64_t *times = simAlloc(trials, sizeof(uint64_t));
    simLateScratch ws;
    char b1[32];
    size_t c;
    uint64_t i;
    int t, blk, k;

    gate_test_current = "table E";
    printf("\n(E) LATE AND LOST ANSWERS: A REQUEST COUNTS AS A WRONG GUESS UNTIL ITS ANSWER ARRIVES\n"
           "    One threshold of a score gate (1 row x %d thresholds: log bar %.4f, bound exp(-bar) =\n"
           "    %.4e) whose true risk is alpha or 1.25 alpha, so that it must not be proven. The requests\n"
           "    of its region are the clock: an answer is 'L requests late' if it arrives when L more\n"
           "    requests have been asked. Each trial is one IID stream of %llu requests and counts if the\n"
           "    region reaches the bar at ANY moment; %llu trials per row, and within a block of rows every\n"
           "    teacher answers the same streams. Four teachers: one whose wrong answers never arrive, two\n"
           "    whose wrong answers are 5 / alpha and 2 / alpha requests late (an error free threshold\n"
           "    needs about 11 / alpha requests to reach this bar), and one that is up to 1 / alpha\n"
           "    requests late with every answer, whatever the answer is. (The gate first waits a quarter\n"
           "    of the feedback timeout for a late answer. This table is about what it does after that.)\n",
           T, bar, bound, (unsigned long long)horizon, (unsigned long long)n);

    simLateScratchInit(&ws, horizon);
    for (blk = 0; blk < 4; blk++) {
        double alpha = alphas[blk / 2], p = alpha * mults[blk % 2], log1mp = log1p(-p);
        uint64_t rng = simStream(seed, 0xe000 + (uint64_t)blk);
        simBoundary bd;

        simBoundaryBuild(&bd, alpha, bar, horizon);
        exact[blk] = simExactCrossProb(&bd, p);
        x_true[blk] = 0;
        for (t = 0; t < SIM_TEACHERS; t++) {
            simTeacherFor(t, alpha, &teachers[blk][t]);
            x_cnt[blk][t] = x_ign[blk][t] = 0;
            /* If no answer is more than any_lag late, every request up to
             * horizon - any_lag is in at the end. */
            exact_short[blk][t] = teachers[blk][t].any_lag == 0 ? exact[blk] :
                simExactCrossProbWithin(&bd, p, horizon - teachers[blk][t].any_lag);
        }
        for (i = 0; i < n; i++) {
            uint32_t nerr = simDrawErrors(log1mp, horizon, &rng, ws.err);
            uint64_t truth = simLagWalk(&bd, ws.err, nerr, 0);

            x_true[blk] += truth != 0;
            for (t = 0; t < SIM_TEACHERS; t++) {
                const simTeacher *tch = &teachers[blk][t];
                uint64_t counted = 0, ignored = 0;

                if (tch->any_lag == 0) {
                    /* Only wrong answers are late. COUNTED has them as
                     * errors anyway: it is the stream with its true
                     * answers. IGNORED sees them 'wrong_lag' late. */
                    counted = truth;
                    ignored = simLagWalk(&bd, ws.err, nerr, tch->wrong_lag);
                } else if (truth != 0 || tch->wrong_lag == SIM_LOST ||
                           simIgnoredCandidate(&bd, ws.err, nerr, (uint64_t)tch->any_lag + tch->wrong_lag)) {
                    /* Every answer may be late: replay the stream, if it
                     * can reach the bar at all. COUNTED cannot unless the
                     * true answers do, IGNORED cannot unless the true
                     * answers reach the lowered boundary. */
                    uint64_t id = ((uint64_t)blk << 40) + i * SIM_TEACHERS + (uint64_t)t;

                    (void)simLateReplay(&bd, tch, &ws, nerr, simStream(seed ^ 0x1a6, id), 0, &counted, &ignored);
                    replays++;
                }
                x_cnt[blk][t] += counted != 0;
                x_ign[blk][t] += ignored != 0;
            }
        }
        simBoundaryFree(&bd);
    }

    printf("\n    (E1) THE RULE OF THE GATE: a request is an error of its region from the moment it is asked\n"
           "    until its answer arrives, and for good if it never does. Such a region never holds more\n"
           "    wealth than the same requests with their true answers, so it cannot reach the bar more\n"
           "    often than table (A) says, whatever the teacher does. A teacher that delays or withholds\n"
           "    exactly the WRONG answers changes nothing at all, because they are errors either way: that\n"
           "    is the worst case, and its 'exact' column is the one of table (A). A teacher that is slow\n"
           "    with every answer only makes proof rarer (a stream is proven under it only if it is proven\n"
           "    on its true answers).\n\n");
    printf("    %-5s  %-9s  %-32s  %7s  %-9s  %-22s  %-9s  %s\n", "alpha", "true risk", "teacher", "reached",
           "frequency", "95% Wilson interval", "exact", "exact/bound");
    for (blk = 0; blk < 4; blk++) {
        double alpha = alphas[blk / 2], p = alpha * mults[blk % 2], lo, hi;

        simWilson(x_true[blk], n, &lo, &hi);
        printf("    %.3f  %.5f    %-32s  %7llu  %.3e  [%.3e, %.3e]  %.3e  %5.3f  %s\n", alpha, p,
               "wrong answers late or never", (unsigned long long)x_true[blk], (double)x_true[blk] / (double)n,
               lo, hi, exact[blk], exact[blk] / bound, exact[blk] <= bound && lo <= bound ? "ok" : "VIOLATION");
        CHECK(exact[blk] <= bound);
        CHECK(lo <= bound);
        CHECK(fabs((double)x_true[blk] - (double)n * exact[blk]) <= 4.5 * sqrt((double)n * exact[blk]) + 3.0);
        for (t = 0; t < SIM_TEACHERS; t++) {
            if (teachers[blk][t].any_lag == 0) {
                /* The three teachers of the line above, literally. */
                CHECK_EQ_INT(x_cnt[blk][t], x_true[blk]);
                continue;
            }
            simWilson(x_cnt[blk][t], n, &lo, &hi);
            printf("    %.3f  %.5f    %-32s  %7llu  %.3e  [%.3e, %.3e]  %-9s  %5s  %s\n", alpha, p,
                   teachers[blk][t].name, (unsigned long long)x_cnt[blk][t], (double)x_cnt[blk][t] / (double)n,
                   lo, hi, "<= above", "", lo <= bound ? "ok" : "VIOLATION");
            CHECK(lo <= bound);
            CHECK(x_cnt[blk][t] <= x_true[blk]);
        }
    }

    printf("\n    (E2) WHAT THE GATE DOES NOT DO: leave a request out of the region until its answer arrives.\n"
           "    Against a teacher whose wrong answers are late or lost the region sees correct answers\n"
           "    first, and the bound no longer holds. 'x bound' is the frequency as a multiple of\n"
           "    exp(-bar). Against a teacher that is slow whatever the answer is, the answers that are in\n"
           "    are an IID sample again and the rule is harmless: its probability lies between the exact\n"
           "    values of table (A) for a horizon of %llu less the largest lag and for %llu (the two\n"
           "    numbers under 'exact').\n\n",
           (unsigned long long)horizon, (unsigned long long)horizon);
    printf("    %-5s  %-9s  %-32s  %7s  %-9s  %-22s  %-22s  %s\n", "alpha", "true risk", "teacher", "reached",
           "frequency", "95% Wilson interval", "exact", "x bound");
    for (blk = 0; blk < 4; blk++) {
        double alpha = alphas[blk / 2], p = alpha * mults[blk % 2], lo, hi;

        for (t = 0; t < SIM_TEACHERS; t++) {
            const simTeacher *tch = &teachers[blk][t];
            double freq = (double)x_ign[blk][t] / (double)n;

            simWilson(x_ign[blk][t], n, &lo, &hi);
            printf("    %.3f  %.5f    %-32s  %7llu  %.3e  [%.3e, %.3e]  ", alpha, p, tch->name,
                   (unsigned long long)x_ign[blk][t], freq, lo, hi);
            if (tch->any_lag == 0) {
                printf("%-22s  %7.1f  %s\n", "", freq / bound,
                       lo > bound ? "EXCEEDS THE BOUND" : freq > bound ? "above (not significant)" : "not above");
                /* Leaving errors out can only add wealth. */
                CHECK(x_ign[blk][t] >= x_true[blk]);
                /* The teacher that withholds the wrong answers breaks the
                 * rule in every stream. The one whose wrong answers are
                 * 5 / alpha requests late must break it at the boundary
                 * of H0, where the bound is nearly attained: the frequency
                 * is some twenty times the bound there, which shows as
                 * soon as there are enough streams for the bound itself to
                 * be visible. */
                if (t == SIM_TEACHER_LOST) CHECK(lo > bound);
                if (t == SIM_TEACHER_LATE && blk % 2 == 0) {
                    if ((double)n * bound >= 1.0) CHECK(lo > bound);
                    if (freq / bound < excess_lo) excess_lo = freq / bound;
                    if (freq / bound > excess_hi) excess_hi = freq / bound;
                }
            } else {
                double e_lo = exact_short[blk][t], e_hi = exact[blk];

                printf("[%.3e, %.3e]  %7.3f  %s\n", e_lo, e_hi, freq / bound, lo <= bound ? "ok" : "VIOLATION");
                CHECK(lo <= bound);
                CHECK((double)x_ign[blk][t] >= (double)n * e_lo - 4.5 * sqrt((double)n * e_lo) - 3.0);
                CHECK((double)x_ign[blk][t] <= (double)n * e_hi + 4.5 * sqrt((double)n * e_hi) + 3.0);
            }
        }
        printf("\n");
    }
    printf("    Leaving outstanding answers out proves a threshold at the boundary of H0 in every stream if\n"
           "    the wrong answers never arrive, and %.0f to %.0f times as often as the bound allows if they are\n"
           "    5 / alpha requests late. Counting them as wrong guesses stays below the bound in every row.\n"
           "    (%llu streams were replayed request by request for the slow teacher; the others cannot\n"
           "    reach the bar under it.)\n",
           floor(excess_lo), ceil(excess_hi), (unsigned long long)replays);
    simLateScratchFree(&ws);

    printf("\n    (E3) WHAT AN OUTSTANDING ANSWER COSTS. If k answers are outstanding at any moment, the region\n"
           "    holds k errors that are not there, and with every guess right it needs the smallest n with\n"
           "        (1/4) sum_j (1 - c_j)^k (1 + c_j alpha / (1 - alpha))^(n - k) >= exp(bar)\n"
           "    requests, where bet j alone would need n - k = (bar + k |e_j|) / a_j of them (a_j, e_j: its\n"
           "    log gain on a correct sample and on an error) and the mixture is between a quarter of its\n"
           "    best bet and its best bet:\n"
           "        k + min_j ceil((bar + k |e_j|) / a_j)  <=  n  <=  k + min_j ceil((bar + log 4 + k |e_j|) / a_j)\n"
           "    'library' drives gateRegionUpdate() / gateRegionFlip() as the gate does, 'exact' is the\n"
           "    first expression in high precision (computed outside this program), 'per answer' is\n"
           "    (n(k) - n(0)) / k. 'median' is the same with a true risk of alpha / 2: %llu IID streams\n"
           "    per row, followed for at most %llu requests.\n\n",
           (unsigned long long)trials, (unsigned long long)cost_horizon);
    printf("    %-5s  %-36s  %3s  %7s  %7s  %-16s  %10s  %15s\n", "alpha", "bar of", "k", "library", "exact",
           "closed form", "per answer", "median, alpha/2");
    for (c = 0; c < sizeof(simCostExact) / sizeof(simCostExact[0]); c++) {
        double alpha = simCostExact[c].alpha, cbar = simBar(simCostExact[c].regions), log1mp = log1p(-0.5 * alpha);
        uint64_t n0 = 0;
        simBoundary bd;

        simBoundaryBuild(&bd, alpha, cbar, cost_horizon);
        for (k = 0; k < SIM_COST_KS; k++) {
            uint32_t outstanding = simCostK[k];
            uint64_t left, lib = simOutstandingCount(alpha, cbar, outstanding, cost_horizon, &left);
            uint64_t rng = simStream(seed, 0xe800 + c * 16 + (uint64_t)k), med;
            double lo = INFINITY, hi = INFINITY;
            simBoundary shifted;
            int j;

            for (j = 0; j < GATE_NBETS; j++) {
                double need = (cbar + outstanding * fabs(bd.bets.inc_err[j])) / bd.bets.inc_ok[j];
                double diluted = (cbar + log(4.0) + outstanding * fabs(bd.bets.inc_err[j])) / bd.bets.inc_ok[j];

                if (ceil(need) < lo) lo = ceil(need);
                if (ceil(diluted) < hi) hi = ceil(diluted);
            }
            simBoundaryShift(&bd, outstanding, &shifted);
            for (i = 0; i < trials; i++) {
                uint64_t at = simCrossTime(&shifted, log1mp, &rng);

                times[i] = at ? at + outstanding : SIM_NEVER;
            }
            qsort(times, trials, sizeof(uint64_t), simCmpU64);
            med = simQuantile(times, trials, 0.5);
            if (k == 0) n0 = lib;
            printf("    %.3f  %-36s  %3u  %7llu  %7u  [%6.0f, %6.0f]  ", alpha,
                   simCostExact[c].regions == T ? "score gate, 1 row x 32 thresholds"
                                                : "classifier, 2 labels x 32 thresholds",
                   outstanding, (unsigned long long)lib, simCostExact[c].n[k], outstanding + lo, outstanding + hi);
            if (k == 0) printf("%10s", "-"); else printf("%10.1f", (double)(lib - n0) / outstanding);
            printf("  %15s  %s\n", simFmt(b1, sizeof(b1), med, cost_horizon),
                   lib == simCostExact[c].n[k] ? "ok" : "DIFFERS");
            /* The library, the exact value, the boundary table and the
             * closed form tell the same story. */
            CHECK_EQ_INT(lib, simCostExact[c].n[k]);
            CHECK_EQ_INT(left, outstanding);
            CHECK(shifted.len > 0 && shifted.nmin[0] + outstanding == lib);
            CHECK((double)lib >= outstanding + lo && (double)lib <= outstanding + hi);
            /* Errors can only delay it. */
            CHECK(times[0] >= lib);
            simBoundaryFree(&shifted);
        }
        simBoundaryFree(&bd);
        printf("\n");
    }
    {
        gateBets at5, at2, at1;

        gateBetsInit(&at5, 0.05);
        gateBetsInit(&at2, 0.02);
        gateBetsInit(&at1, 0.01);
        printf("    While few answers are outstanding the 0.9 bet carries the proof and each of them costs\n"
               "    1 + log(10) / a_3 requests: %.1f / %.1f / %.1f for alpha = 0.05 / 0.02 / 0.01, about\n"
               "    2.5 / alpha. With many the 0.1 bet takes over and the cost per answer tends to\n"
               "    1 + |e_0| / a_0: %.1f / %.1f / %.1f, about 1 / alpha. An answer that arrives after all\n"
               "    costs nothing.\n",
               1.0 - at5.inc_err[3] / at5.inc_ok[3], 1.0 - at2.inc_err[3] / at2.inc_ok[3],
               1.0 - at1.inc_err[3] / at1.inc_ok[3], 1.0 - at5.inc_err[0] / at5.inc_ok[0],
               1.0 - at2.inc_err[0] / at2.inc_ok[0], 1.0 - at1.inc_err[0] / at1.inc_ok[0]);
    }
    free(times);
    printf("    The table took %.1f s.\n", simSeconds(start));
}

/* ---------------------------------- main --------------------------------- */

int main(int argc, char **argv) {
    uint64_t trials = 30000, seed = 20261128;
    clock_t start = clock();

    if (argc > 1) trials = strtoull(argv[1], NULL, 10);
    if (argc > 2) seed = strtoull(argv[2], NULL, 10);
    if (argc > 3 || trials < 1 || trials > 100000000ull) {
        fprintf(stderr, "usage: %s [trials (1..100000000) [seed]]\n", argv[0]);
        return 2;
    }

    printf("GATE certification Monte Carlo: trials = %llu, seed = %llu\n"
           "Proof: mixture of %d betting tests (fractions %.2f, %.2f, %.2f, %.2f). Every threshold is a test of\n"
           "its own, proven when its log wealth reaches bar = log(g (g + 1)) + log(tested rows x %d) - log(DELTA),\n"
           "here with g = 1 and DELTA = %.2f. All streams are IID.\n\n",
           (unsigned long long)trials, (unsigned long long)seed, GATE_NBETS,
           gateBetFractions[0], gateBetFractions[1], gateBetFractions[2], gateBetFractions[3], T, SIM_DELTA);

    gate_test_current = "self check";
    simSelfCheckStream(seed);
    simSelfCheckRace(seed);
    simSelfCheckPopulation(seed);
    simSelfCheckLate(seed);

    simTableA(trials, seed);
    simTableB(trials, seed);
    simTableC(trials, seed);
    simTableD(trials, seed);
    simTableE(trials, seed);

    printf("\nTotal CPU time %.1f s. Consistency checks: ", simSeconds(start));
    return TEST_REPORT();
}
