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
 * budget and for the alarm times of the drift detector), taking every input
 * the C code receives as a double (alpha, the bet fractions, delta, the end
 * points of the grid) at its exact binary value, and are written with 20
 * significant digits. What is left between them and a correct
 * implementation is libm rounding only, which is why the tolerances can be
 * a few ulps. The handful of values a person can verify with pencil and
 * paper are derived in the comments next to them.
 *
 * The expected values of the gateRegionFlip() tests come from a model of a
 * region that is two integers and one rule (an update adds a sample and
 * possibly an error; a flip takes one error back if there is one), from the
 * closed form of the log wealths for those two counts in 80 digit decimals,
 * and, for the numbers of certified paths, from exact rational arithmetic
 * (the mixture wealth and the threshold are both rational numbers, so no
 * logarithm is involved). The same scripts reproduce the older constants
 * of the e-process tests they were run against, to their 20 digits: the
 * increments, the hand computed, short and 1000 sample streams, every
 * count of zero_error_certification_n and the five probabilities of
 * exhaustive_certification_probability. What "up to rounding" means for a
 * flip is derived, not tuned: see the comment above flipSlack().
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
 * For comparison, a binomial test that looks once, after a fixed number of
 * samples, needs 90 / 228 / 459 of them at delta = 0.01: the anytime-valid
 * mixture needs 128 / 325 / 653. That is the price of being allowed to look
 * after every sample. */
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
     * 'nregions' regions. Every threshold of every tested row is a region
     * (gateBar()): 32 is the first snapshot of a score gate, 64 that of a
     * classifier with two labels, 2048 that of one with 64 labels, and
     * (2, 32) the second snapshot of a score gate. 8 is no gate's bar. */
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
        {0.01, 0.01, 5, 64, 1498}, {0.01, 0.05, 3, 256, 1372},
        {0.05, 0.01, 1, 8, 189},  {0.02, 0.01, 1, 8, 481},
        {0.01, 0.01, 1, 8, 966},
        {0.05, 0.01, 1, 64, 235},  {0.02, 0.01, 1, 64, 596},
        {0.01, 0.01, 1, 64, 1198},
        {0.05, 0.01, 2, 32, 243},  {0.02, 0.01, 2, 32, 618},
        {0.01, 0.01, 2, 32, 1243},
        {0.05, 0.01, 1, 2048, 310}, {0.02, 0.01, 1, 2048, 787},
        {0.01, 0.01, 1, 2048, 1581}
    };
    size_t i, total = COUNTOF(direct) + COUNTOF(budget);
    int late;

    /* Every case twice: with correct samples, and with samples that are
     * first counted as errors and then set right by gateRegionFlip(), which
     * is what the gate does with an answer that is late and right. Once the
     * answer is in it must have cost nothing: the same n, to the sample. */
    for (i = 0; i < 2 * total; i++) {
        double alpha, thr, lw = 0.0, prev = 0.0;
        int expected, n = 0, monotone = 1;
        gateBets b;
        gateRegion r;

        late = i >= total;
        if (i % total < COUNTOF(direct)) {
            alpha = direct[i % total].alpha;
            thr = -log(direct[i % total].delta_region);
            expected = direct[i % total].n;
        } else {
            size_t k = i % total - COUNTOF(direct);

            alpha = budget[k].alpha;
            thr = gateBudgetLogThreshold(budget[k].delta_total, budget[k].g, budget[k].nregions);
            expected = budget[k].n;
        }
        gateBetsInit(&b, alpha);
        gateRegionInit(&r);
        CHECK(!gateRegionCertifies(&r, thr)); /* never with zero samples */
        while (!gateRegionCertifies(&r, thr) && n < expected + 1000) {
            if (late) {
                gateRegionUpdate(&r, &b, 1);
                gateRegionFlip(&r, &b);
                lw = gateRegionLogWealth(&r);
            } else {
                lw = gateRegionUpdate(&r, &b, 0);
            }
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

/* What makes the test valid. A bet is fair exactly at the boundary of the
 * null hypothesis: with error rate p the expected factor of one sample is
 *
 *     (1 - p) (1 + lambda alpha) + p (1 - c) = 1 + lambda (alpha - p)
 *
 * which is 1 at p = alpha, below 1 for any p > alpha (the wealth of a bad
 * region is a supermartingale: Ville's inequality applies) and above 1 for
 * p < alpha (a good region grows, i.e. the test has power). */
TEST(bets_are_fair_at_the_boundary) {
    static const double alphas[] = {1e-6, 0.001, 0.01, 0.02, 0.05, 0.1, 0.3, 0.5, 0.9, 0.999};
    size_t i;
    int j;

    for (i = 0; i < COUNTOF(alphas); i++) {
        double alpha = alphas[i];
        gateBets b;

        gateBetsInit(&b, alpha);
        for (j = 0; j < GATE_NBETS; j++) {
            double ok = exp(b.inc_ok[j]), err = exp(b.inc_err[j]);
            double worse = alpha + 0.5 * (1.0 - alpha), better = 0.5 * alpha;

            CHECK_NEAR((1.0 - alpha) * ok + alpha * err, 1.0, 4.0 * DBL_EPSILON);
            CHECK((1.0 - worse) * ok + worse * err < 1.0);
            CHECK((1.0 - better) * ok + better * err > 1.0);
            /* The slope in p is -lambda = -c / (1 - alpha). The gain is a
             * difference of two numbers near 1 (or near lambda when alpha is
             * close to 1), hence a tolerance relative to 1 + gain. */
            {
                double gain = gateBetFractions[j] / (1.0 - alpha) * (alpha - better);

                CHECK_NEAR(((1.0 - better) * ok + better * err) - 1.0, gain, 1e-13 * (1.0 + gain));
            }
        }
    }
}

/* Expected mixture wealth after 'left' more samples of an IID stream with
 * error rate p, by following every one of the 2^left paths. */
static double expectedWealth(const gateRegion *r, const gateBets *b, double p, int left) {
    gateRegion ok = *r, err = *r;

    if (left == 0) return exp(gateRegionLogWealth(r));
    gateRegionUpdate(&ok, b, 0);
    gateRegionUpdate(&err, b, 1);
    return (1.0 - p) * expectedWealth(&ok, b, p, left - 1) + p * expectedWealth(&err, b, p, left - 1);
}

/* The same property for the quantity that is actually compared with the
 * threshold: over all 4096 streams of 12 samples the mean of the mixture
 * wealth is (1/4) sum_j (1 + lambda_j (alpha - p))^12, i.e. exactly 1 at
 * p = alpha whatever the bets are, and less for a bad region. */
TEST(mixture_wealth_is_a_martingale_at_the_boundary) {
    static const double alphas[] = {0.02, 0.05, 0.3};
    static const double mults[] = {1.0, 1.5, 3.0, 0.5, 0.0};
    const int n = 12;
    size_t i, m;
    int j;

    for (i = 0; i < COUNTOF(alphas); i++) {
        double alpha = alphas[i];
        gateBets b;
        gateRegion r;

        gateBetsInit(&b, alpha);
        gateRegionInit(&r);
        for (m = 0; m < COUNTOF(mults); m++) {
            double p = alpha * mults[m], closed = 0.0, mean = expectedWealth(&r, &b, p, n);

            for (j = 0; j < GATE_NBETS; j++)
                closed += 0.25 * pow(1.0 + gateBetFractions[j] / (1.0 - alpha) * (alpha - p), n);
            CHECK_REL(mean, closed, 1e-12);
            if (mults[m] == 1.0) CHECK_NEAR(mean, 1.0, 1e-13);
            if (mults[m] > 1.0) CHECK(mean < 1.0);
            if (mults[m] < 1.0) CHECK(mean > 1.0);
        }
    }
}

/* Probability that an IID stream with error rate p is certified by one of
 * its first 'left' samples, by following every path until it certifies.
 * '*paths' counts the full length paths that got certified. */
static double certifiedProbability(const gateRegion *r, const gateBets *b, double thr,
                                   double p, int left, long *paths) {
    double total = 0.0;
    int e;

    if (left == 0) return 0.0;
    for (e = 0; e <= 1; e++) {
        gateRegion next = *r;
        double weight = e ? p : 1.0 - p;

        gateRegionUpdate(&next, b, e);
        if (gateRegionCertifies(&next, thr)) {
            total += weight;
            *paths += 1L << (left - 1);
        } else {
            total += weight * certifiedProbability(&next, b, thr, p, left - 1, paths);
        }
    }
    return total;
}

/* The whole procedure (update, mixture, threshold, first passage) against
 * exact probabilities. The reference comes from a recursion over (samples,
 * errors) in high precision; here every path is walked through the library.
 * In all the cases the closest any reachable state gets to the threshold is
 * 0.03, so no path depends on rounding.
 *
 * At p = alpha the result is the false certification probability of a
 * boundary region within 16 samples: it must be below delta. */
TEST(exhaustive_certification_probability) {
    static const struct {
        double alpha, delta, p;
        int n;
        double expected;
    } cases[] = {
        {0.3, 0.25, 0.30, 16, 1.0270099218547001259e-1},  /* H0 boundary: <= 0.25 */
        {0.3, 0.25, 0.45, 16, 1.6551824795573516210e-2},  /* bad region */
        {0.3, 0.25, 0.10, 16, 6.5622750798302997496e-1},  /* good region: power */
        {0.5, 0.10, 0.50, 16, 2.6245117187500000000e-2},  /* H0 boundary: <= 0.1 */
        {0.5, 0.10, 0.25, 16, 3.9355368912220001221e-1}
    };
    gateBets b;
    gateRegion r;
    size_t i;
    long paths;
    double pr;

    for (i = 0; i < COUNTOF(cases); i++) {
        paths = 0;
        gateBetsInit(&b, cases[i].alpha);
        gateRegionInit(&r);
        pr = certifiedProbability(&r, &b, -log(cases[i].delta), cases[i].p, cases[i].n, &paths);
        CHECK_NEAR(pr, cases[i].expected, 1e-13);
        if (cases[i].p >= cases[i].alpha) CHECK(pr <= cases[i].delta);
    }

    /* alpha = 0.5 and p = 0.5 make the 65536 paths equally likely: exactly
     * 1720 of them are certified, and 1720 / 65536 has no rounding. */
    paths = 0;
    gateBetsInit(&b, 0.5);
    gateRegionInit(&r);
    pr = certifiedProbability(&r, &b, -log(0.1), 0.5, 16, &paths);
    CHECK_EQ_INT(paths, 1720);
    CHECK(pr == 1720.0 / 65536.0);

    /* No path can beat the error free one. With alpha = 0.1 and delta = 0.2
     * it needs 27 samples, so nothing is certified within 12, whatever the
     * error rate. */
    paths = 0;
    gateBetsInit(&b, 0.1);
    gateRegionInit(&r);
    CHECK(certifiedProbability(&r, &b, -log(0.2), 0.0, 12, &paths) == 0.0);
    CHECK(certifiedProbability(&r, &b, -log(0.2), 0.05, 12, &paths) == 0.0);
    CHECK_EQ_INT(paths, 0);
    {
        int n = 0;

        while (!gateRegionCertifies(&r, -log(0.2)) && n < 1000) {
            gateRegionUpdate(&r, &b, 0);
            n++;
        }
        CHECK_EQ_INT(n, 27);
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
        /* Only the 0.9 bet is left in the mixture: its wealth / 4. */
        CHECK_REL(lw, (1e12 + 1.0) * ref_inc_ok[0].inc_ok[3] - log(4.0), 1e-13);
        CHECK(gateRegionCertifies(&r, 100.0));

        for (j = 0; j < GATE_NBETS; j++) r.logw[j] = 1e12 * b.inc_err[j];
        lw = gateRegionUpdate(&r, &b, 1);
        CHECK(isfinite(lw));
        /* And here only the 0.1 bet, which loses the least. */
        CHECK_REL(lw, (1e12 + 1.0) * ref_inc_err[0] - log(4.0), 1e-13);
        CHECK(!gateRegionCertifies(&r, 0.0));
    }
}

/* Corrupted evidence must never be read as evidence. No sequence of updates
 * can produce a NaN or an infinite log wealth, so one in any slot means the
 * state is not to be trusted: the mixture is reported as NaN and nothing
 * certifies. The other three bets are far above the threshold in every
 * case, so a wrong answer here would be a certification. */
TEST(non_finite_wealth_never_certifies) {
    static const double thresholds[] = {-DBL_MAX, -1.0, 0.0, 1.0, 4.6};
    double poison[3];
    gateBets b;
    gateRegion r;
    size_t k, v;
    int j, i;

    poison[0] = NAN;
    poison[1] = INFINITY;
    poison[2] = -INFINITY;
    gateBetsInit(&b, 0.05);
    for (v = 0; v < COUNTOF(poison); v++) {
        /* One bad slot, in each of the four positions. */
        for (j = 0; j < GATE_NBETS; j++) {
            gateRegionInit(&r);
            for (i = 0; i < GATE_NBETS; i++) r.logw[i] = 100.0;
            CHECK(gateRegionCertifies(&r, 4.6)); /* healthy: it would */
            r.logw[j] = poison[v];
            CHECK(isnan(gateRegionLogWealth(&r)));
            for (k = 0; k < COUNTOF(thresholds); k++) CHECK(!gateRegionCertifies(&r, thresholds[k]));
            CHECK(!gateRegionCertifies(&r, -INFINITY));
            /* It does not heal: more good samples change nothing. */
            for (i = 0; i < 500; i++) gateRegionUpdate(&r, &b, 0);
            CHECK(isnan(gateRegionLogWealth(&r)));
            CHECK(!gateRegionCertifies(&r, 1.0));
            CHECK(!gateRegionCertifies(&r, -DBL_MAX));
        }
        /* All four slots. For -inf this is "total wealth zero", which the
         * arithmetic alone would call -inf. */
        for (i = 0; i < GATE_NBETS; i++) r.logw[i] = poison[v];
        CHECK(isnan(gateRegionLogWealth(&r)));
        for (k = 0; k < COUNTOF(thresholds); k++) CHECK(!gateRegionCertifies(&r, thresholds[k]));
        CHECK(!gateRegionCertifies(&r, -INFINITY));
        /* Two different kinds in the same region. */
        for (i = 0; i < GATE_NBETS; i++) r.logw[i] = 100.0;
        r.logw[1] = poison[v];
        r.logw[2] = poison[(v + 1) % COUNTOF(poison)];
        CHECK(isnan(gateRegionLogWealth(&r)));
        CHECK(!gateRegionCertifies(&r, -DBL_MAX));
    }

    /* The largest and smallest finite values are still numbers. */
    for (i = 0; i < GATE_NBETS; i++) r.logw[i] = DBL_MAX;
    CHECK(gateRegionLogWealth(&r) == DBL_MAX);
    CHECK(gateRegionCertifies(&r, 1e300));
    for (i = 0; i < GATE_NBETS; i++) r.logw[i] = -DBL_MAX;
    CHECK(gateRegionLogWealth(&r) == -DBL_MAX);
    CHECK(!gateRegionCertifies(&r, 0.0));
    CHECK(gateRegionCertifies(&r, -DBL_MAX)); /* finite and equal */

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
        /* The mechanism: every increment is NaN, so is every wealth. */
        for (k = 0; k < GATE_NBETS; k++) CHECK(isnan(b.inc_ok[k]) && isnan(b.inc_err[k]));
        gateRegionInit(&r);
        for (k = 0; k < 5000; k++) {
            gateRegionUpdate(&r, &b, k % 50 == 49);
            certified += gateRegionCertifies(&r, -DBL_MAX);
            certified += gateRegionCertifies(&r, 0.5);
        }
        CHECK_EQ_INT(certified, 0);
        CHECK_EQ_INT(r.n, 5000);
        CHECK_EQ_INT(r.errors, 100);
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

/* gateRegionCertifies() is defined by the mixture wealth: 1 iff it is finite
 * and >= the threshold. Random evidence, with the thresholds placed where
 * a shortcut could go wrong: on the mixture itself, on the best single bet
 * (the mixture can be up to log(4) below it) and in between. */
TEST(certifies_agrees_with_log_wealth) {
    uint64_t rng = 0xce27;
    gateRegion r;
    int i, j, k, mismatches = 0, yes = 0, no = 0;

    gateRegionInit(&r);
    for (i = 0; i < 20000; i++) {
        double spread = i % 4 == 0 ? 0.0 : (i % 4 == 1 ? 1e-9 : (i % 4 == 2 ? 2.0 : 40.0));
        double base = (gate_test_uniform(&rng) - 0.5) * 60.0, max = -INFINITY, lw, thr[12];

        for (j = 0; j < GATE_NBETS; j++) {
            r.logw[j] = base + spread * gate_test_uniform(&rng);
            if (r.logw[j] > max) max = r.logw[j];
        }
        lw = gateRegionLogWealth(&r);
        thr[0] = lw;
        thr[1] = nextafter(lw, INFINITY);
        thr[2] = nextafter(lw, -INFINITY);
        thr[3] = max;
        thr[4] = nextafter(max, INFINITY);
        thr[5] = nextafter(max, -INFINITY);
        thr[6] = max - log(4.0);
        thr[7] = 0.5 * (lw + max);
        thr[8] = lw + (max - lw) * gate_test_uniform(&rng);
        thr[9] = lw - 1e-12;
        thr[10] = lw + 1e-12;
        thr[11] = base + 50.0 * (gate_test_uniform(&rng) - 0.5);
        for (k = 0; k < 12; k++) {
            int expected = isfinite(lw) && lw >= thr[k];

            if (gateRegionCertifies(&r, thr[k]) != expected) mismatches++;
            if (expected) yes++; else no++;
        }
        /* The bounds the shortcut rests on. */
        if (!(lw <= max && lw >= max - log(4.0) - 1e-12)) mismatches++;
    }
    CHECK_EQ_INT(mismatches, 0);
    CHECK(yes > 20000 && no > 20000);   /* both answers well represented */
}

/* ------------------ Taking an error back: gateRegionFlip ----------------- */

/* The gate counts a request whose answer is late as an error and takes the
 * error back when the answer arrives and the guess turns out to have been
 * right (gate_core.h, "WHICH REQUESTS ARE PROOF" and "MISSING"). The
 * contract of gateRegionFlip(): 'n' unchanged, one error less, and the
 * wealth of a region in which that sample was correct from the start, up to
 * rounding; nothing at all if the region holds no error.
 *
 * WHAT "UP TO ROUNDING" IS. Let u = 2^-53 (DBL_EPSILON / 2): a floating
 * point addition is off by at most u times its result. With a = inc_ok[j]
 * and e = inc_err[j] a flip adds d = fl(a - e) to a log wealth. So where
 * "correct" computes fl(L + a) from a log wealth L, "error, then flip"
 * computes fl(fl(L + e) + d). Four roundings separate the two:
 *
 *     u |L + e|  +  u (a - e)  +  u |L + a|  +  u |L + a|
 *                                        <=  u (3 |L| + 3 a + 2 |e|)
 *
 * plus terms in u^2. In a fresh region L = 0, two of the four additions are
 * exact and u (2 a + |e|) is left: under three ulps of the larger of the
 * two increments. Mind that this is an absolute error of the size of an ulp
 * of |e| whatever 'a' is: at alpha = 1e-9 the log wealth 1e-10 of a correct
 * sample comes back from a flip right to about 1e-7 of itself. That is
 * harmless (thresholds are a few units) and it is why nothing below
 * compares a flipped wealth with a relative tolerance.
 *
 * Over a stream every addition contributes u times the log wealth it
 * produced, and every flip u (a - e) more for the rounding of d. After
 * 'ops' additions whose results never exceeded 'peak' in magnitude, 'flips'
 * of them made by flips, a log wealth is within
 *
 *     u (ops * peak + flips * (a - e))
 *
 * of the exact sum of the increments it stands for. flipSlack() and
 * trackedSlack() are these two bounds with 1% on top for the u^2 terms and
 * for their own evaluation. They are derived, not tuned: no tolerance of
 * the flip tests was widened to make a test pass. */
#define U_ROUND (0.5 * DBL_EPSILON)

static double flipSlack(double before, double ok, double err) {
    return 1.01 * U_ROUND * (3.0 * fabs(before) + 3.0 * ok + 2.0 * fabs(err));
}

/* gateRegionLogWealth() adds a rounding of its own: the four differences
 * logw[j] - max, four exp(), three additions, one log() and the final
 * addition. With a libm that is within one ulp that is at most
 * 24 u + u |result|; 32 is allowed. The log of a mean of exponentials moves
 * by no more than its arguments do, so two regions whose log wealths agree
 * within 'slack' report mixtures within slack plus one mixSlack() each. */
static double mixSlack(double lw) {
    return U_ROUND * (32.0 + fabs(lw));
}

/* A region together with what bounds the rounding of its log wealths. */
typedef struct tracked {
    gateRegion r;
    double peak;        /* largest magnitude any of its log wealths has had */
    uint64_t ops;       /* additions made to each log wealth */
    uint64_t flips;     /* ... of which by a flip */
} tracked;

static void trackedInit(tracked *t) {
    gateRegionInit(&t->r);
    t->peak = 0.0;
    t->ops = t->flips = 0;
}

static void trackedPeak(tracked *t) {
    int j;

    for (j = 0; j < GATE_NBETS; j++) {
        if (fabs(t->r.logw[j]) > t->peak) t->peak = fabs(t->r.logw[j]);
    }
}

static double trackedUpdate(tracked *t, const gateBets *b, int error) {
    double lw = gateRegionUpdate(&t->r, b, error);

    t->ops++;
    trackedPeak(t);
    return lw;
}

/* The whole model of a flip: it acts if and only if there is an error. */
static void trackedFlip(tracked *t, const gateBets *b) {
    if (t->r.errors > 0) {
        t->ops++;
        t->flips++;
    }
    gateRegionFlip(&t->r, b);
    trackedPeak(t);
}

static double trackedSlack(const tracked *t, const gateBets *b) {
    /* a - e is largest for the 0.9 bet: one bound for the four bets. */
    double d = b->inc_ok[GATE_NBETS - 1] - b->inc_err[GATE_NBETS - 1];

    return 1.01 * U_ROUND * ((double)t->ops * t->peak + (double)t->flips * d);
}

/* What separates the exact sum of the library's increments from a closed
 * form in the reference increments: the library's are within 1e-14
 * (relative) of the reference ones, which bets_increments checks, and a
 * closed form evaluated in doubles adds a few u. */
static double refSlack(uint64_t ops, const gateBets *b) {
    return 1.5e-14 * (double)ops * (b->inc_ok[GATE_NBETS - 1] - b->inc_err[GATE_NBETS - 1]);
}

/* 1 if two regions that hold the same samples, accounted by different
 * routes, agree: same counters, log wealths and mixture within the rounding
 * of the two routes. */
static int trackedAgree(const tracked *x, const tracked *y, const gateBets *b) {
    double slack = trackedSlack(x, b) + trackedSlack(y, b), lx, ly;
    int j;

    if (x->r.n != y->r.n || x->r.errors != y->r.errors) return 0;
    for (j = 0; j < GATE_NBETS; j++) {
        if (!(fabs(x->r.logw[j] - y->r.logw[j]) <= slack)) return 0;
    }
    lx = gateRegionLogWealth(&x->r);
    ly = gateRegionLogWealth(&y->r);
    return fabs(lx - ly) <= slack + mixSlack(lx) + mixSlack(ly);
}

/* The log wealths that the counts (n, errors) stand for, in the reference
 * increments of risk target ref_inc_ok[a]. */
static void countsLogw(size_t a, uint64_t n, uint64_t errors, double *logw) {
    int j;

    for (j = 0; j < GATE_NBETS; j++)
        logw[j] = (double)(n - errors) * ref_inc_ok[a].inc_ok[j] + (double)errors * ref_inc_err[j];
}

/* log((1/4) sum_j exp(logw[j])) the plain way: good enough for the handful
 * of samples of the tests that use it. */
static double plainLogMixture(const double *logw) {
    double sum = 0.0;
    int j;

    for (j = 0; j < GATE_NBETS; j++) sum += exp(logw[j]);
    return log(0.25 * sum);
}

/* Play a string of samples: 'C' correct, 'E' error, 'F' flip. */
static void play(gateRegion *r, const gateBets *b, const char *ops) {
    for (; *ops; ops++) {
        if (*ops == 'F') gateRegionFlip(r, b); else gateRegionUpdate(r, b, *ops == 'E');
    }
}

/* "Error, then flip" against "correct": the contract in its simplest form,
 * for every risk target of the reference table and then from any state. */
TEST(flip_error_then_flip_is_correct) {
    uint64_t rng = 0xf11b;
    size_t i;
    int j, k, trial, beyond = 0, counters = 0;

    for (i = 0; i < COUNTOF(ref_inc_ok); i++) {
        gateBets b;
        gateRegion flipped, correct;
        double worst = 0.0, lw;

        gateBetsInit(&b, ref_inc_ok[i].alpha);
        gateRegionInit(&flipped);
        gateRegionInit(&correct);
        gateRegionUpdate(&flipped, &b, 1);
        gateRegionFlip(&flipped, &b);
        lw = gateRegionUpdate(&correct, &b, 0);

        CHECK_EQ_INT(flipped.n, 1);
        CHECK_EQ_INT(flipped.errors, 0);
        CHECK_EQ_INT(flipped.certified_at, 0);
        for (j = 0; j < GATE_NBETS; j++) {
            /* A fresh region: u (2 a + |e|). */
            double slack = 1.01 * U_ROUND * (2.0 * b.inc_ok[j] + fabs(b.inc_err[j]));

            CHECK_NEAR(flipped.logw[j], correct.logw[j], slack);
            /* The reference value itself; the library's inc_ok is within
             * 1e-14 of it. */
            CHECK_NEAR(flipped.logw[j], ref_inc_ok[i].inc_ok[j], slack + 1e-14 * ref_inc_ok[i].inc_ok[j]);
            /* Still a gain, like any correct sample: even where the gain
             * (1e-10 at alpha = 1e-9) is a billion times smaller than the
             * error that was taken back. */
            CHECK(flipped.logw[j] > 0.0);
            if (slack > worst) worst = slack;
        }
        CHECK_NEAR(gateRegionLogWealth(&flipped), lw, worst + 2.0 * mixSlack(lw));
    }

    /* From any state: u (3 |L| + 3 a + 2 |e|) for the log wealth that was L
     * before the sample. */
    for (trial = 0; trial < 3000; trial++) {
        int len = (int)(gate_test_rand64(&rng) % 300);
        double p = 0.5 * gate_test_uniform(&rng), worst = 0.0, lw;
        gateBets b;
        gateRegion start, flipped, correct;

        gateBetsInit(&b, ref_inc_ok[trial % COUNTOF(ref_inc_ok)].alpha);
        gateRegionInit(&start);
        for (k = 0; k < len; k++) gateRegionUpdate(&start, &b, gate_test_uniform(&rng) < p);
        flipped = correct = start;
        gateRegionUpdate(&flipped, &b, 1);
        gateRegionFlip(&flipped, &b);
        lw = gateRegionUpdate(&correct, &b, 0);

        if (flipped.n != correct.n || flipped.errors != correct.errors || flipped.certified_at != 0)
            counters++;
        for (j = 0; j < GATE_NBETS; j++) {
            double slack = flipSlack(start.logw[j], b.inc_ok[j], b.inc_err[j]);

            if (!(fabs(flipped.logw[j] - correct.logw[j]) <= slack)) beyond++;
            if (slack > worst) worst = slack;
        }
        if (!(fabs(gateRegionLogWealth(&flipped) - lw) <= worst + 2.0 * mixSlack(lw))) beyond++;
    }
    CHECK_EQ_INT(beyond, 0);
    CHECK_EQ_INT(counters, 0);
}

TEST(flip_counters) {
    const double thr = gateBudgetLogThreshold(0.01, 1, 32);
    gateBets b;
    gateRegion r, before;
    int i, j;

    /* 3 errors, then 40 correct samples. Three flips take the errors back
     * one at a time: 'n' stays, every bet gains log(1 + c_j alpha / (1 -
     * alpha)) - log(1 - c_j), and so does the mixture. */
    gateBetsInit(&b, 0.05);
    gateRegionInit(&r);
    feed(&r, &b, 40, 3, 1);
    for (i = 3; i > 0; i--) {
        before = r;
        gateRegionFlip(&r, &b);
        CHECK_EQ_INT(r.n, 43);
        CHECK_EQ_INT(r.errors, i - 1);
        CHECK_EQ_INT(r.certified_at, 0);
        for (j = 0; j < GATE_NBETS; j++) {
            CHECK(r.logw[j] > before.logw[j]);
            /* Log wealths below 8 in magnitude (3e-15 of rounding) and an
             * increment within 1e-14 of the reference (3e-14). */
            CHECK_NEAR(r.logw[j] - before.logw[j], ref_inc_ok[0].inc_ok[j] - ref_inc_err[j], 1e-13);
        }
        CHECK(gateRegionLogWealth(&r) > gateRegionLogWealth(&before));
    }
    /* All three taken back: 43 correct samples (46 additions below 8 in
     * magnitude, increments within 1e-14: 2e-13). */
    for (j = 0; j < GATE_NBETS; j++) CHECK_NEAR(r.logw[j], 43.0 * ref_inc_ok[0].inc_ok[j], 1e-12);

    /* Nothing is left to take back. */
    before = r;
    gateRegionFlip(&r, &b);
    CHECK(memcmp(&r, &before, sizeof(r)) == 0);

    /* A flip never latches a certification, not even the flip that takes
     * the region over the threshold: latching is the caller's decision.
     * 219 correct samples and one error are far below the bar of a first
     * score gate snapshot, the same 220 samples without the error are
     * above it (zero_error_certification_n). */
    gateRegionInit(&r);
    feed(&r, &b, 219, 1, 1);
    CHECK(!gateRegionCertifies(&r, thr));
    gateRegionFlip(&r, &b);
    CHECK(gateRegionCertifies(&r, thr));
    CHECK_EQ_INT(r.certified_at, 0);
    CHECK_EQ_INT(r.n, 220);
    CHECK_EQ_INT(r.errors, 0);

    /* ... and it never clears or moves one. */
    gateRegionInit(&r);
    feed(&r, &b, 10, 2, 0);
    r.certified_at = 7;
    gateRegionFlip(&r, &b);
    CHECK_EQ_INT(r.certified_at, 7);
    gateRegionFlip(&r, &b);
    gateRegionFlip(&r, &b); /* no error left */
    CHECK_EQ_INT(r.certified_at, 7);
    CHECK_EQ_INT(r.n, 12);
    CHECK_EQ_INT(r.errors, 0);

    /* The counters are 64 bit: across 2^32, and well above it. */
    r.n = 0x100000000ull;
    r.errors = 0x100000000ull;
    gateRegionFlip(&r, &b);
    CHECK(r.n == 0x100000000ull);
    CHECK(r.errors == 0xffffffffull);
    r.n = 0x300000000ull;
    r.errors = 0x200000001ull;
    gateRegionFlip(&r, &b);
    CHECK(r.n == 0x300000000ull);
    CHECK(r.errors == 0x200000000ull);
    gateRegionFlip(&r, &b);     /* the low 32 bits are zero: still an error to take back */
    CHECK(r.errors == 0x1ffffffffull);
}

/* "Does nothing if the region holds no error": not one bit changes,
 * whatever the bets are. A flip with nothing to take back must never be a
 * way to gain wealth. */
TEST(flip_without_an_error_does_nothing) {
    static const double alphas[] = {0.05, 0.5, 1e-9, 0.999};
    gateBets b, poisoned;
    gateRegion r, before;
    size_t i;
    int k;

    gateBetsInit(&poisoned, 2.0);   /* not a risk target: NaN increments */
    CHECK(isnan(poisoned.inc_ok[0]) && isnan(poisoned.inc_err[0]));
    for (i = 0; i < COUNTOF(alphas); i++) {
        gateBetsInit(&b, alphas[i]);

        /* A fresh region. */
        gateRegionInit(&r);
        before = r;
        gateRegionFlip(&r, &b);
        gateRegionFlip(&r, &poisoned);
        CHECK(memcmp(&r, &before, sizeof(r)) == 0);
        CHECK(gateRegionLogWealth(&r) == 0.0);

        /* Correct samples only. */
        for (k = 0; k < 100; k++) gateRegionUpdate(&r, &b, 0);
        r.certified_at = 55;
        before = r;
        for (k = 0; k < 5; k++) gateRegionFlip(&r, &b);
        gateRegionFlip(&r, &poisoned);
        CHECK(memcmp(&r, &before, sizeof(r)) == 0);

        /* Errors that were all taken back already. */
        gateRegionUpdate(&r, &b, 1);
        gateRegionUpdate(&r, &b, 1);
        gateRegionFlip(&r, &b);
        gateRegionFlip(&r, &b);
        CHECK_EQ_INT(r.errors, 0);
        CHECK_EQ_INT(r.n, 102);
        before = r;
        gateRegionFlip(&r, &b);
        gateRegionFlip(&r, &poisoned);
        CHECK(memcmp(&r, &before, sizeof(r)) == 0);
    }
}

/* alpha = 0.5 once more: a correct sample multiplies wealth j by 1 + c_j,
 * an error by 1 - c_j. "Error, error, correct" and one flip leave one error
 * and two correct samples, (1 + c_j)^2 (1 - c_j):
 *
 *     1.1^2 * 0.9 = 1.089      1.25^2 * 0.75 = 1.171875
 *     1.5^2 * 0.5 = 1.125      1.9^2 * 0.1   = 0.361
 *     mean = 3.746875 / 4 = 0.93671875
 *
 * and a second flip leaves three correct samples, (1 + c_j)^3:
 *
 *     1.331   1.953125   3.375   6.859     mean = 13.518125 / 4 = 3.37953125
 */
TEST(flip_hand_computed) {
    static const double wealth1[GATE_NBETS] = {1.089, 1.171875, 1.125, 0.361};
    static const double logw1[GATE_NBETS] = {
        8.5259843950823422785e-2, 1.5860503017663858409e-1,
        1.1778303565638345454e-1, -1.0188773206492563307e+0
    };
    static const double wealth0[GATE_NBETS] = {1.331, 1.953125, 3.375, 6.859};
    static const double logw0[GATE_NBETS] = {
        2.8593053941297459527e-1, 6.6943065394262926730e-1,
        1.2163953243244931459e+0, 1.9255616585171843630e+0
    };
    /* 3 correct samples and 1 error at alpha = 0.05: the numbers of
     * hand_computed_alpha_005, reached here through flips, one of which
     * (in the third and fourth string) has nothing to take back. */
    static const double logw_005[GATE_NBETS] = {
        -8.9612447999395119137e-2, -2.4846582774972261419e-1,
        -6.1522072135016333085e-1, -2.1637428353020271406e+0
    };
    static const char *routes[] = {"ECECF", "EEEFCF", "FCCEC", "EFFEFEFE", "CCEEF", "EFEFEFE"};
    gateBets b;
    gateRegion r;
    double lw;
    size_t i;
    int j;

    gateBetsInit(&b, 0.5);
    gateRegionInit(&r);
    play(&r, &b, "EECF");
    lw = gateRegionLogWealth(&r);
    for (j = 0; j < GATE_NBETS; j++) {
        CHECK_NEAR(exp(r.logw[j]), wealth1[j], 1e-14);
        CHECK_NEAR(r.logw[j], logw1[j], 1e-12);
    }
    CHECK_NEAR(exp(lw), 0.93671875, 1e-14);
    CHECK_NEAR(lw, -6.5372201886148623500e-2, 1e-12);
    CHECK_EQ_INT(r.n, 3);
    CHECK_EQ_INT(r.errors, 1);

    gateRegionFlip(&r, &b);
    lw = gateRegionLogWealth(&r);
    for (j = 0; j < GATE_NBETS; j++) {
        CHECK_NEAR(exp(r.logw[j]), wealth0[j], 1e-13);
        CHECK_NEAR(r.logw[j], logw0[j], 1e-12);
    }
    CHECK_NEAR(exp(lw), 3.37953125, 1e-13);
    CHECK_NEAR(lw, 1.2177370164455384246e+0, 1e-12);
    CHECK_EQ_INT(r.n, 3);
    CHECK_EQ_INT(r.errors, 0);

    gateBetsInit(&b, 0.05);
    for (i = 0; i < COUNTOF(routes); i++) {
        gateRegionInit(&r);
        play(&r, &b, routes[i]);
        lw = gateRegionLogWealth(&r);
        for (j = 0; j < GATE_NBETS; j++) CHECK_NEAR(r.logw[j], logw_005[j], 1e-12);
        CHECK_NEAR(lw, -5.3200762558875099968e-1, 1e-12);
        CHECK_NEAR(exp(lw), 0.587424456690, 1e-11);
        CHECK_EQ_INT(r.n, 4);
        CHECK_EQ_INT(r.errors, 1);
    }
}

/* A flip may come any time after its error: what was accounted in between
 * does not matter. Three regions receive the same samples; in one the first
 * sample is correct from the start, in the others it is an error that is
 * taken back after everything else, or somewhere in the middle. */
TEST(flip_commutes_with_later_updates) {
    uint64_t rng = 0xc0221;
    int trial, i, disagree = 0, flips = 0, loose = 0;

    for (trial = 0; trial < 1200; trial++) {
        int len = 1 + (int)(gate_test_rand64(&rng) % 400);
        int mid = (int)(gate_test_rand64(&rng) % (uint64_t)(len + 1));
        double p = 0.02 + 0.4 * gate_test_uniform(&rng);
        tracked late, middle, never;
        gateBets b;

        gateBetsInit(&b, ref_inc_ok[trial % COUNTOF(ref_inc_ok)].alpha);
        trackedInit(&late);
        trackedInit(&middle);
        trackedInit(&never);
        trackedUpdate(&late, &b, 1);
        trackedUpdate(&middle, &b, 1);
        trackedUpdate(&never, &b, 0);
        for (i = 0; i < len; i++) {
            int e = gate_test_uniform(&rng) < p;

            if (i == mid) trackedFlip(&middle, &b);
            trackedUpdate(&late, &b, e);
            trackedUpdate(&middle, &b, e);
            trackedUpdate(&never, &b, e);
        }
        if (mid == len) trackedFlip(&middle, &b);
        trackedFlip(&late, &b);

        if (!trackedAgree(&late, &never, &b)) disagree++;
        if (!trackedAgree(&middle, &never, &b)) disagree++;
        if (!trackedAgree(&late, &middle, &b)) disagree++;
        flips += (int)(late.flips + middle.flips);
        /* The bound that was applied is a million times below the smallest
         * thing a wrong flip could add or leave out, one increment. (Not
         * at alpha = 1e-9, where that increment is 1e-10 and the log
         * wealths are in the hundreds: flip_depends_only_on_the_counts
         * covers that corner with short streams.) */
        if (b.alpha >= 0.01 && !(trackedSlack(&late, &b) + trackedSlack(&never, &b) < 1e-6 * b.inc_ok[0]))
            loose++;
    }
    CHECK_EQ_INT(disagree, 0);
    CHECK_EQ_INT(loose, 0);
    CHECK_EQ_INT(flips, 2400);  /* every flip had its error to take back */
}

/* Long streams of updates and flips against the model: the counters must be
 * exactly those of the model, the log wealths and the mixture the closed
 * form for those counters. A flip is tried with probability P(flip),
 * whether there is an error to take back or not; otherwise a sample is
 * accounted, an error with probability P(error). The reference values were
 * computed for the five marks of each stream from the counters alone.
 *
 * The four streams differ in what the flips meet: in the first errors pile
 * up faster than flips remove them, in the second flips outnumber errors
 * and most of them find nothing to take back, the third is a good region
 * with rare late answers, the fourth mostly errors at alpha = 0.5. */
TEST(flip_long_random_sequences) {
    static const struct {
        double alpha;
        uint64_t seed;
        double p_flip, p_err;
        int idle;               /* flips that found no error, in the model */
        struct {
            int ops;
            uint64_t n, errors;
            double logw[GATE_NBETS], mix;
        } at[5];
    } cases[] = {
        {0.05, 0xf11b05, 0.20, 0.30, 8, {
            {10, 9, 1, {-6.3365668568676472039e-2, -1.8310541991295875878e-1,
                        -4.8534328933386003325e-1, -1.9323390724819958649e+0}, -4.5745251773038843669e-1},
            {100, 75, 4, {-4.8737794715100440797e-2, -2.2261049852507696289e-1,
                          -9.2832918760827441170e-1, -5.9244069399317395092e+0}, -6.2048040930516295033e-1},
            {1000, 806, 53, {-1.6313423475985660391e+0, -5.4038724197277525290e+0,
                             -1.7177259308021824780e+1, -8.7187603247987722900e+1},
                             -2.9949030958270639270e+0},
            {10000, 8016, 454, {-8.1380448976342616873e+0, -3.1756580080786886130e+1,
                                -1.1826219179275806318e+2, -6.9539858133028153997e+2},
                                -9.5243392586988681736e+0},
            {100000, 79927, 3612, {2.0042411894990088337e+1, -4.1511740883305984736e+1,
                                   -5.2132837131768522629e+2, -4.7850217239723564516e+3},
                                   1.8656117533870197718e+1}}},
        {0.02, 0xf11b02, 0.15, 0.10, 6649, {
            {10, 9, 1, {-8.9050622139040030101e-2, -2.4696951639201070470e-1,
                        -6.1192820884780124990e-1, -2.1569794558192749310e+0}, -5.3020301442400760152e-1},
            {100, 83, 0, {1.6921514525740762693e-1, 4.2239276912011606092e-1,
                          8.4264683151349461744e-1, 1.5106584856882488664e+0}, 8.7152359436396406921e-1},
            {1000, 862, 0, {1.7573910266492213785e+0, 4.3867779154402415002e+0,
                            8.7513442019835224125e+0, 1.5689007405581572564e+1}, 1.4303696331518957148e+1},
            {10000, 8525, 7, {1.6628435514522904598e+1, 4.1334919557477878170e+1,
                              8.1625869866585770200e+1, 1.3891550653087907437e+2}, 1.3752921216975918375e+2},
            {100000, 84768, 2, {1.7260483322036404503e+2, 4.3080470172540677574e+2,
                                8.5918962515783052796e+2, 1.5381957599085914673e+3},
                                1.5368094655474715767e+3}}},
        {0.01, 0xf11b01, 0.01, 0.02, 0, {
            {10, 10, 1, {-9.6274194845654895898e-2, -2.6498344755739212039e-1,
                         -6.4780703429033087620e-1, -2.2211365733147847332e+0}, -5.5029818132829064724e-1},
            {100, 100, 3, {-2.1815120044229815383e-1, -6.1840548238248563972e-1,
                           -1.5907755207739914814e+0, -6.0299212335501006330e+0}, -9.4869773818906802893e-1},
            {1000, 997, 20, {-1.1208397094352518109e+0, -3.2895796132780780506e+0,
                             -8.9410188439307616049e+0, -3.7210012556921121912e+1},
                             -2.3985280084792006471e+0},
            {10000, 9901, 113, {-2.0238595904972865428e+0, -7.8220585796870622049e+0,
                                -2.9015703438053145263e+1, -1.7161232543937070295e+2},
                                -3.4071255296005604766e+0},
            {100000, 98967, 950, {-1.1353890919788541664e+0, -2.6092289243268803270e+1,
                                  -1.6470036409763716612e+2, -1.3004181101885500130e+3},
                                  -2.5216834530842451867e+0}}},
        {0.5, 0xf11b50, 0.30, 0.50, 6, {
            {10, 5, 1, {2.7588020355947315297e-1, 6.0489213280505809563e-1,
                        9.2871325187271221849e-1, 2.6483045169553324465e-1}, 5.5721357457275343053e-1},
            {100, 68, 8, {4.8757266629968814463e+0, 1.1087156499238337926e+1,
                          1.8782729042010300443e+1, 2.0090552426391320012e+1}, 1.8943693168633322680e+1},
            {1000, 702, 56, {5.5670187276755589634e+1, 1.2804053809167977029e+2,
                             2.2311421772651725343e+2, 2.8569284525970046210e+2}, 2.8430655089858057148e+2},
            {10000, 6960, 425, {5.7807380586668681272e+2, 1.3359782270463538598e+3,
                                2.3551269297488774797e+3, 3.2159164816141304274e+3},
                                3.2145301872530105368e+3},
            {100000, 70052, 5110, {5.6512414618409729579e+3, 1.3021333119218809420e+4,
                                   2.2789732958099090763e+4, 2.9917065250608087721e+4},
                                   2.9915678956246967831e+4}}}
    };
    size_t c;
    int i, j;

    for (c = 0; c < COUNTOF(cases); c++) {
        uint64_t rng = cases[c].seed;
        tracked t;
        gateBets b;
        int m = 0, idle = 0;

        gateBetsInit(&b, cases[c].alpha);
        trackedInit(&t);
        for (i = 1; i <= cases[c].at[4].ops; i++) {
            if (gate_test_uniform(&rng) < cases[c].p_flip) {
                if (t.r.errors == 0) idle++;
                trackedFlip(&t, &b);
            } else {
                trackedUpdate(&t, &b, gate_test_uniform(&rng) < cases[c].p_err);
            }
            if (i == cases[c].at[m].ops) {
                /* Rounding of this route, plus what separates the library's
                 * increments from the reference ones. */
                double slack = trackedSlack(&t, &b) + refSlack(t.ops, &b);

                CHECK_EQ_INT(t.r.n, cases[c].at[m].n);
                CHECK_EQ_INT(t.r.errors, cases[c].at[m].errors);
                for (j = 0; j < GATE_NBETS; j++) CHECK_NEAR(t.r.logw[j], cases[c].at[m].logw[j], slack);
                CHECK_NEAR(gateRegionLogWealth(&t.r), cases[c].at[m].mix,
                           slack + mixSlack(cases[c].at[m].mix));
                /* After 100000 operations the bound is still a thousand
                 * times below the smallest increment there is here (1e-3):
                 * one sample or one flip accounted wrongly cannot hide. */
                CHECK(slack < 1e-6);
                m++;
            }
        }
        CHECK_EQ_INT(m, 5);
        CHECK_EQ_INT(idle, cases[c].idle);
        CHECK_EQ_INT(t.r.certified_at, 0);
    }
}

/* Every string of up to 9 operations over {correct, error, flip}: 29524 of
 * them per risk target. The state of a region is a function of two
 * counters and of nothing else about the way they were reached. */
typedef struct flipWalk {
    size_t a;               /* index into ref_inc_ok */
    const gateBets *b;
    long nodes, bad;
    double lo[10][10], hi[10][10];  /* mixture seen at (n, errors) */
    long seen[10][10];
    double slack;                   /* largest bound applied */
} flipWalk;

static void flipWalkFrom(flipWalk *w, const tracked *t, int n, int errors, int left) {
    double expect[GATE_NBETS], mix, lw = gateRegionLogWealth(&t->r);
    double slack = trackedSlack(t, w->b) + refSlack(t->ops, w->b);
    int j, op;

    w->nodes++;
    if (t->r.n != (uint64_t)n || t->r.errors != (uint64_t)errors || t->r.certified_at != 0) w->bad++;
    countsLogw(w->a, (uint64_t)n, (uint64_t)errors, expect);
    for (j = 0; j < GATE_NBETS; j++) {
        if (!(fabs(t->r.logw[j] - expect[j]) <= slack)) w->bad++;
    }
    mix = plainLogMixture(expect);
    if (!(fabs(lw - mix) <= slack + 2.0 * mixSlack(mix))) w->bad++;
    if (lw < w->lo[n][errors]) w->lo[n][errors] = lw;
    if (lw > w->hi[n][errors]) w->hi[n][errors] = lw;
    w->seen[n][errors]++;
    if (slack > w->slack) w->slack = slack;
    if (left == 0) return;

    for (op = 0; op < 3; op++) {
        tracked next = *t;

        if (op == 2) {
            trackedFlip(&next, w->b);
            flipWalkFrom(w, &next, n, errors > 0 ? errors - 1 : 0, left - 1);
        } else {
            trackedUpdate(&next, w->b, op);
            flipWalkFrom(w, &next, n + 1, errors + op, left - 1);
        }
    }
}

TEST(flip_depends_only_on_the_counts) {
    size_t a;
    int n, k;

    for (a = 0; a < COUNTOF(ref_inc_ok); a++) {
        gateBets b;
        tracked start;
        flipWalk w;
        int cells = 0, crowded = 0, spread = 0;

        gateBetsInit(&b, ref_inc_ok[a].alpha);
        memset(&w, 0, sizeof(w));
        w.a = a;
        w.b = &b;
        for (n = 0; n < 10; n++) {
            for (k = 0; k < 10; k++) {
                w.lo[n][k] = INFINITY;
                w.hi[n][k] = -INFINITY;
            }
        }
        trackedInit(&start);
        flipWalkFrom(&w, &start, 0, 0, 9);

        CHECK_EQ_INT(w.nodes, 29524);   /* (3^10 - 1) / 2 */
        CHECK_EQ_INT(w.bad, 0);
        /* Every pair of counters with errors <= n <= 9 was reached: 43 of
         * the 55 by ten strings or more ((5, 0) by 2885; the pairs with
         * n = 9 only by the strings without a flip). All the strings that
         * reach a pair report the same mixture. */
        for (n = 0; n < 10; n++) {
            for (k = 0; k <= n; k++) {
                if (w.seen[n][k] == 0) continue;
                cells++;
                crowded += w.seen[n][k] >= 10;
                if (!(w.hi[n][k] - w.lo[n][k] <= 2.0 * (w.slack + 2.0 * mixSlack(w.hi[n][k])))) spread++;
            }
        }
        CHECK_EQ_INT(cells, 55);
        CHECK_EQ_INT(crowded, 43);
        CHECK_EQ_INT(w.seen[5][0], 2885);
        CHECK_EQ_INT(spread, 0);
        /* The bound that was applied is below a hundredth of the smallest
         * increment, at alpha = 1e-9 too: one sample or one flip accounted
         * wrongly cannot hide in it. */
        CHECK(w.slack < 0.01 * ref_inc_ok[a].inc_ok[0]);
    }
}

/* Corrupted evidence stays corrupted: a flip never turns a NaN or an
 * infinite log wealth into a number, so it can never turn a region that
 * certifies nothing into one that does (see
 * non_finite_wealth_never_certifies for why such a region certifies
 * nothing). The other three bets are far above the threshold throughout. */
TEST(flip_never_cures_a_non_finite_wealth) {
    static const double thresholds[] = {-DBL_MAX, -1.0, 0.0, 1.0, 4.6};
    double poison[3];
    gateBets b, bad;
    gateRegion r;
    size_t v, k;
    int j, i, f;

    poison[0] = NAN;
    poison[1] = INFINITY;
    poison[2] = -INFINITY;
    gateBetsInit(&b, 0.05);
    for (v = 0; v < COUNTOF(poison); v++) {
        for (j = 0; j < GATE_NBETS; j++) {
            gateRegionInit(&r);
            for (i = 0; i < GATE_NBETS; i++) r.logw[i] = 100.0;
            CHECK(gateRegionCertifies(&r, 4.6)); /* healthy: it would */
            r.n = 500;
            r.errors = 3;
            r.logw[j] = poison[v];
            /* Five flips for three errors: the last two have nothing to
             * take back. */
            for (f = 0; f < 5; f++) {
                gateRegionFlip(&r, &b);
                /* Not finite, and of the same kind as before. */
                CHECK(!isfinite(r.logw[j]));
                CHECK(isnan(poison[v]) ? isnan(r.logw[j]) : r.logw[j] == poison[v]);
                CHECK(isnan(gateRegionLogWealth(&r)));
                for (k = 0; k < COUNTOF(thresholds); k++) CHECK(!gateRegionCertifies(&r, thresholds[k]));
                CHECK(!gateRegionCertifies(&r, -INFINITY));
            }
            CHECK_EQ_INT(r.n, 500);
            CHECK_EQ_INT(r.errors, 0);
            /* The healthy bets were flipped three times, as usual. */
            for (i = 0; i < GATE_NBETS; i++) {
                if (i == j) continue;
                CHECK_NEAR(r.logw[i], 100.0 + 3.0 * (ref_inc_ok[0].inc_ok[i] - ref_inc_err[i]), 1e-12);
            }
        }
        /* All four slots. */
        for (i = 0; i < GATE_NBETS; i++) r.logw[i] = poison[v];
        r.errors = 2;
        gateRegionFlip(&r, &b);
        gateRegionFlip(&r, &b);
        for (i = 0; i < GATE_NBETS; i++) CHECK(!isfinite(r.logw[i]));
        CHECK(isnan(gateRegionLogWealth(&r)));
        CHECK(!gateRegionCertifies(&r, -DBL_MAX));
    }

    /* Bets that are not a risk target (the caller validates alpha; if it
     * did not, every increment is NaN): a flip poisons a rich region
     * instead of adding to it, and no later flip heals it. */
    gateBetsInit(&bad, 1.0);
    gateRegionInit(&r);
    for (i = 0; i < GATE_NBETS; i++) r.logw[i] = 100.0;
    r.n = 10;
    r.errors = 2;
    CHECK(gateRegionCertifies(&r, 4.6));
    gateRegionFlip(&r, &bad);
    for (i = 0; i < GATE_NBETS; i++) CHECK(isnan(r.logw[i]));
    CHECK(isnan(gateRegionLogWealth(&r)));
    CHECK(!gateRegionCertifies(&r, -DBL_MAX));
    gateRegionFlip(&r, &b);
    for (i = 0; i < GATE_NBETS; i++) CHECK(isnan(r.logw[i]));
    CHECK(!gateRegionCertifies(&r, -DBL_MAX));
    CHECK_EQ_INT(r.n, 10);
    CHECK_EQ_INT(r.errors, 0);

    /* The other way round: the largest and smallest finite values stay
     * finite, a flip cannot push a log wealth to infinity. */
    for (i = 0; i < GATE_NBETS; i++) r.logw[i] = DBL_MAX;
    r.errors = 1;
    gateRegionFlip(&r, &b);
    for (i = 0; i < GATE_NBETS; i++) CHECK(r.logw[i] == DBL_MAX);
    CHECK(gateRegionLogWealth(&r) == DBL_MAX);
    for (i = 0; i < GATE_NBETS; i++) r.logw[i] = -DBL_MAX;
    r.errors = 1;
    gateRegionFlip(&r, &b);
    for (i = 0; i < GATE_NBETS; i++) CHECK(r.logw[i] == -DBL_MAX);
    CHECK(!gateRegionCertifies(&r, 0.0));
}

/* 'late' counts every request whose answer is outstanding as an error,
 * 'truth' holds the same requests with their true answers, and
 * 'outstanding' of the answers that are not in yet are correct ones.
 * Returns a mask of what is wrong: 1 the counters, 2 the log wealths (each
 * outstanding correct answer is one e in the place of one a, nothing
 * else), 4 "late is never above truth". */
static int lateVersusTruth(const tracked *late, const tracked *truth, const gateBets *b,
                           uint64_t outstanding) {
    double slack = trackedSlack(late, b) + trackedSlack(truth, b), ll, lt;
    int j, bad = 0;

    if (late->r.n != truth->r.n || late->r.errors != truth->r.errors + outstanding) bad |= 1;
    for (j = 0; j < GATE_NBETS; j++) {
        double gap = (double)outstanding * (b->inc_ok[j] - b->inc_err[j]);
        double expect = truth->r.logw[j] - gap;

        /* 2 u (gap + |expect|): the rounding of d, counted once per
         * outstanding answer, and that of the two lines above. */
        if (!(fabs(late->r.logw[j] - expect) <= slack + 2.0 * U_ROUND * (gap + fabs(expect)))) bad |= 2;
        if (!(late->r.logw[j] <= truth->r.logw[j] + slack)) bad |= 4;
    }
    ll = gateRegionLogWealth(&late->r);
    lt = gateRegionLogWealth(&truth->r);
    if (!(ll <= lt + slack + mixSlack(ll) + mixSlack(lt))) bad |= 4;
    return bad;
}

/* The argument that makes late answers safe (gate_core.h, MISSING). A
 * region in which every request counts as an error until its answer
 * arrives never holds more wealth than the same requests with their true
 * answers: bet by bet, hence for the mixture, at every moment and whatever
 * the teacher does. So if it reaches a threshold, the true answers reached
 * it too, and "ever reaches the threshold" keeps the probability that
 * Ville's inequality gives the true answers.
 *
 * 600 requests per trial, of which a good region gets about half as many
 * wrong as its target allows; four teachers:
 *   0  every answer is 0 to 30 requests late;
 *   1  wrong answers never arrive, correct ones are 0 to 5 requests late;
 *   2  one answer in ten never arrives, the others are 0 to 100 late;
 *   3  answers arrive together, every 50 requests.
 * The two regions are compared after every answer and every request. */
TEST(flip_outstanding_answers_never_add_wealth) {
    enum { N = 600, NEVER = -1 };
    static const double alphas[] = {0.05, 0.02, 0.3};
    static unsigned char wrong[N];
    static int due[N];
    const double thr = log(20.0);
    uint64_t rng = 0x1a7e;
    int trial, n, i, problems = 0, unsafe = 0, proven_true = 0, proven_late = 0, held_back = 0;

    for (trial = 0; trial < 240; trial++) {
        double alpha = alphas[trial % 3], p = 0.5 * alpha;
        int mode = (trial / 3) % 4, first_late = 0, first_true = 0;
        uint64_t outstanding = 0;
        tracked late, truth;
        gateBets b;

        gateBetsInit(&b, alpha);
        trackedInit(&late);
        trackedInit(&truth);
        for (i = 0; i < N; i++) {
            int lag;

            wrong[i] = gate_test_uniform(&rng) < p;
            switch (mode) {
            case 0: lag = (int)(gate_test_rand64(&rng) % 31); break;
            case 1: lag = wrong[i] ? NEVER : (int)(gate_test_rand64(&rng) % 6); break;
            case 2: lag = gate_test_uniform(&rng) < 0.1 ? NEVER : (int)(gate_test_rand64(&rng) % 101); break;
            default: lag = 49 - i % 50; break;
            }
            due[i] = lag == NEVER ? NEVER : i + lag;
        }

        for (n = 0; n < N; n++) {
            trackedUpdate(&truth, &b, wrong[n]);
            trackedUpdate(&late, &b, 1);    /* asked: wrong until answered */
            if (!wrong[n]) outstanding++;
            problems += lateVersusTruth(&late, &truth, &b, outstanding) != 0;
            for (i = n > 100 ? n - 100 : 0; i <= n; i++) {
                if (due[i] != n || wrong[i]) continue;  /* a wrong answer changes nothing */
                trackedFlip(&late, &b);
                outstanding--;
                problems += lateVersusTruth(&late, &truth, &b, outstanding) != 0;
                /* Reaching the threshold with outstanding answers counted
                 * as errors implies that the true answers are there. */
                if (gateRegionCertifies(&late.r, thr) &&
                    !(gateRegionLogWealth(&truth.r) >=
                      thr - trackedSlack(&late, &b) - trackedSlack(&truth, &b)))
                    unsafe++;
            }
            if (!first_late && gateRegionCertifies(&late.r, thr)) first_late = n + 1;
            if (!first_true && gateRegionCertifies(&truth.r, thr)) first_true = n + 1;
        }
        proven_true += first_true != 0;
        proven_late += first_late != 0;
        held_back += first_true != 0 && (first_late == 0 || first_late > first_true);
        if (first_late != 0 && (first_true == 0 || first_true > first_late)) unsafe++;

        /* The correct answers that are still out arrive after all: the two
         * regions end up equal. */
        for (i = 0; i < N; i++) {
            if (wrong[i] || (due[i] != NEVER && due[i] < N)) continue;
            trackedFlip(&late, &b);
            outstanding--;
        }
        problems += outstanding != 0;
        problems += !trackedAgree(&late, &truth, &b);
    }
    CHECK_EQ_INT(problems, 0);
    CHECK_EQ_INT(unsafe, 0);
    /* The trials did exercise it: most streams were proven on their true
     * answers, fewer with late answers counted as errors, and in many the
     * late answers held the proof back. */
    CHECK(proven_true > 120);
    CHECK(proven_late > 30 && proven_late < proven_true);
    CHECK(held_back > 60);
}

/* Is the stream certified at some request when every request counts as an
 * error until its answer arrives, 'lag' requests later? Bit i of 'pattern'
 * says that request i is wrong. Request i is asked (an error for now), then
 * the answer of request i - lag arrives and is set right if it is correct,
 * then the region is tested. */
static int lateCertifies(const gateBets *b, double thr, unsigned pattern, int n, int lag) {
    gateRegion r;
    int i;

    gateRegionInit(&r);
    for (i = 0; i < n; i++) {
        gateRegionUpdate(&r, b, 1);
        if (i >= lag && !((pattern >> (i - lag)) & 1u)) gateRegionFlip(&r, b);
        if (gateRegionCertifies(&r, thr)) return 1;
    }
    return 0;
}

/* The rule the gate does NOT use: a request is ignored until its answer
 * arrives. Correct answers arrive at once, wrong ones 'lag' requests late
 * (never if lag < 0). The answers of one moment arrive in the order of
 * their requests, as in table (E) of sim_certify: the wrong answer of
 * request i - lag comes before the correct answer of request i, and the
 * region is tested after every correct answer. */
static int ignoringCertifies(const gateBets *b, double thr, unsigned pattern, int n, int lag) {
    gateRegion r;
    int i;

    gateRegionInit(&r);
    for (i = 0; i < n; i++) {
        if (lag >= 0 && i >= lag && ((pattern >> (i - lag)) & 1u)) gateRegionUpdate(&r, b, 1);
        if (!((pattern >> i) & 1u)) {
            gateRegionUpdate(&r, b, 0);
            if (gateRegionCertifies(&r, thr)) return 1;
        }
    }
    return 0;
}

/* The same rule against exact probabilities: all 65536 streams of 16
 * requests, walked through the library. As in
 * exhaustive_certification_probability no reachable state is closer than
 * 0.03 to the threshold, so no path depends on rounding.
 *
 * alpha = 0.5, delta = 0.1, error rate 0.5 (the boundary of H0: every
 * stream is equally likely). Without errors the threshold is reached at
 * sample 6, with one error at sample 10, with two at sample 13, with three
 * not within 16. So when every answer is one request late (one error is
 * always outstanding) a stream is certified if its first 9 requests are
 * correct (2^7 streams), or if exactly one of the first 12 is wrong and it
 * is one of the first 9 (9 * 2^4): 272. Two requests late: the first 11
 * must be correct, 2^5 = 32. Three late: never. With every answer at once
 * the rule is the plain test again: the 1720 streams of
 * exhaustive_certification_probability. All are at most delta = 0.1 of the
 * 65536, as they must: a late answer only ever costs.
 *
 * Ignoring a request until its answer arrives is another matter. If the
 * wrong answers never arrive, every stream with 6 correct requests is
 * certified: 65536 - (1 + 16 + 120 + 560 + 1820 + 4368) = 58651 of them,
 * 89% where 10% were promised. A teacher that is merely slower on the
 * requests the gate gets wrong does the same on a smaller scale: 23669
 * streams (36%) when the wrong answers are 9 requests late, and 6390
 * where 1720 are due when they are 5 late (these counts come from the
 * exact enumeration). A wrong answer that is one request late is in before
 * the next correct one is looked at and changes nothing. This is why the
 * gate does not do it. */
TEST(flip_exhaustive_late_answers) {
    static const struct {
        int lag;
        long paths;
    } counted[] = {{0, 1720}, {1, 272}, {2, 32}, {3, 0}, {8, 0}},
      ignored[] = {{-1, 58651}, {9, 23669}, {5, 6390}, {2, 2388}, {1, 1720}, {0, 1720}};
    /* alpha = 0.3, delta = 0.25, error rate 0.3: the boundary of H0 again,
     * streams weighted by their probability. */
    static const struct {
        int lag;
        double pr;
    } counted03[] = {{0, 1.0270099218547001259e-1}, {1, 9.6889010407000019977e-3}, {2, 0.0}},
      ignored03[] = {{-1, 9.9287047756316350206e-1}, {5, 2.5910494619636622139e-1},
                     {1, 1.0270099218547001259e-1}};
    const int n = 16;
    static unsigned char plain[1 << 16];    /* certified with every answer at once */
    double weight[17], thr;
    gateBets b;
    gateRegion r;
    unsigned pattern;
    size_t i;
    int k;

    /* The four certification times the comment relies on. */
    gateBetsInit(&b, 0.5);
    thr = -log(0.1);
    for (k = 0; k <= 3; k++) {
        static const int first[] = {6, 10, 13, 0};
        int at = 0, m;

        gateRegionInit(&r);
        for (m = 0; m < k; m++) gateRegionUpdate(&r, &b, 1);
        for (m = k + 1; m <= n && !at; m++) {
            gateRegionUpdate(&r, &b, 0);
            if (gateRegionCertifies(&r, thr)) at = m;
        }
        CHECK_EQ_INT(at, first[k]);
    }

    for (i = 0; i < COUNTOF(counted); i++) {
        long paths = 0, outside = 0;

        for (pattern = 0; pattern < (1u << n); pattern++) {
            int c = lateCertifies(&b, thr, pattern, n, counted[i].lag);

            if (counted[i].lag == 0) plain[pattern] = (unsigned char)c;
            paths += c;
            /* Whatever is certified with late answers is certified with
             * prompt ones. */
            if (c && !plain[pattern]) outside++;
        }
        CHECK_EQ_INT(paths, counted[i].paths);
        CHECK_EQ_INT(outside, 0);
        CHECK(paths <= 6553);   /* delta = 0.1 of the 65536 streams */
    }
    for (i = 0; i < COUNTOF(ignored); i++) {
        long paths = 0;

        for (pattern = 0; pattern < (1u << n); pattern++)
            paths += ignoringCertifies(&b, thr, pattern, n, ignored[i].lag);
        CHECK_EQ_INT(paths, ignored[i].paths);
    }

    gateBetsInit(&b, 0.3);
    thr = -log(0.25);
    for (k = 0; k <= n; k++) weight[k] = pow(0.3, k) * pow(0.7, n - k);
    for (i = 0; i < COUNTOF(counted03) + COUNTOF(ignored03); i++) {
        int late = i < COUNTOF(counted03);
        int lag = late ? counted03[i].lag : ignored03[i - COUNTOF(counted03)].lag;
        double expected = late ? counted03[i].pr : ignored03[i - COUNTOF(counted03)].pr, pr = 0.0;

        for (pattern = 0; pattern < (1u << n); pattern++) {
            unsigned bits = pattern;
            int errors = 0;

            if (!(late ? lateCertifies(&b, thr, pattern, n, lag)
                       : ignoringCertifies(&b, thr, pattern, n, lag)))
                continue;
            for (; bits; bits &= bits - 1) errors++;
            pr += weight[errors];
        }
        /* A plain sum of up to 50000 weights; the smallest weight of one
         * stream is 0.3^16 = 4e-9, so 1e-11 still tells every stream. */
        CHECK_NEAR(pr, expected, 1e-11);
        /* The guarantee holds for the rule in use and, with wrong answers
         * that never arrive or are 5 requests late, not for the other. */
        if (late) CHECK(pr <= 0.25);
        if (!late && lag != 1) CHECK(pr > 0.25);
    }
}

/* What an outstanding answer costs. If 'k' answers are outstanding at any
 * moment (the teacher is k requests behind) and every guess is right, the
 * region holds n - k correct samples and k errors after n requests, and it
 * is certified at the first n with
 *
 *     log((1/4) sum_j (1 - c_j)^k (1 + c_j alpha / (1 - alpha))^(n - k)) >= threshold.
 *
 * The reference n come from that expression in high precision; each clears
 * the threshold by more than 8e-5 at n and misses it by more than 8e-4 at
 * n - 1, far beyond the rounding of the few thousand updates and flips.
 *
 * There is a closed form on either side of it. Bet j alone is there once
 * (n - k) a_j >= threshold + k |e_j|, and the mixture is between a quarter
 * of its best bet and its best bet:
 *
 *     k + min_j ceil((thr + k |e_j|) / a_j)  <=  n  <=  k + min_j ceil((thr + log 4 + k |e_j|) / a_j)
 *
 * with a_j = inc_ok[j] and e_j = inc_err[j]. For a few outstanding answers
 * the 0.9 bet decides and each costs about 1 + log(10) / a_3 samples (51 at
 * alpha = 0.05); for many the 0.1 bet takes over, at 1 + |e_0| / a_0 (21). */
TEST(flip_cost_of_outstanding_answers) {
    static const struct {
        double alpha;
        uint32_t nregions;
        int k, n;
    } cases[] = {
        {0.05, 32, 0, 220},  {0.05, 32, 1, 270},   {0.05, 32, 3, 371},
        {0.05, 32, 10, 665}, {0.05, 32, 30, 1220}, {0.05, 32, 100, 3069},
        {0.02, 64, 0, 596},  {0.02, 64, 1, 723},   {0.02, 64, 3, 976},
        {0.02, 64, 10, 1749}, {0.01, 32, 30, 6165}, {0.01, 64, 100, 15708}
    };
    size_t i;
    int j;

    for (i = 0; i < COUNTOF(cases); i++) {
        const double thr = gateBudgetLogThreshold(0.01, 1, cases[i].nregions);
        double lo = INFINITY, hi = INFINITY;
        int k = cases[i].k, n = 0;
        gateBets b;
        gateRegion r;

        gateBetsInit(&b, cases[i].alpha);
        gateRegionInit(&r);
        while (!gateRegionCertifies(&r, thr) && n < 100000) {
            gateRegionUpdate(&r, &b, 1);            /* request n is asked */
            n++;
            if (n > k) gateRegionFlip(&r, &b);      /* the answer of request n - k */
        }
        CHECK_EQ_INT(n, cases[i].n);
        CHECK_EQ_INT(r.n, (uint64_t)n);
        CHECK_EQ_INT(r.errors, (uint64_t)k);

        for (j = 0; j < GATE_NBETS; j++) {
            double need = (thr + k * fabs(b.inc_err[j])) / b.inc_ok[j];
            double diluted = (thr + log(4.0) + k * fabs(b.inc_err[j])) / b.inc_ok[j];

            if (ceil(need) < lo) lo = ceil(need);
            if (ceil(diluted) < hi) hi = ceil(diluted);
        }
        CHECK(k + lo <= n && n <= k + hi);
    }
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
            /* 1e-10: ten times the worst case rounding of 100000
             * additions. */
            CHECK_REL(sum, gateBudgetSpent(total, g), 1e-10);
            CHECK_REL(sum, total * (double)g / (double)(g + 1), 1e-10);
        }
        CHECK(below);
        CHECK(growing);
        /* It approaches the total: 1/100001 of it is left after 100000
         * generations. */
        CHECK_REL(total - sum, total / 100001.0, 1e-5);
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

/* ---------------------------- Threshold grid ----------------------------- */

#define T GATE_THRESHOLDS
#define SENTINEL (-7.0)

/* The four grids the module builds: classifier gates with 2, 3 and 4 or more
 * labels (from max(1/K, 0.25) to 0.999) and score gates (from 0 to 0.998).
 * 'value' is the closed form of the header,
 *
 *     1 - (1 - lo) * ((1 - hi) / (1 - lo)) ^ (t / 31)
 *
 * at the thresholds listed in ref_grid_at, and 'step' is the constant
 * difference of log(1 - threshold) between neighbours. */
static const uint32_t ref_grid_at[] = {1, 2, 8, 15, 16, 23, 30};
static const struct {
    double lo, hi;
    double value[COUNTOF(ref_grid_at)];
    double step; /* (log(1 - hi) - log(1 - lo)) / 31 */
} ref_grids[] = {
    {0.5, 0.999,
     {5.9082748284636533525e-1, 6.6515570241231709206e-1, 8.9943158254551161465e-1,
      9.7528180372509556323e-1, 9.7977198682139951175e-1, 9.9502826023660686171e-1,
      9.9877802154583060085e-1},
     -2.0047122898136099531e-1},
    {1.0 / 3.0, 0.999,
     {4.5947608885369834120e-1, 5.6175085221865749623e-1, 8.7550322596584365818e-1,
      9.7132514135768434978e-1, 9.7675082988263232354e-1, 9.9464510890471938023e-1,
      9.9876662872276482323e-1},
     -2.0975129583464425192e-1},
    {0.25, 0.999,
     {3.9421662332407307694e-1, 5.1070200072414938238e-1, 8.6413425653497566097e-1,
      9.6952788577891434003e-1, 9.7538733300359492815e-1, 9.9447984472853474102e-1,
      9.9876193367319614544e-1},
     -2.1355074859775339472e-1},
    {0.0, 0.998,
     {1.8165496569273067050e-1, 3.3031140482463418411e-1, 7.9886316509102322930e-1,
      9.5056360745019112646e-1, 9.5954397364279902350e-1, 9.9005652047321372342e-1,
      9.9755604309166120170e-1},
     -2.0047122898136099531e-1}
};

static int strictlyIncreasing(const double *grid, uint32_t n) {
    uint32_t t;

    for (t = 1; t < n; t++) {
        if (!(grid[t] > grid[t - 1])) return 0;
    }
    return 1;
}

TEST(grid_end_points_and_order) {
    double grid[T + 2];
    size_t i;
    uint32_t t;

    CHECK_EQ_INT(T, 32);
    for (i = 0; i < COUNTOF(ref_grids); i++) {
        for (t = 0; t < COUNTOF(grid); t++) grid[t] = SENTINEL;
        CHECK_EQ_INT(gateGridBuild(grid, ref_grids[i].lo, ref_grids[i].hi), GATE_OK);
        /* Exact, not "close to": one ulp above 'lo' the widest region would
         * drop the least confident scores a model can produce. */
        CHECK(grid[0] == ref_grids[i].lo);
        CHECK(grid[T - 1] == ref_grids[i].hi);
        CHECK(strictlyIncreasing(grid, T));
        /* Exactly T entries are written. */
        CHECK(grid[T] == SENTINEL && grid[T + 1] == SENTINEL);
    }
}

TEST(grid_closed_form) {
    double grid[T], score[T];
    size_t i, j;
    uint32_t t;

    for (i = 0; i < COUNTOF(ref_grids); i++) {
        int uneven = 0;

        CHECK_EQ_INT(gateGridBuild(grid, ref_grids[i].lo, ref_grids[i].hi), GATE_OK);
        for (j = 0; j < COUNTOF(ref_grid_at); j++)
            CHECK_NEAR(grid[ref_grid_at[j]], ref_grids[i].value[j], 1e-12);
        /* Evenly spaced in log(1 - threshold). Next to 0.999 the logarithm
         * amplifies the rounding of a threshold a thousand times, to 1e-13,
         * hence 1e-12. */
        for (t = 0; t + 1 < T; t++) {
            double step = log1p(-grid[t + 1]) - log1p(-grid[t]);

            if (!(fabs(step - ref_grids[i].step) <= 1e-12)) uneven++;
        }
        CHECK_EQ_INT(uneven, 0);
    }

    /* (1 - 0.998) / (1 - 0) and (1 - 0.999) / (1 - 0.5) are the same ratio,
     * so the score grid is the 2 label grid with every distance to 1
     * doubled. No reference value is involved in this one. */
    CHECK_EQ_INT(gateGridBuild(grid, 0.5, 0.999), GATE_OK);
    CHECK_EQ_INT(gateGridBuild(score, 0.0, 0.998), GATE_OK);
    for (t = 0; t < T; t++) CHECK_NEAR(1.0 - score[t], 2.0 * (1.0 - grid[t]), 1e-14);
}

/* Any 0 <= lo < hi < 1 is a grid, however lopsided. */
TEST(grid_other_ranges) {
    static const struct {
        double lo, hi;
    } ranges[] = {
        {0.0, 0.5}, {0.9, 0.99}, {0.999, 0.9999999}, {0.0, 1e-300}, {1e-300, 1e-200}, {-0.0, 0.998},
        {0.0, 1.0 - DBL_EPSILON / 2} /* the largest double below 1 */
    };
    double grid[T];
    uint64_t rng = 0x961d;
    size_t i;
    int bad = 0;

    for (i = 0; i < COUNTOF(ranges); i++) {
        CHECK_EQ_INT(gateGridBuild(grid, ranges[i].lo, ranges[i].hi), GATE_OK);
        CHECK(grid[0] == ranges[i].lo);
        CHECK(grid[T - 1] == ranges[i].hi);
        CHECK(strictlyIncreasing(grid, T));
    }
    /* The end points are the arguments themselves, bit for bit: a value
     * taken through log1p() and expm1() rarely comes back unchanged. */
    for (i = 0; i < 2000; i++) {
        double lo = 0.9 * gate_test_uniform(&rng);
        double hi = lo + 0.01 + (0.98 - lo) * gate_test_uniform(&rng); /* below 0.99 */

        if (gateGridBuild(grid, lo, hi) != GATE_OK || grid[0] != lo || grid[T - 1] != hi ||
            !strictlyIncreasing(grid, T)) bad++;
    }
    CHECK_EQ_INT(bad, 0);

    /* A range too narrow to hold 32 different doubles cannot be a grid: it
     * is refused, never returned with repeated thresholds (the regions
     * would no longer be nested one inside the other). */
    CHECK_EQ_INT(gateGridBuild(grid, 0.5, nextafter(0.5, 1.0)), GATE_ERR);
    CHECK_EQ_INT(gateGridBuild(grid, 0.5, 0.5 + 10 * DBL_EPSILON), GATE_ERR); /* 21 doubles */
}

TEST(grid_invalid_arguments) {
    const double bad[][2] = {
        /* lo < 0 */
        {-0.1, 0.5}, {-1e-300, 0.5}, {-1.0, 0.999}, {-INFINITY, 0.5},
        /* lo >= hi */
        {0.5, 0.5}, {0.6, 0.5}, {0.0, 0.0}, {0.999, 0.5}, {0.5, nextafter(0.5, 0.0)},
        /* hi >= 1 */
        {0.5, 1.0}, {0.0, 1.0}, {0.5, nextafter(1.0, 2.0)}, {0.5, 1.5}, {0.0, INFINITY},
        {1.0, 2.0}, {INFINITY, INFINITY},
        /* not a number */
        {NAN, 0.999}, {0.5, NAN}, {NAN, NAN}
    };
    double grid[T + 2];
    size_t i;
    uint32_t t;

    for (i = 0; i < COUNTOF(bad); i++) {
        int untouched = 1;

        for (t = 0; t < COUNTOF(grid); t++) grid[t] = SENTINEL;
        CHECK_EQ_INT(gateGridBuild(grid, bad[i][0], bad[i][1]), GATE_ERR);
        for (t = 0; t < COUNTOF(grid); t++) {
            if (grid[t] != SENTINEL) untouched = 0;
        }
        CHECK(untouched);
    }
    CHECK_EQ_INT(gateGridBuild(NULL, 0.5, 0.999), GATE_ERR);
    CHECK_EQ_INT(gateGridBuild(NULL, NAN, 2.0), GATE_ERR);
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

    for (t = 0; t < n; t++) {
        if (gateGridCount(grid, n, grid[t]) != t + 1) exact++;
        if (gateGridCount(grid, n, nextafter(grid[t], -INFINITY)) != t) exact++;
        if (gateGridCount(grid, n, nextafter(grid[t], INFINITY)) != t + 1) exact++;
        if (t + 1 < n && gateGridCount(grid, n, 0.5 * (grid[t] + grid[t + 1])) != t + 1) exact++;
    }
    CHECK_EQ_INT(exact, 0);

    if (n > 0) {
        CHECK_EQ_INT(gateGridCount(grid, n, nextafter(grid[0], -INFINITY)), 0);
        CHECK_EQ_INT(gateGridCount(grid, n, grid[0] - 1e-9), 0);
        CHECK_EQ_INT(gateGridCount(grid, n, nextafter(grid[n - 1], INFINITY)), n);
    }
    CHECK_EQ_INT(gateGridCount(grid, n, -1.0), 0);
    CHECK_EQ_INT(gateGridCount(grid, n, -DBL_MAX), 0);
    CHECK_EQ_INT(gateGridCount(grid, n, -INFINITY), 0);
    CHECK_EQ_INT(gateGridCount(grid, n, 1.0), n);
    CHECK_EQ_INT(gateGridCount(grid, n, 2.0), n);
    CHECK_EQ_INT(gateGridCount(grid, n, DBL_MAX), n);
    CHECK_EQ_INT(gateGridCount(grid, n, INFINITY), n);
    CHECK_EQ_INT(gateGridCount(grid, n, NAN), 0);
    CHECK_EQ_INT(gateGridCount(grid, n, -NAN), 0);

    /* 22001 scores from -0.05 to 1.05 in steps of 0.00005: four or more
     * between any two neighbours, the two most confident ones included. */
    for (i = 0; i <= 22000; i++) {
        double s = -0.05 + i * 0.00005;

        if (gateGridCount(grid, n, s) != bruteCount(grid, n, s)) sweep++;
    }
    CHECK_EQ_INT(sweep, 0);
}

TEST(grid_count) {
    static const uint32_t prefixes[] = {0, 1, 2, 3, 5, 16, 31};
    static const double odd[] = {0.1, 0.2, 0.7};
    double grid[T];
    uint64_t rng = 4242;
    size_t i;
    int k, mismatches = 0;

    for (i = 0; i < COUNTOF(ref_grids); i++) {
        CHECK_EQ_INT(gateGridBuild(grid, ref_grids[i].lo, ref_grids[i].hi), GATE_OK);
        checkGridCount(grid, T);
        /* Random scores where this grid lives. */
        for (k = 0; k < 100000; k++) {
            double s = ref_grids[i].lo + (1.0 - ref_grids[i].lo) * gate_test_uniform(&rng);

            if (gateGridCount(grid, T, s) != bruteCount(grid, T, s)) mismatches++;
        }
    }
    CHECK_EQ_INT(mismatches, 0);

    /* The 2 label grid. The least confident score a binary model can
     * produce is in region 0 and only there; 0.999 and above is in all 32.
     * In between, from the closed form: thresholds 8 and 9 are 0.8994 and
     * 0.9177, thresholds 19 and 20 are 0.98891 and 0.99093. */
    CHECK_EQ_INT(gateGridBuild(grid, 0.5, 0.999), GATE_OK);
    CHECK_EQ_INT(gateGridCount(grid, T, 0.5), 1);
    CHECK_EQ_INT(gateGridCount(grid, T, 0.4999999), 0);
    CHECK_EQ_INT(gateGridCount(grid, T, 0.9), 9);
    CHECK_EQ_INT(gateGridCount(grid, T, 0.99), 20);
    CHECK_EQ_INT(gateGridCount(grid, T, 0.9989999), 31);
    CHECK_EQ_INT(gateGridCount(grid, T, 0.999), 32);
    CHECK_EQ_INT(gateGridCount(grid, T, 1.0), 32);
    /* The function takes the number of thresholds: any prefix is a grid. */
    for (i = 0; i < COUNTOF(prefixes); i++) checkGridCount(grid, prefixes[i]);

    /* The score grid. 0 is in region 0 (so is -0.0), anything negative in
     * none. 1 - 0.002^(t/31) <= 0.5 holds up to t = 31 log(0.5) / log(0.002)
     * = 3.46, so a score of one half is in regions 0..3. */
    CHECK_EQ_INT(gateGridBuild(grid, 0.0, 0.998), GATE_OK);
    CHECK_EQ_INT(gateGridCount(grid, T, 0.0), 1);
    CHECK_EQ_INT(gateGridCount(grid, T, -0.0), 1);
    CHECK_EQ_INT(gateGridCount(grid, T, -1e-300), 0);
    CHECK_EQ_INT(gateGridCount(grid, T, 0.5), 4);
    CHECK_EQ_INT(gateGridCount(grid, T, 0.998), 32);
    CHECK_EQ_INT(gateGridCount(grid, T, 0.9979999), 31);

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

/* ----------------------------- Drift detector ---------------------------- */

#define ALTS GATE_DRIFT_ALTS

/* log(p1 / alpha) and log((1 - p1) / (1 - alpha)) for the alternatives
 * p1 = 2 alpha and p1 = 4 alpha, each capped at (1 + alpha) / 2. */
static const struct {
    double alpha;
    double inc_err[ALTS], inc_ok[ALTS];
} ref_drift[] = {
    /* p1 = 0.02 and 0.04. */
    {0.01, {6.9314718055994530942e-1, 1.3862943611198906188e+0},
           {-1.0152371464018007439e-2, -3.0771658666753689028e-2}},
    /* p1 = 0.1 and 0.2: an error multiplies by 2 and 4, a correct answer by
     * 18/19 and 16/19. */
    {0.05, {6.9314718055994530942e-1, 1.3862943611198906188e+0},
           {-5.4067221270275771048e-2, -1.7185025692665923330e-1}},
    /* p1 = 0.6, and 1.2 capped at 0.65: factors 2 and 13/6, 4/7 and 1/2. */
    {0.3,  {6.9314718055994530942e-1, 7.7318988823348176371e-1},
           {-5.5961578793542264662e-1, -6.9314718055994530942e-1}},
    /* 1.2 and 2.4, both capped at 0.8: the two alternatives coincide, with
     * factors 4/3 and 1/2. */
    {0.6,  {2.8768207245178095057e-1, 2.8768207245178095057e-1},
           {-6.9314718055994530942e-1, -6.9314718055994530942e-1}}
};

TEST(drift_increments) {
    /* 1/7 is where 4 alpha meets its cap and 1/3 where 2 alpha does. */
    static const double alphas[] = {1e-12, 1e-6, 0.001, 0.01, 0.02, 0.05, 0.1, 1.0 / 7.0, 0.2,
                                    0.3, 1.0 / 3.0, 0.5, 0.6, 0.9, 0.999999};
    gateDriftBets b;
    size_t i;
    int k;

    CHECK_EQ_INT(ALTS, 2);
    for (i = 0; i < COUNTOF(ref_drift); i++) {
        gateDriftBetsInit(&b, ref_drift[i].alpha);
        for (k = 0; k < ALTS; k++) {
            CHECK_REL(b.inc_err[k], ref_drift[i].inc_err[k], 1e-13);
            CHECK_REL(b.inc_ok[k], ref_drift[i].inc_ok[k], 1e-13);
        }
    }
    /* The same as the factors of the header, where they are round. */
    gateDriftBetsInit(&b, 0.05);
    CHECK_NEAR(exp(b.inc_err[0]), 2.0, 1e-14);
    CHECK_NEAR(exp(b.inc_err[1]), 4.0, 1e-14);
    CHECK_NEAR(exp(b.inc_ok[0]), 18.0 / 19.0, 1e-14);
    CHECK_NEAR(exp(b.inc_ok[1]), 16.0 / 19.0, 1e-14);
    gateDriftBetsInit(&b, 0.3);
    CHECK_NEAR(exp(b.inc_err[1]), 13.0 / 6.0, 1e-14);
    CHECK_NEAR(exp(b.inc_ok[0]), 4.0 / 7.0, 1e-14);

    /* What makes it a detector for every target. With error rate p the
     * expected factor of one spot check is
     *
     *     p e(1) + (1 - p) e(0) = 1 + (p - alpha) (p1 - alpha) / (alpha (1 - alpha))
     *
     * which is 1 at p = alpha, below 1 for a better policy (it cannot grow
     * by itself: few false alarms) and above 1 for a worse one. */
    for (i = 0; i < COUNTOF(alphas); i++) {
        double alpha = alphas[i], better = 0.5 * alpha, worse = alpha + 0.5 * (1.0 - alpha);

        gateDriftBetsInit(&b, alpha);
        for (k = 0; k < ALTS; k++) {
            double e1 = exp(b.inc_err[k]), e0 = exp(b.inc_ok[k]);

            /* An error is evidence of drift, a correct answer against it. */
            CHECK(isfinite(b.inc_err[k]) && b.inc_err[k] > 0.0);
            CHECK(isfinite(b.inc_ok[k]) && b.inc_ok[k] < 0.0);
            CHECK_NEAR(alpha * e1 + (1.0 - alpha) * e0, 1.0, 1e-13);
            CHECK(better * e1 + (1.0 - better) * e0 < 1.0);
            CHECK(worse * e1 + (1.0 - worse) * e0 > 1.0);
        }
        /* The second alternative is never the closer one. */
        CHECK(b.inc_err[1] >= b.inc_err[0]);
        CHECK(b.inc_ok[1] <= b.inc_ok[0]);
    }
}

TEST(drift_init_and_counters) {
    gateDriftBets b;
    gateDrift d, same;
    int k;

    memset(&d, 0xff, sizeof(d));
    gateDriftInit(&d);
    for (k = 0; k < ALTS; k++) CHECK(d.logw[k] == 0.0);
    CHECK_EQ_INT(d.n, 0);
    CHECK_EQ_INT(d.errors, 0);
    /* Statistic 1 before any spot check, with no rounding. */
    CHECK(gateDriftLogStat(&d) == 0.0);

    gateDriftBetsInit(&b, 0.05);
    CHECK_EQ_INT(gateDriftPush(&d, &b, 0, log(1000.0)), 0);
    CHECK_EQ_INT(d.n, 1);
    CHECK_EQ_INT(d.errors, 0);
    CHECK_EQ_INT(gateDriftPush(&d, &b, 1, log(1000.0)), 0);
    CHECK_EQ_INT(d.n, 2);
    CHECK_EQ_INT(d.errors, 1);

    /* Whatever is not 0 is one error. */
    same = d;
    gateDriftPush(&d, &b, 5, log(1000.0));
    gateDriftPush(&d, &b, -1, log(1000.0));
    gateDriftPush(&same, &b, 1, log(1000.0));
    gateDriftPush(&same, &b, 1, log(1000.0));
    CHECK_EQ_INT(d.n, 4);
    CHECK_EQ_INT(d.errors, 3);
    CHECK(memcmp(&d, &same, sizeof(d)) == 0);

    /* The counters are 64 bit. */
    d.n = 0xffffffffull;
    d.errors = 0xffffffffull;
    gateDriftPush(&d, &b, 1, log(1000.0));
    CHECK(d.n == 0x100000000ull);
    CHECK(d.errors == 0x100000000ull);
}

/* alpha = 0.2 makes every factor a small fraction: the alternatives are
 * p1 = 0.4 and p1 = 0.6 (0.8, capped at (1 + 0.2) / 2), so an error
 * multiplies the two statistics by 2 and 3 and a correct answer by 3/4 and
 * 1/2. A statistic that is below 1 restarts from 1 before it is updated
 * (marked * below), and the detector reports the log of the mean of the two.
 *
 *     push  E    W1                 W2
 *       1   0    0.75               0.5
 *       2   0    0.75 *             0.5 *
 *       3   1    2 *                3 *
 *       4   1    4                  9
 *       5   0    3                  4.5
 *       6   0    2.25               2.25
 *       7   0    1.6875             1.125
 *       8   0    1.265625           0.5625
 *       9   1    2.53125            3 *          (1.6875 without the restart)
 *      10   0    1.8984375          1.5
 *      11   0    1.423828125        0.75
 *      12   0    1.06787109375      0.5 *
 *      13   0    0.8009033203125    0.5 *
 *      14   0    0.75 *             0.5 *
 *      15   1    2 *                3 *
 *
 * Without the restart rule the mean would be 0.406 after push 2 and 0.358
 * after push 15, instead of 0.625 and 2.5. */
TEST(drift_hand_computed_sequence) {
    static const struct {
        int error;
        double w[ALTS];
    } steps[] = {
        {0, {0.75, 0.5}},           {0, {0.75, 0.5}},            {1, {2.0, 3.0}},
        {1, {4.0, 9.0}},            {0, {3.0, 4.5}},             {0, {2.25, 2.25}},
        {0, {1.6875, 1.125}},       {0, {1.265625, 0.5625}},     {1, {2.53125, 3.0}},
        {0, {1.8984375, 1.5}},      {0, {1.423828125, 0.75}},    {0, {1.06787109375, 0.5}},
        {0, {0.8009033203125, 0.5}}, {0, {0.75, 0.5}},           {1, {2.0, 3.0}}
    };
    gateDriftBets b;
    gateDrift d;
    size_t i;
    int k;

    gateDriftBetsInit(&b, 0.2);
    gateDriftInit(&d);
    for (i = 0; i < COUNTOF(steps); i++) {
        CHECK_EQ_INT(gateDriftPush(&d, &b, steps[i].error, log(1000.0)), 0);
        for (k = 0; k < ALTS; k++) CHECK_NEAR(d.logw[k], log(steps[i].w[k]), 1e-12);
        CHECK_NEAR(gateDriftLogStat(&d), log(0.5 * (steps[i].w[0] + steps[i].w[1])), 1e-12);
    }
    /* Two of them spelled out: log(2.765625) after push 9 and
     * log(0.65045166015625) after push 13. */
    gateDriftInit(&d);
    for (i = 0; i < 9; i++) gateDriftPush(&d, &b, steps[i].error, log(1000.0));
    CHECK_NEAR(gateDriftLogStat(&d), 1.0172666492141572855e+0, 1e-12);
    for (; i < 13; i++) gateDriftPush(&d, &b, steps[i].error, log(1000.0));
    CHECK_NEAR(gateDriftLogStat(&d), -4.3008829561827816301e-1, 1e-12);
    for (; i < COUNTOF(steps); i++) gateDriftPush(&d, &b, steps[i].error, log(1000.0));
    CHECK_EQ_INT(d.n, 15);
    CHECK_EQ_INT(d.errors, 4);
}

/* Push 'lead' correct spot checks on a fresh detector and then one error
 * every 'every' spot checks, starting with an error. Returns the push that
 * raised the alarm at level 1000 (counted from the very first push), 0 if
 * none of the first 100000 did. */
static int driftAlarmPush(double alpha, int every, int lead) {
    gateDriftBets b;
    gateDrift d;
    int i;

    gateDriftBetsInit(&b, alpha);
    gateDriftInit(&d);
    for (i = 1; i <= 100000; i++) {
        int error = i > lead && (i - lead - 1) % every == 0;

        if (gateDriftPush(&d, &b, error, log(1000.0))) return i;
    }
    return 0;
}

/* The exact push of the alarm, from the definition in exact rational
 * arithmetic. In every case the mean statistic is at least 0.3% away from
 * 1000 on both sides of the alarm, far beyond any rounding.
 *
 * A run of errors multiplies the two statistics by 2 and 4 whenever neither
 * alternative is capped (alpha <= 1/7): their mean is (2^n + 4^n) / 2, that
 * is 528 after 5 errors and 2080 after 6. */
TEST(drift_alarm_push) {
    CHECK(GATE_DRIFT_ALARM == 1000.0);

    CHECK_EQ_INT(driftAlarmPush(0.02, 1, 0), 6);
    CHECK_EQ_INT(driftAlarmPush(0.05, 1, 0), 6);
    /* alpha = 0.3: factors 2 and 13/6, mean 782.1 after 9 errors and 1651.9
     * after 10. alpha = 0.6: both factors are 4/3, and (4/3)^24 = 996.6 while
     * (4/3)^25 = 1328.8. */
    CHECK_EQ_INT(driftAlarmPush(0.3, 1, 0), 10);
    CHECK_EQ_INT(driftAlarmPush(0.6, 1, 0), 25);

    /* Half of the answers wrong, "error, correct, error, ...": the correct
     * ones cost more at alpha = 0.05 (16/19 against 46/49 at 0.02). */
    CHECK_EQ_INT(driftAlarmPush(0.02, 2, 0), 11);
    CHECK_EQ_INT(driftAlarmPush(0.05, 2, 0), 13);
    /* One answer in three wrong. */
    CHECK_EQ_INT(driftAlarmPush(0.02, 3, 0), 16);
    CHECK_EQ_INT(driftAlarmPush(0.05, 3, 0), 19);

    /* However long the policy was fine before, the alarm takes the same six
     * errors: the past is no credit. */
    CHECK_EQ_INT(driftAlarmPush(0.02, 1, 300), 306);
    CHECK_EQ_INT(driftAlarmPush(0.05, 1, 300), 306);
    CHECK_EQ_INT(driftAlarmPush(0.05, 1, 50000), 50006);
}

/* "Reached" means >=: an alarm level equal to the statistic fires, one ulp
 * above it does not. */
TEST(drift_alarm_boundary) {
    gateDriftBets b;
    gateDrift probe, d;
    double stat;

    gateDriftBetsInit(&b, 0.05);
    gateDriftInit(&probe);
    gateDriftPush(&probe, &b, 1, INFINITY);
    gateDriftPush(&probe, &b, 1, INFINITY);
    stat = gateDriftLogStat(&probe);
    CHECK_NEAR(stat, log(10.0), 1e-14); /* (4 + 16) / 2 */

    gateDriftInit(&d);
    CHECK_EQ_INT(gateDriftPush(&d, &b, 1, stat), 0); /* log(3) < log(10) */
    CHECK_EQ_INT(gateDriftPush(&d, &b, 1, stat), 1);
    gateDriftInit(&d);
    gateDriftPush(&d, &b, 1, stat);
    CHECK_EQ_INT(gateDriftPush(&d, &b, 1, nextafter(stat, INFINITY)), 0);
    gateDriftInit(&d);
    gateDriftPush(&d, &b, 1, stat);
    CHECK_EQ_INT(gateDriftPush(&d, &b, 1, nextafter(stat, -INFINITY)), 1);

    /* The alarm is a level, not a latch held by the detector: the same
     * state is below a higher level. */
    CHECK_EQ_INT(gateDriftPush(&d, &b, 0, log(1000.0)), 0);
}

/* A policy that is never wrong never alarms, and its statistic does not
 * creep: every correct spot check restarts from 1 and lands on the mean of
 * the two "correct" factors, (46/49 + 48/49) / 2 at alpha = 0.02 and
 * (18/19 + 16/19) / 2 = 17/19 at alpha = 0.05. */
TEST(drift_all_correct_never_alarms) {
    static const struct {
        double alpha, stat; /* stat = 0: no reference, only the properties */
    } cases[] = {
        {0.02, -4.1672696400568024294e-2}, {0.05, -1.1122563511022438663e-1},
        {1e-9, 0.0}, {0.01, 0.0}, {0.3, 0.0}, {0.6, 0.0}, {0.99, 0.0}
    };
    size_t i;
    int n;

    for (i = 0; i < COUNTOF(cases); i++) {
        gateDriftBets b;
        gateDrift d;
        double first = 0.0, highest = -INFINITY;
        int alarms = 0, moved = 0;

        gateDriftBetsInit(&b, cases[i].alpha);
        gateDriftInit(&d);
        for (n = 1; n <= 100000; n++) {
            double stat;

            /* Not even at a level barely above "no evidence". */
            alarms += gateDriftPush(&d, &b, 0, n % 2 ? log(1000.0) : 1e-12);
            stat = gateDriftLogStat(&d);
            if (n == 1) first = stat;
            if (stat != first) moved++;
            if (stat > highest) highest = stat;
        }
        CHECK_EQ_INT(alarms, 0);
        CHECK(highest <= 0.0);
        CHECK_EQ_INT(moved, 0);
        CHECK(first < 0.0);
        if (cases[i].stat != 0.0) CHECK_NEAR(first, cases[i].stat, 1e-14);
        CHECK_EQ_INT(d.n, 100000);
        CHECK_EQ_INT(d.errors, 0);
    }
}

/* The header's definition taken literally, in linear space:
 * W <- max(W, 1) * e(E) for each alternative, with e(1) = p1 / alpha and
 * e(0) = (1 - p1) / (1 - alpha). Returns the mean of the alternatives. */
typedef struct linearDrift {
    double w[ALTS], e_ok[ALTS], e_err[ALTS];
} linearDrift;

static void linearDriftInit(linearDrift *l, double alpha) {
    static const double multiple[ALTS] = {2.0, 4.0};
    int k;

    for (k = 0; k < ALTS; k++) {
        double p1 = multiple[k] * alpha;

        if (p1 > (1.0 + alpha) / 2.0) p1 = (1.0 + alpha) / 2.0;
        l->w[k] = 1.0;
        l->e_err[k] = p1 / alpha;
        l->e_ok[k] = (1.0 - p1) / (1.0 - alpha);
    }
}

static double linearDriftPush(linearDrift *l, int error) {
    double mean = 0.0;
    int k;

    for (k = 0; k < ALTS; k++) {
        if (l->w[k] < 1.0) l->w[k] = 1.0;
        l->w[k] *= error ? l->e_err[k] : l->e_ok[k];
        mean += l->w[k] / ALTS;
    }
    return mean;
}

/* The log space detector against the linear one on long random streams whose
 * error rate moves between fine and bad, so that statistics restart, build
 * up and alarm many times. After an alarm both start afresh, as the gate
 * does with the generation it drops. */
TEST(drift_matches_linear_reference) {
    static const double alphas[] = {0.01, 0.05, 0.3, 0.6};
    const double log_alarm = log(1000.0);
    size_t a;
    int i, k;

    for (a = 0; a < COUNTOF(alphas); a++) {
        double alpha = alphas[a], worst = 0.0, closest = INFINITY;
        uint64_t rng = 0xd21f7 + a;
        int alarms = 0, wrong_alarm = 0, below_floor = 0, restarts = 0;
        gateDriftBets b;
        gateDrift d;
        linearDrift l;

        gateDriftBetsInit(&b, alpha);
        gateDriftInit(&d);
        linearDriftInit(&l, alpha);
        for (i = 0; i < 200000; i++) {
            double p = (i / 2000) % 4 == 3 ? alpha + 0.5 * (1.0 - alpha) : 0.8 * alpha;
            int e = gate_test_uniform(&rng) < p;
            double mean = linearDriftPush(&l, e);
            int alarm = gateDriftPush(&d, &b, e, log_alarm);
            double diff = fabs(gateDriftLogStat(&d) - log(mean));

            if (!(diff <= worst)) worst = diff;
            if (fabs(log(mean) - log_alarm) < closest) closest = fabs(log(mean) - log_alarm);
            if (alarm != (mean >= 1000.0)) wrong_alarm++;
            for (k = 0; k < ALTS; k++) {
                /* A statistic is at least 1 when it is updated, so it never
                 * ends up below one "correct" factor. */
                if (!(d.logw[k] >= b.inc_ok[k])) below_floor++;
                if (l.w[k] < 1.0) restarts++;
            }
            if (alarm) {
                alarms++;
                gateDriftInit(&d);
                linearDriftInit(&l, alpha);
            }
        }
        CHECK(worst <= 1e-12);
        CHECK_EQ_INT(wrong_alarm, 0);
        CHECK_EQ_INT(below_floor, 0);
        /* The comparison of the alarms was never a matter of rounding, and
         * the stream exercised all of it. */
        CHECK(closest > 1e-7);
        CHECK(alarms >= 20);
        CHECK(restarts > 10000);
    }
}

/* What the detector saw before can only bring the alarm forward. On the
 * same stream, one detector watches from the start and another one starts
 * fresh somewhere in the middle: from then on the old one is never behind,
 * alternative by alternative, and it never alarms later. (This is why the
 * delay of a fresh detector is the worst case.) */
TEST(drift_history_never_delays_the_alarm) {
    static const double alphas[] = {0.02, 0.05, 0.3};
    size_t a;
    int trial, i, k;

    for (a = 0; a < COUNTOF(alphas); a++) {
        uint64_t rng = 0x51de + a;
        int behind = 0, late = 0, sooner = 0;
        gateDriftBets b;

        gateDriftBetsInit(&b, alphas[a]);
        for (trial = 0; trial < 300; trial++) {
            int start = 1 + (int)(gate_test_rand64(&rng) % 400), old_alarm = 0, fresh_alarm = 0;
            gateDrift old, fresh;

            gateDriftInit(&old);
            gateDriftInit(&fresh);
            /* The error rate is at the target, then twice the target. */
            for (i = 1; i <= 3000 && !fresh_alarm; i++) {
                int e = gate_test_uniform(&rng) < (i < start ? 1.0 : 2.0) * alphas[a];

                if (gateDriftPush(&old, &b, e, log(1000.0)) && !old_alarm) old_alarm = i;
                if (i < start) continue;
                if (gateDriftPush(&fresh, &b, e, log(1000.0))) fresh_alarm = i;
                for (k = 0; k < ALTS; k++) {
                    if (!(old.logw[k] >= fresh.logw[k])) behind++;
                }
            }
            if (fresh_alarm && !(old_alarm && old_alarm <= fresh_alarm)) late++;
            if (old_alarm && (!fresh_alarm || old_alarm < fresh_alarm)) sooner++;
        }
        CHECK_EQ_INT(behind, 0);
        CHECK_EQ_INT(late, 0);
        CHECK(sooner > 0); /* and the history did matter in some trials */
    }
}

/* Arguments outside the documented domain must raise the alarm, never
 * switch the detector off. */
TEST(drift_fails_closed) {
    const double bad[] = {0.0, -0.0, 1.0, -0.1, 1.5, 1e300, -1e300, NAN, INFINITY, -INFINITY};
    gateDriftBets b;
    gateDrift d;
    size_t i;
    int k, e;

    /* An alpha that is not a risk target: the very first spot check alarms,
     * whatever it is and whatever the level, and so does every later one. */
    for (i = 0; i < COUNTOF(bad); i++) {
        gateDriftBetsInit(&b, bad[i]);
        for (k = 0; k < ALTS; k++) CHECK(isnan(b.inc_ok[k]) && isnan(b.inc_err[k]));
        for (e = 0; e <= 1; e++) {
            gateDriftInit(&d);
            CHECK_EQ_INT(gateDriftPush(&d, &b, e, log(1000.0)), 1);
            CHECK_EQ_INT(gateDriftPush(&d, &b, !e, INFINITY), 1);
            CHECK_EQ_INT(gateDriftPush(&d, &b, 0, DBL_MAX), 1);
            CHECK_EQ_INT(d.n, 3);
            CHECK_EQ_INT(d.errors, 1);
        }
    }

    /* A level that is not a number cannot vouch for anything: it alarms on
     * a correct spot check of a healthy detector. So does a level that is
     * already reached. An infinite level is never reached. */
    gateDriftBetsInit(&b, 0.05);
    gateDriftInit(&d);
    CHECK_EQ_INT(gateDriftPush(&d, &b, 0, NAN), 1);
    CHECK_EQ_INT(gateDriftPush(&d, &b, 0, log(1000.0)), 0);
    CHECK_EQ_INT(gateDriftPush(&d, &b, 0, -INFINITY), 1);
    CHECK_EQ_INT(gateDriftPush(&d, &b, 0, -1.0), 1); /* log(17/19) = -0.11 */
    for (k = 0; k < 1000; k++) CHECK_EQ_INT(gateDriftPush(&d, &b, 1, INFINITY), 0);
    CHECK(isfinite(gateDriftLogStat(&d)));
    CHECK_NEAR(gateDriftLogStat(&d), 1000.0 * log(4.0) - log(2.0), 1e-9);

    /* A corrupted statistic is not finite: alarm, and it does not heal. */
    for (k = 0; k < ALTS; k++) {
        gateDriftInit(&d);
        d.logw[k] = NAN;
        CHECK_EQ_INT(gateDriftPush(&d, &b, 0, log(1000.0)), 1);
        CHECK(isnan(gateDriftLogStat(&d)));
        CHECK_EQ_INT(gateDriftPush(&d, &b, 0, INFINITY), 1);

        gateDriftInit(&d);
        d.logw[k] = INFINITY;
        CHECK_EQ_INT(gateDriftPush(&d, &b, 0, log(1000.0)), 1);
        CHECK_EQ_INT(gateDriftPush(&d, &b, 0, INFINITY), 1);

        /* Minus infinity is only "far below 1": it restarts like any other
         * statistic below 1. */
        gateDriftInit(&d);
        d.logw[k] = -INFINITY;
        CHECK_EQ_INT(gateDriftPush(&d, &b, 0, log(1000.0)), 0);
        CHECK(d.logw[k] == b.inc_ok[k]);
    }
}

/* An alternative has to be a probability above alpha and below 1. For the
 * largest double below 1 there is none: halfway between alpha and 1 rounds
 * to 1 itself, and an alternative of 1 would make a correct spot check
 * impossible under it (log 0), which reads as an alarm on every correct
 * answer. Such an alternative stays silent instead: both its increments
 * are 0, and the detector of this alpha never moves, whatever it is fed.
 *
 * One step further down there is exactly one double between alpha and 1,
 * and it is the alternative: with alpha = 1 - 2^-52 and p1 = 1 - 2^-53 a
 * correct answer multiplies by (1 - p1) / (1 - alpha) = 1/2 and an error by
 * p1 / alpha, which is above 1 by about 2^-53. */
TEST(drift_alpha_next_to_one) {
    const double top = 1.0 - DBL_EPSILON / 2, second = 1.0 - DBL_EPSILON;
    gateDriftBets b;
    gateDrift d;
    int k, n, alarms = 0;

    CHECK(top < 1.0 && nextafter(top, 2.0) == 1.0);
    CHECK(second < top && nextafter(second, 2.0) == top);

    gateDriftBetsInit(&b, top);
    for (k = 0; k < ALTS; k++) {
        CHECK(b.inc_err[k] == 0.0);
        CHECK(b.inc_ok[k] == 0.0);
    }
    gateDriftInit(&d);
    for (n = 0; n < 2000; n++) alarms += gateDriftPush(&d, &b, n >= 1000, log(1000.0));
    CHECK_EQ_INT(alarms, 0);            /* not on a correct answer, not on an error */
    CHECK(gateDriftLogStat(&d) == 0.0);
    CHECK_EQ_INT(d.n, 2000);
    CHECK_EQ_INT(d.errors, 1000);

    gateDriftBetsInit(&b, second);
    for (k = 0; k < ALTS; k++) {
        CHECK(b.inc_err[k] > 0.0 && b.inc_err[k] < 4.0 * DBL_EPSILON);
        /* The difference of two logarithms near -36: 1e-13. */
        CHECK_NEAR(b.inc_ok[k], -0.69314718055994530942, 1e-13);
    }
    gateDriftInit(&d);
    alarms = 0;
    for (n = 0; n < 2000; n++) alarms += gateDriftPush(&d, &b, n >= 1000, log(1000.0));
    CHECK_EQ_INT(alarms, 0);
    CHECK(isfinite(gateDriftLogStat(&d)));
    /* A thousand errors in a row move it, by a thousand times next to
     * nothing. */
    CHECK(gateDriftLogStat(&d) > 0.0 && gateDriftLogStat(&d) < 1e-11);
}

/* No hidden state: the same spot checks give the same detector, bit for
 * bit, and the same alarms. */
TEST(drift_determinism) {
    gateDriftBets b1, b2;
    gateDrift d1, d2;
    uint64_t rng = 20261128;
    int i, differ = 0, alarms = 0;

    gateDriftBetsInit(&b1, 0.02);
    memset(&b2, 0xff, sizeof(b2));
    gateDriftBetsInit(&b2, 0.02);
    CHECK(memcmp(&b1, &b2, sizeof(b1)) == 0);

    memset(&d1, 0xff, sizeof(d1));
    gateDriftInit(&d1);
    gateDriftInit(&d2);
    for (i = 0; i < 50000; i++) {
        int e = gate_test_uniform(&rng) < 0.03;
        int a1 = gateDriftPush(&d1, &b1, e, log(1000.0));
        int a2 = gateDriftPush(&d2, &b2, e, log(1000.0));

        if (a1 != a2 || memcmp(&d1, &d2, sizeof(d1)) != 0) differ++;
        if (gateDriftLogStat(&d1) != gateDriftLogStat(&d2)) differ++;
        alarms += a1;
    }
    CHECK_EQ_INT(differ, 0);
    CHECK(alarms > 0);
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
    RUN(bets_are_fair_at_the_boundary);
    RUN(mixture_wealth_is_a_martingale_at_the_boundary);
    RUN(exhaustive_certification_probability);
    RUN(high_error_stream_never_certifies);
    RUN(log_wealth_extremes);
    RUN(non_finite_wealth_never_certifies);
    RUN(invalid_alpha_never_certifies);
    RUN(certifies_boundary);
    RUN(certifies_agrees_with_log_wealth);
    RUN(flip_error_then_flip_is_correct);
    RUN(flip_counters);
    RUN(flip_without_an_error_does_nothing);
    RUN(flip_hand_computed);
    RUN(flip_commutes_with_later_updates);
    RUN(flip_long_random_sequences);
    RUN(flip_depends_only_on_the_counts);
    RUN(flip_never_cures_a_non_finite_wealth);
    RUN(flip_outstanding_answers_never_add_wealth);
    RUN(flip_exhaustive_late_answers);
    RUN(flip_cost_of_outstanding_answers);
    RUN(budget_generation_delta);
    RUN(budget_sum_never_exceeds_total);
    RUN(budget_log_threshold);
    RUN(budget_fails_closed);
    RUN(grid_end_points_and_order);
    RUN(grid_closed_form);
    RUN(grid_other_ranges);
    RUN(grid_invalid_arguments);
    RUN(grid_count);
    RUN(drift_increments);
    RUN(drift_init_and_counters);
    RUN(drift_hand_computed_sequence);
    RUN(drift_alarm_push);
    RUN(drift_alarm_boundary);
    RUN(drift_all_correct_never_alarms);
    RUN(drift_matches_linear_reference);
    RUN(drift_history_never_delays_the_alarm);
    RUN(drift_fails_closed);
    RUN(drift_alpha_next_to_one);
    RUN(drift_determinism);
    return TEST_REPORT();
}
