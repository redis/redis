/*
 * Gate data type: Monte Carlo study of the certification test and of the
 * drift guard implemented in gate_stats.c.
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
 * It prints four tables:
 *
 *   (A) how often one region whose true risk is at or above alpha gets
 *       certified anyway (must stay below delta_region);
 *   (B) the same for a whole generation: 32 nested thresholds racing on one
 *       stream, for a classifier whose risk is not monotone in the threshold
 *       (must stay below delta_g), and how much coverage is left on the
 *       table by stopping at the first certification;
 *   (C) how many samples a good region needs to be certified;
 *   (D) false alarms and detection delay of the drift window.
 *
 * Simulation is evidence, not proof: the guarantee is Ville's inequality,
 * these numbers only show that the code does what the inequality promises
 * and what it costs. All the streams are IID, which is the assumption the
 * guarantee itself makes; nothing here speaks for correlated traffic.
 *
 * HOW A CERTIFICATION TIME IS COMPUTED. The wealth of a region depends on
 * the number of samples n and of errors k it has seen, not on their order.
 * So "certified" is a region of the (n, k) plane and its border is a table
 * nmin[k]: the smallest n at which k errors still certify. The table is
 * built by asking gateRegionCertifies() about a region holding exactly
 * those counts, and a stream is then certified at the first n with
 * n >= nmin[errors so far]. This turns a trial into a walk over its errors
 * only (the gaps between errors are drawn from the geometric distribution)
 * and makes millions of streams affordable, which the rare events of (A)
 * and (B) need. The race of (B) uses the same table for all its regions and
 * keeps per band counts, recounting the 32 regions only at the samples
 * where one of them could be due. None of this is a second implementation
 * of the test: before any table is printed the program replays a few
 * hundred streams, and a dozen complete races, sample by sample through
 * gateRegionUpdate() / gateRegionCertifies() / gateGridCount() and requires
 * identical certification times. The drift window of (D) is always driven
 * audit by audit through gateDriftPush().
 *
 * The exit status is non zero if a self check fails or if a simulated false
 * certification frequency is significantly above its bound.
 */

#include <time.h>
#include "testutil.h"
#include "gate_stats.h"

#define SIM_NEVER UINT64_MAX
#define SIM_Z95 1.959963984540054
#define SIM_AUDIT_RATE 0.05     /* q of the request count conversions */

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
    uint64_t idx = (uint64_t)ceil(q * (double)n);

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

/* The same after dividing by 'per' (samples -> requests). */
static const char *simFmtScaled(char *buf, size_t len, uint64_t v, uint64_t cap, double per) {
    if (v == SIM_NEVER) {
        snprintf(buf, len, ">%.0f", ceil((double)cap / per));
    } else {
        snprintf(buf, len, "%.0f", ceil((double)v / per));
    }
    return buf;
}

static double simSeconds(clock_t since) {
    return (double)(clock() - since) / CLOCKS_PER_SEC;
}

/* ------------------------- certification boundary ------------------------ */

typedef struct simBoundary {
    double alpha;
    double thr;         /* log wealth to reach */
    uint64_t horizon;   /* streams are followed for this many samples */
    uint32_t len;       /* nmin[0..len-1] are <= horizon; more errors than
                         * len-1 cannot certify within the horizon */
    uint32_t *nmin;
    gateBets bets;
} simBoundary;

/* Would a region that has seen n samples, k of them errors, be certified?
 * The log wealths are the closed form of what n updates add up to. */
static int simCountsCertify(const gateBets *b, double thr, uint64_t n, uint64_t k) {
    gateRegion r;
    int j;

    gateRegionInit(&r);
    for (j = 0; j < GATE_NBETS; j++)
        r.logw[j] = (double)(n - k) * b->inc_ok[j] + (double)k * b->inc_err[j];
    r.n = n;
    r.errors = k;
    return gateRegionCertifies(&r, thr);
}

/* Smallest n in [from, horizon] at which k errors certify, 0 if there is
 * none. For a fixed k the wealth grows with n, so this is a search for the
 * first true of a monotone predicate: gallop, then bisect. */
static uint64_t simFirstCertifying(const gateBets *b, double thr, uint64_t k,
                                   uint64_t from, uint64_t horizon) {
    uint64_t lo, hi, step = 1;

    if (from > horizon || !simCountsCertify(b, thr, horizon, k)) return 0;
    if (simCountsCertify(b, thr, from, k)) return from;
    lo = from;      /* does not certify */
    hi = horizon;   /* certifies */
    while (lo + step < hi) {
        if (simCountsCertify(b, thr, lo + step, k)) {
            hi = lo + step;
            break;
        }
        lo += step;
        step *= 2;
    }
    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;

        if (simCountsCertify(b, thr, mid, k)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return hi;
}

static void simBoundaryBuild(simBoundary *bd, double alpha, double thr, uint64_t horizon) {
    uint32_t cap = 256;
    uint64_t prev = 0;

    if (horizon >= UINT32_MAX) {    /* the table stores 32 bit sample counts */
        fprintf(stderr, "sim_certify: horizon too long\n");
        exit(2);
    }
    bd->alpha = alpha;
    bd->thr = thr;
    bd->horizon = horizon;
    bd->len = 0;
    bd->nmin = simAlloc(cap, sizeof(uint32_t));
    gateBetsInit(&bd->bets, alpha);
    for (;;) {
        /* One more error can only be afforded with more samples: if (n, k)
         * certifies so does (n - 1, k - 1), which had the same correct
         * samples and one error less. Hence nmin[k] > nmin[k - 1]. */
        uint64_t n = simFirstCertifying(&bd->bets, thr, bd->len, prev + 1, horizon);

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

/* Sample at which an IID stream with error rate p is certified, 0 if not
 * within the horizon. 'at' is the position of the latest error (0 = none)
 * and 'next' the position of the following one: every sample in between has
 * exactly k errors behind it, so the stream is certified there as soon as
 * n reaches nmin[k]. */
static uint64_t simCertTime(const simBoundary *bd, double log1mp, uint64_t *rng) {
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
 * library. Slow; used to validate simCertTime(). */
static uint64_t simCertTimeLibrary(const simBoundary *bd, double log1mp, uint64_t *rng) {
    gateRegion r;
    uint64_t n, next = simGap(log1mp, rng) + 1;

    gateRegionInit(&r);
    for (n = 1; n <= bd->horizon; n++) {
        int error = n == next;

        if (error) next = n + simGap(log1mp, rng) + 1;
        gateRegionUpdate(&r, &bd->bets, error);
        if (gateRegionCertifies(&r, bd->thr)) return n;
    }
    return 0;
}

/* Exact probability that an IID stream with error rate p is certified
 * within the horizon: forward recursion over the number of errors of the
 * streams that are not certified yet. Mass that moves beyond len-1 errors
 * can never come back and is dropped. No randomness involved: this is the
 * number the Monte Carlo frequency of table (A) estimates. */
static double simExactCertProb(const simBoundary *bd, double p) {
    double *alive, absorbed = 0.0;
    uint32_t low = 0, k;
    uint64_t n;

    if (bd->len == 0) return 0.0;
    alive = simAlloc(bd->len, sizeof(double));
    alive[0] = 1.0;
    for (n = 1; n <= bd->horizon; n++) {
        for (k = bd->len - 1; k > 0; k--) alive[k] = alive[k] * (1.0 - p) + alive[k - 1] * p;
        alive[0] *= 1.0 - p;
        /* States with nmin[k] <= n are certified at this sample. */
        while (low < bd->len && bd->nmin[low] <= n) low++;
        for (k = 0; k < low; k++) {
            absorbed += alive[k];
            alive[k] = 0.0;
        }
    }
    free(alive);
    return absorbed;
}

/* ------------------------------ self checks ------------------------------ */

static void simSelfCheckStream(uint64_t seed) {
    static const double alphas[] = {0.05, 0.01};
    static const double mults[] = {0.5, 0.8, 1.0};
    const uint64_t horizon = 20000;
    uint64_t streams = 0, certified = 0, different = 0;
    size_t a, m;
    int i;

    for (a = 0; a < 2; a++) {
        simBoundary bd;

        simBoundaryBuild(&bd, alphas[a], gateBudgetLogThreshold(0.01, 1, 32), horizon);
        for (m = 0; m < 3; m++) {
            double log1mp = log1p(-alphas[a] * mults[m]);

            for (i = 0; i < 60; i++) {
                uint64_t id = 0x5c000 + a * 1000 + m * 100 + (uint64_t)i;
                uint64_t r1 = simStream(seed, id), r2 = simStream(seed, id);
                uint64_t fast = simCertTime(&bd, log1mp, &r1);
                uint64_t slow = simCertTimeLibrary(&bd, log1mp, &r2);

                streams++;
                certified += slow != 0;
                different += fast != slow;
            }
        }
        simBoundaryFree(&bd);
    }
    printf("self check 1: %llu streams replayed sample by sample through gateRegionUpdate(): "
           "%llu certified, %llu certification times differ from the boundary walk\n",
           (unsigned long long)streams, (unsigned long long)certified, (unsigned long long)different);
    CHECK_EQ_INT(different, 0);
    CHECK(certified > streams / 4 && certified < streams); /* both outcomes exercised */
}

/* ------------------------------- table (A) ------------------------------- */

static void simTableA(uint64_t trials, uint64_t seed) {
    static const double alphas[] = {0.05, 0.02};
    static const double mults[] = {1.0, 1.25};
    const uint64_t horizon = 20000, n = trials * 5;
    clock_t start = clock();
    size_t a, d, m;
    uint64_t row = 0;

    gate_test_current = "table A";
    printf("\n(A) FALSE CERTIFICATION OF ONE REGION\n"
           "    A region whose true risk is alpha or more must not be certified. Each trial is one IID\n"
           "    error stream of %llu in-region samples; it counts as certified if the log mixture\n"
           "    wealth reaches the threshold after ANY of them. Bound: delta_region (Ville).\n"
           "    delta_region = 1.5625e-04 is the default gate: delta_total 0.01, generation 1, 32 regions.\n"
           "    'exact' is the same probability computed without sampling (recursion over the error\n"
           "    count); the simulated frequency must agree with it within Monte Carlo error.\n\n",
           (unsigned long long)horizon);
    printf("    alpha  true risk  delta_region  log thr   trials  certified   frequency   95%% Wilson interval"
           "       exact     exact/bound  within bound\n");
    for (a = 0; a < 2; a++) {
        for (d = 0; d < 2; d++) {
            for (m = 0; m < 2; m++) {
                double alpha = alphas[a], p = alpha * mults[m];
                double delta = d == 0 ? 0.01 : gateBudgetGenerationDelta(0.01, 1) / 32.0;
                double thr = d == 0 ? -log(0.01) : gateBudgetLogThreshold(0.01, 1, 32);
                double log1mp = log1p(-p), lo, hi, exact;
                uint64_t rng = simStream(seed, 0xa000 + row++), x = 0, i;
                simBoundary bd;

                simBoundaryBuild(&bd, alpha, thr, horizon);
                for (i = 0; i < n; i++) x += simCertTime(&bd, log1mp, &rng) != 0;
                exact = simExactCertProb(&bd, p);
                simWilson(x, n, &lo, &hi);
                printf("    %.3f  %.5f    %.4e    %7.4f  %7llu  %7llu    %.3e   [%.3e, %.3e]   %.3e    %5.3f      %s\n",
                       alpha, p, delta, thr, (unsigned long long)n, (unsigned long long)x,
                       (double)x / (double)n, lo, hi, exact, exact / delta,
                       exact <= delta && lo <= delta ? "yes" : "NO: VIOLATION");
                /* The guarantee, and the simulator against the exact value
                 * (4.5 standard deviations, +3 for tiny expected counts). */
                CHECK(exact <= delta);
                CHECK(lo <= delta);
                CHECK(fabs((double)x - (double)n * exact) <= 4.5 * sqrt((double)n * exact) + 3.0);
                simBoundaryFree(&bd);
            }
        }
    }
    printf("\n    At true risk = alpha the test spends most of its budget (exact/bound 0.7 - 0.9): it is valid\n"
           "    without being wasteful. A quarter above alpha it is already 20 - 100 times below the bound.\n"
           "    The table took %.1f s.\n", simSeconds(start));
}

/* ------------------------------- table (B) ------------------------------- */

#define SIM_NT 32               /* thresholds of the default grid */

typedef struct simPopulation {
    double grid[SIM_NT];
    double mass[SIM_NT];        /* P(score in band b = [grid[b], grid[b+1])) */
    double cum[SIM_NT];         /* P(band <= b) */
    double err[SIM_NT];         /* P(error | band b) */
    double coverage[SIM_NT];    /* P(score >= grid[t]) */
    double risk[SIM_NT];        /* P(error | score >= grid[t]) */
    int bad[SIM_NT];            /* risk[t] > alpha (>= alpha for "flat") */
} simPopulation;

/* Region t is the union of the bands t..31, so its coverage and risk are
 * suffix sums: the truth the simulation is judged against. */
static void simPopulationFinish(simPopulation *pop, double alpha, int boundary_is_bad) {
    double c = 0.0, e = 0.0, total = 0.0;
    int b;

    if (gateGridBuild(pop->grid, SIM_NT, GATE_GRID_LOGIT, 2) != GATE_OK) {
        fprintf(stderr, "sim_certify: cannot build the grid\n");
        exit(2);
    }
    for (b = 0; b < SIM_NT; b++) {
        total += pop->mass[b];
        pop->cum[b] = total;
    }
    pop->cum[SIM_NT - 1] = 1.0;
    for (b = SIM_NT - 1; b >= 0; b--) {
        c += pop->mass[b];
        e += pop->mass[b] * pop->err[b];
        pop->coverage[b] = c;
        pop->risk[b] = e / c;
        pop->bad[b] = boundary_is_bad ? 1 : pop->risk[b] > alpha;
    }
}

/* A binary classifier that is overconfident and has a blind spot.
 *   bands  0-9   scores 0.50-0.95: 21% of the traffic, error 45% falling to 8%
 *   bands 10-25  scores 0.95-0.9996: 51.2%, error 6.7% falling to 0.4%
 *   band  26     scores 0.99956-0.99967: 3% of the traffic, HALF of it wrong
 *                (a cluster of inputs the model is confidently wrong about)
 *   bands 27-31  scores above 0.99967: 24.8%, error 0.3%
 * Raising the threshold therefore lowers the risk, then raises it again as
 * the blind spot gets less diluted, then drops it to almost nothing. */
static void simPopulationRealistic(simPopulation *pop, double alpha) {
    int b;

    for (b = 0; b < SIM_NT; b++) {
        double w = b <= 9 ? 1.2 + 0.2 * b : b <= 25 ? 3.2 : b == 26 ? 3.0 : b <= 30 ? 5.0 : 4.8;

        pop->mass[b] = w / 100.0;
        pop->err[b] = b <= 25 ? 0.45 * exp(-0.19 * b) : b == 26 ? 0.5 : 0.003;
    }
    simPopulationFinish(pop, alpha, 0);
}

/* Same traffic, but every band errs at exactly alpha: all 32 regions sit on
 * the boundary of the null hypothesis, the least favourable case for the
 * union bound. Any certification is a false one. */
static void simPopulationFlat(simPopulation *pop, double alpha) {
    int b;

    simPopulationRealistic(pop, alpha);
    for (b = 0; b < SIM_NT; b++) pop->err[b] = alpha;
    simPopulationFinish(pop, alpha, 1);
}

/* One audited request: its band, a score inside the band and whether the
 * frozen prediction was wrong. */
static int simDrawSample(const simPopulation *pop, uint64_t *rng, int *error, double *score) {
    double u = gate_test_uniform(rng), v = gate_test_uniform(rng), lo, hi;
    int b = 0;

    /* Number of cum[0..30] at or below u, i.e. the band: a binary search
     * written without branches, because its outcome is random by design
     * and every branch would be mispredicted half of the time. */
    b += (u >= pop->cum[b + 15]) << 4;
    b += (u >= pop->cum[b + 7]) << 3;
    b += (u >= pop->cum[b + 3]) << 2;
    b += (u >= pop->cum[b + 1]) << 1;
    b += (u >= pop->cum[b]);

    lo = pop->grid[b];
    hi = b + 1 < SIM_NT ? pop->grid[b + 1] : 1.0;
    *score = lo + v * (hi - lo);
    if (!(*score < hi)) *score = lo;    /* rounding must not leave the band */
    *error = gate_test_uniform(rng) < pop->err[b];
    return b;
}

/* One race: 'horizon' audited samples, every region updated by the samples
 * that fall in it. cert[t] receives the index (1-based, in audited samples)
 * of the sample that certified region t, 0 if none.
 *
 * Region t holds the bands t..31, so its sample and error counts are suffix
 * sums of per band counters, and it is certified when its samples reach
 * nmin[its errors]. Call the difference its slack. A correct sample lowers
 * the slack of the regions it falls in by one; an error never lowers it
 * (nmin[k+1] >= nmin[k] + 1). So nothing can be certified before 'check' =
 * now + the smallest slack, and the regions only need to be recounted
 * then: a region found at slack 0 is certified by exactly that sample. */
static void simRace(const simPopulation *pop, const simBoundary *bd, uint64_t *rng, uint32_t *cert) {
    uint32_t seen[SIM_NT], wrong[SIM_NT];
    uint32_t i, check, horizon = (uint32_t)bd->horizon;
    int t;

    for (t = 0; t < SIM_NT; t++) seen[t] = wrong[t] = cert[t] = 0;
    check = bd->len ? bd->nmin[0] : UINT32_MAX;
    for (i = 1; i <= horizon; i++) {
        int error;
        double score;
        int band = simDrawSample(pop, rng, &error, &score);
        uint32_t n = 0, k = 0, slack = UINT32_MAX;

        seen[band]++;
        wrong[band] += (uint32_t)error;
        if (i < check) continue;

        for (t = SIM_NT - 1; t >= 0; t--) {
            uint32_t need;

            n += seen[t];
            k += wrong[t];
            /* Done, or too many errors to make it within the horizon. */
            if (cert[t] != 0 || k >= bd->len) continue;
            need = bd->nmin[k];
            if (n >= need) {
                cert[t] = i;
            } else if (need - n < slack) {
                slack = need - n;
            }
        }
        check = slack == UINT32_MAX ? UINT32_MAX : i + slack;
    }
}

/* The same race through the library: the score goes through the real grid
 * lookup and each region is a real gateRegion updated sample by sample. */
static void simRaceLibrary(const simPopulation *pop, const simBoundary *bd, uint64_t *rng,
                           uint32_t *cert, uint64_t *misplaced) {
    gateRegion r[SIM_NT];
    uint32_t i, t, horizon = (uint32_t)bd->horizon;

    for (t = 0; t < SIM_NT; t++) {
        gateRegionInit(&r[t]);
        cert[t] = 0;
    }
    for (i = 1; i <= horizon; i++) {
        int error;
        double score;
        int band = simDrawSample(pop, rng, &error, &score);
        uint32_t count = gateGridCount(pop->grid, SIM_NT, score);

        /* A score of band b belongs to the regions 0..b and to no other. */
        if (count != (uint32_t)band + 1) (*misplaced)++;
        for (t = 0; t < count; t++) {
            gateRegionUpdate(&r[t], &bd->bets, error);
            if (r[t].certified_at == 0 && gateRegionCertifies(&r[t], bd->thr)) {
                r[t].certified_at = r[t].n;
                cert[t] = i;
            }
        }
    }
}

static void simSelfCheckRace(uint64_t seed) {
    const double alpha = 0.05;
    const int races = 12;
    simPopulation pop;
    simBoundary bd;
    uint64_t different = 0, certified = 0, misplaced = 0;
    int i, t;

    simPopulationRealistic(&pop, alpha);
    simBoundaryBuild(&bd, alpha, gateBudgetLogThreshold(0.01, 1, SIM_NT), 20000);
    for (i = 0; i < races; i++) {
        uint64_t r1 = simStream(seed, 0x5d000 + (uint64_t)i), r2 = r1;
        uint32_t fast[SIM_NT], slow[SIM_NT];

        simRace(&pop, &bd, &r1, fast);
        simRaceLibrary(&pop, &bd, &r2, slow, &misplaced);
        for (t = 0; t < SIM_NT; t++) {
            different += fast[t] != slow[t];
            certified += slow[t] != 0;
        }
    }
    printf("self check 2: %d races of %llu samples replayed through gateGridCount() and 32 gateRegion "
           "structures:\n              %llu region certifications, %llu differ from the count based race, "
           "%llu scores in the wrong regions\n",
           races, (unsigned long long)bd.horizon, (unsigned long long)certified,
           (unsigned long long)different, (unsigned long long)misplaced);
    simBoundaryFree(&bd);
    CHECK_EQ_INT(different, 0);
    CHECK_EQ_INT(misplaced, 0);
    CHECK(certified > (uint64_t)races * 5);
}

#define SIM_NCHECK 5    /* the first certification + four checkpoints */
static const uint32_t simCheckpoints[SIM_NCHECK - 1] = {2000, 5000, 10000, 20000};

typedef struct simRaceStats {
    uint64_t trials;
    uint64_t any_bad;               /* trials with a bad region certified */
    uint64_t any_cert;              /* trials with any certification */
    uint64_t region_cert[SIM_NT];   /* trials in which region t certified */
    uint32_t *region_time;          /* trials x SIM_NT certification samples */
    uint64_t *first;                /* sample of the first certification (sorted) */
    /* The widest certified good region, [0] when the first region
     * certifies and [1..4] at the checkpoints: sum of its true coverage
     * and, per trial, its index (SIM_NEVER if none; sorted). */
    double coverage[SIM_NCHECK];
    uint64_t *widest[SIM_NCHECK];
} simRaceStats;

/* Lowest (= widest) good region certified by sample 'by': its index, or -1. */
static int simWidestGood(const simPopulation *pop, const uint32_t *cert, uint32_t by) {
    int t;

    for (t = 0; t < SIM_NT; t++) {
        if (!pop->bad[t] && cert[t] != 0 && cert[t] <= by) return t;
    }
    return -1;
}

static void simRunRaces(const simPopulation *pop, const simBoundary *bd, uint64_t trials,
                        uint64_t seed, uint64_t id, simRaceStats *st) {
    uint64_t rng = simStream(seed, id), i;
    int t, c;

    memset(st, 0, sizeof(*st));
    st->trials = trials;
    st->first = simAlloc(trials, sizeof(uint64_t));
    st->region_time = simAlloc(trials * SIM_NT, sizeof(uint32_t));
    for (c = 0; c < SIM_NCHECK; c++) st->widest[c] = simAlloc(trials, sizeof(uint64_t));
    for (i = 0; i < trials; i++) {
        uint32_t *cert = st->region_time + i * SIM_NT;
        uint32_t first = 0;
        int bad = 0;

        simRace(pop, bd, &rng, cert);
        for (t = 0; t < SIM_NT; t++) {
            if (cert[t] == 0) continue;
            st->region_cert[t]++;
            if (pop->bad[t]) bad = 1;
            if (first == 0 || cert[t] < first) first = cert[t];
        }
        st->any_bad += (uint64_t)bad;
        st->any_cert += first != 0;
        st->first[i] = first ? first : SIM_NEVER;
        for (c = 0; c < SIM_NCHECK; c++) {
            /* Before the first certification there is nothing to serve. */
            int w = c == 0 ? (first ? simWidestGood(pop, cert, first) : -1)
                           : simWidestGood(pop, cert, simCheckpoints[c - 1]);

            st->widest[c][i] = w >= 0 ? (uint64_t)w : SIM_NEVER;
            if (w >= 0) st->coverage[c] += pop->coverage[w];
        }
    }
    qsort(st->first, trials, sizeof(uint64_t), simCmpU64);
    for (c = 0; c < SIM_NCHECK; c++) qsort(st->widest[c], trials, sizeof(uint64_t), simCmpU64);
}

static void simRaceStatsFree(simRaceStats *st) {
    int c;

    free(st->first);
    free(st->region_time);
    for (c = 0; c < SIM_NCHECK; c++) free(st->widest[c]);
}

/* Median sample at which region t was certified, over all the trials. */
static uint64_t simRegionMedian(const simRaceStats *st, int t, uint64_t *scratch) {
    uint64_t i;

    for (i = 0; i < st->trials; i++) {
        uint32_t v = st->region_time[i * SIM_NT + (uint64_t)t];

        scratch[i] = v ? v : SIM_NEVER;
    }
    qsort(scratch, st->trials, sizeof(uint64_t), simCmpU64);
    return simQuantile(scratch, st->trials, 0.5);
}

static void simPrintWidest(const simPopulation *pop, const simRaceStats *st, int c, const char *label) {
    uint64_t median = simQuantile(st->widest[c], st->trials, 0.5);

    printf("        %-50s  %.4f      ", label, st->coverage[c] / (double)st->trials);
    if (median == SIM_NEVER) {
        printf("none certified\n");
    } else {
        printf("t = %2llu  (threshold %.6f, coverage %.4f)\n", (unsigned long long)median,
               pop->grid[median], pop->coverage[median]);
    }
}

static void simTableB(uint64_t trials, uint64_t seed) {
    const double alpha = 0.05, delta_total = 0.01;
    const uint64_t horizon = 20000;
    const double delta_g = gateBudgetGenerationDelta(delta_total, 1);
    const double thr = gateBudgetLogThreshold(delta_total, 1, SIM_NT);
    clock_t start = clock();
    simPopulation pop;
    simBoundary bd;
    simRaceStats st;
    uint64_t *scratch = simAlloc(trials, sizeof(uint64_t));
    uint64_t first;
    double lo, hi;
    char buf[32], buf2[32], label[64];
    int t, c;

    gate_test_current = "table B";
    simBoundaryBuild(&bd, alpha, thr, horizon);

    printf("\n(B) ONE GENERATION: 32 NESTED THRESHOLDS RACING ON THE SAME STREAM\n"
           "    Default gate: logit grid for 2 classes, alpha = %.2f, delta_total = %.2f, generation 1:\n"
           "    delta_g = %.4f, delta_region = delta_g / 32 = %.4e, log threshold %.4f.\n"
           "    Each trial is %llu audited requests (%.0f requests at audit rate %.2f). A request with\n"
           "    score s updates every region t with s >= threshold[t]; one request can move many regions.\n\n"
           "    (B1) A classifier whose risk is NOT monotone in the threshold: overconfident, and with a\n"
           "    blind spot at scores 0.99956-0.99967 where half of its answers are wrong (3%% of traffic).\n"
           "    'BAD' = true risk above alpha: must not be certified. %llu trials. 'certified' = share of\n"
           "    the trials in which the region was certified, 'median sample' = audited sample at which\n"
           "    that happened (median over all trials).\n\n",
           alpha, delta_total, delta_g, delta_g / SIM_NT, thr, (unsigned long long)horizon,
           (double)horizon / SIM_AUDIT_RATE, SIM_AUDIT_RATE, (unsigned long long)trials);

    simPopulationRealistic(&pop, alpha);
    simRunRaces(&pop, &bd, trials, seed, 0xb100, &st);
    printf("     t  threshold  band mass  band error  coverage  true risk  risk/alpha         certified   median sample\n");
    for (t = 0; t < SIM_NT; t++) {
        printf("    %2d  %.6f    %5.2f%%     %.4f     %.4f    %.5f     %5.3f  %s   %7.3f%%   %s\n",
               t, pop.grid[t], 100.0 * pop.mass[t], pop.err[t], pop.coverage[t], pop.risk[t],
               pop.risk[t] / alpha, pop.bad[t] ? "BAD " : "good",
               100.0 * (double)st.region_cert[t] / (double)trials,
               simFmt(buf, sizeof(buf), simRegionMedian(&st, t, scratch), horizon));
    }
    simWilson(st.any_bad, trials, &lo, &hi);
    printf("\n    Trials in which ANY bad region was certified: %llu of %llu = %.3e, 95%% Wilson [%.3e, %.3e];"
           " bound delta_g = %.1e: %s\n",
           (unsigned long long)st.any_bad, (unsigned long long)trials,
           (double)st.any_bad / (double)trials, lo, hi, delta_g, lo <= delta_g ? "ok" : "VIOLATION");
    CHECK(lo <= delta_g);

    first = simQuantile(st.first, trials, 0.5);
    printf("    Trials with at least one certification: %.2f%%. The first one comes at audited sample %s (median),\n"
           "    i.e. after %s requests at audit rate %.2f.\n",
           100.0 * (double)st.any_cert / (double)trials, simFmt(buf, sizeof(buf), first, horizon),
           simFmtScaled(buf2, sizeof(buf2), first, horizon, SIM_AUDIT_RATE), SIM_AUDIT_RATE);
    printf("\n    The widest good region certified so far: what the gate could serve. Mean of its true coverage\n"
           "    over the trials (nothing certified = 0) and the median region.\n"
           "                                                         mean coverage     median region\n");
    simPrintWidest(&pop, &st, 0, "when the first region certifies (stop there)");
    for (c = 1; c < SIM_NCHECK; c++) {
        snprintf(label, sizeof(label), "test continued to audited sample %u%s", simCheckpoints[c - 1],
                 simCheckpoints[c - 1] == horizon ? " (horizon)" : "");
        simPrintWidest(&pop, &st, c, label);
    }
    printf("    Continuing the SAME test after the first certification spends no extra error budget: the\n"
           "    bound above is on a bad region EVER being certified, not on the first certification. It only\n"
           "    needs the audit sampling to stay independent of what is being served, which the protocol has.\n");
    simRaceStatsFree(&st);

    printf("\n    (B2) Least favourable case for the union bound: same traffic, but every band errs at exactly\n"
           "    alpha, so all 32 regions have risk = alpha and any certification is a false one.\n");
    simPopulationFlat(&pop, alpha);
    simRunRaces(&pop, &bd, trials, seed, 0xb200, &st);
    simWilson(st.any_bad, trials, &lo, &hi);
    printf("    Trials in which ANY region was certified: %llu of %llu = %.3e, 95%% Wilson [%.3e, %.3e];"
           " bound delta_g = %.1e: %s\n",
           (unsigned long long)st.any_bad, (unsigned long long)trials,
           (double)st.any_bad / (double)trials, lo, hi, delta_g, lo <= delta_g ? "ok" : "VIOLATION");
    printf("    (delta_g is 32 x the single region bound. Nested regions share their samples, so the\n"
           "    family-wise frequency is far below the sum: the union bound is paid for in samples.)\n");
    CHECK(lo <= delta_g);
    simRaceStatsFree(&st);

    simBoundaryFree(&bd);
    free(scratch);
    printf("\n    The table took %.1f s.\n", simSeconds(start));
}

/* ------------------------------- table (C) ------------------------------- */

/* Certification time of a stream without errors, straight from the library. */
static uint64_t simZeroErrorCount(double alpha, double thr, uint64_t cap) {
    gateBets b;
    gateRegion r;

    gateBetsInit(&b, alpha);
    gateRegionInit(&r);
    while (r.n < cap) {
        gateRegionUpdate(&r, &b, 0);
        if (gateRegionCertifies(&r, thr)) return r.n;
    }
    return 0;
}

static void simTableC(uint64_t trials, uint64_t seed) {
    static const double alphas[] = {0.05, 0.02, 0.01};
    static const uint64_t gens[] = {1, 5};
    static const uint32_t regions[] = {32, 64};
    static const double mults[] = {0.25, 0.5, 0.8};
    static const char *names[] = {"alpha/4", "alpha/2", "0.8 alpha"};
    const double delta_total = 0.01, coverage = 0.6, per = SIM_AUDIT_RATE * coverage;
    const uint64_t horizon = 100000;
    clock_t start = clock();
    uint64_t *times = simAlloc(trials, sizeof(uint64_t));
    uint64_t row = 0, i;
    char b1[32], b2[32], b3[32], b4[32];
    size_t a, g, r, m;

    gate_test_current = "table C";
    printf("\n(C) SAMPLES NEEDED TO CERTIFY ONE GOOD REGION   (delta_total = %.2f)\n"
           "    'samples' are audited requests that fall in the region. At zero errors the count is exact;\n"
           "    at a true risk > 0 it is the median and the 90th percentile of %llu IID streams, each\n"
           "    followed for at most %llu samples ('uncert.' = share still not certified by then).\n"
           "    'requests' = samples / (%.2f x %.1f): the GPREDICT calls it takes to collect them at audit\n"
           "    rate q = %.2f when the region covers %.0f%% of the traffic.\n"
           "    For comparison the fixed-n zero-error binomial test of the spec needs 90 / 228 / 459 samples\n"
           "    (alpha 0.05 / 0.02 / 0.01, delta 0.01, one region, one look).\n\n",
           delta_total, (unsigned long long)trials, (unsigned long long)horizon,
           SIM_AUDIT_RATE, coverage, SIM_AUDIT_RATE, 100.0 * coverage);
    printf("    alpha   g  regions  log thr   true risk            median      p90  uncert.   requests(median)  requests(p90)\n");
    for (a = 0; a < 3; a++) {
        for (g = 0; g < 2; g++) {
            for (r = 0; r < 2; r++) {
                double alpha = alphas[a];
                double thr = gateBudgetLogThreshold(delta_total, gens[g], regions[r]);
                uint64_t n0 = simZeroErrorCount(alpha, thr, horizon);
                simBoundary bd;

                simBoundaryBuild(&bd, alpha, thr, horizon);
                /* The boundary table and the library agree on the one
                 * point that needs no simulation. */
                CHECK(bd.len > 0 && bd.nmin[0] == n0);
                printf("    %.3f  %2llu    %3u    %7.4f   %-9s %.5f  %8llu %8llu   %5.1f%%   %12.0f   %12.0f\n",
                       alpha, (unsigned long long)gens[g], regions[r], thr, "0 (exact)", 0.0,
                       (unsigned long long)n0, (unsigned long long)n0, 0.0,
                       ceil((double)n0 / per), ceil((double)n0 / per));
                for (m = 0; m < 3; m++) {
                    double p = alpha * mults[m], log1mp = log1p(-p);
                    uint64_t rng = simStream(seed, 0xc000 + row++), uncert = 0, med, p90;

                    for (i = 0; i < trials; i++) {
                        uint64_t t = simCertTime(&bd, log1mp, &rng);

                        times[i] = t ? t : SIM_NEVER;
                        uncert += t == 0;
                    }
                    qsort(times, trials, sizeof(uint64_t), simCmpU64);
                    med = simQuantile(times, trials, 0.5);
                    p90 = simQuantile(times, trials, 0.9);
                    printf("    %.3f  %2llu    %3u    %7.4f   %-9s %.5f  %8s %8s   %5.1f%%   %12s   %12s\n",
                           alpha, (unsigned long long)gens[g], regions[r], thr, names[m], p,
                           simFmt(b1, sizeof(b1), med, horizon), simFmt(b2, sizeof(b2), p90, horizon),
                           100.0 * (double)uncert / (double)trials,
                           simFmtScaled(b3, sizeof(b3), med, horizon, per),
                           simFmtScaled(b4, sizeof(b4), p90, horizon, per));
                    /* No stream can beat the zero error count. */
                    CHECK(times[0] >= n0);
                }
                simBoundaryFree(&bd);
                printf("\n");
            }
        }
    }
    free(times);
    printf("    The table took %.1f s.\n", simSeconds(start));
}

/* ------------------------------- table (D) ------------------------------- */

/* Audit at which the drift window first raises its alarm for an IID stream
 * with error rate p, SIM_NEVER if not within 'cap' audits. */
static uint64_t simAlarmTime(double p, double limit, uint64_t cap, uint64_t *rng) {
    gateDriftWindow w;
    uint64_t i;

    gateDriftInit(&w);
    for (i = 1; i <= cap; i++) {
        if (gateDriftPush(&w, gate_test_uniform(rng) < p, limit)) return i;
    }
    return SIM_NEVER;
}

/* Audits between a jump of the error rate from p0 to p1 and the alarm. The
 * window is first filled with 'burn' audits at p0; a run that alarms during
 * the burn-in is discarded and redrawn (a revoked gate is not serving). */
static uint64_t simDetectionDelay(double p0, double p1, double limit, uint64_t burn,
                                  uint64_t cap, uint64_t *rng, uint64_t *redrawn) {
    for (;;) {
        gateDriftWindow w;
        uint64_t i;
        int alarm = 0;

        gateDriftInit(&w);
        for (i = 0; i < burn && !alarm; i++) alarm = gateDriftPush(&w, gate_test_uniform(rng) < p0, limit);
        if (alarm) {
            (*redrawn)++;
            continue;
        }
        for (i = 1; i <= cap; i++) {
            if (gateDriftPush(&w, gate_test_uniform(rng) < p1, limit)) return i;
        }
        return SIM_NEVER;
    }
}

static void simTableD(uint64_t trials, uint64_t seed) {
    static const double alphas[] = {0.05, 0.02, 0.01};
    static const double mults[] = {0.25, 0.5, 1.0};
    static const char *names[] = {"alpha/4", "alpha/2", "alpha"};
    const uint64_t short_cap = 10000, long_cap = 1000000, burn = 2 * GATE_DRIFT_WINDOW;
    /* Long runs are expensive when the alarm (rightly) never comes: only
     * the first 'deep' streams of each row are followed up to long_cap. */
    uint64_t deep = trials / 50 < 200 ? 200 : trials / 50;
    clock_t start = clock();
    uint64_t *times, row = 0, i;
    char b1[32], b2[32], b3[32];
    size_t a, m;

    if (deep > trials) deep = trials;
    times = simAlloc(trials, sizeof(uint64_t));
    gate_test_current = "table D";
    printf("\n(D) DRIFT GUARD   window = %d audits, first evaluated at %d, alarm when errors > 2 alpha n\n"
           "    The guard is a heuristic: it has false alarms and a detection delay, measured here.\n"
           "    An alarm revokes the serving policy, so a false alarm costs a recalibration.\n\n"
           "    (D1) False alarms on IID traffic whose risk is at or below the target. Probability that the\n"
           "    alarm has fired by audit 100 / 1000 / 10000 (%llu streams per row) and median number of\n"
           "    audits until it fires (first %llu streams, each followed for up to %llu audits).\n\n",
           GATE_DRIFT_WINDOW, GATE_DRIFT_MIN, (unsigned long long)trials,
           (unsigned long long)deep, (unsigned long long)long_cap);
    printf("    alpha  limit  true risk            P(by 100)  P(by 1000)  P(by 10000)   median audits to alarm"
           "   accepted requests (q = %.2f)\n", SIM_AUDIT_RATE);
    for (a = 0; a < 3; a++) {
        for (m = 0; m < 3; m++) {
            double alpha = alphas[a], p = alpha * mults[m], limit = 2.0 * alpha;
            uint64_t rng = simStream(seed, 0xd000 + row++);
            uint64_t by100 = 0, by1000 = 0, by10000 = 0, earliest = SIM_NEVER, med;

            for (i = 0; i < trials; i++) {
                uint64_t t = simAlarmTime(p, limit, i < deep ? long_cap : short_cap, &rng);

                by100 += t <= 100;
                by1000 += t <= 1000;
                by10000 += t <= 10000;
                if (t < earliest) earliest = t;
                times[i] = t;
            }
            qsort(times, deep, sizeof(uint64_t), simCmpU64);
            med = simQuantile(times, deep, 0.5);
            printf("    %.3f  %.3f  %-9s %.5f   %.5f    %.5f     %.5f      %12s             %12s\n",
                   alpha, limit, names[m], p, (double)by100 / (double)trials,
                   (double)by1000 / (double)trials, (double)by10000 / (double)trials,
                   simFmt(b1, sizeof(b1), med, long_cap),
                   simFmtScaled(b2, sizeof(b2), med, long_cap, SIM_AUDIT_RATE));
            /* Nothing can fire before the window holds GATE_DRIFT_MIN. */
            CHECK(earliest >= GATE_DRIFT_MIN);
        }
        printf("\n");
    }

    printf("    (D2) Detection delay. The window is full of audits at risk alpha/2 (%llu audits without an\n"
           "    alarm), then the risk jumps. Audits from the jump to the alarm, %llu streams per row.\n\n",
           (unsigned long long)burn, (unsigned long long)trials);
    printf("    alpha  limit  risk after the jump    median audits  p90 audits   accepted requests, median (q = %.2f)"
           "   redrawn\n", SIM_AUDIT_RATE);
    row = 0;
    for (a = 0; a < 3; a++) {
        for (m = 0; m < 2; m++) {
            double alpha = alphas[a], p1 = m == 0 ? 5.0 * alpha : 0.5, limit = 2.0 * alpha;
            uint64_t rng = simStream(seed, 0xd800 + row++), redrawn = 0, med, p90;

            for (i = 0; i < trials; i++)
                times[i] = simDetectionDelay(0.5 * alpha, p1, limit, burn, short_cap, &rng, &redrawn);
            qsort(times, trials, sizeof(uint64_t), simCmpU64);
            med = simQuantile(times, trials, 0.5);
            p90 = simQuantile(times, trials, 0.9);
            printf("    %.3f  %.3f  %-9s %.4f      %12s  %10s             %12s                    %5.2f%%\n",
                   alpha, limit, m == 0 ? "5 alpha" : "0.5", p1,
                   simFmt(b1, sizeof(b1), med, short_cap), simFmt(b2, sizeof(b2), p90, short_cap),
                   simFmtScaled(b3, sizeof(b3), med, short_cap, SIM_AUDIT_RATE),
                   100.0 * (double)redrawn / (double)(trials + redrawn));
            /* A jump this large is always caught, and quickly. */
            CHECK(times[trials - 1] != SIM_NEVER);
        }
    }
    free(times);
    printf("\n    The table took %.1f s.\n", simSeconds(start));
}

/* ---------------------------------- main --------------------------------- */

int main(int argc, char **argv) {
    uint64_t trials = 20000, seed = 20261128;
    clock_t start = clock();
    int status;

    if (argc > 1) trials = strtoull(argv[1], NULL, 10);
    if (argc > 2) seed = strtoull(argv[2], NULL, 10);
    if (argc > 3 || trials < 1 || trials > 100000000ull) {
        fprintf(stderr, "usage: %s [trials (1..100000000) [seed]]\n", argv[0]);
        return 2;
    }

    printf("GATE certification Monte Carlo: trials = %llu, seed = %llu\n"
           "Mixture of %d betting e-processes (c = %.2f, %.2f, %.2f, %.2f), log threshold -log(delta_region).\n\n",
           (unsigned long long)trials, (unsigned long long)seed, GATE_NBETS,
           gateBetFractions[0], gateBetFractions[1], gateBetFractions[2], gateBetFractions[3]);

    gate_test_current = "self check";
    simSelfCheckStream(seed);
    simSelfCheckRace(seed);

    simTableA(trials, seed);
    simTableB(trials, seed);
    simTableC(trials, seed);
    simTableD(trials, seed);

    printf("\nTotal CPU time %.1f s. Consistency checks: ", simSeconds(start));
    status = TEST_REPORT();
    return status;
}
