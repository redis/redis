/*
 * Gate data type: unit tests of the statistical core (gate_stats.c).
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * WHERE THE EXPECTED NUMBERS COME FROM. No constant in this file was produced
 * by the code under test. They were computed from the formulas in
 * gate_stats.h with 60 digit decimal arithmetic (exact rationals for the
 * budget), taking every input the C code receives as a double (alpha, the
 * bet fractions, delta, 0.9999) at its exact binary value, and are written
 * with 20 significant digits. What is left between them and a correct
 * implementation is libm rounding only, which is why the tolerances can be
 * a few ulps. The handful of values a person can verify with pencil and
 * paper are derived in the comments next to them.
 */

#include <float.h>
#include "testutil.h"
#include "gate_stats.h"

/* |a - b| <= rel * |b|, for quantities spanning many orders of magnitude. */
#define CHECK_REL(a, b, rel) CHECK_NEAR((a), (b), fabs((double)(b)) * (rel))

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* log(1 - c_j) for the four bet fractions. */
static const double ref_inc_err[GATE_NBETS] = {
    -1.0536051565782630740e-1, -2.8768207245178092744e-1,
    -6.9314718055994530942e-1, -2.3025850929940459061e+0
};

/* log(1 + c_j * alpha / (1 - alpha)). */
static const struct {
    double alpha;
    double inc_ok[GATE_NBETS];
} ref_inc_ok[] = {
    {0.05,  {5.2493558861437294195e-3, 1.3072081567352771083e-2,
             2.5975486403260659521e-2, 4.6280752564006255142e-2}},
    {0.02,  {2.0387366898482846618e-3, 5.0890695074712778425e-3,
             1.0152371464018007439e-2, 1.8200704646846371884e-2}},
    {0.01,  {1.0095912013523790553e-3, 2.5220694327098674495e-3,
             5.0377940299571592461e-3, 9.0498355199179080951e-3}},
    {0.5,   {9.5310179804324865090e-2, 2.2314355131420975577e-1,
             4.0546510810816438198e-1, 6.4185388617239478768e-1}},
    {1e-9,  {1.0000000009500001187e-10, 2.5000000021875001576e-10,
             5.0000000037500003143e-10, 9.0000000049500007859e-10}},
    {0.999, {4.6141299273595624472e+0, 5.5244564268420440265e+0,
             6.2156075987552743885e+0, 6.8025058682630387016e+0}}
};

/* Feed 'n_ok' correct samples and 'n_err' errors; errors first if
 * 'errors_first', otherwise last. */
static void feed(gateRegion *r, const gateBets *b, int n_ok, int n_err, int errors_first) {
    int i;

    if (errors_first) for (i = 0; i < n_err; i++) gateRegionUpdate(r, b, 1);
    for (i = 0; i < n_ok; i++) gateRegionUpdate(r, b, 0);
    if (!errors_first) for (i = 0; i < n_err; i++) gateRegionUpdate(r, b, 1);
}

/* ------------------------------ E-process -------------------------------- */

TEST(bet_fractions) {
    CHECK_EQ_INT(GATE_NBETS, 4);
    CHECK(gateBetFractions[0] == 0.1);
    CHECK(gateBetFractions[1] == 0.25);
    CHECK(gateBetFractions[2] == 0.5);
    CHECK(gateBetFractions[3] == 0.9);
}

TEST(bets_increments) {
    size_t i;
    int j;

    for (i = 0; i < COUNTOF(ref_inc_ok); i++) {
        double alpha = ref_inc_ok[i].alpha;
        gateBets b;

        gateBetsInit(&b, alpha);
        CHECK(b.alpha == alpha);
        for (j = 0; j < GATE_NBETS; j++) {
            double c = gateBetFractions[j];

            /* 1e-14 relative: at alpha = 1e-9 a log(1 + x) in place of
             * log1p(x) is off by 1e-7 relative and fails this. */
            CHECK_REL(b.inc_ok[j], ref_inc_ok[i].inc_ok[j], 1e-14);
            CHECK_NEAR(b.inc_err[j], ref_inc_err[j], 1e-15);
            /* The same thing stated as the wealth factors of the header. */
            CHECK_REL(exp(b.inc_ok[j]), 1.0 + c * alpha / (1.0 - alpha), 1e-14);
            CHECK_REL(exp(b.inc_err[j]), 1.0 - c, 1e-14);
            /* A correct sample must gain, an error must lose, and every
             * factor must stay positive (finite logarithm). */
            CHECK(b.inc_ok[j] > 0.0 && isfinite(b.inc_ok[j]));
            CHECK(b.inc_err[j] < 0.0 && isfinite(b.inc_err[j]));
        }
        /* More aggressive bets win more on a correct sample and lose more
         * on an error. */
        for (j = 1; j < GATE_NBETS; j++) {
            CHECK(b.inc_ok[j] > b.inc_ok[j - 1]);
            CHECK(b.inc_err[j] < b.inc_err[j - 1]);
        }
    }
}

TEST(region_init_and_counters) {
    gateRegion r;
    gateBets b;
    double lw;
    int j;

    memset(&r, 0xff, sizeof(r));
    gateRegionInit(&r);
    for (j = 0; j < GATE_NBETS; j++) CHECK(r.logw[j] == 0.0);
    CHECK_EQ_INT(r.n, 0);
    CHECK_EQ_INT(r.errors, 0);
    CHECK_EQ_INT(r.certified_at, 0);
    /* Wealth 1 before any evidence, with no rounding. */
    CHECK(gateRegionLogWealth(&r) == 0.0);

    gateBetsInit(&b, 0.05);
    lw = gateRegionUpdate(&r, &b, 0);
    CHECK_EQ_INT(r.n, 1);
    CHECK_EQ_INT(r.errors, 0);
    CHECK(lw == gateRegionLogWealth(&r));
    lw = gateRegionUpdate(&r, &b, 1);
    CHECK_EQ_INT(r.n, 2);
    CHECK_EQ_INT(r.errors, 1);
    CHECK(lw == gateRegionLogWealth(&r));
    for (j = 0; j < GATE_NBETS; j++)
        CHECK_NEAR(r.logw[j], ref_inc_ok[0].inc_ok[j] + ref_inc_err[j], 1e-15);

    /* The update never touches certified_at, even while the region is far
     * beyond any threshold: latching it is the caller's decision. */
    r.certified_at = 77;
    for (j = 0; j < 2000; j++) gateRegionUpdate(&r, &b, 0);
    CHECK(gateRegionCertifies(&r, 50.0));
    CHECK_EQ_INT(r.certified_at, 77);
    CHECK_EQ_INT(r.n, 2002);
    CHECK_EQ_INT(r.errors, 1);

    /* The counters are 64 bit. */
    r.n = 0xffffffffull;
    r.errors = 0xffffffffull;
    gateRegionUpdate(&r, &b, 1);
    CHECK(r.n == 0x100000000ull);
    CHECK(r.errors == 0x100000000ull);
}

/* alpha = 0.05, so lambda_j = c_j / 0.95: a correct sample multiplies wealth
 * j by 1 + c_j / 19 and an error by 1 - c_j. After 3 correct and 1 error:
 *
 *     c = 0.10: (19.10/19)^3 * 0.90 = 0.914285449774...
 *     c = 0.25: (19.25/19)^3 * 0.75 = 0.779996514616...
 *     c = 0.50: (19.50/19)^3 * 0.50 = 0.540521577489...
 *     c = 0.90: (19.90/19)^3 * 0.10 = 0.114894284881...
 *     mixture = their mean          = 0.587424456690...
 */
TEST(hand_computed_alpha_005) {
    static const double logw[GATE_NBETS] = {
        -8.9612447999395119137e-2, -2.4846582774972261419e-1,
        -6.1522072135016333085e-1, -2.1637428353020271406e+0
    };
    static const double wealth[GATE_NBETS] = {
        0.914285449774, 0.779996514616, 0.540521577489, 0.114894284881
    };
    /* Where the error falls among the 4 samples must not matter. */
    static const int orders[4][4] = {
        {0, 0, 0, 1}, {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}
    };
    gateBets b;
    gateRegion r;
    double lw = 0;
    int o, i, j;

    gateBetsInit(&b, 0.05);
    for (o = 0; o < 4; o++) {
        gateRegionInit(&r);
        for (i = 0; i < 4; i++) lw = gateRegionUpdate(&r, &b, orders[o][i]);
        for (j = 0; j < GATE_NBETS; j++) {
            CHECK_NEAR(r.logw[j], logw[j], 1e-12);
            CHECK_NEAR(exp(r.logw[j]), wealth[j], 1e-11);
        }
        CHECK_NEAR(lw, -5.3200762558875099968e-1, 1e-12);
        CHECK_NEAR(exp(lw), 0.587424456690, 1e-11);
        CHECK_EQ_INT(r.n, 4);
        CHECK_EQ_INT(r.errors, 1);
    }

    /* One sample: the mixture is the mean factor, i.e. 1 + mean(c)/19 with
     * mean(c) = (0.1 + 0.25 + 0.5 + 0.9) / 4 = 0.4375 after a correct
     * sample (1.023026315789...), and 1 - 0.4375 = 0.5625 after an error. */
    gateRegionInit(&r);
    lw = gateRegionUpdate(&r, &b, 0);
    CHECK_NEAR(lw, 2.2765210773012438190e-2, 1e-13);
    CHECK_NEAR(exp(lw), 1.0 + 0.4375 / 19.0, 1e-13);
    gateRegionInit(&r);
    lw = gateRegionUpdate(&r, &b, 1);
    CHECK_NEAR(lw, -5.7536414490356186721e-1, 1e-13);
    CHECK_NEAR(exp(lw), 0.5625, 1e-13);
}

/* alpha = 0.5 makes lambda_j = 2 c_j: a correct sample multiplies by 1 + c_j
 * and an error by 1 - c_j. Two of each give (1 - c_j^2)^2 exactly:
 *
 *     0.99^2 = 0.9801   0.9375^2 = 0.87890625   0.75^2 = 0.5625
 *     0.19^2 = 0.0361   mean = 2.45760625 / 4 = 0.6144015625
 */
TEST(hand_computed_alpha_05) {
    static const double wealth[GATE_NBETS] = {0.9801, 0.87890625, 0.5625, 0.0361};
    static const double logw[GATE_NBETS] = {
        -2.0100671707002884610e-2, -1.2907704227514234335e-1,
        -5.7536414490356185488e-1, -3.3214624136433022368e+0
    };
    gateBets b;
    gateRegion r;
    double lw;
    int j;

    gateBetsInit(&b, 0.5);
    gateRegionInit(&r);
    gateRegionUpdate(&r, &b, 0);
    gateRegionUpdate(&r, &b, 1);
    gateRegionUpdate(&r, &b, 1);
    lw = gateRegionUpdate(&r, &b, 0);
    for (j = 0; j < GATE_NBETS; j++) {
        CHECK_NEAR(exp(r.logw[j]), wealth[j], 1e-14);
        CHECK_NEAR(r.logw[j], logw[j], 1e-12);
    }
    CHECK_NEAR(exp(lw), 0.6144015625, 1e-14);
    CHECK_NEAR(lw, -4.8710655402039798495e-1, 1e-12);
}

/* Two more streams against the reference: alpha = 0.02 with 5 correct and 2
 * errors, alpha = 0.01 with 40 correct. */
TEST(short_streams) {
    static const double logw_a[GATE_NBETS] = {
        -2.0052734786641119148e-1, -5.4991879736620546567e-1,
        -1.3355325037998005816e+0, -4.5141666627538599527e+0
    };
    static const double logw_b[GATE_NBETS] = {
        4.0383648054095162210e-2, 1.0088277730839469798e-1,
        2.0151176119828636984e-1, 3.6199342079671632380e-1
    };
    gateBets b;
    gateRegion r;
    int j;

    gateBetsInit(&b, 0.02);
    gateRegionInit(&r);
    feed(&r, &b, 5, 2, 0);
    for (j = 0; j < GATE_NBETS; j++) CHECK_NEAR(r.logw[j], logw_a[j], 1e-12);
    CHECK_NEAR(gateRegionLogWealth(&r), -8.7391000373957978656e-1, 1e-12);

    gateBetsInit(&b, 0.01);
    gateRegionInit(&r);
    feed(&r, &b, 40, 0, 0);
    for (j = 0; j < GATE_NBETS; j++) CHECK_NEAR(r.logw[j], logw_b[j], 1e-12);
    CHECK_NEAR(gateRegionLogWealth(&r), 1.8373664701528692070e-1, 1e-12);
}

/* An error costs the factor (1 - c_j) and nothing else: not a function of
 * alpha, although lambda_j is. A lambda that forgot the 1/(1 - alpha), for
 * instance, would make the factor 1 - c_j (1 - alpha). */
TEST(error_factor_is_independent_of_alpha) {
    static const double alphas[] = {1e-12, 1e-6, 0.001, 0.01, 0.02, 0.05, 0.3,
                                    0.5, 0.9, 0.99, 0.999999, 1.0 - DBL_EPSILON};
    gateBets ref;
    size_t i;
    int j;

    gateBetsInit(&ref, 0.5);
    for (i = 0; i < COUNTOF(alphas); i++) {
        gateBets b;
        gateRegion r;
        double lw;

        gateBetsInit(&b, alphas[i]);
        gateRegionInit(&r);
        lw = gateRegionUpdate(&r, &b, 1);
        for (j = 0; j < GATE_NBETS; j++) {
            CHECK_NEAR(exp(r.logw[j]), 1.0 - gateBetFractions[j], 1e-15);
            CHECK_NEAR(r.logw[j], ref_inc_err[j], 1e-15);
            /* Not merely close: the very same double for every alpha. */
            CHECK(b.inc_err[j] == ref.inc_err[j]);
            CHECK(isfinite(b.inc_ok[j]) && b.inc_ok[j] > 0.0);
        }
        /* The mixture after one error is 1 - mean(c) = 0.5625. */
        CHECK_NEAR(exp(lw), 0.5625, 1e-15);
        CHECK_EQ_INT(r.errors, 1);

        /* Whatever is not 0 is an error. */
        gateRegionInit(&r);
        gateRegionUpdate(&r, &b, 2);
        gateRegionUpdate(&r, &b, -1);
        CHECK_EQ_INT(r.n, 2);
        CHECK_EQ_INT(r.errors, 2);
        for (j = 0; j < GATE_NBETS; j++)
            CHECK_NEAR(r.logw[j], 2.0 * ref_inc_err[j], 1e-14);
    }
}

/* The wealth is a product of per sample factors, so it depends on how many
 * correct samples and errors were seen and not on their order. Reference:
 * alpha = 0.03, 950 correct + 50 errors. */
TEST(order_independence) {
    static const double logw[GATE_NBETS] = {
        -2.3344156287435869675e+0, -7.0669942786013330502e+0,
        -2.0079067837069909539e+1, -8.9047291108512962749e+1
    };
    const double mix = -3.7119447573558026255e+0;
    unsigned char seq[1000];
    uint64_t rng = 0x0dde21ull;
    gateBets b;
    gateRegion r[4];
    int i, j, k;

    gateBetsInit(&b, 0.03);

    gateRegionInit(&r[0]);          /* all the errors first */
    feed(&r[0], &b, 950, 50, 1);
    gateRegionInit(&r[1]);          /* all the errors last */
    feed(&r[1], &b, 950, 50, 0);
    gateRegionInit(&r[2]);          /* one error every 20 samples */
    for (i = 0; i < 1000; i++) gateRegionUpdate(&r[2], &b, i % 20 == 7);
    gateRegionInit(&r[3]);          /* a random permutation (Fisher-Yates) */
    for (i = 0; i < 1000; i++) seq[i] = i < 50;
    for (i = 999; i > 0; i--) {
        int s = (int)(gate_test_rand64(&rng) % (uint64_t)(i + 1));
        unsigned char tmp = seq[i];

        seq[i] = seq[s];
        seq[s] = tmp;
    }
    for (i = 0; i < 1000; i++) gateRegionUpdate(&r[3], &b, seq[i]);

    for (k = 0; k < 4; k++) {
        CHECK_EQ_INT(r[k].n, 1000);
        CHECK_EQ_INT(r[k].errors, 50);
        for (j = 0; j < GATE_NBETS; j++) CHECK_NEAR(r[k].logw[j], logw[j], 1e-10);
        CHECK_NEAR(gateRegionLogWealth(&r[k]), mix, 1e-10);
    }
    /* The paths themselves are very different: with the errors first the
     * 0.9 bet is at 0.1^50 after 50 samples. */
    gateRegionInit(&r[0]);
    feed(&r[0], &b, 0, 50, 1);
    CHECK_NEAR(r[0].logw[3], 50.0 * ref_inc_err[3], 1e-11);
}

/* With no errors the log mixture is log((1/4) sum_j (1 + c_j a/(1-a))^n) and
 * the certification time is the first n at which it reaches the threshold.
 * The reference n was found by evaluating that expression in high precision;
 * in every case below it clears the threshold by more than 1e-4 at n and
 * misses it by more than 2e-4 at n - 1, a million times the rounding of the
 * running sums, so the exact n is not a matter of luck.
 *
 * For comparison, the fixed sample binomial test of the spec needs 90 / 228
 * / 459 samples at delta = 0.01: the anytime-valid mixture needs 128 / 325 /
 * 653. That is the price of being allowed to look after every sample. */
TEST(zero_error_certification_n) {
    static const struct {
        double alpha, delta_region;
        int n;
    } direct[] = {
        {0.05, 0.01, 128},   {0.05, 0.05, 91},    {0.05, 1e-6, 329},
        {0.02, 0.01, 325},   {0.02, 0.05, 230},   {0.02, 1e-6, 836},
        {0.01, 0.01, 653},   {0.01, 0.05, 462},   {0.01, 1e-6, 1680},
        {0.1, 0.01, 62},     {0.1, 0.05, 44},     {0.1, 1e-6, 160},
        {0.25, 0.01, 23},    {0.25, 0.05, 16},    {0.25, 1e-6, 58},
        {0.001, 0.01, 6556}, {0.001, 0.05, 4637}, {0.001, 1e-6, 16881}
    };
    /* The same through the budget: threshold of generation g of a gate with
     * 'nregions' regions. */
    static const struct {
        double alpha, delta_total;
        uint64_t g;
        uint32_t nregions;
        int n;
    } budget[] = {
        {0.05, 0.01, 1, 1, 144},  {0.05, 0.01, 1, 32, 220},
        {0.05, 0.01, 5, 64, 293}, {0.05, 0.05, 3, 256, 269},
        {0.02, 0.01, 1, 1, 364},  {0.02, 0.01, 1, 32, 558},
        {0.02, 0.01, 5, 64, 745}, {0.02, 0.05, 3, 256, 682},
        {0.01, 0.01, 1, 1, 732},  {0.01, 0.01, 1, 32, 1121},
        {0.01, 0.01, 5, 64, 1498}, {0.01, 0.05, 3, 256, 1372}
    };
    size_t i, total = COUNTOF(direct) + COUNTOF(budget);

    for (i = 0; i < total; i++) {
        double alpha, thr, lw = 0.0, prev = 0.0;
        int expected, n = 0, monotone = 1;
        gateBets b;
        gateRegion r;

        if (i < COUNTOF(direct)) {
            alpha = direct[i].alpha;
            thr = -log(direct[i].delta_region);
            expected = direct[i].n;
        } else {
            size_t k = i - COUNTOF(direct);

            alpha = budget[k].alpha;
            thr = gateBudgetLogThreshold(budget[k].delta_total, budget[k].g, budget[k].nregions);
            expected = budget[k].n;
        }
        gateBetsInit(&b, alpha);
        gateRegionInit(&r);
        CHECK(!gateRegionCertifies(&r, thr)); /* never with zero samples */
        while (!gateRegionCertifies(&r, thr) && n < expected + 1000) {
            lw = gateRegionUpdate(&r, &b, 0);
            if (!(lw > prev)) monotone = 0;
            prev = lw;
            n++;
        }
        CHECK_EQ_INT(n, expected);
        CHECK_EQ_INT(r.n, (uint64_t)expected);
        CHECK_EQ_INT(r.errors, 0);
        CHECK(monotone);  /* every correct sample strictly adds evidence */
        CHECK(lw >= thr);

        /* Certification is a first passage, not a state: one error right
         * after it takes the wealth back below the threshold (the 0.9 bet
         * that carried it there loses 90%). */
        gateRegionUpdate(&r, &b, 1);
        CHECK(!gateRegionCertifies(&r, thr));
    }
}

/* A stream whose error rate is above alpha is exactly what the test must not
 * certify. 'p' is the true error rate; the stream is IID from a fixed seed.
 * Besides "never certified at any of the n prefixes" this checks that the
 * evidence goes the right way: every bet has a negative drift at p > alpha,
 * so the log wealth must fall roughly linearly. */
static void highErrorStream(double alpha, double p, const double *inc_ok_ref,
                            uint64_t seed, double final_below) {
    const int n = 100000, step = 10000;
    const double thr = -log(0.01);
    uint64_t rng = seed;
    gateBets b;
    gateRegion r;
    double lw = 0.0, max_lw = -INFINITY, checkpoint = 0.0;
    int i, j, errors = 0, certified = 0, decreasing = 1;

    gateBetsInit(&b, alpha);
    gateRegionInit(&r);
    for (i = 1; i <= n; i++) {
        int e = gate_test_uniform(&rng) < p;

        errors += e;
        lw = gateRegionUpdate(&r, &b, e);
        if (lw > max_lw) max_lw = lw;
        if (gateRegionCertifies(&r, thr)) certified++;
        if (i % step == 0) {
            if (!(lw < checkpoint)) decreasing = 0;
            checkpoint = lw;
        }
    }
    CHECK_EQ_INT(certified, 0);
    CHECK(max_lw < thr);
    CHECK(decreasing);          /* lower at every 10000 sample checkpoint */
    CHECK(lw < final_below);
    CHECK(isfinite(lw));

    /* The stream really has the intended rate (within 6 sigma)... */
    CHECK(fabs(errors - n * p) < 6.0 * sqrt(n * p * (1.0 - p)));
    CHECK_EQ_INT(r.n, n);
    CHECK_EQ_INT(r.errors, errors);
    /* ... and each wealth is the closed form for the counts that occurred.
     * 1e-6 covers the rounding of 100000 additions at magnitude 1e4. */
    for (j = 0; j < GATE_NBETS; j++) {
        double expected = (double)(n - errors) * inc_ok_ref[j] + (double)errors * ref_inc_err[j];

        CHECK_NEAR(r.logw[j], expected, 1e-6);
    }
}

TEST(high_error_stream_never_certifies) {
    /* Drift of the best bet (c = 0.1) per sample: -0.0036 at alpha = 0.05,
     * p = 0.08 and -0.00116 at alpha = 0.02, p = 0.03; times 100000, with a
     * few standard deviations (about 10 each) of slack. */
    highErrorStream(0.05, 0.08, ref_inc_ok[0].inc_ok, 20261128ull, -300.0);
    highErrorStream(0.02, 0.03, ref_inc_ok[1].inc_ok, 77ull, -80.0);
}

/* log((1/4) sum exp(logw)) where a naive evaluation overflows, underflows or
 * loses everything. */
TEST(log_wealth_extremes) {
    static const struct {
        double logw[GATE_NBETS];
        double expected, tol;
    } cases[] = {
        {{3.25, -1.5, 0.125, 2.0}, 2.1556984758860732147e+0, 1e-14},
        /* exp(1000) overflows a double. */
        {{1000, 1001, 999, 990}, 1.0000213227139181636e+3, 1e-12},
        {{710, 709.5, 0, -710}, 7.0908778262306021606e+2, 1e-12},
        /* exp(-1000) and exp(-745.5) underflow to 0: naive gives -inf. */
        {{-1000, -1001, -999, -990}, -9.9138610886689036960e+2, 1e-12},
        {{-745.5, -746, -750, -760}, -7.4640532596486994072e+2, 1e-12},
        /* One bet holds everything: wealth / 4, i.e. -log(4). */
        {{0, -800, -800, -800}, -1.3862943611198906188e+0, 1e-15},
        /* 1e15 is the log wealth after some 2e16 correct samples. The
         * spacing of doubles there is 0.125. */
        {{1e15, 1e15 - 1, 1e15 - 2, 0}, 9.9999999999999902131e+14, 0.126}
    };
    static const int perm[4][GATE_NBETS] = {
        {0, 1, 2, 3}, {3, 2, 1, 0}, {1, 3, 0, 2}, {2, 0, 3, 1}
    };
    uint64_t rng = 99;
    gateRegion r;
    size_t i;
    int p, j;

    gateRegionInit(&r);
    for (i = 0; i < COUNTOF(cases); i++) {
        /* The maximum may sit in any slot. */
        for (p = 0; p < 4; p++) {
            for (j = 0; j < GATE_NBETS; j++) r.logw[j] = cases[i].logw[perm[p][j]];
            CHECK_NEAR(gateRegionLogWealth(&r), cases[i].expected, cases[i].tol);
        }
    }

    /* Four equal wealths: the mixture is that wealth, exactly. */
    for (j = 0; j < GATE_NBETS; j++) r.logw[j] = 5.0;
    CHECK(gateRegionLogWealth(&r) == 5.0);
    for (j = 0; j < GATE_NBETS; j++) r.logw[j] = -123456.789;
    CHECK(gateRegionLogWealth(&r) == -123456.789);

    /* An average of four numbers lies between max/4 and max. */
    for (i = 0; i < 2000; i++) {
        double max = -INFINITY, lw;

        for (j = 0; j < GATE_NBETS; j++) {
            r.logw[j] = (gate_test_uniform(&rng) - 0.5) * 3000.0;
            if (r.logw[j] > max) max = r.logw[j];
        }
        lw = gateRegionLogWealth(&r);
        CHECK(lw <= max && lw >= max - log(4.0) - 1e-12);
    }

    /* The header promises no overflow for any realistic stream. 1e12
     * samples: in linear space the wealth would be exp(4.6e10) after all
     * correct samples and exp(-1e11) after all errors. */
    {
        gateBets b;
        double lw;

        gateBetsInit(&b, 0.05);
        for (j = 0; j < GATE_NBETS; j++) r.logw[j] = 1e12 * b.inc_ok[j];
        lw = gateRegionUpdate(&r, &b, 0);
        CHECK(isfinite(lw));
        CHECK_REL(lw, 1e12 * ref_inc_ok[0].inc_ok[3] - log(4.0), 1e-12);
        CHECK(gateRegionCertifies(&r, 100.0));

        for (j = 0; j < GATE_NBETS; j++) r.logw[j] = 1e12 * b.inc_err[j];
        lw = gateRegionUpdate(&r, &b, 1);
        CHECK(isfinite(lw));
        CHECK_REL(lw, 1e12 * ref_inc_err[0] - log(4.0), 1e-12);
        CHECK(!gateRegionCertifies(&r, 0.0));
    }
}

/* Corrupted evidence must never be read as evidence. The other three bets
 * are far above the threshold in every case, so a wrong answer here would
 * be a certification. */
TEST(non_finite_wealth_never_certifies) {
    static const double thresholds[] = {-DBL_MAX, -1.0, 0.0, 1.0, 4.6};
    gateBets b;
    gateRegion r;
    size_t k;
    int j, i;

    gateBetsInit(&b, 0.05);
    for (j = 0; j < GATE_NBETS; j++) {
        /* NaN in slot j. */
        gateRegionInit(&r);
        for (i = 0; i < GATE_NBETS; i++) r.logw[i] = 100.0;
        r.logw[j] = NAN;
        CHECK(isnan(gateRegionLogWealth(&r)));
        for (k = 0; k < COUNTOF(thresholds); k++) CHECK(!gateRegionCertifies(&r, thresholds[k]));
        CHECK(!gateRegionCertifies(&r, -INFINITY));
        /* It does not heal: more good samples keep it NaN. */
        for (i = 0; i < 500; i++) gateRegionUpdate(&r, &b, 0);
        CHECK(isnan(gateRegionLogWealth(&r)));
        CHECK(!gateRegionCertifies(&r, 1.0));

        /* +inf in slot j: infinite wealth is not finite evidence. */
        gateRegionInit(&r);
        for (i = 0; i < GATE_NBETS; i++) r.logw[i] = 100.0;
        r.logw[j] = INFINITY;
        CHECK(gateRegionLogWealth(&r) == INFINITY);
        for (k = 0; k < COUNTOF(thresholds); k++) CHECK(!gateRegionCertifies(&r, thresholds[k]));

        /* +inf and NaN together. */
        r.logw[(j + 1) % GATE_NBETS] = NAN;
        CHECK(isnan(gateRegionLogWealth(&r)));
        CHECK(!gateRegionCertifies(&r, 1.0));

        /* -inf in slot j is a bet that lost everything (wealth 0). The
         * mixture of the other three is still a valid, finite wealth:
         * log(3 e^10 / 4) = 10 + log(0.75). */
        gateRegionInit(&r);
        for (i = 0; i < GATE_NBETS; i++) r.logw[i] = 10.0;
        r.logw[j] = -INFINITY;
        CHECK_NEAR(gateRegionLogWealth(&r), 10.0 - 0.28768207245178092744, 1e-14);
        CHECK(gateRegionCertifies(&r, 9.7));
        CHECK(!gateRegionCertifies(&r, 9.72));
    }

    /* All four at +inf, all four at -inf (total wealth 0), all NaN. */
    for (i = 0; i < GATE_NBETS; i++) r.logw[i] = INFINITY;
    CHECK(gateRegionLogWealth(&r) == INFINITY);
    CHECK(!gateRegionCertifies(&r, 1.0));
    CHECK(!gateRegionCertifies(&r, -INFINITY));
    for (i = 0; i < GATE_NBETS; i++) r.logw[i] = -INFINITY;
    CHECK(gateRegionLogWealth(&r) == -INFINITY);
    for (k = 0; k < COUNTOF(thresholds); k++) CHECK(!gateRegionCertifies(&r, thresholds[k]));
    CHECK(!gateRegionCertifies(&r, -INFINITY));
    for (i = 0; i < GATE_NBETS; i++) r.logw[i] = NAN;
    CHECK(!gateRegionCertifies(&r, -DBL_MAX));

    /* A NaN or infinite threshold cannot be met by a healthy region. */
    gateRegionInit(&r);
    for (i = 0; i < GATE_NBETS; i++) r.logw[i] = 1e6;
    CHECK(gateRegionCertifies(&r, 1e6));
    CHECK(!gateRegionCertifies(&r, NAN));
    CHECK(!gateRegionCertifies(&r, INFINITY));
}

/* The header says the caller validates alpha. If it does not, nothing may
 * ever be certified: a zero error stream of any length included. */
TEST(invalid_alpha_never_certifies) {
    static const double bad[] = {0.0, -0.0, 1.0, -0.1, 1.5, 2.0, 1e300, -1e300};
    double alphas[COUNTOF(bad) + 3];
    size_t i, n = 0;
    int k;

    for (i = 0; i < COUNTOF(bad); i++) alphas[n++] = bad[i];
    alphas[n++] = NAN;
    alphas[n++] = INFINITY;
    alphas[n++] = -INFINITY;

    for (i = 0; i < n; i++) {
        gateBets b;
        gateRegion r;
        int certified = 0;

        gateBetsInit(&b, alphas[i]);
        gateRegionInit(&r);
        for (k = 0; k < 5000; k++) {
            gateRegionUpdate(&r, &b, 0);
            certified += gateRegionCertifies(&r, -DBL_MAX);
            certified += gateRegionCertifies(&r, 0.5);
        }
        CHECK_EQ_INT(certified, 0);
        CHECK_EQ_INT(r.n, 5000);
    }
}

TEST(certifies_boundary) {
    gateBets b;
    gateRegion r;
    double lw = 0.0;
    int i, j;

    /* Four equal log wealths give exactly that value: an exact boundary. */
    gateRegionInit(&r);
    for (j = 0; j < GATE_NBETS; j++) r.logw[j] = 5.0;
    CHECK(gateRegionCertifies(&r, 5.0));                           /* equal */
    CHECK(gateRegionCertifies(&r, nextafter(5.0, -INFINITY)));     /* above */
    CHECK(!gateRegionCertifies(&r, nextafter(5.0, INFINITY)));     /* 1 ulp short */

    /* The same on a real stream, against the value the update returned. */
    gateBetsInit(&b, 0.05);
    gateRegionInit(&r);
    for (i = 0; i < 203; i++) lw = gateRegionUpdate(&r, &b, i == 40 || i == 90 || i == 150);
    CHECK(isfinite(lw) && lw > 0.0);
    CHECK(gateRegionCertifies(&r, lw));
    CHECK(gateRegionCertifies(&r, nextafter(lw, -INFINITY)));
    CHECK(!gateRegionCertifies(&r, nextafter(lw, INFINITY)));

    /* And against the budget threshold: 219 samples are one short for
     * alpha = 0.05, generation 1, 32 regions (see the table above). */
    gateRegionInit(&r);
    for (i = 0; i < 219; i++) gateRegionUpdate(&r, &b, 0);
    CHECK(!gateRegionCertifies(&r, gateBudgetLogThreshold(0.01, 1, 32)));
    gateRegionUpdate(&r, &b, 0);
    CHECK(gateRegionCertifies(&r, gateBudgetLogThreshold(0.01, 1, 32)));
}

/* ----------------------------- Error budget ------------------------------ */

#define G32 4294967296ull               /* 2^32 */
#define G53 9007199254740994ull         /* 2^53 + 2 */
#define G62 4611686018427387904ull      /* 2^62 */
#define G63 9223372036854775808ull      /* 2^63 */
#define GMAX 18446744073709551615ull    /* 2^64 - 1 */

TEST(budget_generation_delta) {
    /* delta_total / (g (g + 1)) as an exact rational, rounded once. At
     * g = 2^32 the integer product g (g + 1) = 2^64 + 2^32 wraps around to
     * 2^32: a 64 bit implementation hands that generation 2.3e-12 instead
     * of 5.4e-22. */
    static const struct {
        double delta_total;
        uint64_t g;
        double delta_g;
    } cases[] = {
        {0.01, 1, 5.0000000000000001041e-3},
        {0.01, 2, 1.6666666666666667014e-3},
        {0.01, 3, 8.3333333333333335068e-4},
        {0.01, 10, 9.0909090909090910983e-5},
        {0.01, 1000, 9.9900099900099902179e-9},
        {0.01, 65536, 2.3282709099440011471e-12},
        {0.01, G32, 5.4210108611653448348e-22},
        {0.01, G32 + 1, 5.4210108586409899399e-22},
        {0.01, G53, 1.2325951644078302874e-34},
        {0.01, G62, 4.7019774032891501287e-40},
        {0.01, GMAX, 2.9387358770557188313e-41},
        {0.05, 1, 2.5000000000000001388e-2},
        {0.05, 2, 8.3333333333333337959e-3},
        {0.05, 3, 4.1666666666666668980e-3},
        {0.05, 10, 4.5454545454545457069e-4},
        {0.05, 1000, 4.9950049950049952823e-8},
        {0.05, 65536, 1.1641354549720006139e-11},
        {0.05, G32, 2.7105054305826725115e-21},
        {0.05, G32 + 1, 2.7105054293204950640e-21},
        {0.05, G53, 6.1629758220391516508e-34},
        {0.05, G62, 2.3509887016445751459e-39},
        {0.05, GMAX, 1.4693679385278594666e-40}
    };
    size_t i;

    for (i = 0; i < COUNTOF(cases); i++) {
        double d = gateBudgetGenerationDelta(cases[i].delta_total, cases[i].g);

        CHECK_REL(d, cases[i].delta_g, 1e-15);
        CHECK(d > 0.0);
    }
    /* The first generation gets half of everything. */
    CHECK(gateBudgetGenerationDelta(0.01, 1) == 0.005);
}

TEST(budget_sum_never_exceeds_total) {
    static const double totals[] = {0.01, 0.05, 0.001, 0.5};
    static const uint64_t marks[] = {1, 2, 3, 10, 100, 1000, 10000, 100000};
    static const struct {
        double delta_total;
        uint64_t g;
        double spent, remaining;
    } cases[] = {
        {0.01, 1, 5.0000000000000001041e-3, 5.0000000000000001041e-3},
        {0.01, 2, 6.6666666666666668054e-3, 3.3333333333333334027e-3},
        {0.01, 3, 7.5000000000000001561e-3, 2.5000000000000000520e-3},
        {0.01, 7, 8.7500000000000001821e-3, 1.2500000000000000260e-3},
        {0.01, 99, 9.9000000000000002061e-3, 1.0000000000000000208e-4},
        {0.01, 1000, 9.9900099900099902179e-3, 9.9900099900099902179e-6},
        {0.01, 1000000, 9.9999900000100001982e-3, 9.9999900000100001982e-9},
        {0.01, 1099511627776ull, 9.9999999999909052611e-3, 9.0949470177210107624e-15},
        {0.05, 1, 2.5000000000000001388e-2, 2.5000000000000001388e-2},
        {0.05, 2, 3.3333333333333335184e-2, 1.6666666666666667592e-2},
        {0.05, 3, 3.7500000000000002082e-2, 1.2500000000000000694e-2},
        {0.05, 7, 4.3750000000000002429e-2, 6.2500000000000003469e-3},
        {0.05, 99, 4.9500000000000002748e-2, 5.0000000000000002776e-4},
        {0.05, 1000, 4.9950049950049952823e-2, 4.9950049950049952823e-5},
        {0.05, 1000000, 4.9999950000050002726e-2, 4.9999950000050002726e-8},
        {0.05, 1099511627776ull, 4.9999999999954528040e-2, 4.5474735088605055389e-14}
    };
    size_t i, m;
    int k;

    /* The spent total against the exact value, and what is left against
     * delta_total / (g + 1). The subtraction loses digits as the remainder
     * shrinks, hence the absolute tolerance of a few ulps of delta_total. */
    for (i = 0; i < COUNTOF(cases); i++) {
        double spent = gateBudgetSpent(cases[i].delta_total, cases[i].g);

        CHECK_REL(spent, cases[i].spent, 1e-15);
        CHECK_NEAR(cases[i].delta_total - spent, cases[i].remaining, 4.0 * DBL_EPSILON * cases[i].delta_total);
        CHECK(spent < cases[i].delta_total);
    }

    /* Adding up what the generations were given, one by one, is what
     * gateBudgetSpent() reports, and it is always below the total. */
    for (i = 0; i < COUNTOF(totals); i++) {
        double total = totals[i], sum = 0.0;
        uint64_t g = 0;
        int below = 1, growing = 1;

        CHECK(gateBudgetSpent(total, 0) == 0.0);
        for (m = 0; m < COUNTOF(marks); m++) {
            while (g < marks[m]) {
                double before = sum;

                g++;
                sum += gateBudgetGenerationDelta(total, g);
                if (!(sum > before)) growing = 0;
                if (!(sum < total)) below = 0;
            }
            /* 1e-11: bound on the rounding of 100000 additions. */
            CHECK_REL(sum, gateBudgetSpent(total, g), 1e-11);
            CHECK_REL(sum, total * (double)g / (double)(g + 1), 1e-11);
        }
        CHECK(below);
        CHECK(growing);
        /* It approaches the total: 1/100001 of it is left after 100000
         * generations. */
        CHECK_REL(total - sum, total / 100001.0, 1e-6);
    }

    /* Monotone, and never above the total however large g is: not even by
     * the one ulp a carelessly ordered expression would allow. */
    for (i = 0; i < COUNTOF(totals); i++) {
        double prev = 0.0;

        for (k = 0; k <= 64; k++) {
            uint64_t g = k < 64 ? (uint64_t)1 << k : GMAX;
            double spent = gateBudgetSpent(totals[i], g);

            CHECK(spent >= prev && spent <= totals[i]);
            if (k <= 50) CHECK(spent > prev && spent < totals[i]);
            prev = spent;
        }
        CHECK_REL(prev, totals[i], 1e-15);
    }
}

TEST(budget_log_threshold) {
    static const struct {
        double delta_total;
        uint64_t g;
        uint32_t nregions;
        double thr;
    } cases[] = {
        {0.01, 1, 1, 5.2983173665480366566e+0},       /* log(200) */
        {0.01, 1, 32, 8.7640532693477632037e+0},      /* log(6400) */
        {0.01, 2, 32, 9.8626655580158728951e+0},
        {0.01, 5, 64, 1.2165250651009918579e+1},
        {0.05, 3, 256, 1.1025816367821553723e+1},
        {0.001, 7, 16384, 2.0637167497556520596e+1},
        {0.01, 1000, 32, 2.1887416147085175532e+1},
        {0.01, G32, 32, 5.2432325644857148341e+1},
        {0.01, G62, 32, 9.4021156478221036262e+1},
        {0.01, G63, 4294967295u, 1.1412242471422661959e+2},
        {0.01, GMAX, 4294967295u, 1.1550871907534651021e+2},
        /* delta_g = 1e-300 / 2.1e37 is not representable (it underflows to
         * zero): only a log space evaluation gets these two right. */
        {1e-300, G62, 32, 7.8019151419044665010e+2},
        {5e-324, G62, 4294967295u, 8.5257103208849989994e+2},
        /* Close to the smallest threshold there is: log(2 / 0.99). */
        {0.99, 1, 1, 7.0319751641344675957e-1}
    };
    static const uint32_t regions[] = {1, 2, 32, 64, 256, 16384};
    gateRegion fresh;
    size_t i, k;
    uint64_t g;
    int e;

    for (i = 0; i < COUNTOF(cases); i++) {
        double thr = gateBudgetLogThreshold(cases[i].delta_total, cases[i].g, cases[i].nregions);

        CHECK(isfinite(thr));
        CHECK_REL(thr, cases[i].thr, 1e-14);
    }

    /* For small g it is -log(delta_g / nregions) with delta_g as returned by
     * gateBudgetGenerationDelta(): the two functions describe one budget. */
    for (g = 1; g <= 300; g++) {
        for (k = 0; k < COUNTOF(regions); k++) {
            double thr = gateBudgetLogThreshold(0.01, g, regions[k]);
            double delta_region = gateBudgetGenerationDelta(0.01, g) / regions[k];

            CHECK_NEAR(thr, -log(delta_region), 1e-12);
            CHECK_REL(exp(-thr), delta_region, 1e-12);
        }
    }

    /* Finite and increasing over the whole range of g. Doubling g adds
     * log(2) + log((2g + 1) / (g + 1)): log(3) at g = 1, tending to
     * 2 log(2) from below. */
    for (k = 0; k < COUNTOF(regions); k++) {
        double prev = gateBudgetLogThreshold(0.01, 1, regions[k]);

        for (e = 1; e <= 63; e++) {
            double thr = gateBudgetLogThreshold(0.01, (uint64_t)1 << e, regions[k]);

            CHECK(isfinite(thr));
            CHECK(thr - prev > 1.0986 && thr - prev < 1.3863);
            if (e >= 30) CHECK_NEAR(thr - prev, 2.0 * 0.69314718055994530942, 1e-8);
            prev = thr;
        }
        CHECK(gateBudgetLogThreshold(0.01, GMAX, regions[k]) >= prev);
        CHECK(isfinite(gateBudgetLogThreshold(0.01, GMAX, regions[k])));
    }
    /* Strictly increasing generation by generation while g is small. */
    for (g = 1; g < 5000; g++)
        CHECK(gateBudgetLogThreshold(0.01, g + 1, 32) > gateBudgetLogThreshold(0.01, g, 32));

    /* More regions or a smaller total budget make it harder, never easier. */
    CHECK(gateBudgetLogThreshold(0.01, 3, 33) > gateBudgetLogThreshold(0.01, 3, 32));
    CHECK(gateBudgetLogThreshold(0.005, 3, 32) > gateBudgetLogThreshold(0.01, 3, 32));
    CHECK_NEAR(gateBudgetLogThreshold(0.01, 3, 64) - gateBudgetLogThreshold(0.01, 3, 32),
               0.69314718055994530942, 1e-13);

    /* The threshold is positive for every valid budget, so a region with no
     * evidence (log wealth 0) is never certified. */
    gateRegionInit(&fresh);
    CHECK(gateBudgetLogThreshold(nextafter(1.0, 0.0), 1, 1) > 0.69);
    CHECK(!gateRegionCertifies(&fresh, gateBudgetLogThreshold(nextafter(1.0, 0.0), 1, 1)));
    CHECK(!gateRegionCertifies(&fresh, gateBudgetLogThreshold(0.01, 1, 1)));
}

/* Arguments outside the documented domain are a caller bug. The answers
 * must be the harmless ones: no budget, and a threshold nothing can reach.
 * (log(0) = -inf would be a threshold everything passes.) */
TEST(budget_fails_closed) {
    static const double bad_totals[] = {0.0, -0.0, 1.0, -0.01, 1.5, 1e300, -1e300};
    double totals[COUNTOF(bad_totals) + 3];
    gateRegion rich;
    size_t i, n = 0;
    int j;

    for (i = 0; i < COUNTOF(bad_totals); i++) totals[n++] = bad_totals[i];
    totals[n++] = NAN;
    totals[n++] = INFINITY;
    totals[n++] = -INFINITY;

    gateRegionInit(&rich);
    for (j = 0; j < GATE_NBETS; j++) rich.logw[j] = 1e300;

    for (i = 0; i < n; i++) {
        double thr = gateBudgetLogThreshold(totals[i], 1, 32);

        CHECK(gateBudgetGenerationDelta(totals[i], 1) == 0.0);
        CHECK(gateBudgetSpent(totals[i], 5) == 0.0);
        CHECK(thr == INFINITY);
        CHECK(!gateRegionCertifies(&rich, thr));
    }
    /* Generation 0 does not exist; a gate has at least one region. */
    CHECK(gateBudgetGenerationDelta(0.01, 0) == 0.0);
    CHECK(gateBudgetSpent(0.01, 0) == 0.0);
    CHECK(gateBudgetLogThreshold(0.01, 0, 32) == INFINITY);
    CHECK(gateBudgetLogThreshold(0.01, 1, 0) == INFINITY);
    CHECK(!gateRegionCertifies(&rich, gateBudgetLogThreshold(0.01, 0, 32)));
    CHECK(!gateRegionCertifies(&rich, gateBudgetLogThreshold(0.01, 1, 0)));
    /* The valid neighbours are unaffected. */
    CHECK(gateRegionCertifies(&rich, gateBudgetLogThreshold(0.01, 1, 1)));
    CHECK(gateBudgetGenerationDelta(nextafter(1.0, 0.0), 1) > 0.49);
    CHECK(gateBudgetGenerationDelta(DBL_MIN, 1) > 0.0);
}

/* ---------------------------- Threshold grids ---------------------------- */

#define SENTINEL (-7.0)

static int strictlyIncreasing(const double *grid, uint32_t n) {
    uint32_t t;

    for (t = 1; t < n; t++) {
        if (!(grid[t] > grid[t - 1])) return 0;
    }
    return 1;
}

static double logit(double p) {
    return log(p / (1.0 - p));
}

TEST(grid_uniform) {
    static const uint32_t sizes[] = {2, 3, 7, 32, 100, 256};
    static const struct {
        uint32_t n, t;
        double value;
    } ref[] = {
        {3, 1, 0.5},
        {7, 1, 1.6666666666666666667e-1}, {7, 3, 0.5}, {7, 5, 8.3333333333333333333e-1},
        {32, 1, 3.2258064516129032258e-2}, {32, 16, 5.1612903225806451613e-1},
        {32, 30, 9.6774193548387096774e-1},
        {256, 1, 3.9215686274509803922e-3}, {256, 100, 3.9215686274509803922e-1},
        {256, 254, 9.9607843137254901961e-1}
    };
    double grid[GATE_MAX_THRESHOLDS + 2];
    size_t i;
    uint32_t t;

    for (i = 0; i < COUNTOF(sizes); i++) {
        uint32_t n = sizes[i];
        int evenly = 1;

        for (t = 0; t < COUNTOF(grid); t++) grid[t] = SENTINEL;
        /* The class count means nothing to a uniform grid. */
        CHECK_EQ_INT(gateGridBuild(grid, n, GATE_GRID_UNIFORM, i % 2 ? 0 : 5), GATE_OK);
        CHECK(grid[0] == 0.0);
        CHECK(grid[n - 1] == 1.0);
        CHECK(strictlyIncreasing(grid, n));
        for (t = 0; t + 1 < n; t++) {
            if (fabs((grid[t + 1] - grid[t]) - 1.0 / (n - 1)) > 1e-15) evenly = 0;
        }
        CHECK(evenly);
        /* Nothing is written past the n entries. */
        CHECK(grid[n] == SENTINEL && grid[n + 1] == SENTINEL);
    }
    /* t / (n - 1) is a single correctly rounded division: exact match. */
    for (i = 0; i < COUNTOF(ref); i++) {
        CHECK_EQ_INT(gateGridBuild(grid, ref[i].n, GATE_GRID_UNIFORM, 0), GATE_OK);
        CHECK(grid[ref[i].t] == ref[i].value);
    }
    /* n = 2 is the two regions "everything" and "score 1 only". */
    CHECK_EQ_INT(gateGridBuild(grid, 2, GATE_GRID_UNIFORM, 0), GATE_OK);
    CHECK(grid[0] == 0.0 && grid[1] == 1.0);
}

TEST(grid_logit) {
    static const uint32_t classes[] = {2, 3, 10};
    static const uint32_t sizes[] = {2, 32, 256};
    /* sigmoid(lo + t (hi - lo) / (n - 1)), lo = logit(1/K), hi =
     * logit(0.9999). */
    static const struct {
        uint32_t k, n, t;
        double value;
    } ref[] = {
        {2, 32, 1, 5.7373454250034900197e-1},
        {2, 32, 2, 6.4433031473443107479e-1},
        {2, 32, 16, 9.9145368846173099148e-1},
        {2, 32, 30, 9.9986540905804038898e-1},
        {3, 32, 1, 4.0765136363084342694e-1},
        {3, 32, 15, 9.8367847875978175482e-1},
        {3, 32, 30, 9.9986236618248398357e-1},
        {10, 32, 1, 1.3832861670890706385e-1},
        {10, 32, 16, 9.7564827039818746219e-1},
        {10, 32, 30, 9.9985552471636605739e-1},
        {2, 256, 1, 5.0902866590920546625e-1},
        {2, 256, 127, 9.8991990041026863238e-1},
        {2, 256, 254, 9.9989632250226730401e-1},
        {10, 256, 1, 1.0409882519657079805e-1},
        {10, 256, 128, 9.7149829138351213897e-1},
        {10, 256, 254, 9.9989542539213164659e-1},
        {64, 256, 1, 1.6451204577043515808e-2},
        {64, 256, 100, 7.4903701761249832986e-1},
        {64, 256, 254, 9.9989462441302129508e-1},
        {3, 3, 1, 9.8605438781707466901e-1}
    };
    double grid[GATE_MAX_THRESHOLDS + 2];
    size_t i, j;
    uint32_t t;

    for (i = 0; i < COUNTOF(classes); i++) {
        for (j = 0; j < COUNTOF(sizes); j++) {
            uint32_t k = classes[i], n = sizes[j];
            double lo = logit(1.0 / k), hi = logit(0.9999);
            int evenly = 1;

            for (t = 0; t < COUNTOF(grid); t++) grid[t] = SENTINEL;
            CHECK_EQ_INT(gateGridBuild(grid, n, GATE_GRID_LOGIT, k), GATE_OK);
            /* Exact end points, not "close to". */
            CHECK(grid[0] == 1.0 / k);
            CHECK(grid[n - 1] == 0.9999);
            CHECK(grid[n - 1] == GATE_GRID_LOGIT_TOP);
            CHECK(strictlyIncreasing(grid, n));
            /* Uniform in log odds. The logit amplifies the rounding of a
             * value near 0.9999 by 1e4, hence 1e-9. */
            for (t = 0; t + 1 < n; t++) {
                if (fabs((logit(grid[t + 1]) - logit(grid[t])) - (hi - lo) / (n - 1)) > 1e-9) evenly = 0;
            }
            CHECK(evenly);
            CHECK(grid[n] == SENTINEL && grid[n + 1] == SENTINEL);
        }
    }
    CHECK_EQ_INT(gateGridBuild(grid, 2, GATE_GRID_LOGIT, 2), GATE_OK);
    CHECK(grid[0] == 0.5 && grid[1] == 0.9999);
    CHECK_EQ_INT(gateGridBuild(grid, 2, GATE_GRID_LOGIT, 3), GATE_OK);
    CHECK(grid[0] == 0.33333333333333331483 && grid[1] == 0.9999);
    CHECK_EQ_INT(gateGridBuild(grid, 256, GATE_GRID_LOGIT, 10), GATE_OK);
    CHECK(grid[0] == 0.1 && grid[255] == 0.9999);

    for (i = 0; i < COUNTOF(ref); i++) {
        CHECK_EQ_INT(gateGridBuild(grid, ref[i].n, GATE_GRID_LOGIT, ref[i].k), GATE_OK);
        CHECK_NEAR(grid[ref[i].t], ref[i].value, 1e-13);
    }
}

/* Every size and every class count the module accepts, plus class counts far
 * beyond: always strictly increasing with exact end points. */
TEST(grid_all_shapes) {
    static const uint32_t huge[] = {65, 1000, 1000000, 4294967295u};
    double grid[GATE_MAX_THRESHOLDS];
    uint32_t n, k;
    size_t i;
    int built = 0, bad = 0;

    for (n = GATE_MIN_THRESHOLDS; n <= GATE_MAX_THRESHOLDS; n++) {
        if (gateGridBuild(grid, n, GATE_GRID_UNIFORM, 0) != GATE_OK ||
            grid[0] != 0.0 || grid[n - 1] != 1.0 || !strictlyIncreasing(grid, n)) bad++;
        built++;
        for (k = GATE_MIN_CLASSES; k <= GATE_MAX_CLASSES; k++) {
            if (gateGridBuild(grid, n, GATE_GRID_LOGIT, k) != GATE_OK ||
                grid[0] != 1.0 / k || grid[n - 1] != GATE_GRID_LOGIT_TOP ||
                !strictlyIncreasing(grid, n)) bad++;
            built++;
        }
    }
    CHECK_EQ_INT(built, 255 * 64);
    CHECK_EQ_INT(bad, 0);

    for (i = 0; i < COUNTOF(huge); i++) {
        CHECK_EQ_INT(gateGridBuild(grid, 256, GATE_GRID_LOGIT, huge[i]), GATE_OK);
        CHECK(grid[0] == 1.0 / huge[i]);
        CHECK(grid[255] == GATE_GRID_LOGIT_TOP);
        CHECK(strictlyIncreasing(grid, 256));
    }
}

TEST(grid_invalid_arguments) {
    static const struct {
        uint32_t n;
        int kind;
        uint32_t nclasses;
    } bad[] = {
        {0, GATE_GRID_UNIFORM, 2},        {1, GATE_GRID_UNIFORM, 2},
        {257, GATE_GRID_UNIFORM, 2},      {4294967295u, GATE_GRID_UNIFORM, 2},
        {0, GATE_GRID_LOGIT, 2},          {1, GATE_GRID_LOGIT, 2},
        {257, GATE_GRID_LOGIT, 2},        {4294967295u, GATE_GRID_LOGIT, 2},
        {32, GATE_GRID_LOGIT, 0},         {32, GATE_GRID_LOGIT, 1},
        {32, 2, 2},                       {32, -1, 2},
        {32, 1000, 2},                    {1, 7, 0}
    };
    double grid[GATE_MAX_THRESHOLDS + 2];
    size_t i;
    uint32_t t;

    CHECK_EQ_INT(GATE_MIN_THRESHOLDS, 2);
    CHECK_EQ_INT(GATE_MAX_THRESHOLDS, 256);
    for (i = 0; i < COUNTOF(bad); i++) {
        int untouched = 1;

        for (t = 0; t < COUNTOF(grid); t++) grid[t] = SENTINEL;
        CHECK_EQ_INT(gateGridBuild(grid, bad[i].n, bad[i].kind, bad[i].nclasses), GATE_ERR);
        for (t = 0; t < COUNTOF(grid); t++) {
            if (grid[t] != SENTINEL) untouched = 0;
        }
        CHECK(untouched);
    }
    CHECK_EQ_INT(gateGridBuild(NULL, 32, GATE_GRID_UNIFORM, 2), GATE_ERR);
    /* The limits themselves are fine. */
    CHECK_EQ_INT(gateGridBuild(grid, 2, GATE_GRID_LOGIT, 2), GATE_OK);
    CHECK_EQ_INT(gateGridBuild(grid, 256, GATE_GRID_LOGIT, 2), GATE_OK);
}

static uint32_t bruteCount(const double *grid, uint32_t n, double score) {
    uint32_t t, count = 0;

    for (t = 0; t < n; t++) {
        if (grid[t] <= score) count++;
    }
    return count;
}

/* Everything gateGridCount() can get wrong on one grid: the thresholds
 * themselves (a score equal to a threshold is IN that region), their
 * immediate neighbours, both tails, non finite scores and a dense sweep. */
static void checkGridCount(const double *grid, uint32_t n) {
    uint32_t t;
    int exact = 0, sweep = 0, i;
    double below = nextafter(grid[0], -INFINITY), above = nextafter(grid[n - 1], INFINITY);

    for (t = 0; t < n; t++) {
        if (gateGridCount(grid, n, grid[t]) != t + 1) exact++;
        if (gateGridCount(grid, n, nextafter(grid[t], -INFINITY)) != t) exact++;
        if (gateGridCount(grid, n, nextafter(grid[t], INFINITY)) != t + 1) exact++;
        if (t + 1 < n && gateGridCount(grid, n, 0.5 * (grid[t] + grid[t + 1])) != t + 1) exact++;
    }
    CHECK_EQ_INT(exact, 0);

    CHECK_EQ_INT(gateGridCount(grid, n, below), 0);
    CHECK_EQ_INT(gateGridCount(grid, n, grid[0] - 1e-9), 0);
    CHECK_EQ_INT(gateGridCount(grid, n, -1.0), 0);
    CHECK_EQ_INT(gateGridCount(grid, n, -DBL_MAX), 0);
    CHECK_EQ_INT(gateGridCount(grid, n, -INFINITY), 0);
    CHECK_EQ_INT(gateGridCount(grid, n, above), n);
    CHECK_EQ_INT(gateGridCount(grid, n, 1.0), n);
    CHECK_EQ_INT(gateGridCount(grid, n, 2.0), n);
    CHECK_EQ_INT(gateGridCount(grid, n, DBL_MAX), n);
    CHECK_EQ_INT(gateGridCount(grid, n, INFINITY), n);
    CHECK_EQ_INT(gateGridCount(grid, n, NAN), 0);
    CHECK_EQ_INT(gateGridCount(grid, n, -NAN), 0);

    /* 22001 scores from -0.05 to 1.05 in steps of 0.00005. */
    for (i = 0; i <= 22000; i++) {
        double s = -0.05 + i * 0.00005;

        if (gateGridCount(grid, n, s) != bruteCount(grid, n, s)) sweep++;
    }
    CHECK_EQ_INT(sweep, 0);
}

TEST(grid_count) {
    static const uint32_t uniform_sizes[] = {2, 3, 4, 5, 7, 31, 32, 33, 100, 255, 256};
    static const uint32_t classes[] = {2, 3, 10};
    static const uint32_t logit_sizes[] = {2, 32, 256};
    static const double odd[] = {0.1, 0.2, 0.7};
    double grid[GATE_MAX_THRESHOLDS];
    uint64_t rng = 4242;
    size_t i, j;
    int k, mismatches = 0;

    for (i = 0; i < COUNTOF(uniform_sizes); i++) {
        CHECK_EQ_INT(gateGridBuild(grid, uniform_sizes[i], GATE_GRID_UNIFORM, 0), GATE_OK);
        checkGridCount(grid, uniform_sizes[i]);
    }
    for (i = 0; i < COUNTOF(classes); i++) {
        for (j = 0; j < COUNTOF(logit_sizes); j++) {
            CHECK_EQ_INT(gateGridBuild(grid, logit_sizes[j], GATE_GRID_LOGIT, classes[i]), GATE_OK);
            checkGridCount(grid, logit_sizes[j]);
        }
    }

    /* The 32 point two class grid is the default: random scores where a
     * binary classifier puts them, [0.5, 1]. */
    CHECK_EQ_INT(gateGridBuild(grid, 32, GATE_GRID_LOGIT, 2), GATE_OK);
    for (k = 0; k < 200000; k++) {
        double s = 0.5 + 0.5 * gate_test_uniform(&rng);

        if (gateGridCount(grid, 32, s) != bruteCount(grid, 32, s)) mismatches++;
    }
    CHECK_EQ_INT(mismatches, 0);
    /* The least confident score a binary model can produce is in region 0
     * and only there; 0.9999 and above is in all 32. */
    CHECK_EQ_INT(gateGridCount(grid, 32, 0.5), 1);
    CHECK_EQ_INT(gateGridCount(grid, 32, 0.9999), 32);
    CHECK_EQ_INT(gateGridCount(grid, 32, 0.99989999), 31);
    /* On the uniform grid 0 is in region 0 (so is -0.0) and 1 in all. */
    CHECK_EQ_INT(gateGridBuild(grid, 32, GATE_GRID_UNIFORM, 0), GATE_OK);
    CHECK_EQ_INT(gateGridCount(grid, 32, 0.0), 1);
    CHECK_EQ_INT(gateGridCount(grid, 32, -0.0), 1);
    CHECK_EQ_INT(gateGridCount(grid, 32, 1.0), 32);
    CHECK_EQ_INT(gateGridCount(grid, 32, 0.5), 16); /* 15/31 <= 0.5 < 16/31 */

    /* The function is a plain upper bound search: any increasing array. */
    checkGridCount(odd, 3);
    CHECK_EQ_INT(gateGridCount(odd, 3, 0.05), 0);
    CHECK_EQ_INT(gateGridCount(odd, 3, 0.1), 1);
    CHECK_EQ_INT(gateGridCount(odd, 3, 0.15), 1);
    CHECK_EQ_INT(gateGridCount(odd, 3, 0.2), 2);
    CHECK_EQ_INT(gateGridCount(odd, 3, 0.69), 2);
    CHECK_EQ_INT(gateGridCount(odd, 3, 0.7), 3);
    CHECK_EQ_INT(gateGridCount(odd, 1, 0.05), 0);
    CHECK_EQ_INT(gateGridCount(odd, 1, 0.1), 1);
    CHECK_EQ_INT(gateGridCount(odd, 0, 0.5), 0);
}

/* ------------------------------ Drift window ----------------------------- */

static int popcount64(uint64_t x) {
    int c = 0;

    for (; x; x &= x - 1) c++;
    return c;
}

static int windowBits(const gateDriftWindow *w) {
    int c = 0;
    size_t i;

    for (i = 0; i < COUNTOF(w->bits); i++) c += popcount64(w->bits[i]);
    return c;
}

TEST(drift_init) {
    gateDriftWindow w, z;

    CHECK_EQ_INT(GATE_DRIFT_WINDOW, 256);
    CHECK_EQ_INT(GATE_DRIFT_MIN, 100);
    CHECK_EQ_INT(sizeof(w.bits) * 8, GATE_DRIFT_WINDOW);

    memset(&w, 0xff, sizeof(w));
    gateDriftInit(&w);
    CHECK_EQ_INT(w.n, 0);
    CHECK_EQ_INT(w.pos, 0);
    CHECK_EQ_INT(w.errors, 0);
    CHECK_EQ_INT(w.total, 0);
    CHECK_EQ_INT(w.total_errors, 0);
    CHECK_EQ_INT(windowBits(&w), 0);
    /* Fully defined, padding included. */
    memset(&z, 0, sizeof(z));
    CHECK(memcmp(&w, &z, sizeof(w)) == 0);
}

TEST(drift_nothing_fires_below_min) {
    gateDriftWindow w;
    int i, fired = 0;

    /* 99 errors out of 99 is as bad as it gets and must stay silent, even
     * with a limit of zero. */
    gateDriftInit(&w);
    for (i = 0; i < GATE_DRIFT_MIN - 1; i++) fired += gateDriftPush(&w, 1, 0.0);
    CHECK_EQ_INT(fired, 0);
    CHECK_EQ_INT(w.n, 99);
    CHECK_EQ_INT(w.errors, 99);
    /* The 100th audit is the first one evaluated, whatever it is. */
    CHECK_EQ_INT(gateDriftPush(&w, 0, 0.1), 1);
    CHECK_EQ_INT(w.n, 100);
    CHECK_EQ_INT(w.errors, 99);

    gateDriftInit(&w);
    for (i = 0; i < GATE_DRIFT_MIN - 1; i++) fired += gateDriftPush(&w, 1, 0.1);
    CHECK_EQ_INT(fired, 0);
    CHECK_EQ_INT(gateDriftPush(&w, 1, 0.1), 1);

    /* A clean window never fires, at 100 or later. */
    gateDriftInit(&w);
    for (i = 0; i < 1000; i++) fired += gateDriftPush(&w, 0, 0.1);
    CHECK_EQ_INT(fired, 0);
    CHECK_EQ_INT(w.n, 256);
    CHECK_EQ_INT(w.errors, 0);
    /* Not even with a limit of exactly zero: 0 > 0 is false. */
    CHECK_EQ_INT(gateDriftPush(&w, 0, 0.0), 0);
    /* One error is above a zero limit. */
    CHECK_EQ_INT(gateDriftPush(&w, 1, 0.0), 1);
}

/* Push 'errors' errors followed by correct audits up to 'n' in total and
 * return what the n-th push returned. */
static int driftAfter(int errors, int n, double limit) {
    gateDriftWindow w;
    int i, alarm = 0;

    gateDriftInit(&w);
    for (i = 0; i < n; i++) alarm = gateDriftPush(&w, i < errors, limit);
    return alarm;
}

/* "errors > limit * n" is strict: a window sitting exactly on the limit is
 * within it. The limits 0.25 and 0.125 are exact in binary, so the products
 * are exact integers and the boundary is not a matter of rounding. */
TEST(drift_strict_boundary) {
    gateDriftWindow w;
    int i;

    /* n = 100 (the first evaluation), limit 0.25: 25 is allowed. */
    CHECK_EQ_INT(driftAfter(25, 100, 0.25), 0);
    CHECK_EQ_INT(driftAfter(26, 100, 0.25), 1);
    CHECK_EQ_INT(driftAfter(24, 100, 0.25), 0);
    /* Full window, limit 0.125: 32 of 256 is allowed. */
    CHECK_EQ_INT(driftAfter(32, 256, 0.125), 0);
    CHECK_EQ_INT(driftAfter(33, 256, 0.125), 1);
    /* The default limit for alpha = 0.05 is 2 * alpha = 0.1: 10 errors in
     * the first 100 audits are tolerated, the 11th is not. In a full window
     * the limit is 25.6 errors. */
    CHECK_EQ_INT(driftAfter(10, 100, 2 * 0.05), 0);
    CHECK_EQ_INT(driftAfter(11, 100, 2 * 0.05), 1);
    CHECK_EQ_INT(driftAfter(25, 256, 2 * 0.05), 0);
    CHECK_EQ_INT(driftAfter(26, 256, 2 * 0.05), 1);
    /* alpha = 0.02: 4 of 100, 10.24 of 256. alpha = 0.01: 2 of 100, 5.12
     * of 256. */
    CHECK_EQ_INT(driftAfter(4, 100, 2 * 0.02), 0);
    CHECK_EQ_INT(driftAfter(5, 100, 2 * 0.02), 1);
    CHECK_EQ_INT(driftAfter(10, 256, 2 * 0.02), 0);
    CHECK_EQ_INT(driftAfter(11, 256, 2 * 0.02), 1);
    CHECK_EQ_INT(driftAfter(2, 100, 2 * 0.01), 0);
    CHECK_EQ_INT(driftAfter(3, 100, 2 * 0.01), 1);
    CHECK_EQ_INT(driftAfter(5, 256, 2 * 0.01), 0);
    CHECK_EQ_INT(driftAfter(6, 256, 2 * 0.01), 1);

    /* While the window fills, n grows with every audit. 26 errors against a
     * limit of 0.25: alarm at n = 100..103 (26 > 25.75), silent from n =
     * 104, where 26 errors are exactly a quarter. */
    gateDriftInit(&w);
    for (i = 1; i <= 99; i++) CHECK_EQ_INT(gateDriftPush(&w, i <= 26, 0.25), 0);
    for (i = 100; i <= 103; i++) CHECK_EQ_INT(gateDriftPush(&w, 0, 0.25), 1);
    for (i = 104; i <= 110; i++) CHECK_EQ_INT(gateDriftPush(&w, 0, 0.25), 0);
    /* One more error (27 of 111 > 27.75? no) stays silent, two more at
     * n = 112 give 28 errors = 0.25 * 112: still silent; a third fires. */
    CHECK_EQ_INT(gateDriftPush(&w, 1, 0.25), 0);
    CHECK_EQ_INT(gateDriftPush(&w, 1, 0.25), 0);
    CHECK_EQ_INT(gateDriftPush(&w, 1, 0.25), 1);
    CHECK_EQ_INT(w.n, 113);
    CHECK_EQ_INT(w.errors, 29);
}

/* 60 errors, then correct audits. Once the ring is full each push
 * overwrites the oldest sample, so from audit 257 on one old error leaves
 * per push. With limit 0.1 the alarm needs more than 25.6 errors in the
 * window: it holds while 60 - k >= 26, i.e. up to audit 256 + 34 = 290. */
TEST(drift_wraparound_evicts_old_errors) {
    gateDriftWindow w;
    int i;

    gateDriftInit(&w);
    for (i = 1; i <= 300; i++) {
        int alarm = gateDriftPush(&w, i <= 60, 0.1);
        int expect_errors = i <= 60 ? i : (i <= 256 ? 60 : 60 - (i - 256));

        CHECK_EQ_INT(w.errors, expect_errors);
        CHECK_EQ_INT(w.n, i < 256 ? i : 256);
        CHECK_EQ_INT(w.pos, i % 256);
        CHECK_EQ_INT(alarm, i >= 100 && i <= 290);
        CHECK_EQ_INT(windowBits(&w), expect_errors);
    }
    CHECK_EQ_INT(w.errors, 16);
    CHECK_EQ_INT(w.pos, 44);
    /* The lifetime totals are not a window. */
    CHECK_EQ_INT(w.total, 300);
    CHECK_EQ_INT(w.total_errors, 60);

    for (i = 301; i <= 316; i++) gateDriftPush(&w, 0, 0.1);
    CHECK_EQ_INT(w.errors, 0);
    CHECK_EQ_INT(windowBits(&w), 0);

    /* A single error lives for exactly 256 pushes: it is still there after
     * 255 more audits and gone after the 256th. */
    gateDriftPush(&w, 1, 0.1);
    CHECK_EQ_INT(w.errors, 1);
    for (i = 0; i < 255; i++) gateDriftPush(&w, 0, 0.1);
    CHECK_EQ_INT(w.errors, 1);
    gateDriftPush(&w, 0, 0.1);
    CHECK_EQ_INT(w.errors, 0);
    CHECK_EQ_INT(w.total, 316 + 257);
    CHECK_EQ_INT(w.total_errors, 61);

    /* An error overwritten by an error leaves the count unchanged. */
    gateDriftInit(&w);
    for (i = 0; i < 1000; i++) gateDriftPush(&w, 1, 0.5);
    CHECK_EQ_INT(w.errors, 256);
    CHECK_EQ_INT(w.n, 256);
    CHECK_EQ_INT(windowBits(&w), 256);
    CHECK_EQ_INT(w.total_errors, 1000);
    CHECK_EQ_INT(gateDriftPush(&w, 1, 1.0), 0); /* 256 > 1.0 * 256 is false */
    CHECK_EQ_INT(gateDriftPush(&w, 1, 0.999), 1);
}

/* The ring against the definition: keep the whole history and recount the
 * last min(i, 256) audits from scratch after every push. The error rate
 * moves between calm and bad so that the alarm goes on and off. */
TEST(drift_matches_recount) {
    enum { N = 6000 };
    static unsigned char hist[N];
    const double limit = 0.1;
    gateDriftWindow w;
    uint64_t rng = 31337;
    uint64_t total_errors = 0;
    int i, k, mismatches = 0, alarms = 0, flips = 0, prev = 0;

    gateDriftInit(&w);
    for (i = 0; i < N; i++) {
        double p = (i / 500) % 3 == 1 ? 0.3 : 0.04;
        int e = gate_test_uniform(&rng) < p;
        int n = i + 1 < GATE_DRIFT_WINDOW ? i + 1 : GATE_DRIFT_WINDOW;
        int errors = 0, expected, alarm;

        hist[i] = (unsigned char)e;
        total_errors += (uint64_t)e;
        for (k = 0; k < n; k++) errors += hist[i - k];
        expected = n >= GATE_DRIFT_MIN && errors > limit * n;

        alarm = gateDriftPush(&w, e, limit);
        if (alarm != expected || w.n != (uint32_t)n || w.errors != (uint32_t)errors ||
            w.pos != (uint32_t)((i + 1) % GATE_DRIFT_WINDOW) || w.total != (uint64_t)(i + 1) ||
            w.total_errors != total_errors || windowBits(&w) != errors) mismatches++;
        alarms += alarm;
        if (alarm != prev) flips++;
        prev = alarm;
    }
    CHECK_EQ_INT(mismatches, 0);
    /* The scenario exercised both states, several times. */
    CHECK(alarms > 500 && alarms < N - 500);
    CHECK(flips >= 6);
}

TEST(drift_edge_inputs) {
    gateDriftWindow w;
    int i, fired = 0;

    /* Anything that is not 0 is one error. */
    gateDriftInit(&w);
    gateDriftPush(&w, 5, 0.1);
    gateDriftPush(&w, -1, 0.1);
    gateDriftPush(&w, 0, 0.1);
    CHECK_EQ_INT(w.errors, 2);
    CHECK_EQ_INT(w.total_errors, 2);
    CHECK_EQ_INT(windowBits(&w), 2);
    CHECK_EQ_INT(w.n, 3);

    /* A limit that is not a number cannot vouch for the window: the guard
     * fires (once it has its 100 audits) rather than switching itself off. */
    gateDriftInit(&w);
    for (i = 0; i < GATE_DRIFT_MIN - 1; i++) fired += gateDriftPush(&w, 0, NAN);
    CHECK_EQ_INT(fired, 0);
    CHECK_EQ_INT(gateDriftPush(&w, 0, NAN), 1);
    /* A negative limit is exceeded by a clean window, an infinite one by
     * nothing. */
    CHECK_EQ_INT(gateDriftPush(&w, 0, -0.5), 1);
    CHECK_EQ_INT(gateDriftPush(&w, 1, INFINITY), 0);
    CHECK_EQ_INT(gateDriftPush(&w, 1, 0.0), 1);

    /* The lifetime counters are 64 bit. */
    w.total = 0xffffffffull;
    w.total_errors = 0xffffffffull;
    gateDriftPush(&w, 1, 0.1);
    CHECK(w.total == 0x100000000ull);
    CHECK(w.total_errors == 0x100000000ull);
}

int main(void) {
    RUN(bet_fractions);
    RUN(bets_increments);
    RUN(region_init_and_counters);
    RUN(hand_computed_alpha_005);
    RUN(hand_computed_alpha_05);
    RUN(short_streams);
    RUN(error_factor_is_independent_of_alpha);
    RUN(order_independence);
    RUN(zero_error_certification_n);
    RUN(high_error_stream_never_certifies);
    RUN(log_wealth_extremes);
    RUN(non_finite_wealth_never_certifies);
    RUN(invalid_alpha_never_certifies);
    RUN(certifies_boundary);
    RUN(budget_generation_delta);
    RUN(budget_sum_never_exceeds_total);
    RUN(budget_log_threshold);
    RUN(budget_fails_closed);
    RUN(grid_uniform);
    RUN(grid_logit);
    RUN(grid_all_shapes);
    RUN(grid_invalid_arguments);
    RUN(grid_count);
    RUN(drift_init);
    RUN(drift_nothing_fires_below_min);
    RUN(drift_strict_boundary);
    RUN(drift_wraparound_evicts_old_errors);
    RUN(drift_matches_recount);
    RUN(drift_edge_inputs);
    return TEST_REPORT();
}
