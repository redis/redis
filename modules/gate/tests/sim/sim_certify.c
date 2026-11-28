/*
 * Gate data type: Monte Carlo study of the proof (betting test, bar, entry
 * points) and of the drift detector implemented in gate_stats.c.
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
 * 'trials' (default 30000, about a minute on one core) is the number of
 * races per scorer of table (B) and of streams per row of table (C). Table
 * (A) estimates much smaller probabilities and uses 5 x trials streams per
 * row. Table (D) pushes every spot check through the detector, up to
 * 400,000 per stream when no alarm comes, and uses trials / 100 streams per
 * row for the false alarms and trials / 10 for the detection delays (at
 * least 10). Everything is deterministic for a given (trials, seed).
 *
 * THE RULES (gate_core.h). Threshold t of a row stands for the region
 * "score >= grid[t]" and has its own betting test, fed with the teacher
 * answers that fall in the region. It has CROSSED once its log wealth
 * reached the bar of its snapshot,
 *
 *     bar = log(g (g + 1)) + log(tested rows * entry points) - log(DELTA)
 *
 * (g = 1 and DELTA = 0.01 throughout), and it is PROVEN if it crossed and
 * it is an entry point or the next more confident threshold is proven. The
 * gate answers at the lowest proven threshold of the row. Wrongly proving
 * anything takes the highest bad threshold at or below some entry point to
 * cross; there is one such threshold per entry point and each crosses with
 * probability at most exp(-bar). So a row proves a bad threshold with
 * probability at most DELTA / (g (g + 1) * tested rows), however many
 * thresholds it tests.
 *
 * It prints four tables:
 *
 *   (A) how often one threshold whose true risk is at or above alpha crosses
 *       its bar anyway (must stay below exp(-bar));
 *   (B) one row of a classifier with two labels, 32 nested thresholds fed
 *       by one stream, under three choices of entry points: how long until
 *       the first threshold is proven, how much the row answers later, and
 *       how often a bad threshold is ever proven (must stay below the row's
 *       share of DELTA);
 *   (C) how many samples a good threshold needs to cross the bar;
 *   (D) false alarms and detection delay of the drift detector.
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
 * due. None of this is a second implementation of the test: before any
 * table is printed the program replays a few hundred streams, and a dozen
 * complete races, sample by sample through gateRegionUpdate() /
 * gateRegionCertifies() / gateGridCount() and requires identical crossing
 * times. It also draws the scorers of (B) from their definition and
 * compares them with the bands the race samples from. The detector of (D)
 * is always driven spot check by spot check through gateDriftPush().
 *
 * The exit status is non zero if a self check fails, if a simulated false
 * proof frequency is significantly above its bound, or if the detector's
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
 * 'regions' = tested rows x entry points. */
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
 * the horizon: forward recursion over the number of errors of the streams
 * that have not crossed yet. Mass that moves beyond len-1 errors can never
 * come back and is dropped. No randomness involved: this is the number the
 * Monte Carlo frequency of table (A) estimates. */
static double simExactCrossProb(const simBoundary *bd, double p) {
    double *alive, absorbed = 0.0;
    uint32_t low = 0, k;
    uint64_t n;

    if (bd->len == 0) return 0.0;
    alive = simAlloc(bd->len, sizeof(double));
    alive[0] = 1.0;
    for (n = 1; n <= bd->horizon; n++) {
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

        simBoundaryBuild(&bd, alphas[a], simBar(SIM_ROWS * 4), horizon);
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
    static const struct {
        uint32_t rows, entries;
    } bars[] = {{1, 1}, {SIM_ROWS, 4}, {1, 32}};
    const uint64_t horizon = 20000, n = trials * 5;
    clock_t start = clock();
    /* Range of exact/bound over the rows at risk = alpha and 1.25 alpha. */
    double ratio_lo[2] = {INFINITY, INFINITY}, ratio_hi[2] = {0.0, 0.0};
    size_t a, d, m;
    uint64_t row = 0;

    gate_test_current = "table A";
    printf("\n(A) ONE BAD THRESHOLD CROSSING ITS BAR\n"
           "    A threshold whose true risk is alpha or more must not cross. Each trial is one IID stream of\n"
           "    %llu samples in its region and counts if the log wealth reaches the bar after ANY of them;\n"
           "    %llu trials per row. Bound: exp(-bar), the share of DELTA the threshold was given (Ville's\n"
           "    inequality). The bars are those of a first snapshot: 2 rows x 4 entry points is a classifier\n"
           "    with two labels, 1 x 32 a score gate. No gate has a bar as low as 1 x 1 (it is the 'most\n"
           "    confident only' rule of table (B) with a single row): it is here because its crossings are\n"
           "    frequent enough to compare the simulation with the exact value sharply.\n"
           "    'exact' is the same probability computed without sampling (recursion over the error count);\n"
           "    the simulated frequency must agree with it within Monte Carlo error.\n\n",
           (unsigned long long)horizon, (unsigned long long)n);
    printf("    %-5s  %-9s  %-19s  %7s  %-10s  %7s  %-9s  %-22s  %-9s  %s\n", "alpha", "true risk",
           "rows x entry points", "log bar", "bound", "crossed", "frequency", "95% Wilson interval", "exact",
           "exact/bound");
    for (a = 0; a < 2; a++) {
        for (d = 0; d < 3; d++) {
            for (m = 0; m < 2; m++) {
                uint32_t regions = bars[d].rows * bars[d].entries;
                double alpha = alphas[a], p = alpha * mults[m], bar = simBar(regions);
                double bound = gateBudgetGenerationDelta(SIM_DELTA, 1) / regions;
                double log1mp = log1p(-p), lo, hi, exact;
                uint64_t rng = simStream(seed, 0xa000 + row++), x = 0, i;
                simBoundary bd;

                simBoundaryBuild(&bd, alpha, bar, horizon);
                for (i = 0; i < n; i++) x += simCrossTime(&bd, log1mp, &rng) != 0;
                exact = simExactCrossProb(&bd, p);
                simWilson(x, n, &lo, &hi);
                printf("    %.3f  %.5f    %u x %-15u  %7.4f  %.4e  %7llu  %.3e  [%.3e, %.3e]  %.3e  %5.3f  %s\n",
                       alpha, p, bars[d].rows, bars[d].entries, bar, bound, (unsigned long long)x,
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

/* The choices of entry points that table (B) compares: every 'step'-th
 * threshold counting from the most confident one. Ordered by bar, lowest
 * first, which simRace() relies on. */
#define SIM_RULES 3
static const struct {
    int step;
    const char *name;
} simRules[SIM_RULES] = {
    {T, "the most confident only"},
    {8, "every eighth (classifier)"},
    {1, "every threshold"}
};

static int simIsEntry(int step, int t) {
    return (T - 1 - t) % step == 0;
}

static uint32_t simEntries(int step) {
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
        simBoundaryBuild(&bd[r], SIM_RACE_ALPHA, simBar(SIM_ROWS * simEntries(simRules[r].step)),
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

/* When each threshold became proven (0 = never), from when it crossed: the
 * rule of gate_core.h, with the entry points of 'step'. */
static void simProvenTimes(int step, const uint32_t *cross, uint32_t *proven) {
    uint32_t above = 0;     /* when the next more confident one was proven */
    int t;

    for (t = T - 1; t >= 0; t--) {
        uint32_t at = 0;

        if (cross[t] != 0) {
            if (simIsEntry(step, t)) {
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
    uint64_t never;
    double lo, hi;
    int r, c;

    printf("    %-26s  %-23s  %s\n    %-26s  %7s %7s  %6s", "", "first proven threshold",
           "mean coverage of the lowest proven threshold", "entry points", "median", "p90", "never");
    for (c = 0; c < SIM_NCHECK; c++) printf("  after %5u", simCheckpoints[c]);
    printf(" samples\n");
    for (r = 0; r < SIM_RULES; r++) {
        for (never = 0; never < trials && st[r].first[trials - 1 - never] == SIM_NEVER; never++) {}
        printf("    %-26s  %7s %7s  %5.1f%%", simRules[r].name,
               simFmt(b1, sizeof(b1), simQuantile(st[r].first, trials, 0.5), SIM_RACE_HORIZON),
               simFmt(b2, sizeof(b2), simQuantile(st[r].first, trials, 0.9), SIM_RACE_HORIZON),
               100.0 * (double)never / (double)trials);
        for (c = 0; c < SIM_NCHECK; c++) printf("  %11.4f", st[r].coverage[c] / (double)trials);
        printf("\n");
        /* No proof can come before a threshold could have crossed this
         * bar with no error at all. */
        CHECK(st[r].first[0] >= bd[r].nmin[0]);
    }
    printf("\n    %-26s  %-25s  %-9s  %-22s  %s\n", "entry points", "bad threshold ever proven", "frequency",
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

    printf("\n(B) ONE ROW OF A CLASSIFIER WITH TWO LABELS: 32 NESTED THRESHOLDS FED BY ONE STREAM\n"
           "    Grid 0.5 .. 0.999, alpha = %.2f. Each trial is %d teacher answers to guesses of this label;\n"
           "    an answer whose guess had score s feeds every threshold <= s, so one answer moves many tests.\n"
           "    A threshold is PROVEN when it crossed the bar and it is an entry point or the next more\n"
           "    confident threshold is proven; the row answers at the lowest proven threshold. Three choices\n"
           "    of entry points run on the same streams. Fewer entry points make the bar,\n"
           "    log(g (g + 1) x rows x entry points / DELTA), lower but leave more thresholds waiting for\n"
           "    the ones above them:\n",
           alpha, SIM_RACE_HORIZON);
    for (r = 0; r < SIM_RULES; r++) {
        printf("        %-26s  %2u entry point%s  bar log(2 x %d x %2u / %.2f) = %.4f\n", simRules[r].name,
               simEntries(simRules[r].step), simEntries(simRules[r].step) == 1 ? " " : "s",
               SIM_ROWS, simEntries(simRules[r].step), SIM_DELTA, bd[r].bar);
    }
    printf("    Whatever the choice, the row may prove a bad threshold with probability at most\n"
           "    DELTA / (g (g + 1) x rows) = %.1e.\n\n"
           "    The scorers: the logit z of a request is N(0, sd^2), its score is sigmoid(|z|) and the guess\n"
           "    is wrong with probability 1 - score (calibrated). 'coverage' = share of the row's requests\n"
           "    at or above the threshold, 'risk' = share of those that are wrong, BAD = risk above alpha.\n"
           "    'entry' marks the entry points of the classifier rule.\n\n"
           "    %-20s     sd %-31.1fsd %.1f\n    %2s  %-9s  %-5s", bound, "", sds[0], sds[1], "t", "threshold",
           "entry");
    for (p = 0; p < 2; p++) printf("     %-8s  %-7s  %s", "coverage", "risk", "risk/alpha");
    printf("\n");
    for (t = 0; t < T; t++) {
        printf("    %2d  %-9.6f  %-5s", t, pop[0].grid[t], simIsEntry(8, t) ? "entry" : "");
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
           "    exactly alpha, so every threshold has risk = alpha and any proof is a false one. 'crossed'\n"
           "    counts the trials in which some threshold reached its bar. That alone is not an error: a\n"
           "    crossing only becomes a proof through an entry point. %llu trials.\n\n",
           sds[1], (unsigned long long)trials);
    printf("    %-26s  %-22s  %-22s  %-22s  %s\n", "entry points", "some threshold crossed",
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
        {SIM_ROWS * 4, "classifier, 2 labels x 4 entry points"},
        {32, "score gate, 32 entry points"}
    };
    static const double mults[] = {0.25, 0.5, 0.8};
    static const char *names[] = {"alpha/4", "alpha/2", "0.8 alpha"};
    const uint64_t horizon = 100000;
    clock_t start = clock();
    uint64_t *times = simAlloc(trials, sizeof(uint64_t));
    uint64_t row = 0, i;
    double scaled[3][4];    /* median x alpha of the classifier rows */
    char b1[32], b2[32];
    size_t a, d, m;

    gate_test_current = "table C";
    printf("\n(C) SAMPLES A GOOD THRESHOLD NEEDS TO CROSS THE BAR   (DELTA = %.2f, first snapshot)\n"
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
           "    the classifier bar, for alpha = 0.05 / 0.02 / 0.01:\n"
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
           "Proof: mixture of %d betting tests (fractions %.2f, %.2f, %.2f, %.2f). A threshold has crossed when\n"
           "its log wealth reaches bar = log(g (g + 1)) + log(tested rows x entry points) - log(DELTA), here\n"
           "with g = 1 and DELTA = %.2f. All streams are IID.\n\n",
           (unsigned long long)trials, (unsigned long long)seed, GATE_NBETS,
           gateBetFractions[0], gateBetFractions[1], gateBetFractions[2], gateBetFractions[3], SIM_DELTA);

    gate_test_current = "self check";
    simSelfCheckStream(seed);
    simSelfCheckRace(seed);
    simSelfCheckPopulation(seed);

    simTableA(trials, seed);
    simTableB(trials, seed);
    simTableC(trials, seed);
    simTableD(trials, seed);

    printf("\nTotal CPU time %.1f s. Consistency checks: ", simSeconds(start));
    return TEST_REPORT();
}
