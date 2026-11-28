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
 *
 * The counts of gateBetsFlawless() were decided with exact rationals (the
 * wealth of n right answers against g (g + 1) regions / delta). For the
 * drift detector see the comment at the head of its section: a model that
 * keeps sets instead of counters, exact rationals for every alarm count,
 * an exact recursion for the alarm probabilities of a block.
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
        /* The count in closed form: what the gate sizes a snapshot's
         * budget with. */
        if (!late) CHECK(gateBetsFlawless(&b, thr) == (uint64_t)expected);
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

/* ------------ The proof a flawless threshold needs: gateBetsFlawless ------
 *
 * The smallest n >= 1 with log((1/4) sum_j exp(n inc_ok[j])) >= threshold:
 * how many right answers in a row take a fresh region to the threshold. The
 * gate sizes the budget of a classifier snapshot with it (ten times this,
 * for each label it may answer).
 *
 * The reference counts were decided in exact rational arithmetic: the
 * wealth (1/4) sum_j (1 + c_j alpha / (1 - alpha))^n against
 * g (g + 1) regions / delta, or against exp(threshold) in 80 digits. The
 * margins on both sides of each count are far beyond rounding, except
 * where noted. zero_error_certification_n checks the same function against
 * its 42 counts, which were derived on their own. */

/* log((1/4) sum_j exp(n inc_ok[j])) from the reference increments of
 * ref_inc_ok[a]. */
static double flawlessWealth(size_t a, double n) {
    double lw[GATE_NBETS], m = -INFINITY, sum = 0.0;
    int j;

    for (j = 0; j < GATE_NBETS; j++) {
        lw[j] = n * ref_inc_ok[a].inc_ok[j];
        if (lw[j] > m) m = lw[j];
    }
    for (j = 0; j < GATE_NBETS; j++) sum += exp(lw[j] - m);
    return m + log(0.25 * sum);
}

TEST(flawless_at_the_bar) {
    static const struct {
        double alpha, delta;
        uint64_t g;
        uint32_t regions;
        uint64_t n;
    } cases[] = {
        /* The two counts of the candidate budget: a classifier with two
         * labels at ALPHA 0.2 and DELTA 0.2, first and second snapshot.
         * (1/4) sum_j (1 + c_j / 4)^39 = 712 >= 640 = 2 * 64 / 0.2, and
         * 584 with 38; the second bar is 6 * 64 / 0.2 = 1920. */
        {0.2, 0.2, 1, 64, 39},
        {0.2, 0.2, 2, 64, 44},
        {0.2, 0.2, 3, 64, 48},
        {0.2, 0.2, 1, 32, 35},
        {0.2, 0.01, 1, 64, 54},
        {0.05, 0.01, 1, 32, 220},
        {0.05, 0.01, 1, 64, 235},
        {0.05, 0.01, 2, 64, 258},
        {0.05, 0.01, 1, 2048, 310},
        {0.02, 0.01, 1, 64, 596},
        {0.01, 0.01, 1, 64, 1198},
        {0.01, 0.01, 7, 96, 1612},
        {0.001, 0.01, 1, 64, 12033},
        /* A count that no loop over samples would want to find: it clears
         * the bar by 4.9e-7 and misses it by 4.1e-7 one sample earlier, a
         * hundred million times the rounding of one product. */
        {1e-06, 0.01, 1, 64, 12038842},
        {1e-09, 0.01, 1, 32, 11265102604ull},
        {0.5, 0.01, 1, 64, 17},
        {0.9, 0.01, 1, 64, 5},
        {0.99, 0.01, 1, 64, 3},
        {0.999, 0.01, 1, 64, 2},
        /* One right answer is worth more than the whole bar. */
        {0.999999, 0.01, 1, 64, 1},
        {0.3, 0.05, 4, 128, 38},
        {0.07, 0.3, 1000, 640, 349}
    };
    size_t i;

    for (i = 0; i < COUNTOF(cases); i++) {
        double thr = gateBudgetLogThreshold(cases[i].delta, cases[i].g, cases[i].regions);
        gateBets b;

        gateBetsInit(&b, cases[i].alpha);
        CHECK(gateBetsFlawless(&b, thr) == cases[i].n);

        /* It is what a region needs: one right answer less is not there. */
        if (cases[i].n <= 20000) {
            gateRegion r;
            uint64_t k;

            gateRegionInit(&r);
            for (k = 1; k < cases[i].n; k++) gateRegionUpdate(&r, &b, 0);
            CHECK(!gateRegionCertifies(&r, thr));
            gateRegionUpdate(&r, &b, 0);
            CHECK(gateRegionCertifies(&r, thr));
        }
    }
}

TEST(flawless_any_threshold) {
    static const struct {
        double alpha, thr;
        uint64_t n;
    } cases[] = {
        /* One right answer at alpha = 0.05 is a log wealth of 0.0227652
         * (see hand_computed_alpha_005): enough for anything up to that. */
        {0.05, 1e-300, 1},
        {0.05, 0.0227, 1},
        {0.05, 0.0228, 2},
        {0.05, 1.0, 37},
        {0.05, 100.0, 2191},
        {0.05, 1e6, 21607285},
        /* alpha = 0.5: one right answer multiplies the wealth by 1.4375
         * (log 0.3629), two by 2.158 (log 0.7692). */
        {0.5, 0.2, 1},
        {0.5, 0.3, 1},
        {0.5, 0.37, 2},
        {0.5, 0.76, 2},
        {0.5, 0.77, 3},
        {0.5, 5.0, 10},
        {0.999, 1.0, 1},
        {0.999, 6.0, 1},
        {0.999, 7.0, 2},
        {0.2, 6.5, 39},
        /* 1e13 right answers: the count clears the threshold by 2.9e-13
         * and misses it by 6.1e-13 one earlier, still a hundred times the
         * rounding of the one product and the logarithm involved. */
        {1e-12, 8.0, 10410557241132ull}
    };
    size_t i;
    gateBets b;
    uint64_t n;

    for (i = 0; i < COUNTOF(cases); i++) {
        gateBetsInit(&b, cases[i].alpha);
        CHECK(gateBetsFlawless(&b, cases[i].thr) == cases[i].n);
    }

    /* alpha = 1e-15 and a threshold of 20: 23762466314379272 right answers,
     * give or take the rounding of the wealth at that point (the margin
     * there is 4e-16 on either side, less than an ulp of 20): the count is
     * right to 1e-13 of itself. */
    gateBetsInit(&b, 1e-15);
    n = gateBetsFlawless(&b, 20.0);
    CHECK(n > 23762466314379272ull - 2400 && n < 23762466314379272ull + 2400);
}

/* The definition, restated: at the returned n the wealth of n right
 * answers is at the threshold, at n - 1 it is not; a higher threshold or a
 * smaller alpha never needs fewer answers. The wealth is evaluated here
 * from the reference increments; 1e-11 covers what separates them from the
 * library's (1e-14 of a log wealth of at most 40) and the rounding of the
 * logarithms, and is a hundredth of the smallest step there is here (one
 * answer at alpha = 1e-9 adds 9e-10). */
TEST(flawless_is_the_first_n_at_the_threshold) {
    uint64_t rng = 0xf1a37;
    size_t a;
    int i, bad = 0, order = 0, bracket = 0;

    for (a = 0; a < COUNTOF(ref_inc_ok); a++) {
        gateBets b;
        uint64_t prev = 0;

        gateBetsInit(&b, ref_inc_ok[a].alpha);
        for (i = 0; i < 400; i++) {
            /* Thresholds from 0.01 to 40, increasing. */
            double thr = 0.01 + 0.1 * i + 0.09 * gate_test_uniform(&rng);
            uint64_t n = gateBetsFlawless(&b, thr);
            double best = ref_inc_ok[a].inc_ok[GATE_NBETS - 1];

            if (n == UINT64_MAX || n == 0) {
                bad++;
                continue;
            }
            if (!(flawlessWealth(a, (double)n) >= thr - 1e-11)) bad++;
            if (n > 1 && !(flawlessWealth(a, (double)(n - 1)) < thr + 1e-11)) bad++;
            if (n < prev) order++;
            prev = n;
            /* The mixture is between a quarter of its best bet and its
             * best bet. */
            if (!((double)n >= thr / best - 1.0 && (double)n <= (thr + log(4.0)) / best + 2.0)) bracket++;
        }
    }
    CHECK_EQ_INT(bad, 0);
    CHECK_EQ_INT(order, 0);
    CHECK_EQ_INT(bracket, 0);

    /* A stricter risk target needs more right answers, about in proportion. */
    {
        static const double alphas[] = {0.5, 0.2, 0.1, 0.05, 0.02, 0.01, 0.001, 1e-6};
        uint64_t prev = 0;
        size_t k;

        for (k = 0; k < COUNTOF(alphas); k++) {
            gateBets b;
            uint64_t n;

            gateBetsInit(&b, alphas[k]);
            n = gateBetsFlawless(&b, gateBudgetLogThreshold(0.01, 1, 64));
            CHECK(n > prev);
            prev = n;
        }
    }
}

/* What is not a count is UINT64_MAX, never a small number: the gate would
 * drop every snapshot at once. */
TEST(flawless_fails_closed) {
    static const double bad_alpha[] = {0.0, -0.0, 1.0, -0.1, 1.5, 1e300};
    gateBets b, bad;
    size_t i;

    gateBetsInit(&b, 0.05);
    /* A threshold that is already met needs the one answer that "n >= 1"
     * asks for. */
    CHECK(gateBetsFlawless(&b, 0.0) == 1);
    CHECK(gateBetsFlawless(&b, -0.0) == 1);
    CHECK(gateBetsFlawless(&b, -1.0) == 1);
    CHECK(gateBetsFlawless(&b, -DBL_MAX) == 1);
    CHECK(gateBetsFlawless(&b, DBL_MIN) == 1);

    /* "Reached" means >=. Four equal increments of exactly 1 make the
     * wealth of n right answers exactly n: a threshold of 5 is met by the
     * fifth, one ulp more needs the sixth. */
    {
        gateBets even;
        int j;

        gateBetsInit(&even, 0.5);
        for (j = 0; j < GATE_NBETS; j++) even.inc_ok[j] = 1.0;
        CHECK(gateBetsFlawless(&even, 5.0) == 5);
        CHECK(gateBetsFlawless(&even, nextafter(5.0, INFINITY)) == 6);
        CHECK(gateBetsFlawless(&even, nextafter(5.0, -INFINITY)) == 5);
        CHECK(gateBetsFlawless(&even, 1.0) == 1);
        CHECK(gateBetsFlawless(&even, 1000000.0) == 1000000);
    }

    /* Not numbers. Minus infinity is not a threshold either (it is what a
     * log of a zero budget would be). */
    CHECK(gateBetsFlawless(&b, NAN) == UINT64_MAX);
    CHECK(gateBetsFlawless(&b, INFINITY) == UINT64_MAX);
    CHECK(gateBetsFlawless(&b, -INFINITY) == UINT64_MAX);
    /* The bar of a budget that does not exist (gateBudgetLogThreshold()
     * returns +inf for it). */
    CHECK(gateBetsFlawless(&b, gateBudgetLogThreshold(0.01, 0, 64)) == UINT64_MAX);

    /* Bets that are not a risk target: every increment is NaN. */
    for (i = 0; i < COUNTOF(bad_alpha) + 1; i++) {
        gateBetsInit(&bad, i < COUNTOF(bad_alpha) ? bad_alpha[i] : NAN);
        CHECK(gateBetsFlawless(&bad, 8.76) == UINT64_MAX);
        CHECK(gateBetsFlawless(&bad, 0.0) == UINT64_MAX);
        CHECK(gateBetsFlawless(&bad, -1.0) == UINT64_MAX);
        CHECK(gateBetsFlawless(&bad, NAN) == UINT64_MAX);
    }

    /* A count that does not fit: 1e301 right answers at alpha = 1e-300,
     * 2e301 for a threshold of 1e300. */
    gateBetsInit(&bad, 1e-300);
    CHECK(gateBetsFlawless(&bad, gateBudgetLogThreshold(0.01, 1, 64)) == UINT64_MAX);
    gateBetsInit(&bad, DBL_MIN);
    CHECK(gateBetsFlawless(&bad, 1.0) == UINT64_MAX);
    CHECK(gateBetsFlawless(&b, 1e300) == UINT64_MAX);
    CHECK(gateBetsFlawless(&b, DBL_MAX) == UINT64_MAX);
    /* ... and one that does, at the far end: alpha = 1e-17 needs about
     * 1.2e18 right answers for the bar of a first snapshot with two
     * labels (between bar / inc_ok[3] = 1.05e18 and log 4 more). */
    {
        gateBets tiny;
        double thr = gateBudgetLogThreshold(0.01, 1, 64);
        uint64_t n;

        gateBetsInit(&tiny, 1e-17);
        n = gateBetsFlawless(&tiny, thr);
        CHECK(n != UINT64_MAX);
        CHECK((double)n >= thr / tiny.inc_ok[GATE_NBETS - 1] * (1.0 - 1e-12));
        CHECK((double)n <= (thr + log(4.0)) / tiny.inc_ok[GATE_NBETS - 1] * (1.0 + 1e-12));
        CHECK(n > 1000000000000000000ull && n < ((uint64_t)1 << 62));
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

/* ----------------------------- Drift detector ----------------------------
 *
 * WHAT IS TESTED. The contract of gate_stats.h: spot checks are numbered in
 * the order they are asked and cut into blocks of 64, 512 and 4,096; the
 * statistic of a block is the average over the two alternatives of the
 * product of e() over the spot checks asked in it so far, each counting as
 * right until a disagreeing answer arrives; a disagreement raises WORSE
 * when the statistic of a block it belongs to reaches the alarm level; a
 * second statistic counts lost answers next to the disagreements and
 * raises UNCHECKED at its own level; an answer whose block has ended counts
 * only in the longer blocks still open.
 *
 * WHERE THE EXPECTED NUMBERS COME FROM. A model of the detector that keeps
 * sets (which spot checks were reported wrong, which lost) instead of
 * counters, with the factors p1 / alpha and (1 - p1) / (1 - alpha) as exact
 * rationals (alpha at its binary value, p1 = min(2 alpha or 4 alpha,
 * (1 + alpha) / 2)): every alarm count below was decided by comparing exact
 * rationals with 1000 and with 10^9, every logarithm is 80 digit decimal
 * arithmetic printed with 20 digits, and the alarm probabilities are an
 * exact recursion over (asked in block, wrong) in 34 digit decimals. The
 * tests that compare long random or exhaustive call sequences use the same
 * model written in C below (refDrift): it shares no code and no constant
 * with gate_stats.c. */

#define ALTS GATE_DRIFT_ALTS
#define SCALES GATE_DRIFT_SCALES
#define QUIET GATE_DRIFT_QUIET
#define WORSE GATE_DRIFT_ALARM_WORSE
#define UNCHECKED GATE_DRIFT_ALARM_UNCHECKED

/* log(1000) and log(10^9): the two alarm levels of the gate. */
#define REF_LOG_ALARM 6.9077552789821370521
#define REF_LOG_LOST 20.723265836946411156

/* log(p1 / alpha) and log((1 - p1) / (1 - alpha)) for the alternatives
 * p1 = 2 alpha and p1 = 4 alpha, each at most (1 + alpha) / 2. */
enum { A001, A01, A02, A05, A10, A20, A30, A60, A90 }; /* indexes into ref_drift */
static const struct {
    double alpha;
    double inc_err[ALTS], inc_ok[ALTS];
} ref_drift[] = {
    {0.001, {6.9314718055994530942e-1, 1.3862943611198906188e+0},
            {-1.0015023370895438724e-3, -3.0075210639552849114e-3}},
    /* p1 = 0.02 and 0.04. */
    {0.01,  {6.9314718055994530942e-1, 1.3862943611198906188e+0},
            {-1.0152371464018007439e-2, -3.0771658666753689028e-2}},
    {0.02,  {6.9314718055994530942e-1, 1.3862943611198906188e+0},
            {-2.0619287202735681589e-2, -6.3178901621531611372e-2}},
    /* p1 = 0.1 and 0.2: a disagreement multiplies by 2 and 4, a right
     * answer by 18/19 and 16/19. */
    {0.05,  {6.9314718055994530942e-1, 1.3862943611198906188e+0},
            {-5.4067221270275771048e-2, -1.7185025692665923330e-1}},
    {0.1,   {6.9314718055994530942e-1, 1.3862943611198906188e+0},
            {-1.1778303565638346225e-1, -4.0546510810816441282e-1}},
    /* p1 = 0.4, and 0.8 capped at 0.6: factors 2 and 3, 3/4 and 1/2. */
    {0.2,   {6.9314718055994530942e-1, 1.0986122886681096451e+0},
            {-2.8768207245178095057e-1, -6.9314718055994530942e-1}},
    /* p1 = 0.6, and 1.2 capped at 0.65: factors 2 and 13/6, 4/7 and 1/2. */
    {0.3,   {6.9314718055994530942e-1, 7.7318988823348176371e-1},
            {-5.5961578793542264662e-1, -6.9314718055994530942e-1}},
    /* 1.2 and 2.4, both capped at 0.8: the two alternatives coincide, with
     * factors 4/3 and 1/2. */
    {0.6,   {2.8768207245178095057e-1, 2.8768207245178095057e-1},
            {-6.9314718055994530942e-1, -6.9314718055994530942e-1}},
    /* Both capped at 0.95: factors 19/18 and 1/2. */
    {0.9,   {5.4067221270275754816e-2, 5.4067221270275754816e-2},
            {-6.9314718055994530942e-1, -6.9314718055994530942e-1}}
};

static void driftAskMany(gateDrift *d, uint64_t n) {
    while (n--) gateDriftAsk(d);
}

/* 1 if the detector holds exactly these counters. */
static int driftIs(const gateDrift *d, uint64_t asked, uint64_t errors,
                   uint32_t w0, uint32_t w1, uint32_t w2, uint32_t l0, uint32_t l1, uint32_t l2) {
    return d->asked == asked && d->errors == errors &&
           d->wrong[0] == w0 && d->wrong[1] == w1 && d->wrong[2] == w2 &&
           d->lost[0] == l0 && d->lost[1] == l1 && d->lost[2] == l2;
}

/* What holds for every detector, whatever it was fed: no block counts more
 * answers than it has spot checks, and a block is never longer than its
 * length. */
static int driftBounded(const gateDrift *d) {
    int s;

    for (s = 0; s < SCALES; s++) {
        uint32_t open = gateDriftOpen(d, s);

        if (open > gateDriftBlocks[s] || (uint64_t)open > d->asked) return 0;
        if ((uint64_t)d->wrong[s] + d->lost[s] > open) return 0;
    }
    return 1;
}

/* ... and what holds as long as every spot check is reported at most once:
 * a short block is a part of the longer ones, so it is never longer and its
 * counters are never above theirs, and no block holds more disagreements
 * than were received. */
static int driftConsistent(const gateDrift *d) {
    int s;

    if (!driftBounded(d)) return 0;
    for (s = 1; s < SCALES; s++) {
        if (d->wrong[s] < d->wrong[s - 1] || d->lost[s] < d->lost[s - 1]) return 0;
        if (gateDriftOpen(d, s) < gateDriftOpen(d, s - 1)) return 0;
    }
    return d->wrong[SCALES - 1] <= d->errors && d->errors <= d->asked;
}

/* ------------------------- The model (see above) ------------------------- */

static const uint64_t ref_blocks[SCALES] = {64, 512, 4096};

typedef struct refReport {
    uint64_t number;
    int lost;
} refReport;

typedef struct refDrift {
    size_t a;           /* index into ref_drift: the risk target */
    uint64_t asked;
    uint64_t errors;    /* disagreements reported */
    size_t n;           /* reports so far */
    refReport *rep;     /* caller's storage */
} refDrift;

/* Spot checks asked in the block of scale 's' that is now open, and how
 * many of them were reported wrong and lost: by looking at every report. */
static void refCounts(const refDrift *r, int s, uint32_t *open, uint32_t *wrong, uint32_t *lost) {
    uint64_t first;
    size_t i;

    *open = *wrong = *lost = 0;
    if (r->asked == 0) return;
    first = (r->asked - 1) / ref_blocks[s] * ref_blocks[s] + 1;
    *open = (uint32_t)(r->asked - first + 1);
    for (i = 0; i < r->n; i++) {
        if (r->rep[i].number < first) continue; /* its block has ended */
        if (r->rep[i].lost) (*lost)++; else (*wrong)++;
    }
}

/* log((1/2) sum_k e_k(1)^c e_k(0)^(n - c)): a block of n spot checks, c of
 * them counted against the gate. */
static double refBlockStat(size_t a, uint32_t n, uint32_t c) {
    double lw[ALTS], m, sum = 0.0;
    int k;

    for (k = 0; k < ALTS; k++)
        lw[k] = (double)c * ref_drift[a].inc_err[k] + ((double)n - (double)c) * ref_drift[a].inc_ok[k];
    m = lw[0] > lw[1] ? lw[0] : lw[1];
    for (k = 0; k < ALTS; k++) sum += exp(lw[k] - m);
    return m + log(sum / ALTS);
}

/* The largest statistic of the three open blocks. */
static double refStat(const refDrift *r, int with_lost) {
    double best = 0.0;
    int s;

    if (r->asked == 0) return 0.0;
    for (s = 0; s < SCALES; s++) {
        uint32_t open, wrong, lost;
        double stat;

        refCounts(r, s, &open, &wrong, &lost);
        stat = refBlockStat(r->a, open, wrong + (with_lost ? lost : 0));
        if (s == 0 || stat > best) best = stat;
    }
    return best;
}

/* The answer of spot check 'number' disagreed ('lost' 0) or was given up
 * ('lost' 1). Returns the verdict the header promises: WORSE if the
 * disagreements of a block the answer belongs to reach 'la', else UNCHECKED
 * if disagreements and lost answers of such a block together reach 'll'.
 * '*near' is set when a statistic is within 1e-9 of its level: the verdict
 * is then a matter of rounding and the caller does not compare it. */
static int refAnswer(refDrift *r, uint64_t number, int lost, double la, double ll, int *near) {
    int s, worse = 0, unchecked = 0;

    if (number == 0 || number > r->asked) return QUIET; /* never handed out */
    r->rep[r->n].number = number;
    r->rep[r->n].lost = lost;
    r->n++;
    if (!lost) r->errors++;
    for (s = 0; s < SCALES; s++) {
        uint32_t open, wr, lo;
        double stat;

        if (number < (r->asked - 1) / ref_blocks[s] * ref_blocks[s] + 1) continue;
        refCounts(r, s, &open, &wr, &lo);
        if (!lost) {
            stat = refBlockStat(r->a, open, wr);
            if (fabs(stat - la) < 1e-9) *near = 1;
            if (stat >= la) worse = 1;
        }
        stat = refBlockStat(r->a, open, wr + lo);
        if (fabs(stat - ll) < 1e-9) *near = 1;
        if (stat >= ll) unchecked = 1;
    }
    return worse ? WORSE : (unchecked ? UNCHECKED : QUIET);
}

/* The verdict a detector's state stands for: WORSE if the disagreements of
 * an open block are at 'la', else UNCHECKED if disagreements and lost
 * answers are at 'll'. */
static int driftStateVerdict(const gateDrift *d, const gateDriftBets *b, double la, double ll) {
    if (gateDriftLogStat(d, b, 0) >= la) return WORSE;
    if (gateDriftLogStat(d, b, 1) >= ll) return UNCHECKED;
    return QUIET;
}

/* ------------------------------- The basics ------------------------------ */

TEST(drift_constants) {
    CHECK_EQ_INT(ALTS, 2);
    CHECK_EQ_INT(SCALES, 3);
    CHECK_EQ_INT(gateDriftBlocks[0], 64);
    CHECK_EQ_INT(gateDriftBlocks[1], 512);
    CHECK_EQ_INT(gateDriftBlocks[2], 4096);
    CHECK(GATE_DRIFT_ALARM == 1000.0);
    CHECK(GATE_LOST_ALARM == 1e9);
    /* The levels as the gate passes them. */
    CHECK_NEAR(log(GATE_DRIFT_ALARM), REF_LOG_ALARM, 1e-14);
    CHECK_NEAR(log(GATE_LOST_ALARM), REF_LOG_LOST, 1e-14);
    /* Three different answers; QUIET is the zero one. */
    CHECK_EQ_INT(QUIET, 0);
    CHECK(WORSE != QUIET && UNCHECKED != QUIET && WORSE != UNCHECKED);
}

TEST(drift_increments) {
    /* 1/7 is where 4 alpha meets its cap and 1/3 where 2 alpha does. */
    static const double alphas[] = {1e-12, 1e-6, 0.001, 0.01, 0.02, 0.05, 0.1, 1.0 / 7.0, 0.2,
                                    0.3, 1.0 / 3.0, 0.5, 0.6, 0.9, 0.999999};
    gateDriftBets b, same;
    size_t i;
    int k;

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
    gateDriftBetsInit(&b, 0.2);
    CHECK_NEAR(exp(b.inc_err[0]), 2.0, 1e-14);
    CHECK_NEAR(exp(b.inc_err[1]), 3.0, 1e-14);
    CHECK_NEAR(exp(b.inc_ok[0]), 0.75, 1e-14);
    CHECK_NEAR(exp(b.inc_ok[1]), 0.5, 1e-14);
    gateDriftBetsInit(&b, 0.3);
    CHECK_NEAR(exp(b.inc_err[1]), 13.0 / 6.0, 1e-14);
    CHECK_NEAR(exp(b.inc_ok[0]), 4.0 / 7.0, 1e-14);
    gateDriftBetsInit(&b, 0.6);
    CHECK_NEAR(exp(b.inc_err[0]), 4.0 / 3.0, 1e-14);
    CHECK_NEAR(exp(b.inc_ok[1]), 0.5, 1e-14);

    /* What makes a block's statistic a test of "the error rate is at most
     * alpha". With error rate p the expected factor of one spot check is
     *
     *     p e(1) + (1 - p) e(0) = 1 + (p - alpha) (p1 - alpha) / (alpha (1 - alpha))
     *
     * which is 1 at p = alpha, below 1 for a better policy (the product
     * cannot grow by itself: Ville's inequality bounds the false alarms)
     * and above 1 for a worse one. */
    for (i = 0; i < COUNTOF(alphas); i++) {
        double alpha = alphas[i], better = 0.5 * alpha, worse = alpha + 0.5 * (1.0 - alpha);

        gateDriftBetsInit(&b, alpha);
        for (k = 0; k < ALTS; k++) {
            double e1 = exp(b.inc_err[k]), e0 = exp(b.inc_ok[k]);

            /* A disagreement is evidence of drift, a right answer against
             * it: the order independence of the alarm rests on these two
             * signs (see drift_order_*). */
            CHECK(isfinite(b.inc_err[k]) && b.inc_err[k] > 0.0);
            CHECK(isfinite(b.inc_ok[k]) && b.inc_ok[k] < 0.0);
            CHECK_NEAR(alpha * e1 + (1.0 - alpha) * e0, 1.0, 1e-13);
            CHECK(better * e1 + (1.0 - better) * e0 < 1.0);
            CHECK(worse * e1 + (1.0 - worse) * e0 > 1.0);
        }
        /* The second alternative is never the closer one. */
        CHECK(b.inc_err[1] >= b.inc_err[0]);
        CHECK(b.inc_ok[1] <= b.inc_ok[0]);
        /* "At most halfway from alpha to 1": no alternative is above
         * (1 + alpha) / 2, that is e(0) >= 1/2. */
        CHECK(exp(b.inc_ok[1]) >= 0.5 - 1e-14);
        /* From alpha = 1/3 on both alternatives sit on that cap. */
        if (alpha > 0.34) {
            CHECK(b.inc_err[0] == b.inc_err[1] && b.inc_ok[0] == b.inc_ok[1]);
            /* The alternative is the double next to (1 + alpha) / 2, so
             * 1 - p1 is half of 1 - alpha only to within the spacing of the
             * doubles there, relative to 1 - alpha. */
            CHECK_NEAR(exp(b.inc_ok[0]), 0.5, 1e-14 + 2.0 * DBL_EPSILON / (1.0 - alpha));
            CHECK_NEAR(exp(b.inc_err[0]), (1.0 + alpha) / (2.0 * alpha), 1e-13);
        }
        /* Below 1/7 neither is capped: factors 2 and 4 whatever alpha is. */
        if (alpha < 0.14) {
            CHECK_NEAR(b.inc_err[0], 0.69314718055994530942, 1e-15);
            CHECK_NEAR(b.inc_err[1], 1.3862943611198906188, 1e-15);
        }
        /* No hidden state: the same alpha gives the same bits. */
        memset(&same, 0xff, sizeof(same));
        gateDriftBetsInit(&same, alpha);
        CHECK(memcmp(&b, &same, sizeof(b)) == 0);
    }
}

/* An alternative has to be a probability above alpha and below 1. For the
 * largest double below 1 there is none: halfway between alpha and 1 rounds
 * to 1 itself, and an alternative of 1 would make a right spot check
 * impossible under it (log 0). Such an alternative stays silent: both its
 * increments are 0, and the detector of this alpha never moves, whatever
 * it is fed.
 *
 * One step further down there is exactly one double between alpha and 1,
 * and it is the alternative: with alpha = 1 - 2^-52 and p1 = 1 - 2^-53 a
 * right answer multiplies by (1 - p1) / (1 - alpha) = 1/2 and a
 * disagreement by p1 / alpha, which is above 1 by about 2^-53. */
TEST(drift_alpha_next_to_one) {
    const double top = 1.0 - DBL_EPSILON / 2, second = 1.0 - DBL_EPSILON;
    const double la = log(GATE_DRIFT_ALARM), ll = log(GATE_LOST_ALARM);
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
    /* A block of 4,096 in which every answer disagrees, then one in which
     * every answer is lost: not one alarm, and both statistics stay at 1. */
    gateDriftInit(&d);
    for (n = 0; n < 4096; n++) alarms += gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll) != QUIET;
    CHECK(gateDriftLogStat(&d, &b, 0) == 0.0);
    for (n = 0; n < 4096; n++) alarms += gateDriftLost(&d, &b, gateDriftAsk(&d), ll) != QUIET;
    CHECK_EQ_INT(alarms, 0);
    CHECK(gateDriftLogStat(&d, &b, 0) == 0.0);
    CHECK(gateDriftLogStat(&d, &b, 1) == 0.0);
    CHECK(driftIs(&d, 8192, 4096, 0, 0, 0, 64, 512, 4096));

    gateDriftBetsInit(&b, second);
    for (k = 0; k < ALTS; k++) {
        CHECK(b.inc_err[k] > 0.0 && b.inc_err[k] < 4.0 * DBL_EPSILON);
        /* The difference of two logarithms near -36: 1e-13. */
        CHECK_NEAR(b.inc_ok[k], -0.69314718055994530942, 1e-13);
    }
    gateDriftInit(&d);
    alarms = 0;
    for (n = 0; n < 4096; n++) alarms += gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll) != QUIET;
    CHECK_EQ_INT(alarms, 0);
    /* 4,096 disagreements in one block move it, by 4,096 times next to
     * nothing. */
    CHECK(gateDriftLogStat(&d, &b, 0) > 0.0 && gateDriftLogStat(&d, &b, 0) < 1e-11);
    /* And a right answer halves it. */
    gateDriftAsk(&d);
    CHECK_NEAR(gateDriftLogStat(&d, &b, 0), -0.69314718055994530942, 1e-11);
}

/* Numbers are handed out 1, 2, 3, ... and a block of length L holds the
 * numbers k L + 1 .. (k + 1) L. gateDriftOpen() is the place of the latest
 * spot check in its block. */
TEST(drift_init_and_numbering) {
    static const struct {
        uint64_t asked;
        uint32_t open[SCALES];
    } at[] = {
        {1, {1, 1, 1}},         {2, {2, 2, 2}},          {63, {63, 63, 63}},
        {64, {64, 64, 64}},     /* the last of the first block of 64 */
        {65, {1, 65, 65}},      /* the first of the second */
        {128, {64, 128, 128}},  {129, {1, 129, 129}},
        {511, {63, 511, 511}},  {512, {64, 512, 512}},   {513, {1, 1, 513}},
        {1024, {64, 512, 1024}}, {1025, {1, 1, 1025}},
        {4095, {63, 511, 4095}}, {4096, {64, 512, 4096}}, {4097, {1, 1, 1}},
        {8192, {64, 512, 4096}}, {8193, {1, 1, 1}},      {10000, {16, 272, 1808}}
    };
    gateDriftBets b;
    gateDrift d;
    uint64_t n;
    size_t i = 0;
    int s, wrong_number = 0, wrong_open = 0, moved = 0;

    memset(&d, 0xff, sizeof(d));
    gateDriftInit(&d);
    CHECK(driftIs(&d, 0, 0, 0, 0, 0, 0, 0, 0));
    for (s = 0; s < SCALES; s++) CHECK_EQ_INT(gateDriftOpen(&d, s), 0);
    /* Statistic 1 before any spot check, with no rounding. */
    gateDriftBetsInit(&b, 0.05);
    CHECK(gateDriftLogStat(&d, &b, 0) == 0.0);
    CHECK(gateDriftLogStat(&d, &b, 1) == 0.0);

    for (n = 1; n <= 10000; n++) {
        if (gateDriftAsk(&d) != n || d.asked != n) wrong_number++;
        for (s = 0; s < SCALES; s++) {
            if (gateDriftOpen(&d, s) != (n - 1) % ref_blocks[s] + 1) wrong_open++;
        }
        /* Asking counts nothing against the gate. */
        if (!driftIs(&d, n, 0, 0, 0, 0, 0, 0, 0)) moved++;
        if (i < COUNTOF(at) && at[i].asked == n) {
            for (s = 0; s < SCALES; s++) CHECK_EQ_INT(gateDriftOpen(&d, s), at[i].open[s]);
            i++;
        }
    }
    CHECK_EQ_INT(wrong_number, 0);
    CHECK_EQ_INT(wrong_open, 0);
    CHECK_EQ_INT(moved, 0);
    CHECK_EQ_INT(i, COUNTOF(at));

    /* The numbers are 64 bit: across 2^32, which is the end of a block at
     * every length. */
    gateDriftInit(&d);
    d.asked = 0xffffffffull;
    CHECK(gateDriftAsk(&d) == 0x100000000ull);
    CHECK_EQ_INT(gateDriftOpen(&d, 0), 64);
    CHECK_EQ_INT(gateDriftOpen(&d, 1), 512);
    CHECK_EQ_INT(gateDriftOpen(&d, 2), 4096);
    CHECK(gateDriftAsk(&d) == 0x100000001ull);
    for (s = 0; s < SCALES; s++) CHECK_EQ_INT(gateDriftOpen(&d, s), 1);
}

/* Which blocks an answer counts in, step by step. Every counter after every
 * call follows from two rules: the first spot check of a block opens it
 * (its counters start at zero), and an answer counts in the blocks that
 * hold its number and are still open. The levels are infinite: this is
 * about counting. */
TEST(drift_counts_by_block) {
    gateDriftBets b;
    gateDrift d, before;
    int s;

    gateDriftBetsInit(&b, 0.05);
    gateDriftInit(&d);

    /* A number before any spot check was asked: nothing is handed out. */
    before = d;
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 1, INFINITY, INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftLost(&d, &b, 1, INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 0, INFINITY, INFINITY), QUIET);
    CHECK(memcmp(&d, &before, sizeof(d)) == 0);

    driftAskMany(&d, 64);
    /* The first and the last spot check of the first block, at every
     * length. */
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 1, INFINITY, INFINITY), QUIET);
    CHECK(driftIs(&d, 64, 1, 1, 1, 1, 0, 0, 0));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 64, INFINITY, INFINITY), QUIET);
    CHECK(driftIs(&d, 64, 2, 2, 2, 2, 0, 0, 0));
    CHECK_EQ_INT(gateDriftLost(&d, &b, 2, INFINITY), QUIET);
    CHECK(driftIs(&d, 64, 2, 2, 2, 2, 1, 1, 1));

    /* Numbers that were never handed out, and 0: ignored, bit for bit. */
    before = d;
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 0, INFINITY, INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 65, INFINITY, INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 66, -INFINITY, -INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 4096, INFINITY, INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, UINT64_MAX, INFINITY, INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 0x100000001ull, INFINITY, INFINITY), QUIET);    /* 1 + 2^32 */
    CHECK_EQ_INT(gateDriftLost(&d, &b, 0, INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftLost(&d, &b, 65, -INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftLost(&d, &b, UINT64_MAX, INFINITY), QUIET);
    CHECK(memcmp(&d, &before, sizeof(d)) == 0);

    /* Spot check 65 opens the second block of 64: its counters start at
     * zero, the longer blocks keep theirs. Nothing is reset before that:
     * the answers of a full block still count in it. */
    CHECK_EQ_INT(gateDriftAsk(&d), 65);
    CHECK(driftIs(&d, 65, 2, 0, 2, 2, 0, 1, 1));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 65, INFINITY, INFINITY), QUIET);    /* the first of its block */
    CHECK(driftIs(&d, 65, 3, 1, 3, 3, 0, 1, 1));
    /* A disagreement for a number whose block of 64 has ended while its
     * blocks of 512 and 4,096 are open: it counts in those two only. */
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 3, INFINITY, INFINITY), QUIET);
    CHECK(driftIs(&d, 65, 4, 1, 4, 4, 0, 1, 1));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 63, INFINITY, INFINITY), QUIET);    /* the last but one of it */
    CHECK(driftIs(&d, 65, 5, 1, 5, 5, 0, 1, 1));
    CHECK_EQ_INT(gateDriftLost(&d, &b, 4, INFINITY), QUIET);                /* the same for a lost one */
    CHECK(driftIs(&d, 65, 5, 1, 5, 5, 0, 2, 2));
    for (s = 0; s < SCALES; s++) CHECK_EQ_INT(gateDriftOpen(&d, s), s == 0 ? 1 : 65);

    /* On to the end of the first block of 512. */
    driftAskMany(&d, 512 - 65);
    CHECK(driftIs(&d, 512, 5, 0, 5, 5, 0, 2, 2));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 512, INFINITY, INFINITY), QUIET);   /* last of 64 and of 512 */
    CHECK(driftIs(&d, 512, 6, 1, 6, 6, 0, 2, 2));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 449, INFINITY, INFINITY), QUIET);   /* first of that block of 64 */
    CHECK(driftIs(&d, 512, 7, 2, 7, 7, 0, 2, 2));
    CHECK_EQ_INT(gateDriftLost(&d, &b, 448, INFINITY), QUIET);              /* last of the one before */
    CHECK(driftIs(&d, 512, 7, 2, 7, 7, 0, 3, 3));

    /* Spot check 513 opens a block of 64 and a block of 512. */
    CHECK_EQ_INT(gateDriftAsk(&d), 513);
    CHECK(driftIs(&d, 513, 7, 0, 0, 7, 0, 0, 3));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 513, INFINITY, INFINITY), QUIET);
    CHECK(driftIs(&d, 513, 8, 1, 1, 8, 0, 0, 3));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 511, INFINITY, INFINITY), QUIET);   /* only 4,096 is still open */
    CHECK(driftIs(&d, 513, 9, 1, 1, 9, 0, 0, 3));
    CHECK_EQ_INT(gateDriftLost(&d, &b, 5, INFINITY), QUIET);
    CHECK(driftIs(&d, 513, 9, 1, 1, 9, 0, 0, 4));
    CHECK_EQ_INT(gateDriftLost(&d, &b, 513 + 1, INFINITY), QUIET);          /* not handed out yet */
    CHECK(driftIs(&d, 513, 9, 1, 1, 9, 0, 0, 4));

    /* On to the end of the first block of 4,096. */
    driftAskMany(&d, 4096 - 513);
    CHECK(driftIs(&d, 4096, 9, 0, 0, 9, 0, 0, 4));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 4096, INFINITY, INFINITY), QUIET);  /* the last of all three */
    CHECK(driftIs(&d, 4096, 10, 1, 1, 10, 0, 0, 4));
    CHECK_EQ_INT(gateDriftLost(&d, &b, 3585, INFINITY), QUIET);             /* first of this block of 512 */
    CHECK(driftIs(&d, 4096, 10, 1, 1, 10, 0, 1, 5));
    CHECK_EQ_INT(gateDriftLost(&d, &b, 4033, INFINITY), QUIET);             /* first of this block of 64 */
    CHECK(driftIs(&d, 4096, 10, 1, 1, 10, 1, 2, 6));

    /* Spot check 4,097 opens a block at every length. An answer for an
     * older spot check has no open block left: it is counted as a
     * disagreement received and in no block; a lost one changes nothing. */
    CHECK_EQ_INT(gateDriftAsk(&d), 4097);
    CHECK(driftIs(&d, 4097, 10, 0, 0, 0, 0, 0, 0));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 4094, INFINITY, INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 7, INFINITY, INFINITY), QUIET);
    CHECK(driftIs(&d, 4097, 12, 0, 0, 0, 0, 0, 0));
    CHECK_EQ_INT(gateDriftLost(&d, &b, 4095, INFINITY), QUIET);
    CHECK(driftIs(&d, 4097, 12, 0, 0, 0, 0, 0, 0));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 4097, INFINITY, INFINITY), QUIET);
    CHECK(driftIs(&d, 4097, 13, 1, 1, 1, 0, 0, 0));
    CHECK(driftConsistent(&d));

    /* The same far beyond 2^32: block arithmetic on 64 bit numbers. 2^40 is
     * the end of a block at every length. */
    gateDriftInit(&d);
    d.asked = (uint64_t)1 << 40;
    CHECK(gateDriftAsk(&d) == ((uint64_t)1 << 40) + 1);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, (uint64_t)1 << 40, INFINITY, INFINITY), QUIET);   /* ended */
    CHECK(driftIs(&d, ((uint64_t)1 << 40) + 1, 1, 0, 0, 0, 0, 0, 0));
    /* ... one with the low 32 bits of the open spot check ... */
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 1, INFINITY, INFINITY), QUIET);
    CHECK(driftIs(&d, ((uint64_t)1 << 40) + 1, 2, 0, 0, 0, 0, 0, 0));
    /* ... a number that was not handed out yet ... */
    CHECK_EQ_INT(gateDriftWrong(&d, &b, ((uint64_t)1 << 40) + 2, INFINITY, INFINITY), QUIET);
    CHECK(driftIs(&d, ((uint64_t)1 << 40) + 1, 2, 0, 0, 0, 0, 0, 0));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, ((uint64_t)1 << 40) + 1, INFINITY, INFINITY), QUIET);
    CHECK(driftIs(&d, ((uint64_t)1 << 40) + 1, 3, 1, 1, 1, 0, 0, 0));
    CHECK_EQ_INT(gateDriftLost(&d, &b, 1, INFINITY), QUIET);
    CHECK(driftIs(&d, ((uint64_t)1 << 40) + 1, 3, 1, 1, 1, 0, 0, 0));
}

/* alpha = 0.2 makes every factor a small fraction: the alternatives are
 * p1 = 0.4 and p1 = 0.6 (0.8, capped at (1 + 0.2) / 2), so a disagreement
 * multiplies the two products by 2 and 3 and a right answer by 3/4 and 1/2.
 * With n spot checks asked in the block and w of them known wrong the
 * statistic is (2^w (3/4)^(n-w) + 3^w (1/2)^(n-w)) / 2:
 *
 *     call        n  w   2^w (3/4)^(n-w)   3^w (1/2)^(n-w)   statistic
 *     ask         1  0   0.75              0.5               0.625
 *     ask         2  0   0.5625            0.25              0.40625
 *     wrong(1)    2  1   1.5               1.5               1.5
 *     ask         3  1   1.125             0.75              0.9375
 *     wrong(3)    3  2   3                 4.5               3.75
 *     ask         4  2   2.25              2.25              2.25
 *     ask         5  2   1.6875            1.125             1.40625
 *     wrong(5)    5  3   4.5               6.75              5.625
 *     wrong(4)    5  4   12                40.5              26.25
 *     lost(2)     5  4   12                40.5              26.25    (with lost: n 5, w 5: 137.5)
 *
 * The answer of spot check 4 arrives after that of spot check 5, and the
 * answer of spot check 2 never: neither matters to the first statistic. A
 * spot check that was asked counts as right until its answer says
 * otherwise, which is why the statistic falls on every ask. */
TEST(drift_hand_computed_sequence) {
    static const struct {
        int call;           /* 0 ask, 1 wrong, 2 lost */
        uint64_t number;
        double stat, with_lost;
    } steps[] = {
        {0, 1, 0.625, 0.625},     {0, 2, 0.40625, 0.40625}, {1, 1, 1.5, 1.5},
        {0, 3, 0.9375, 0.9375},   {1, 3, 3.75, 3.75},       {0, 4, 2.25, 2.25},
        {0, 5, 1.40625, 1.40625}, {1, 5, 5.625, 5.625},     {1, 4, 26.25, 26.25},
        {2, 2, 26.25, 137.5}
    };
    gateDriftBets b;
    gateDrift d;
    size_t i;

    gateDriftBetsInit(&b, 0.2);
    gateDriftInit(&d);
    for (i = 0; i < COUNTOF(steps); i++) {
        if (steps[i].call == 0) {
            CHECK(gateDriftAsk(&d) == steps[i].number);
        } else if (steps[i].call == 1) {
            CHECK_EQ_INT(gateDriftWrong(&d, &b, steps[i].number, log(1000.0), log(1e9)), QUIET);
        } else {
            CHECK_EQ_INT(gateDriftLost(&d, &b, steps[i].number, log(1e9)), QUIET);
        }
        CHECK_NEAR(gateDriftLogStat(&d, &b, 0), log(steps[i].stat), 1e-13);
        CHECK_NEAR(gateDriftLogStat(&d, &b, 1), log(steps[i].with_lost), 1e-13);
    }
    CHECK(driftIs(&d, 5, 4, 4, 4, 4, 1, 1, 1));
    /* Two of them spelled out: log(26.25) and log(137.5). */
    CHECK_NEAR(gateDriftLogStat(&d, &b, 0), 3.2676659890376326042e+0, 1e-13);
    CHECK_NEAR(gateDriftLogStat(&d, &b, 1), 4.9236239171066263e+0, 1e-13);
}

/* The statistic of a block as a function of (spot checks asked in it,
 * disagreements): both alternatives and the log of their average. The
 * alternatives are checked through the increments the detector is built
 * from, the average through gateDriftLogStat() on a block holding exactly
 * those counts (the first block, where the three lengths coincide). */
TEST(drift_statistic_of_a_block) {
    static const struct {
        int a;
        uint32_t n, w;
        double lw[ALTS], stat;
    } cases[] = {
        {A05, 1, 0, {-5.4067221270275771048e-2, -1.7185025692665923330e-1}, -1.1122563511022438663e-1},
        {A05, 1, 1, {6.9314718055994530942e-1, 1.3862943611198906188e+0}, 1.0986122886681096914e+0},
        {A05, 2, 1, {6.3907995928966953837e-1, 1.2144441041932313855e+0}, 9.6758402626170559043e-1},
        {A05, 6, 6, {4.1588830833596718565e+0, 8.3177661667193437130e+0}, 7.6401231726953636577e+0},
        {A05, 7, 6, {4.1048158620893960855e+0, 8.1459159097926844797e+0}, 7.4701941459465983038e+0},
        {A05, 64, 0, {-3.4603021612976493470e+0, -1.0998416443306190931e+1}, -4.1529170828992733645e+0},
        {A05, 64, 12, {5.5062706606650036185e+0, 7.6993189732524072946e+0}, 7.1119507205531890670e+0},
        {A05, 64, 64, {4.4361419555836499803e+1, 8.8722839111672999605e+1}, 8.8029691931113054296e+1},
        {A20, 7, 7, {4.8520302639196171659e+0, 7.6902860206767675160e+0}, 7.0540177859603888334e+0},
        {A20, 8, 7, {4.5643481914678362154e+0, 6.9971388401168222065e+0}, 6.3881411491830667578e+0},
        {A20, 16, 11, {6.1862086239004936507e+0, 8.6189992725494795494e+0}, 8.0100015816157241082e+0},
        /* Many right answers: here the alternative closer to alpha leads. */
        {A20, 64, 27, {8.0707371944026281832e+0, 4.0160861133209839702e+0}, 7.3947828954933890714e+0},
        {A30, 10, 10, {6.9314718055994530942e+0, 7.7318988823348176371e+0}, 7.4097199825430952058e+0},
        {A30, 20, 12, {3.8408398632359625400e+0, 3.7331012143222186892e+0}, 3.7884207896264196986e+0},
        /* Both alternatives on the cap: the average is either of them.
         * (4/3)^24 = 996.6 and (4/3)^25 = 1328.8. */
        {A60, 24, 24, {6.9043697388427428137e+0, 6.9043697388427428137e+0}, 6.9043697388427428137e+0},
        {A60, 25, 25, {7.1920518112945237642e+0, 7.1920518112945237642e+0}, 7.1920518112945237642e+0},
        {A60, 64, 50, {4.6800430947498131966e+0, 4.6800430947498131966e+0}, 4.6800430947498131966e+0},
        {A01, 6, 6, {4.1588830833596718565e+0, 8.3177661667193437130e+0}, 7.6401231726953636577e+0},
        {A01, 64, 7, {4.2733450904705907419e+0, 7.9500759838342740572e+0}, 7.2819194849560737292e+0},
        {A02, 50, 8, {4.6791673819646638486e+0, 8.4368410208547972731e+0}, 7.7667636439815511493e+0},
        {A10, 30, 9, {3.7648808762554550775e+0, 3.9618819798075629003e+0}, 3.8682247829714019338e+0},
        /* (19/18)^64 = 31.8: at alpha = 0.9 a block of 64 cannot reach 1000. */
        {A90, 64, 64, {3.4603021612976483082e+0, 3.4603021612976483082e+0}, 3.4603021612976483082e+0},
        {A001, 64, 6, {4.1007959478084783119e+0, 8.1433299450099371881e+0}, 7.4675834278882887790e+0}
    };
    size_t i;
    uint32_t j;
    int k, s;

    for (i = 0; i < COUNTOF(cases); i++) {
        uint32_t n = cases[i].n, w = cases[i].w;
        gateDriftBets b;
        gateDrift d;

        gateDriftBetsInit(&b, ref_drift[cases[i].a].alpha);
        for (k = 0; k < ALTS; k++)
            CHECK_NEAR(w * b.inc_err[k] + (double)(n - w) * b.inc_ok[k], cases[i].lw[k],
                       1e-13 * (1.0 + fabs(cases[i].lw[k])));

        /* The disagreements are the last w of the n spot checks, reported
         * after all n were asked ... */
        gateDriftInit(&d);
        driftAskMany(&d, n);
        for (j = 0; j < w; j++) gateDriftWrong(&d, &b, n - j, INFINITY, INFINITY);
        CHECK_NEAR(gateDriftLogStat(&d, &b, 0), cases[i].stat, 1e-13 * (1.0 + fabs(cases[i].stat)));
        CHECK_NEAR(gateDriftLogStat(&d, &b, 0), refBlockStat((size_t)cases[i].a, n, w), 1e-12);
        /* (no lost answer: the second statistic is the first) */
        CHECK(gateDriftLogStat(&d, &b, 1) == gateDriftLogStat(&d, &b, 0));
        for (s = 0; s < SCALES; s++) CHECK_EQ_INT(d.wrong[s], w);

        /* ... or the first w, each reported the moment it is asked: the
         * same statistic. */
        gateDriftInit(&d);
        for (j = 1; j <= n; j++) {
            gateDriftAsk(&d);
            if (j <= w) gateDriftWrong(&d, &b, j, INFINITY, INFINITY);
        }
        CHECK_NEAR(gateDriftLogStat(&d, &b, 0), cases[i].stat, 1e-13 * (1.0 + fabs(cases[i].stat)));

        /* ... or as lost answers: the same number, in the second statistic
         * only. The first is that of a block without a disagreement. */
        gateDriftInit(&d);
        driftAskMany(&d, n);
        for (j = 0; j < w; j++) gateDriftLost(&d, &b, n - j, INFINITY);
        CHECK_NEAR(gateDriftLogStat(&d, &b, 1), cases[i].stat, 1e-13 * (1.0 + fabs(cases[i].stat)));
        CHECK_NEAR(gateDriftLogStat(&d, &b, 0), refBlockStat((size_t)cases[i].a, n, 0), 1e-12);
        CHECK(gateDriftLogStat(&d, &b, 0) <= 0.0);
    }
}

/* Detectors that hold more than one block: N spot checks are asked, then
 * some are reported as disagreements and some as lost (numbers first,
 * first + step, ...). Expected: the counters of the three open blocks and
 * the largest of their statistics, without and with the lost answers. */
TEST(drift_statistic_across_blocks) {
    static const struct {
        int a;
        uint32_t asked;
        uint32_t wrong_at[3], lost_at[3];   /* first, step, count */
        uint32_t wrong[SCALES], lost[SCALES];
        double stat, with_lost;
    } cases[] = {
        {A05, 10, {1, 1, 3}, {0, 0, 0}, {3, 3, 3}, {0, 0, 0},
         2.5136106555476902470e+0, 2.5136106555476902470e+0},
        /* The last six of a block of 64 ... */
        {A05, 64, {59, 1, 6}, {0, 0, 0}, {6, 6, 6}, {0, 0, 0},
         3.9663275886559893096e-1, 3.9663275886559893096e-1},
        /* ... and a seventh that opens the next: one of one in the short
         * block (log 3 = 1.0986), seven of 65 in the long ones (1.1524). */
        {A05, 65, {59, 1, 7}, {0, 0, 0}, {1, 7, 7}, {0, 0, 0},
         1.1523919583959086489e+0, 1.1523919583959086489e+0},
        /* 38 late disagreements of the first block of 64, seen from spot
         * check 100: the blocks of 512 and 4,096 are above log(1000). */
        {A20, 100, {1, 1, 38}, {0, 0, 0}, {0, 38, 38}, {0, 0, 0},
         7.8102165901353213183e+0, 7.8102165901353213183e+0},
        {A02, 512, {1, 1, 26}, {0, 0, 0}, {0, 26, 26}, {0, 0, 0},
         7.3751760406996345218e+0, 7.3751760406996345218e+0},
        /* One spot check later the block of 512 is a new one. */
        {A02, 513, {1, 1, 26}, {0, 0, 0}, {0, 0, 26}, {0, 0, 0},
         7.3518345375700607606e+0, 7.3518345375700607606e+0},
        {A01, 4096, {1, 1, 70}, {0, 0, 0}, {0, 0, 70}, {0, 0, 0},
         6.9537079444997294470e+0, 6.9537079444997294470e+0},
        /* ... and one later every block is: nothing is left but one right
         * spot check, (98/99 + 32/33) / 2. */
        {A01, 4097, {1, 1, 70}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
         -2.0408871631207105169e-2, -2.0408871631207105169e-2},
        {A05, 20, {1, 1, 2}, {5, 1, 14}, {2, 2, 2}, {14, 14, 14},
         1.1203406515391632721e-1, 2.0800186010982624104e+1},
        {A30, 30, {1, 2, 10}, {2, 4, 5}, {10, 10, 10}, {5, 5, 5},
         -4.8106766216471123284e+0, 1.6802025108800146083e+0},
        {A60, 40, {1, 1, 25}, {30, 1, 10}, {25, 25, 25}, {10, 10, 10},
         -3.2051558971046558770e+0, 6.6031366330126067228e+0},
        /* The largest statistic may be the one of an empty short block. */
        {A90, 200, {1, 1, 130}, {131, 1, 60}, {0, 130, 130}, {0, 60, 60},
         -5.5451774444795624753e+0, 3.3413002357529403209e+0},
        {A05, 70, {65, 1, 4}, {1, 1, 10}, {4, 4, 4}, {0, 10, 10},
         4.5844585584186987972e+0, 9.1350709085927600569e+0},
        {A05, 5000, {4097, 1, 4}, {4990, 1, 6}, {0, 0, 4}, {3, 6, 6},
         -7.9656514630249029415e-1, 2.8096328356294636728e+0},
        {A10, 1000, {500, 7, 60}, {3, 11, 40}, {0, 58, 60}, {0, 0, 40},
         -5.4044585502806892754e+0, -5.4044585502806892754e+0},
        {A001, 4096, {4033, 1, 6}, {1, 512, 8}, {6, 6, 6}, {0, 1, 8},
         7.4675834278882887790e+0, 7.5134023265345878159e+0},
        {A20, 600, {400, 3, 60}, {2, 5, 30}, {1, 22, 60}, {0, 0, 30},
         -4.4309259721944490609e+0, -4.4309259721944490609e+0}
    };
    size_t i;
    uint32_t j;
    int order, s;

    for (i = 0; i < COUNTOF(cases); i++) {
        const uint32_t *wa = cases[i].wrong_at, *la = cases[i].lost_at;
        gateDriftBets b;

        gateDriftBetsInit(&b, ref_drift[cases[i].a].alpha);
        /* Disagreements first and upwards, or lost answers first and both
         * downwards: the same detector. */
        for (order = 0; order < 2; order++) {
            gateDrift d;

            gateDriftInit(&d);
            driftAskMany(&d, cases[i].asked);
            if (order == 0) {
                for (j = 0; j < wa[2]; j++) gateDriftWrong(&d, &b, wa[0] + j * wa[1], INFINITY, INFINITY);
                for (j = 0; j < la[2]; j++) gateDriftLost(&d, &b, la[0] + j * la[1], INFINITY);
            } else {
                for (j = la[2]; j-- > 0;) gateDriftLost(&d, &b, la[0] + j * la[1], INFINITY);
                for (j = wa[2]; j-- > 0;) gateDriftWrong(&d, &b, wa[0] + j * wa[1], INFINITY, INFINITY);
            }
            CHECK_EQ_INT(d.asked, cases[i].asked);
            CHECK_EQ_INT(d.errors, wa[2]);
            for (s = 0; s < SCALES; s++) {
                CHECK_EQ_INT(d.wrong[s], cases[i].wrong[s]);
                CHECK_EQ_INT(d.lost[s], cases[i].lost[s]);
            }
            CHECK_NEAR(gateDriftLogStat(&d, &b, 0), cases[i].stat, 1e-12 * (1.0 + fabs(cases[i].stat)));
            CHECK_NEAR(gateDriftLogStat(&d, &b, 1), cases[i].with_lost,
                       1e-12 * (1.0 + fabs(cases[i].with_lost)));
            CHECK(driftConsistent(&d));
        }
    }
}

/* A gate that is never wrong: nothing is ever reported, so nothing can
 * alarm, and the statistic of a block falls with every spot check. The
 * largest of the three is that of the block opened last: one right spot
 * check when a block of 64 has just begun, (18/19 + 16/19) / 2 = 17/19 at
 * alpha = 0.05, and 64 of them at its end. */
TEST(drift_right_answers_only) {
    gateDriftBets b;
    gateDrift d;
    double prev = 0.0;
    int n, rising = 0, positive = 0, differ = 0;

    gateDriftBetsInit(&b, 0.05);
    gateDriftInit(&d);
    for (n = 1; n <= 9000; n++) {
        double stat;

        gateDriftAsk(&d);
        stat = gateDriftLogStat(&d, &b, 0);
        if (!(stat < 0.0)) positive++;
        if (n % 64 != 1 && !(stat < prev)) rising++;
        if (n % 64 == 1) CHECK_NEAR(stat, log(17.0 / 19.0), 1e-14);
        if (n % 64 == 0) CHECK_NEAR(stat, -4.1529170828992733645e+0, 1e-12);
        if (gateDriftLogStat(&d, &b, 1) != stat) differ++;
        prev = stat;
    }
    CHECK_EQ_INT(positive, 0);  /* never above 1 */
    CHECK_EQ_INT(rising, 0);    /* lower with every spot check of a block */
    CHECK_EQ_INT(differ, 0);    /* no lost answer: one statistic */
    CHECK(driftIs(&d, 9000, 0, 0, 0, 0, 0, 0, 0));
}

/* ------------------------------- The alarms ------------------------------ */

/* A fresh detector; 'right' spot checks are asked and never reported (they
 * count as right); then every further spot check is reported the moment it
 * is asked, as a disagreement ('lost' 0) or as a lost answer ('lost' 1), at
 * the levels of the gate. Returns how many of those it takes until the
 * verdict is not QUIET (0 if 5000 are not enough); '*verdict' receives it. */
static int driftRunLength(double alpha, uint32_t right, int lost, int *verdict) {
    const double la = log(GATE_DRIFT_ALARM), ll = log(GATE_LOST_ALARM);
    gateDriftBets b;
    gateDrift d;
    int k;

    gateDriftBetsInit(&b, alpha);
    gateDriftInit(&d);
    driftAskMany(&d, right);
    for (k = 1; k <= 5000; k++) {
        uint64_t n = gateDriftAsk(&d);

        *verdict = lost ? gateDriftLost(&d, &b, n, ll) : gateDriftWrong(&d, &b, n, la, ll);
        if (*verdict != QUIET) return k;
    }
    return 0;
}

/* The exact disagreement that raises the alarm, decided in exact rational
 * arithmetic. In every case the statistic that alarms is more than 0.009
 * (in log) above its level and the closest one before it more than 0.12
 * below: no count here is a matter of rounding.
 *
 * FROM A FRESH BLOCK the statistic after k disagreements in a row is
 * (2^k + 4^k) / 2 whenever neither alternative is capped (alpha <= 1/7):
 * 528 after five, 2080 after six. At alpha = 0.2 it is (2^k + 3^k) / 2:
 * 396.5 after six, 1157.5 after seven. For the second statistic and its
 * level 10^9: (2^15 + 4^15) / 2 = 5.4e8 and (2^16 + 4^16) / 2 = 2.1e9, so
 * the sixteenth lost answer in a row; (2^19 + 3^19) / 2 = 5.8e8 and
 * (2^20 + 3^20) / 2 = 1.7e9, the twentieth, at alpha = 0.2.
 *
 * AFTER r RIGHT SPOT CHECKS IN THE SAME BLOCK it takes more, because there
 * is no restart: at alpha = 0.05 the 50 right ones are worth (18/19)^50 =
 * 0.067 and (16/19)^50 = 0.00019, and 2^k 0.067 + 4^k 0.00019 reaches 2000
 * at k = 12. When the block of 64 ends first, the next one is a fresh
 * start: after 63 right ones the first disagreement is the last of its
 * block and the six that follow raise the alarm (7); at alpha = 0.2, after
 * 50 right ones, the 14 that are left in the block reach 0.005 and the next
 * block needs its seven (21).
 *
 * THE LONGER BLOCKS. r >= 64: the block of 64 holds r mod 64 right spot
 * checks, the longer ones all r, so the short block decides (448, 512 and
 * 4096 are fresh starts; 511 and 4095 are "63"). For lost answers the
 * longer blocks do decide some cases: after 50 right ones at alpha = 0.05
 * the 14 left in the block of 64 are not enough, the next block would need
 * its 16 (30 in all), but the block of 512 is at 4^22 (16/19)^50 / 2 =
 * 1.6e9 with the 22nd. */
TEST(drift_alarm_counts) {
    static const double alphas[] = {0.2, 0.05, 0.02, 0.01};
    static const uint32_t right[] = {0, 10, 50, 63, 64, 100, 448, 500, 511, 512, 1000, 4000, 4095, 4096};
    static const int wrong_run[COUNTOF(alphas)][COUNTOF(right)] = {
        {7, 13, 21, 8, 7, 26, 7, 19, 8, 7, 31, 25, 8, 7},
        {6, 7, 12, 7, 6, 10, 6, 12, 7, 6, 11, 10, 7, 6},
        {6, 6, 8, 7, 6, 8, 6, 8, 7, 6, 8, 7, 7, 6},
        {6, 6, 7, 7, 6, 7, 6, 7, 7, 6, 7, 7, 7, 6}
    };
    static const int lost_run[COUNTOF(alphas)][COUNTOF(right)] = {
        {20, 26, 34, 21, 20, 48, 20, 32, 21, 20, 44, 52, 21, 20},
        {16, 17, 22, 17, 16, 20, 16, 28, 17, 16, 21, 20, 17, 16},
        {16, 16, 18, 17, 16, 18, 16, 28, 17, 16, 18, 17, 17, 16},
        {16, 16, 17, 17, 16, 17, 16, 27, 17, 16, 17, 17, 17, 16}
    };
    size_t a, r;
    int verdict = QUIET;

    for (a = 0; a < COUNTOF(alphas); a++) {
        for (r = 0; r < COUNTOF(right); r++) {
            CHECK_EQ_INT(driftRunLength(alphas[a], right[r], 0, &verdict), wrong_run[a][r]);
            CHECK_EQ_INT(verdict, WORSE);
            CHECK_EQ_INT(driftRunLength(alphas[a], right[r], 1, &verdict), lost_run[a][r]);
            CHECK_EQ_INT(verdict, UNCHECKED);
        }
    }

    /* Capped alternatives. alpha = 0.3: factors 2 and 13/6, their average
     * is 782 after nine disagreements and 1652 after ten; 28 lost answers.
     * alpha = 0.6: both factors are 4/3, and (4/3)^24 = 996.6 while
     * (4/3)^25 = 1328.8; (4/3)^72 = 9.9e8 and (4/3)^73 = 1.3e9: more than
     * a block of 64 holds, so the alarm comes from the block of 512.
     * alpha = 0.5: (3/2)^17 = 985 and (3/2)^18 = 1478. */
    CHECK_EQ_INT(driftRunLength(0.3, 0, 0, &verdict), 10);
    CHECK_EQ_INT(driftRunLength(0.3, 0, 1, &verdict), 28);
    CHECK_EQ_INT(driftRunLength(0.6, 0, 0, &verdict), 25);
    CHECK_EQ_INT(driftRunLength(0.6, 0, 1, &verdict), 73);
    CHECK_EQ_INT(driftRunLength(0.5, 0, 0, &verdict), 18);
    CHECK_EQ_INT(driftRunLength(0.5, 0, 1, &verdict), 52);
    CHECK_EQ_INT(driftRunLength(0.1, 0, 0, &verdict), 6);
    /* alpha = 0.9: the factor is 19/18 and (19/18)^127 = 959, (19/18)^128
     * = 1012. No block of 64 can do that: the alarm is the block of 512's. */
    CHECK_EQ_INT(driftRunLength(0.9, 0, 0, &verdict), 128);
    CHECK_EQ_INT(verdict, WORSE);
    CHECK_EQ_INT(driftRunLength(0.9, 0, 1, &verdict), 384);
    CHECK_EQ_INT(verdict, UNCHECKED);

    /* The far end of the range. At alpha = 0.99 the factor is 199/198: the
     * 1372nd disagreement in a row raises WORSE (it clears log(1000) by
     * 0.004, the one before misses it by 0.0009), while a whole block of
     * 4,096 lost answers is a statistic of e^20.63 = 9.1e8, short of 10^9:
     * no UNCHECKED, whatever is lost. At alpha = 0.999 a block of 4,096
     * disagreements is a statistic of e^2.05 = 7.8: the detector cannot
     * say WORSE at all. (No block of 4,096 can reach 1000 for an alpha
     * above 0.9966, nor 10^9 above 0.9899.) */
    CHECK_EQ_INT(driftRunLength(0.99, 0, 0, &verdict), 1372);
    CHECK_EQ_INT(verdict, WORSE);
    CHECK_EQ_INT(driftRunLength(0.99, 0, 1, &verdict), 0);
    CHECK_EQ_INT(driftRunLength(0.999, 0, 0, &verdict), 0);
    CHECK_EQ_INT(driftRunLength(0.999, 0, 1, &verdict), 0);
}

/* The alarm boundary of a block, measured through the detector for every
 * n up to 4,096: boundary[n] is the smallest number of answers counted
 * against the gate, among the n spot checks asked in a block, at which the
 * verdict is 'alarm' (0 if even n of n are not enough). One detector walks
 * n upwards and always holds one answer less than the boundary; the next
 * answer is tried on a copy. The answers are those of spot checks 1, 2, 3,
 * ..., so from n = 65 on they come after their block of 64 has ended and
 * from n = 513 on after their block of 512 has: what is measured is the
 * block of 64 up to n = 64, the block of 512 up to 512 and the block of
 * 4,096 beyond. Returns the number of steps that broke the rule "one more
 * spot check asked moves the boundary by 0 or 1". */
static int driftMeasureBoundary(const gateDriftBets *b, int lost, double la, double ll,
                                uint32_t *boundary) {
    const int alarm = lost ? UNCHECKED : WORSE;
    gateDrift d, probe;
    uint32_t n, held = 0;   /* answers reported to 'd' */
    int broken = 0;

    gateDriftInit(&d);
    boundary[0] = 0;
    for (n = 1; n <= 4096; n++) {
        int v;

        gateDriftAsk(&d);
        probe = d;
        v = lost ? gateDriftLost(&probe, b, held + 1, ll) : gateDriftWrong(&probe, b, held + 1, la, ll);
        if (v == alarm) {
            boundary[n] = held + 1;
            continue;
        }
        if (v != QUIET) broken++;
        d = probe;
        held++;
        if (held == n) {
            boundary[n] = 0;    /* every one of them, and still quiet */
            continue;
        }
        probe = d;
        v = lost ? gateDriftLost(&probe, b, held + 1, ll) : gateDriftWrong(&probe, b, held + 1, la, ll);
        if (v != alarm) broken++;
        boundary[n] = held + 1;
    }
    return broken;
}

/* Probability that a block of 'len' spot checks raises the alarm when each
 * answer disagrees with probability p and is reported at once: forward
 * recursion over the number of disagreements of the blocks that have not
 * alarmed yet. */
static double driftBlockAlarmProbability(const uint32_t *boundary, uint32_t len, double p) {
    static double alive[4098];
    double absorbed = 0.0;
    uint32_t n, w, top = 0;     /* alive[0..top] may be non zero */

    memset(alive, 0, sizeof(alive));
    alive[0] = 1.0;
    for (n = 1; n <= len; n++) {
        for (w = top + 1; w > 0; w--) alive[w] = alive[w] * (1.0 - p) + alive[w - 1] * p;
        alive[0] *= 1.0 - p;
        top++;
        /* The states at or above the boundary got there by a disagreement
         * (the boundary never falls as n grows): that report alarmed. */
        while (boundary[n] != 0 && top >= boundary[n]) {
            absorbed += alive[top];
            alive[top] = 0.0;
            top--;
        }
    }
    return absorbed;
}

/* The whole boundary against exact arithmetic, for the four risk targets of
 * the table above and both levels. 'sum' is boundary[1] + ... + boundary[L]
 * for L = 64, 512, 4096: one wrong entry anywhere changes it. The closest
 * any decision of the reference comes to its level is 4e-6 (in log).
 *
 * 'at_alpha' and 'at_twice' are the probabilities that a block of 64, of
 * 512, of 4,096 raises WORSE when the answers arrive in order and disagree
 * with probability alpha, and 2 alpha (exact recursion, 34 digits). At
 * alpha this is the false alarm probability the header bounds by
 * 1 / GATE_DRIFT_ALARM for every block: the true values are between 4e-6
 * and 7.2e-4. At 2 alpha it is the power of one block: a block of 64 almost
 * never notices a doubled error rate at a small alpha (0.03% at 0.01), a
 * block of 4,096 nearly always does (95% to 100%). */
TEST(drift_alarm_boundary) {
    static const uint32_t at[] = {1, 5, 6, 7, 8, 16, 17, 20, 21, 32, 50, 63, 64, 65, 100, 128, 256,
                                  511, 512, 513, 1000, 1024, 2048, 4000, 4095, 4096};
    static const struct {
        double alpha;
        uint32_t worse[COUNTOF(at)], worse_sum[SCALES];
        uint32_t lost[COUNTOF(at)], lost_sum[SCALES];
        double at_alpha[SCALES], at_twice[SCALES];
    } cases[] = {
        {0.2,
         {0, 0, 0, 7, 8, 11, 11, 12, 13, 17, 23, 27, 27, 27, 38, 46, 83, 158, 158, 159, 302, 309, 609,
          1181, 1209, 1210}, {1028, 42632, 2494701},
         {0, 0, 0, 0, 0, 0, 0, 20, 21, 25, 32, 37, 37, 38, 51, 60, 97, 172, 173, 173, 316, 323, 623,
          1196, 1223, 1224}, {1291, 49119, 2551669},
         {5.2029067463580602862e-4, 7.1583067889193696936e-4, 7.1583156357946856901e-4},
         {4.9276619318811595410e-1, 9.9999766953250089342e-1, 1.0}},
        {0.05,
         {0, 0, 6, 6, 6, 7, 7, 8, 8, 9, 11, 12, 12, 12, 16, 19, 29, 48, 48, 48, 83, 85, 159, 300, 307,
          307}, {542, 14536, 650418},
         {0, 0, 0, 0, 0, 16, 16, 16, 17, 18, 20, 21, 21, 21, 25, 28, 42, 66, 66, 66, 102, 103, 177, 319,
          325, 326}, {914, 21244, 723390},
         {2.2361118905662657607e-4, 6.1740859609454755239e-4, 6.9990458695976791219e-4},
         {3.4665577698588769468e-2, 7.9002127318880525876e-1, 9.9999999780385852277e-1}},
        {0.02,
         {0, 0, 6, 6, 6, 6, 6, 7, 7, 7, 8, 8, 9, 9, 10, 11, 17, 26, 26, 26, 40, 41, 70, 127, 129, 129},
         {426, 8378, 286925},
         {0, 0, 0, 0, 0, 16, 16, 16, 16, 17, 17, 18, 18, 18, 20, 21, 26, 38, 38, 38, 58, 59, 90, 146,
          149, 149}, {833, 13309, 359118},
         {4.5270576007373757634e-5, 3.9495010486363024346e-4, 7.1025090960929485788e-4},
         {3.8842905594005958688e-3, 2.0384139785600525752e-1, 9.9919944654111319835e-1}},
        {0.01,
         {0, 0, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 8, 9, 11, 17, 17, 17, 26, 26, 41, 69, 70, 70},
         {388, 5784, 165146},
         {0, 0, 0, 0, 0, 16, 16, 16, 16, 16, 17, 17, 17, 17, 18, 18, 21, 27, 27, 27, 37, 38, 60, 89, 90,
          90}, {808, 10607, 231287},
         {4.1034413371159982206e-6, 3.0527549143383291616e-4, 6.9202686253260301465e-4},
         {3.0493198840947744356e-4, 7.0177644003156462776e-2, 9.4683331824027809359e-1}}
    };
    static uint32_t worse[4097], lost[4097];
    const double la = log(GATE_DRIFT_ALARM), ll = log(GATE_LOST_ALARM);
    size_t i, k;
    uint32_t n;
    int s;

    for (i = 0; i < COUNTOF(cases); i++) {
        uint32_t sum_w = 0, sum_l = 0;
        int falls = 0, below = 0;
        gateDriftBets b;

        gateDriftBetsInit(&b, cases[i].alpha);
        CHECK_EQ_INT(driftMeasureBoundary(&b, 0, la, ll, worse), 0);
        CHECK_EQ_INT(driftMeasureBoundary(&b, 1, la, ll, lost), 0);
        for (k = 0; k < COUNTOF(at); k++) {
            CHECK_EQ_INT(worse[at[k]], cases[i].worse[k]);
            CHECK_EQ_INT(lost[at[k]], cases[i].lost[k]);
        }
        for (n = 1, s = 0; n <= 4096; n++) {
            sum_w += worse[n];
            sum_l += lost[n];
            if (n == gateDriftBlocks[s]) {
                CHECK_EQ_INT(sum_w, cases[i].worse_sum[s]);
                CHECK_EQ_INT(sum_l, cases[i].lost_sum[s]);
                s++;
            }
            /* One more right spot check never makes the alarm easier, and
             * 10^9 is never reached before 1000. */
            if (n > 1 && worse[n - 1] != 0 && worse[n] < worse[n - 1]) falls++;
            if (lost[n] != 0 && !(worse[n] != 0 && worse[n] < lost[n])) below++;
        }
        CHECK_EQ_INT(s, SCALES);
        CHECK_EQ_INT(falls, 0);
        CHECK_EQ_INT(below, 0);

        for (s = 0; s < SCALES; s++) {
            double h0 = driftBlockAlarmProbability(worse, gateDriftBlocks[s], cases[i].alpha);
            double h1 = driftBlockAlarmProbability(worse, gateDriftBlocks[s], 2.0 * cases[i].alpha);

            CHECK_REL(h0, cases[i].at_alpha[s], 1e-10);
            CHECK_REL(h1, cases[i].at_twice[s], 1e-10);
            /* "Each block raises a false alarm with probability at most
             * 1/A", and it does not get worse for a better gate. */
            CHECK(h0 <= 1.0 / GATE_DRIFT_ALARM);
            CHECK(driftBlockAlarmProbability(worse, gateDriftBlocks[s], 0.5 * cases[i].alpha) < h0);
            /* The same block with answers lost instead of wrong reaches
             * 10^9 a million times less often than that bound. */
            CHECK(driftBlockAlarmProbability(lost, gateDriftBlocks[s], cases[i].alpha) <=
                  1.0 / GATE_LOST_ALARM);
        }
    }
}

/* Every block of every length follows the same boundary, not only the first
 * one. Spot checks 1 .. skip are asked and never reported, the next 'n' are
 * asked, and then the answers of those n arrive one by one, the oldest
 * first. With skip a multiple of 64 (512, 4096) the n are alone in a block
 * of 64 (512, 4096) while every longer block also holds the skipped right
 * ones and is further from its level; and where n is more than 64 (512)
 * the oldest of them are in blocks of 64 (512) that have ended, so that
 * only the longer block counts them. */
TEST(drift_alarm_boundary_in_later_blocks) {
    static const struct {
        double alpha;
        uint64_t skip;
        uint32_t n, worse, lost;    /* boundary[n] of the table above */
    } cases[] = {
        /* A block of 64. Six of six raise WORSE; nothing six spot checks
         * can do reaches 10^9. */
        {0.05, 64, 6, 6, 0},      {0.05, 64, 50, 11, 20},    {0.05, 64 * 5, 64, 12, 21},
        {0.05, 4096 + 512 + 64, 32, 9, 18},
        {0.2, 64, 20, 12, 20},    {0.01, 64 * 63, 64, 7, 17},
        /* A block of 512. */
        {0.05, 512, 100, 16, 25}, {0.05, 512 * 3, 512, 48, 66},
        {0.2, 512, 256, 83, 97},  {0.01, 512 * 7, 511, 17, 27},
        {0.02, 4096 * 5 + 512 * 2, 128, 11, 21},
        /* A block of 4,096. */
        {0.05, 4096, 513, 48, 66}, {0.05, 4096 * 2, 1000, 83, 102},
        {0.2, 4096 * 3, 2048, 609, 623}, {0.01, 4096, 4096, 70, 90}
    };
    const double la = log(GATE_DRIFT_ALARM), ll = log(GATE_LOST_ALARM);
    size_t i;
    int lost;

    for (i = 0; i < COUNTOF(cases); i++) {
        gateDriftBets b;

        gateDriftBetsInit(&b, cases[i].alpha);
        for (lost = 0; lost < 2; lost++) {
            uint32_t expected = lost ? cases[i].lost : cases[i].worse, k, at = 0;
            gateDrift d;

            gateDriftInit(&d);
            driftAskMany(&d, cases[i].skip + cases[i].n);
            for (k = 1; k <= cases[i].n && at == 0; k++) {
                uint64_t number = cases[i].skip + k;
                int v = lost ? gateDriftLost(&d, &b, number, ll) : gateDriftWrong(&d, &b, number, la, ll);

                if (v != QUIET) {
                    at = k;
                    CHECK_EQ_INT(v, lost ? UNCHECKED : WORSE);
                }
            }
            CHECK_EQ_INT(at, expected);
            CHECK(driftConsistent(&d));
        }
    }
}

/* "Reached" means >=: a level equal to the statistic fires, one ulp above
 * it does not. For both statistics. */
TEST(drift_alarm_level_is_inclusive) {
    gateDriftBets b;
    gateDrift start, d;
    double stat;

    gateDriftBetsInit(&b, 0.05);
    gateDriftInit(&start);
    driftAskMany(&start, 9);
    gateDriftWrong(&start, &b, 2, INFINITY, INFINITY);
    gateDriftLost(&start, &b, 5, INFINITY);

    /* The first statistic after one more disagreement: 2 of 9. */
    d = start;
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 7, INFINITY, INFINITY), QUIET);
    stat = gateDriftLogStat(&d, &b, 0);
    CHECK_NEAR(stat, refBlockStat(A05, 9, 2), 1e-13);
    d = start;
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 7, stat, INFINITY), WORSE);
    d = start;
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 7, nextafter(stat, INFINITY), INFINITY), QUIET);
    d = start;
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 7, nextafter(stat, -INFINITY), INFINITY), WORSE);

    /* The second after that disagreement: 3 of 9 ... */
    d = start;
    gateDriftWrong(&d, &b, 7, INFINITY, INFINITY);
    stat = gateDriftLogStat(&d, &b, 1);
    CHECK_NEAR(stat, refBlockStat(A05, 9, 3), 1e-13);
    d = start;
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 7, INFINITY, stat), UNCHECKED);
    d = start;
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 7, INFINITY, nextafter(stat, INFINITY)), QUIET);
    d = start;
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 7, INFINITY, nextafter(stat, -INFINITY)), UNCHECKED);
    /* ... and after one more lost answer instead: the same 3 of 9. */
    d = start;
    CHECK_EQ_INT(gateDriftLost(&d, &b, 7, stat), UNCHECKED);
    d = start;
    CHECK_EQ_INT(gateDriftLost(&d, &b, 7, nextafter(stat, INFINITY)), QUIET);
    d = start;
    CHECK_EQ_INT(gateDriftLost(&d, &b, 7, nextafter(stat, -INFINITY)), UNCHECKED);

    /* The alarm is a level, not a latch held by the detector: the same
     * state is quiet again at a higher level, and it alarms again on the
     * next disagreement of the block at the lower one. */
    d = start;
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 7, 0.5, INFINITY), WORSE);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 8, log(1000.0), INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 9, 0.5, INFINITY), WORSE);
}

/* ------------------------------ Lost answers ----------------------------- */

/* A lost answer is counted next to the disagreements in the second
 * statistic and nowhere else: not as a disagreement received, not in the
 * first statistic, and it can never raise WORSE, however many there are.
 * To the first statistic a lost spot check is one that was asked: it
 * counts as right, like any spot check without a disagreeing answer. */
TEST(drift_lost_answers_only_in_the_second_statistic) {
    static const double alphas[] = {0.2, 0.05, 0.01};
    const double la = log(GATE_DRIFT_ALARM), ll = log(GATE_LOST_ALARM);
    size_t a;
    int n, k;

    for (a = 0; a < COUNTOF(alphas); a++) {
        size_t ra = alphas[a] == 0.2 ? A20 : (alphas[a] == 0.05 ? A05 : A01);
        gateDriftBets b;
        gateDrift d, right;
        int worse = 0, moved = 0;

        gateDriftBetsInit(&b, alphas[a]);
        gateDriftInit(&d);
        gateDriftInit(&right);  /* the same spot checks, none of them lost */
        for (n = 1; n <= 5000; n++) {
            uint64_t number = gateDriftAsk(&d);
            int v;

            gateDriftAsk(&right);
            /* Lost at a level nothing reaches and at the level that the
             * state is far beyond: UNCHECKED at most, never WORSE. */
            v = gateDriftLost(&d, &b, number, n % 2 ? INFINITY : ll);
            if (v == WORSE || (n % 2 && v != QUIET)) worse++;
            if (d.errors != 0 || d.wrong[0] || d.wrong[1] || d.wrong[2]) moved++;
            if (gateDriftLogStat(&d, &b, 0) != gateDriftLogStat(&right, &b, 0)) moved++;
        }
        CHECK_EQ_INT(worse, 0);
        CHECK_EQ_INT(moved, 0);
        /* 5000 = 4096 + 904 = 9 * 512 + 392 = 78 * 64 + 8. */
        CHECK(driftIs(&d, 5000, 0, 0, 0, 0, 8, 392, 904));
        /* Every spot check of every open block is lost: the second
         * statistic is the one of a block in which all disagree. */
        CHECK_NEAR(gateDriftLogStat(&d, &b, 1), refBlockStat(ra, 904, 904), 1e-9);
        CHECK(gateDriftLogStat(&d, &b, 0) < 0.0);

        /* A disagreement now is the first one to the first statistic: one
         * of nine in the block of 64, far below 1000. The verdict comes
         * from the second statistic, which was beyond its level before. */
        k = gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll);
        CHECK_EQ_INT(k, UNCHECKED);
        CHECK_NEAR(gateDriftLogStat(&d, &b, 0), refBlockStat(ra, 9, 1), 1e-12);
        CHECK(driftIs(&d, 5001, 1, 1, 1, 1, 8, 392, 904));
    }

    /* Lost answers do not bring WORSE forward either. After r spot checks
     * whose answers were lost it takes exactly as many disagreements in a
     * row as after r right ones (drift_alarm_counts): 7 after 10 at
     * alpha = 0.05, 12 after 50. What they do bring is UNCHECKED, as soon
     * as lost and wrong together make 16 of a fresh start: with the sixth
     * disagreement after ten lost answers, (2^16 + 4^16) / 2, and with the
     * first after fifty (the lost answers alone were there before: they
     * are reported here at a level out of reach). */
    {
        static const struct {
            uint32_t lost;
            double lost_level;  /* for the lost answers themselves */
            int unchecked_at, worse_at;
        } after[] = {{10, 1e9, 6, 7}, {50, INFINITY, 1, 12}};
        gateDriftBets b;
        size_t i;

        gateDriftBetsInit(&b, 0.05);
        for (i = 0; i < COUNTOF(after); i++) {
            int first_unchecked = 0, first_worse = 0, between = 0;
            gateDrift d;

            gateDriftInit(&d);
            for (n = 0; n < (int)after[i].lost; n++)
                CHECK_EQ_INT(gateDriftLost(&d, &b, gateDriftAsk(&d), log(after[i].lost_level)), QUIET);
            for (k = 1; k <= 20 && !first_worse; k++) {
                int v = gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll);

                if (v == UNCHECKED && !first_unchecked) first_unchecked = k;
                if (v == WORSE) first_worse = k;
                /* Once UNCHECKED, every disagreement is, until WORSE. */
                if (first_unchecked && v == QUIET) between++;
            }
            CHECK_EQ_INT(first_unchecked, after[i].unchecked_at);
            CHECK_EQ_INT(first_worse, after[i].worse_at);
            CHECK_EQ_INT(between, 0);
        }
    }
}

/* An answer counts in the blocks that hold its spot check and are still
 * open. The latest it can come and still count anywhere is therefore 4,095
 * spot checks after its own was asked, and that only for the first spot
 * check of a block of 4,096 (which ends with the 4,095th after it). Here
 * every one of 20,000 spot checks disagrees, or is lost, and its answer
 * comes a fixed number of spot checks later. 4,096 or more: nothing is ever
 * counted in any block and the detector never says a word. 4,095: one
 * answer per block of 4,096 counts.
 *
 * This is the rule as gate_stats.h states it ("counts only in the longer
 * blocks that are still open"). What it means for a key that asks
 * thousands of spot checks while an answer is on its way is table (D5) of
 * sim_certify. */
TEST(drift_answers_after_their_blocks_have_ended) {
    static const uint32_t lags[] = {4096, 5000, 4095};
    const double la = log(GATE_DRIFT_ALARM), ll = log(GATE_LOST_ALARM);
    gateDriftBets b;
    size_t i;
    int lost;

    gateDriftBetsInit(&b, 0.05);
    for (i = 0; i < COUNTOF(lags); i++) {
        for (lost = 0; lost < 2; lost++) {
            uint32_t lag = lags[i], n, counted = 0, verdicts = 0, above = 0;
            gateDrift d;

            gateDriftInit(&d);
            for (n = 1; n <= 20000; n++) {
                gateDriftAsk(&d);
                if (n <= lag) continue;
                verdicts += (lost ? gateDriftLost(&d, &b, n - lag, ll)
                                  : gateDriftWrong(&d, &b, n - lag, la, ll)) != QUIET;
                counted += d.wrong[0] + d.wrong[1] + d.lost[0] + d.lost[1];
                if (lag >= 4096) counted += d.wrong[2] + d.lost[2];
                /* Both statistics stay those of blocks of right answers
                 * (or of one disagreement in more than 4,000). */
                if (!(gateDriftLogStat(&d, &b, 1) < 0.0)) above++;
                if (lag == 4095 && n == 4096) {
                    /* The answer of spot check 1 as spot check 4,096, the
                     * last of its block, is asked. */
                    CHECK(driftIs(&d, 4096, lost ? 0 : 1, 0, 0, lost ? 0 : 1, 0, 0, lost ? 1 : 0));
                }
                if (lag == 4095 && n == 4097) CHECK(driftIs(&d, 4097, lost ? 0 : 2, 0, 0, 0, 0, 0, 0));
            }
            CHECK_EQ_INT(verdicts, 0);
            CHECK_EQ_INT(counted, 0);
            CHECK_EQ_INT(above, 0);
            CHECK_EQ_INT(d.asked, 20000);
            CHECK_EQ_INT(d.errors, lost ? 0 : 20000 - lag);
        }
    }
}

/* The three verdicts and their precedence: WORSE if the disagreements of
 * any block the answer belongs to are at the level, whatever the lost
 * answers say; else UNCHECKED; else QUIET. alpha = 0.05, where six
 * disagreements of six raise WORSE and sixteen answers of sixteen
 * UNCHECKED. */
TEST(drift_verdict_precedence) {
    const double la = log(GATE_DRIFT_ALARM), ll = log(GATE_LOST_ALARM);
    gateDriftBets b;
    gateDrift d, lost15;
    int k;

    gateDriftBetsInit(&b, 0.05);

    /* QUIET, then WORSE alone: six disagreements, no lost answer. */
    gateDriftInit(&d);
    for (k = 1; k <= 5; k++) CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll), WORSE);
    /* A lost answer in that state is not a disagreement: the first
     * statistic stays where it is, the second is 7 of 7, far from 10^9. */
    CHECK_EQ_INT(gateDriftLost(&d, &b, gateDriftAsk(&d), ll), QUIET);

    /* UNCHECKED alone: fifteen lost answers and one disagreement. */
    gateDriftInit(&lost15);
    for (k = 1; k <= 15; k++) CHECK_EQ_INT(gateDriftLost(&lost15, &b, gateDriftAsk(&lost15), ll), QUIET);
    d = lost15;
    CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll), UNCHECKED);
    /* ... or a sixteenth lost answer. */
    d = lost15;
    CHECK_EQ_INT(gateDriftLost(&d, &b, gateDriftAsk(&d), ll), UNCHECKED);

    /* Both at once, in the same block: WORSE wins. Fifteen lost answers,
     * then disagreements. The first is UNCHECKED (16 of 16). To the first
     * statistic the fifteen count as right, so WORSE needs
     * 2^k (18/19)^15 + 4^k (16/19)^15 >= 2000: the eighth disagreement
     * (0.444 * 256 + 0.0760 * 65536 = 5093; the seventh gives 1302). From
     * then on both statistics are beyond their levels. */
    d = lost15;
    for (k = 1; k <= 7; k++) CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll), UNCHECKED);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll), WORSE);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll), WORSE);
    /* A lost answer is never WORSE, even now. */
    CHECK_EQ_INT(gateDriftLost(&d, &b, gateDriftAsk(&d), ll), UNCHECKED);
    /* With the lost level out of reach only WORSE is left, and with the
     * first level out of reach only UNCHECKED. */
    CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), la, INFINITY), WORSE);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), INFINITY, ll), UNCHECKED);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), INFINITY, INFINITY), QUIET);

    /* In different blocks, short block UNCHECKED and long block WORSE: 30
     * disagreements of the first block of 64 arrive late and unnoticed
     * (level out of reach); then the second block of 64 has 15 lost
     * answers and one disagreement. Its own statistics say UNCHECKED (one
     * disagreement of 16; 16 of 16 with the lost ones), the block of 512
     * holds 31 disagreements of 80: WORSE. */
    gateDriftInit(&d);
    driftAskMany(&d, 64);
    gateDriftAsk(&d);
    for (k = 1; k <= 30; k++) CHECK_EQ_INT(gateDriftWrong(&d, &b, (uint64_t)k, INFINITY, INFINITY), QUIET);
    for (k = 0; k < 15; k++) {
        if (k > 0) gateDriftAsk(&d);
        CHECK_EQ_INT(gateDriftLost(&d, &b, d.asked, INFINITY), QUIET);
    }
    CHECK(driftIs(&d, 79, 30, 0, 30, 30, 15, 15, 15));
    CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll), WORSE);
    CHECK(driftIs(&d, 80, 31, 1, 31, 31, 15, 15, 15));

    /* The other way round, short block WORSE and long block UNCHECKED: 20
     * lost answers of the first block of 64 arrive late, then six
     * disagreements open the second block. The block of 512 holds 6
     * disagreements of 70 (quiet) and 26 answers of 70 with the lost ones
     * (4^26 (16/19)^44 / 2 = 1.2e12: UNCHECKED); the block of 64 holds six
     * of six. */
    gateDriftInit(&d);
    driftAskMany(&d, 64);
    gateDriftAsk(&d);
    for (k = 1; k <= 20; k++) CHECK_EQ_INT(gateDriftLost(&d, &b, (uint64_t)k, INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 65, la, ll), UNCHECKED);    /* 21 of 65 with the lost ones */
    for (k = 2; k <= 5; k++) CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll), UNCHECKED);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), la, ll), WORSE);
    CHECK(driftIs(&d, 70, 6, 6, 6, 6, 0, 20, 20));

    /* An answer only says something about the blocks it belongs to. The
     * block of 64 that is open is beyond both levels; a late disagreement
     * of the block before it counts in the blocks of 512 and 4,096, which
     * are quiet (7 of 71): QUIET. */
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 64, la, INFINITY), QUIET);
    gateDriftAsk(&d);
    CHECK_EQ_INT(gateDriftWrong(&d, &b, 71, la, INFINITY), WORSE);  /* seven of seven */
}

/* ------------------------------- Fail closed ----------------------------- */

/* Arguments outside the documented domain must raise the alarm, never
 * switch the detector off. */
TEST(drift_fails_closed) {
    const double bad[] = {0.0, -0.0, 1.0, -0.1, 1.5, 1e300, -1e300, NAN, INFINITY, -INFINITY};
    const double levels[] = {REF_LOG_ALARM, REF_LOG_LOST, 1e300, DBL_MAX, INFINITY};
    double poison[3];
    gateDriftBets b, good;
    gateDrift d;
    size_t i, j, v;
    int k, slot;

    poison[0] = NAN;
    poison[1] = INFINITY;
    poison[2] = -INFINITY;

    /* An alpha that is not a risk target: every increment is NaN, and the
     * first disagreement alarms, whatever the levels are, wherever it
     * falls in its block and whichever blocks it still counts in. A lost
     * answer cannot be WORSE: it gets the alarm it can get. */
    for (i = 0; i < COUNTOF(bad); i++) {
        gateDriftBetsInit(&b, bad[i]);
        for (k = 0; k < ALTS; k++) CHECK(isnan(b.inc_ok[k]) && isnan(b.inc_err[k]));
        for (j = 0; j < COUNTOF(levels); j++) {
            gateDriftInit(&d);
            CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), levels[j], levels[j]), WORSE);
            CHECK_EQ_INT(gateDriftLost(&d, &b, gateDriftAsk(&d), levels[j]), UNCHECKED);
            CHECK_EQ_INT(gateDriftWrong(&d, &b, gateDriftAsk(&d), levels[j], levels[j]), WORSE);
            CHECK(driftIs(&d, 3, 2, 2, 2, 2, 1, 1, 1));
            CHECK(isnan(gateDriftLogStat(&d, &b, 0)) && isnan(gateDriftLogStat(&d, &b, 1)));

            /* After a long quiet time; an answer that only the block of
             * 512, or only the block of 4,096, still counts. */
            gateDriftInit(&d);
            driftAskMany(&d, 600);
            CHECK_EQ_INT(gateDriftWrong(&d, &b, 600, levels[j], levels[j]), WORSE);
            CHECK_EQ_INT(gateDriftWrong(&d, &b, 520, levels[j], levels[j]), WORSE);
            CHECK_EQ_INT(gateDriftWrong(&d, &b, 3, levels[j], levels[j]), WORSE);
            CHECK_EQ_INT(gateDriftLost(&d, &b, 521, levels[j]), UNCHECKED);
            CHECK_EQ_INT(gateDriftLost(&d, &b, 4, levels[j]), UNCHECKED);
        }
        /* A number that was never handed out is still ignored. */
        gateDriftInit(&d);
        gateDriftAsk(&d);
        CHECK_EQ_INT(gateDriftWrong(&d, &b, 2, REF_LOG_ALARM, REF_LOG_LOST), QUIET);
        CHECK_EQ_INT(gateDriftLost(&d, &b, 0, REF_LOG_LOST), QUIET);
        CHECK(driftIs(&d, 1, 0, 0, 0, 0, 0, 0, 0));
        /* Nothing was asked: no statistic yet. */
        gateDriftInit(&d);
        CHECK(gateDriftLogStat(&d, &b, 0) == 0.0);
    }

    /* One increment that is not a number, in any of the four places: the
     * statistic is not a finite number, which is an alarm. */
    gateDriftBetsInit(&good, 0.05);
    for (v = 0; v < COUNTOF(poison); v++) {
        for (slot = 0; slot < 2 * ALTS; slot++) {
            b = good;
            if (slot < ALTS) b.inc_ok[slot] = poison[v]; else b.inc_err[slot - ALTS] = poison[v];
            gateDriftInit(&d);
            driftAskMany(&d, 3);    /* one disagreement and two right ones: both increments are used */
            CHECK_EQ_INT(gateDriftWrong(&d, &b, 2, REF_LOG_ALARM, REF_LOG_LOST), WORSE);
            CHECK_EQ_INT(gateDriftWrong(&d, &b, 1, INFINITY, INFINITY), WORSE);
            CHECK(isnan(gateDriftLogStat(&d, &b, 0)));
            gateDriftInit(&d);
            driftAskMany(&d, 3);
            CHECK_EQ_INT(gateDriftLost(&d, &b, 2, REF_LOG_LOST), UNCHECKED);
            CHECK_EQ_INT(gateDriftLost(&d, &b, 3, INFINITY), UNCHECKED);
            CHECK(isnan(gateDriftLogStat(&d, &b, 1)));
        }
    }

    /* A level that is not a number cannot vouch for anything: the alarm it
     * stands for is raised by the first answer that is compared with it,
     * on a healthy detector far below any real level. */
    gateDriftInit(&d);
    driftAskMany(&d, 40);
    CHECK_EQ_INT(gateDriftWrong(&d, &good, 1, REF_LOG_ALARM, REF_LOG_LOST), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &good, 2, NAN, REF_LOG_LOST), WORSE);
    CHECK_EQ_INT(gateDriftWrong(&d, &good, 3, NAN, NAN), WORSE);
    CHECK_EQ_INT(gateDriftWrong(&d, &good, 4, REF_LOG_ALARM, NAN), UNCHECKED);
    CHECK_EQ_INT(gateDriftLost(&d, &good, 5, NAN), UNCHECKED);
    CHECK_EQ_INT(gateDriftLost(&d, &good, 6, REF_LOG_LOST), QUIET);     /* the detector itself is fine */
    CHECK_EQ_INT(gateDriftWrong(&d, &good, 7, REF_LOG_ALARM, REF_LOG_LOST), QUIET);
    CHECK(isfinite(gateDriftLogStat(&d, &good, 1)));

    /* A level that is already reached alarms; minus infinity always is. */
    gateDriftInit(&d);
    driftAskMany(&d, 40);
    CHECK_EQ_INT(gateDriftWrong(&d, &good, 1, -INFINITY, INFINITY), WORSE);
    CHECK_EQ_INT(gateDriftWrong(&d, &good, 2, INFINITY, -INFINITY), UNCHECKED);
    CHECK_EQ_INT(gateDriftWrong(&d, &good, 3, -INFINITY, -INFINITY), WORSE);
    CHECK_EQ_INT(gateDriftLost(&d, &good, 4, -INFINITY), UNCHECKED);
    /* One, two and three disagreements of 40 at alpha = 0.05 are statistics
     * of 0.124, 0.268 and 0.597: logs of -2.09, -1.32 and -0.52. */
    gateDriftInit(&d);
    driftAskMany(&d, 40);
    CHECK_EQ_INT(gateDriftWrong(&d, &good, 11, 0.0, INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &good, 12, -1.0, INFINITY), QUIET);
    CHECK_EQ_INT(gateDriftWrong(&d, &good, 13, -1.0, INFINITY), WORSE);
    CHECK_NEAR(gateDriftLogStat(&d, &good, 0), refBlockStat(A05, 40, 3), 1e-13);
    CHECK(gateDriftLogStat(&d, &good, 0) > -0.6 && gateDriftLogStat(&d, &good, 0) < -0.5);

    /* An infinite level is never reached: a block of 4,096 in which every
     * answer disagrees, and one in which every answer is lost. */
    gateDriftInit(&d);
    slot = 0;
    for (k = 0; k < 4096; k++)
        slot += gateDriftWrong(&d, &good, gateDriftAsk(&d), INFINITY, INFINITY) != QUIET;
    CHECK_NEAR(gateDriftLogStat(&d, &good, 0), 4096.0 * log(4.0) - log(2.0), 1e-9);
    for (k = 0; k < 4096; k++) slot += gateDriftLost(&d, &good, gateDriftAsk(&d), INFINITY) != QUIET;
    CHECK_EQ_INT(slot, 0);
    CHECK_NEAR(gateDriftLogStat(&d, &good, 1), 4096.0 * log(4.0) - log(2.0), 1e-9);
    CHECK(gateDriftLogStat(&d, &good, 0) < 0.0);
}

/* No block ever counts more answers than it has spot checks, whatever the
 * caller does: the same answer reported again and again, as a disagreement
 * and as lost, numbers of ended blocks, numbers never handed out. (The
 * gate reports every spot check once. A statistic over more disagreements
 * than spot checks would be meaningless, and the encoding of a gate refuses
 * such a detector.) */
TEST(drift_counters_never_exceed_the_block) {
    const double la = log(GATE_DRIFT_ALARM), ll = log(GATE_LOST_ALARM);
    gateDriftBets b;
    gateDrift d;
    uint64_t rng = 0xb10c;
    int i, s, inconsistent = 0;

    gateDriftBetsInit(&b, 0.05);

    /* Three spot checks, the first reported ten times: three is all a
     * block of three can hold. */
    gateDriftInit(&d);
    driftAskMany(&d, 3);
    for (i = 0; i < 10; i++) gateDriftWrong(&d, &b, 1, la, ll);
    for (s = 0; s < SCALES; s++) CHECK(d.wrong[s] <= 3);
    for (i = 0; i < 10; i++) gateDriftLost(&d, &b, 2, ll);
    for (s = 0; s < SCALES; s++) CHECK(d.wrong[s] + d.lost[s] <= 3);
    CHECK(driftBounded(&d));
    /* The fourth spot check has room for one more answer. */
    gateDriftAsk(&d);
    for (i = 0; i < 10; i++) gateDriftLost(&d, &b, 4, ll);
    for (s = 0; s < SCALES; s++) CHECK(d.wrong[s] + d.lost[s] <= 4);

    /* A block is full when every one of its spot checks was reported, not
     * before: 64 answers of 64, half of them lost, all count. */
    gateDriftInit(&d);
    driftAskMany(&d, 64);
    for (i = 1; i <= 64; i++) {
        if (i % 2) gateDriftWrong(&d, &b, (uint64_t)i, INFINITY, INFINITY);
        else gateDriftLost(&d, &b, (uint64_t)i, INFINITY);
    }
    CHECK(driftIs(&d, 64, 32, 32, 32, 32, 32, 32, 32));
    /* ... and the same at the other two lengths. */
    gateDriftInit(&d);
    driftAskMany(&d, 4096);
    for (i = 1; i <= 4096; i++) {
        if (i % 4) gateDriftWrong(&d, &b, (uint64_t)i, INFINITY, INFINITY);
        else gateDriftLost(&d, &b, (uint64_t)i, INFINITY);
    }
    CHECK(driftIs(&d, 4096, 3072, 48, 384, 3072, 16, 128, 1024));

    /* Anything at all, for 300,000 calls. */
    gateDriftInit(&d);
    for (i = 0; i < 300000; i++) {
        uint64_t r = gate_test_rand64(&rng), number;
        unsigned what = (unsigned)(r & 7);

        r >>= 3;
        if (d.asked == 0 || what < 3) {
            gateDriftAsk(&d);
        } else {
            /* Mostly recent numbers, sometimes any, sometimes beyond. */
            switch (r & 3) {
            case 0: number = d.asked - (r >> 2) % (d.asked < 8 ? d.asked : 8); break;
            case 1: number = d.asked - (r >> 2) % (d.asked < 70 ? d.asked : 70); break;
            case 2: number = (r >> 2) % (d.asked + 3); break;
            default: number = d.asked; break;
            }
            if (what < 6) gateDriftWrong(&d, &b, number, la, ll); else gateDriftLost(&d, &b, number, ll);
        }
        if (!driftBounded(&d)) inconsistent++;
    }
    CHECK_EQ_INT(inconsistent, 0);
    CHECK(d.asked > 100000);
}

/* No hidden state: the same calls give the same detector, bit for bit, and
 * the same verdicts. */
TEST(drift_determinism) {
    const double la = log(GATE_DRIFT_ALARM), ll = log(GATE_LOST_ALARM);
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
    memset(&d2, 0, sizeof(d2));
    gateDriftInit(&d2);
    for (i = 0; i < 50000; i++) {
        double u = gate_test_uniform(&rng);
        uint64_t n1 = gateDriftAsk(&d1), n2 = gateDriftAsk(&d2);
        int v1 = QUIET, v2 = QUIET;

        if (u < 0.04) {
            v1 = gateDriftWrong(&d1, &b1, n1, la, ll);
            v2 = gateDriftWrong(&d2, &b2, n2, la, ll);
        } else if (u < 0.06) {
            v1 = gateDriftLost(&d1, &b1, n1, ll);
            v2 = gateDriftLost(&d2, &b2, n2, ll);
        }
        if (n1 != n2 || v1 != v2 || memcmp(&d1, &d2, sizeof(d1)) != 0) differ++;
        if (gateDriftLogStat(&d1, &b1, 1) != gateDriftLogStat(&d2, &b2, 1)) differ++;
        alarms += v1 != QUIET;
    }
    CHECK_EQ_INT(differ, 0);
    CHECK(alarms > 0);
}

/* --------------------------- Order independence --------------------------
 *
 * WHAT THE HEADER PROMISES. (1) The statistic of a block is a product, so
 * its value does not depend on the order in which answers arrive: the same
 * spot checks with the same answers leave the same detector, however the
 * answers were delayed, batched or reordered. (2) At any moment that value
 * is never above the product over the same spot checks with their true
 * answers taken in the order they were asked. So whenever a block raises an
 * alarm, the same spot checks answered in order, each at once, raise it
 * too, and no later: late answers can lose an alarm, they can never make
 * one. That is what keeps the false alarm probability of a block at 1/A
 * "whatever the timing of the answers".
 *
 * Why (2) holds: with n spot checks asked in a block and w disagreements
 * known, each alternative is e(1)^w e(0)^(n - w) with e(1) >= 1 >= e(0).
 * The disagreements known are some of those among the n, and the in order
 * sequence looked at the block when the last of them was asked, with all
 * of them known and no more spot checks asked than n. The tests below do
 * not rely on this argument: they try. */

/* ---- every interleaving of a few spot checks, by brute force ---- */

typedef struct driftBrute {
    gateDriftBets b;
    double la, ll;
    int m;                      /* spot checks */
    unsigned char truth[8];     /* 0 right, 1 wrong, 2 lost */
    gateDrift start;            /* the detector before the first of them */
    gateDrift in_order;         /* after all of them, each answered at once */
    int in_order_worse, in_order_alarm; /* did that sequence ever say so */
    long leaves, calls, alarms, bad_end, made_up;
} driftBrute;

/* 'a' of the spot checks are asked, 'pending' is the set of those whose
 * answer (wrong or lost) is still out. Tries everything that can come
 * next: the next spot check, or any one of the answers. */
static void driftBruteFrom(driftBrute *q, int a, unsigned pending, const gateDrift *d) {
    int i;

    if (a == q->m && pending == 0) {
        q->leaves++;
        /* The same counters as in order, bit for bit. */
        if (memcmp(d, &q->in_order, sizeof(*d)) != 0) q->bad_end++;
        return;
    }
    if (a < q->m) {
        gateDrift next = *d;

        gateDriftAsk(&next);
        driftBruteFrom(q, a + 1, q->truth[a] ? pending | (1u << a) : pending, &next);
    }
    for (i = 0; i < a; i++) {
        gateDrift next = *d;
        uint64_t number = q->start.asked + (uint64_t)i + 1;
        int v;

        if (!(pending & (1u << i))) continue;
        v = q->truth[i] == 1 ? gateDriftWrong(&next, &q->b, number, q->la, q->ll)
                             : gateDriftLost(&next, &q->b, number, q->ll);
        q->calls++;
        if (v != QUIET) q->alarms++;
        /* Never an alarm the in order sequence did not raise. */
        if (v == WORSE && !q->in_order_worse) q->made_up++;
        if (v == UNCHECKED && !q->in_order_alarm) q->made_up++;
        driftBruteFrom(q, a, pending & ~(1u << i), &next);
    }
}

/* Five spot checks, each right, wrong or lost (243 sets of true answers),
 * and every order in which the five can be asked and their answers can
 * arrive (945 timed sequences when all five have an answer; 49,171 over
 * the 243 sets, 119,610 answers delivered). Two cases do the same with six
 * (1,084,483 sequences). The spot checks begin a detector or straddle the
 * end of a block of 64, of 512, of 4,096. The levels are low enough for a
 * few spot checks to reach them; the last case has the levels of the gate
 * (which the random streams further down use throughout). */
TEST(drift_order_every_interleaving) {
    static const struct {
        int a, m;
        uint64_t before;    /* right spot checks asked before these */
        double alarm, lost; /* the two levels, not as logs */
        long leaves, calls; /* counted by a separate enumeration */
    } cases[] = {
        {A05, 5, 0, 8.0, 64.0, 49171, 119610},    {A05, 5, 62, 8.0, 64.0, 49171, 119610},
        {A20, 5, 0, 6.0, 30.0, 49171, 119610},    {A20, 6, 61, 6.0, 30.0, 1084483, 2694006},
        {A001, 5, 510, 8.0, 64.0, 49171, 119610}, {A001, 5, 4093, 2.5, 9.5, 49171, 119610},
        {A01, 5, 4094, 8.0, 64.0, 49171, 119610}, {A60, 5, 0, 1.7, 3.0, 49171, 119610},
        {A05, 6, 0, 1000.0, 1e9, 1084483, 2694006}
    };
    size_t c;
    int i, pattern, patterns;

    for (c = 0; c < COUNTOF(cases); c++) {
        driftBrute q;
        long with_alarm = 0;

        memset(&q, 0, sizeof(q));
        gateDriftBetsInit(&q.b, ref_drift[cases[c].a].alpha);
        q.la = log(cases[c].alarm);
        q.ll = log(cases[c].lost);
        q.m = cases[c].m;
        gateDriftInit(&q.start);
        driftAskMany(&q.start, cases[c].before);
        for (patterns = 1, i = 0; i < q.m; i++) patterns *= 3;
        for (pattern = 0; pattern < patterns; pattern++) {
            int p = pattern;

            for (i = 0; i < q.m; i++, p /= 3) q.truth[i] = (unsigned char)(p % 3);
            /* In order: every spot check answered before the next is
             * asked. */
            q.in_order = q.start;
            q.in_order_worse = q.in_order_alarm = 0;
            for (i = 0; i < q.m; i++) {
                uint64_t number = gateDriftAsk(&q.in_order);
                int v = QUIET;

                if (q.truth[i] == 1) v = gateDriftWrong(&q.in_order, &q.b, number, q.la, q.ll);
                if (q.truth[i] == 2) v = gateDriftLost(&q.in_order, &q.b, number, q.ll);
                if (v == WORSE) q.in_order_worse = 1;
                if (v != QUIET) q.in_order_alarm = 1;
            }
            with_alarm += q.in_order_alarm;
            driftBruteFrom(&q, 0, 0, &q.start);
        }
        CHECK_EQ_INT(q.leaves, cases[c].leaves);    /* every sequence, once */
        CHECK_EQ_INT(q.calls, cases[c].calls);
        CHECK_EQ_INT(q.bad_end, 0);                 /* the same detector at the end */
        CHECK_EQ_INT(q.made_up, 0);                 /* no alarm the in order sequence lacks */
        if (cases[c].alarm < 100.0) {
            /* Alarms were raised, for some sets of answers and not for
             * others. */
            CHECK(q.alarms > 1000 && with_alarm > 20 && with_alarm < patterns);
        } else {
            /* Six spot checks at the levels of the gate: WORSE if and only
             * if all six disagree, and then exactly once in each of the
             * 10,395 timed sequences, when the last of the six answers
             * arrives. */
            CHECK_EQ_INT(with_alarm, 1);
            CHECK_EQ_INT(q.alarms, 10395);
        }
    }
}

/* ---- every state a few spot checks can be in ---- */

#define WALK_MAX 7

/* A spot check of the window is right (nothing will ever be reported),
 * wrong or lost with its answer still out, or wrong or lost with its
 * answer delivered. A state is the number of spot checks asked and what
 * each of them is. Every timed sequence of asks and answers is a path
 * through these states, and every path is tried edge by edge: an edge
 * taken from a state must lead to the same detector, bit for bit,
 * whichever path led to that state. The detector of every state is also
 * compared with the model built from the state alone, and the verdict of
 * every edge with the model's. */
enum { W_RIGHT, W_WRONG_OUT, W_LOST_OUT, W_WRONG_IN, W_LOST_IN };

typedef struct driftWalk {
    size_t a;                   /* index into ref_drift */
    gateDriftBets b;
    double la, ll;
    int m;                      /* spot checks in the window */
    gateDrift start;            /* the detector before the first of them */
    gateDrift *canon;           /* the detector of each state reached */
    unsigned char *seen;
    unsigned char in_order[3281]; /* per set of true answers of the first a
                                   * spot checks: 1 known, 2 said WORSE, 4
                                   * said anything but QUIET */
    long states, edges, verdicts[3];
    long bad_state, bad_counter, bad_stat, bad_verdict, bad_number, made_up, near;
} driftWalk;

static size_t walkIndex(const unsigned char *st, int a) {
    size_t base = 0, idx = 0, p = 1;
    int i;

    for (i = 0; i < a; i++) {
        base += p;          /* states with fewer spot checks asked */
        idx += st[i] * p;
        p *= 5;
    }
    return base + idx;
}

static int walkTruth(unsigned char st) {
    return st == W_RIGHT ? 0 : (st == W_WRONG_OUT || st == W_WRONG_IN ? 1 : 2);
}

/* What the spot checks of the state, answered in order, each at once, made
 * the detector say. */
static int walkInOrder(driftWalk *w, const unsigned char *st, int a) {
    size_t base = 0, idx = 0, p = 1;
    int i;

    for (i = 0; i < a; i++) {
        base += p;
        idx += (size_t)walkTruth(st[i]) * p;
        p *= 3;
    }
    if (!(w->in_order[base + idx] & 1)) {
        gateDrift d = w->start;
        int flags = 1;

        for (i = 0; i < a; i++) {
            uint64_t number = gateDriftAsk(&d);
            int v = QUIET;

            if (walkTruth(st[i]) == 1) v = gateDriftWrong(&d, &w->b, number, w->la, w->ll);
            if (walkTruth(st[i]) == 2) v = gateDriftLost(&d, &w->b, number, w->ll);
            if (v == WORSE) flags |= 2;
            if (v != QUIET) flags |= 4;
        }
        w->in_order[base + idx] = (unsigned char)flags;
    }
    return w->in_order[base + idx];
}

/* The model of a state: nothing but what was asked and what was reported. */
static void walkModel(const driftWalk *w, const unsigned char *st, int a, refDrift *r, refReport *rep) {
    int i;

    r->a = w->a;
    r->asked = w->start.asked + (uint64_t)a;
    r->errors = 0;
    r->n = 0;
    r->rep = rep;
    for (i = 0; i < a; i++) {
        if (st[i] != W_WRONG_IN && st[i] != W_LOST_IN) continue;
        rep[r->n].number = w->start.asked + (uint64_t)i + 1;
        rep[r->n].lost = st[i] == W_LOST_IN;
        if (st[i] == W_WRONG_IN) r->errors++;
        r->n++;
    }
}

static void walkFrom(driftWalk *w, unsigned char *st, int a, const gateDrift *d) {
    size_t idx = walkIndex(st, a);
    refReport rep[WALK_MAX + 1];
    refDrift r;
    int i, s, with_lost;

    if (w->seen[idx]) {
        if (memcmp(&w->canon[idx], d, sizeof(*d)) != 0) w->bad_state++;
        return;
    }
    w->seen[idx] = 1;
    w->canon[idx] = *d;
    w->states++;

    walkModel(w, st, a, &r, rep);
    if (d->asked != r.asked || d->errors != r.errors) w->bad_counter++;
    for (s = 0; s < SCALES; s++) {
        uint32_t open, wrong, lost;

        refCounts(&r, s, &open, &wrong, &lost);
        if (gateDriftOpen(d, s) != open || d->wrong[s] != wrong || d->lost[s] != lost) w->bad_counter++;
    }
    for (with_lost = 0; with_lost < 2; with_lost++) {
        if (!(fabs(gateDriftLogStat(d, &w->b, with_lost) - refStat(&r, with_lost)) <= 1e-10)) w->bad_stat++;
    }

    if (a < w->m) {
        int kind;

        for (kind = W_RIGHT; kind <= W_LOST_OUT; kind++) {
            gateDrift next = *d;

            if (gateDriftAsk(&next) != r.asked + 1) w->bad_number++;
            st[a] = (unsigned char)kind;
            walkFrom(w, st, a + 1, &next);
        }
    }
    for (i = 0; i < a; i++) {
        gateDrift next = *d;
        uint64_t number = w->start.asked + (uint64_t)i + 1, errors = r.errors;
        size_t reports = r.n;
        int lost = st[i] == W_LOST_OUT, near = 0, v, expected;

        if (st[i] != W_WRONG_OUT && st[i] != W_LOST_OUT) continue;
        v = lost ? gateDriftLost(&next, &w->b, number, w->ll)
                 : gateDriftWrong(&next, &w->b, number, w->la, w->ll);
        expected = refAnswer(&r, number, lost, w->la, w->ll, &near);
        r.n = reports;      /* the model is this state's again */
        r.errors = errors;
        w->edges++;
        if (near) w->near++;
        else if (v != expected) w->bad_verdict++;
        if (v == QUIET || v == WORSE || v == UNCHECKED) w->verdicts[v]++;
        /* Never an alarm that the spot checks asked so far, answered in
         * order, did not raise as well. */
        if (v == WORSE && !(walkInOrder(w, st, a) & 2)) w->made_up++;
        if (v == UNCHECKED && !(walkInOrder(w, st, a) & 4)) w->made_up++;
        st[i] = (unsigned char)(st[i] + 2);
        walkFrom(w, st, a, &next);
        st[i] = (unsigned char)(st[i] - 2);
    }
}

/* Rank of a verdict: WORSE over UNCHECKED over QUIET. */
static int verdictRank(int v) {
    return v == WORSE ? 2 : (v == UNCHECKED ? 1 : 0);
}

/* Everything is asked first and the answers arrive afterwards, in any
 * order: the detector goes through different states, but the worst verdict
 * it gives on the way is the same for every order, and it is the verdict
 * of the final state. Returns the number of sets of true answers for which
 * that is not so. '*alarms' counts the sets that end in an alarm. */
static long walkAnyOrder(driftWalk *w, long *alarms) {
    unsigned char st[WALK_MAX], set[1 << WALK_MAX];
    long bad = 0, patterns = 1;
    long pattern;
    int i, at[WALK_MAX], k;

    for (i = 0; i < w->m; i++) patterns *= 3;
    for (pattern = 0; pattern < patterns; pattern++) {
        long p = pattern;
        unsigned mask, full;

        k = 0;
        for (i = 0; i < w->m; i++, p /= 3) {
            st[i] = (unsigned char)(p % 3);     /* W_RIGHT, W_WRONG_OUT, W_LOST_OUT */
            if (st[i] != W_RIGHT) at[k++] = i;
        }
        full = (1u << k) - 1;
        memset(set, 0, sizeof(set));
        set[0] = 1;     /* nothing delivered: the worst verdict so far is QUIET */
        for (mask = 0; mask <= full; mask++) {
            unsigned char now[WALK_MAX];
            size_t idx;
            int j;

            memcpy(now, st, sizeof(now));
            for (j = 0; j < k; j++) {
                if (mask & (1u << j)) now[at[j]] = (unsigned char)(now[at[j]] + 2);
            }
            idx = walkIndex(now, w->m);
            if (!w->seen[idx]) {
                bad++;
                continue;
            }
            for (j = 0; j < k; j++) {
                gateDrift next = w->canon[idx];
                uint64_t number = w->start.asked + (uint64_t)at[j] + 1;
                int v, x;

                if (mask & (1u << j)) continue;
                v = st[at[j]] == W_LOST_OUT ? gateDriftLost(&next, &w->b, number, w->ll)
                                            : gateDriftWrong(&next, &w->b, number, w->la, w->ll);
                for (x = 0; x < 3; x++) {
                    if (set[mask] & (1u << x)) {
                        int worst = x > verdictRank(v) ? x : verdictRank(v);

                        set[mask | (1u << j)] = (unsigned char)(set[mask | (1u << j)] | (1u << worst));
                    }
                }
            }
            if (mask == full) {
                int final_verdict = driftStateVerdict(&w->canon[idx], &w->b, w->la, w->ll);

                if (set[full] != (1u << verdictRank(final_verdict))) bad++;
                if (final_verdict != QUIET) (*alarms)++;
            }
        }
    }
    return bad;
}

/* Seven spot checks: 97,656 states and every edge between them (263,672
 * answers delivered), or six: 19,531 states. The windows begin a detector
 * or straddle the end of a block of 64, 512 or 4,096 (and the end of all
 * three at once), at risk targets with weak and strong right answers and
 * with capped alternatives. */
TEST(drift_order_every_state) {
    static const struct {
        int a, m;
        uint64_t before;    /* right spot checks asked before the window */
        double alarm, lost; /* the two levels, not as logs */
    } cases[] = {
        {A05, 7, 0, 8.0, 64.0},     {A05, 7, 62, 8.0, 64.0},    {A05, 7, 0, 1000.0, 1e9},
        {A20, 6, 0, 6.0, 30.0},     {A20, 6, 62, 6.0, 30.0},    {A001, 7, 509, 8.0, 64.0},
        {A001, 7, 4092, 2.5, 9.5},  {A01, 6, 4094, 8.0, 64.0},  {A02, 6, 4090, 2.5, 5.0},
        {A60, 6, 62, 1.7, 3.0},     {A001, 7, 507, 1000.0, 1e9}
    };
    size_t c, nstates = 0, p = 1;
    driftWalk *w = malloc(sizeof(*w));
    int i;

    for (i = 0; i <= WALK_MAX; i++, p *= 5) nstates += p;
    CHECK(w != NULL);
    if (w == NULL) return;
    w->canon = malloc(nstates * sizeof(gateDrift));
    w->seen = malloc(nstates);
    CHECK(w->canon != NULL && w->seen != NULL);
    for (c = 0; c < COUNTOF(cases) && w->canon != NULL && w->seen != NULL; c++) {
        unsigned char st[WALK_MAX];
        gateDrift *canon = w->canon;
        unsigned char *seen = w->seen;
        long alarms = 0, reachable = 0, patterns = 1;

        memset(w, 0, sizeof(*w));
        w->canon = canon;
        w->seen = seen;
        memset(seen, 0, nstates);
        w->a = (size_t)cases[c].a;
        gateDriftBetsInit(&w->b, ref_drift[w->a].alpha);
        w->la = log(cases[c].alarm);
        w->ll = log(cases[c].lost);
        w->m = cases[c].m;
        for (i = 0, p = 1; i <= w->m; i++, p *= 5) reachable += (long)p;
        for (i = 0; i < w->m; i++) patterns *= 3;
        gateDriftInit(&w->start);
        driftAskMany(&w->start, cases[c].before);
        memset(st, 0, sizeof(st));
        walkFrom(w, st, 0, &w->start);

        CHECK_EQ_INT(w->states, reachable);     /* every state was reached */
        CHECK_EQ_INT(w->bad_state, 0);          /* ... with the same detector on every path */
        CHECK_EQ_INT(w->bad_counter, 0);        /* ... which is the model's */
        CHECK_EQ_INT(w->bad_stat, 0);
        CHECK_EQ_INT(w->bad_number, 0);
        CHECK_EQ_INT(w->bad_verdict, 0);        /* every answer got the model's verdict */
        CHECK_EQ_INT(w->near, 0);               /* ... none of them a matter of rounding */
        CHECK_EQ_INT(w->made_up, 0);            /* no alarm the in order sequence lacks */
        CHECK_EQ_INT(w->edges, w->m == 7 ? 263672 : 44922);  /* sum over a of 2 a 5^(a-1) */
        CHECK_EQ_INT(walkAnyOrder(w, &alarms), 0);
        if (cases[c].alarm < 100.0) {
            /* All three verdicts were given, many times, and some sets of
             * answers end in an alarm while others do not. */
            CHECK(w->verdicts[QUIET] > 100 && w->verdicts[WORSE] > 100 && w->verdicts[UNCHECKED] > 100);
            CHECK(alarms > 50 && alarms < patterns);
        } else {
            /* At the levels of the gate seven spot checks can only say
             * WORSE: six or seven disagreements. */
            CHECK(w->verdicts[WORSE] > 0 && w->verdicts[UNCHECKED] == 0);
            CHECK(alarms > 0 && alarms <= 15);
        }
    }
    free(w->canon);
    free(w->seen);
    free(w);
}

/* ---- long random streams with late, batched and reordered answers ---- */

typedef struct driftEvent {
    uint32_t when;      /* delivered after this many spot checks were asked */
    uint32_t key;       /* order among those delivered at the same time */
    uint32_t number;
    int lost;
} driftEvent;

static int driftEventCmp(const void *x, const void *y) {
    const driftEvent *a = x, *b = y;

    if (a->when != b->when) return a->when < b->when ? -1 : 1;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return (a->number > b->number) - (a->number < b->number);
}

/* Streams of several thousand spot checks whose error rate and loss rate
 * change from stretch to stretch (fine, at the target, twice and four
 * times the target, half of the answers wrong; no loss, a little, a lot).
 * Each stream is answered twice: in order, every answer at once; and with
 * its answers late by a fixed number of spot checks, late by random
 * amounts, in batches, all at the very end, or held back and released in
 * bursts, those that arrive together in random order. Both detectors run
 * to the end, alarms or not.
 *
 * Checked: every verdict of both runs against the model; the invariants
 * after every call; that the two detectors end bit for bit equal; and that
 * every alarm of the second run was raised by the first run too, in the
 * same block of 4,096 and no later (counted in spot checks asked). */
TEST(drift_order_random_streams) {
    enum { MAXN = 9600 };
    static const int alpha_at[] = {A05, A02, A20, A01, A60, A10};
    static const uint32_t lags[] = {1, 10, 63, 64, 100, 511, 512, 1000, 4095, 4096, 5000};
    static const uint32_t batches[] = {2, 16, 64, 100, 512, 1000, 4096};
    static driftEvent ev[MAXN];
    static refReport rep_a[MAXN], rep_b[MAXN];
    static uint32_t last_worse[MAXN + 1], last_alarm[MAXN + 1];
    static unsigned char truth[MAXN + 1];
    const double la = log(GATE_DRIFT_ALARM), ll = log(GATE_LOST_ALARM);
    uint64_t rng = 0x0bde2;
    long compared = 0, near = 0, bad_verdict = 0, inconsistent = 0, bad_end = 0, bad_stat = 0;
    long made_up = 0, verdicts[3] = {0, 0, 0}, late_verdicts[3] = {0, 0, 0};
    int trial;

    for (trial = 0; trial < 60; trial++) {
        size_t a = (size_t)alpha_at[trial % COUNTOF(alpha_at)];
        double alpha = ref_drift[a].alpha, p = 0.0, loss = 0.0;
        uint32_t n = 4500 + (uint32_t)(gate_test_rand64(&rng) % 5000), i, t, nev = 0, next = 0, left = 0;
        uint32_t burst = 4 + (uint32_t)(gate_test_rand64(&rng) % 12);
        int mode = trial % 6;
        uint32_t lag = lags[(trial / 6) % COUNTOF(lags)], batch = batches[(trial / 6) % COUNTOF(batches)];
        /* One trial in five at levels a few answers reach, for more
         * verdicts of every kind. */
        double la_t = trial % 5 == 4 ? log(20.0) : la, ll_t = trial % 5 == 4 ? log(400.0) : ll;
        gateDriftBets b;
        gateDrift d0, d1;
        refDrift r0, r1;

        gateDriftBetsInit(&b, alpha);
        /* The true answers. */
        for (i = 1; i <= n; i++) {
            double u;

            if (left == 0) {
                static const double mult[] = {0.3, 1.0, 2.0, 4.0, 0.0};
                static const double losses[] = {0.0, 0.0, 0.01, 0.05, 0.3};
                size_t m = gate_test_rand64(&rng) % COUNTOF(mult);

                p = mult[m] > 0.0 ? mult[m] * alpha : 0.5;
                if (p > 0.9) p = 0.9;
                loss = losses[gate_test_rand64(&rng) % COUNTOF(losses)];
                left = 200 + (uint32_t)(gate_test_rand64(&rng) % 2800);
            }
            left--;
            u = gate_test_uniform(&rng);
            truth[i] = u < p ? 1 : (u < p + loss * (1.0 - p) ? 2 : 0);
        }

        /* In order. */
        gateDriftInit(&d0);
        r0.a = a;
        r0.asked = r0.errors = 0;
        r0.n = 0;
        r0.rep = rep_a;
        last_worse[0] = last_alarm[0] = 0;
        for (i = 1; i <= n; i++) {
            int v = QUIET, expected = QUIET, nr = 0;

            gateDriftAsk(&d0);
            r0.asked++;
            last_worse[i] = last_worse[i - 1];
            last_alarm[i] = last_alarm[i - 1];
            if (truth[i]) {
                v = truth[i] == 1 ? gateDriftWrong(&d0, &b, i, la_t, ll_t) : gateDriftLost(&d0, &b, i, ll_t);
                expected = refAnswer(&r0, i, truth[i] == 2, la_t, ll_t, &nr);
                if (nr) near++;
                else {
                    compared++;
                    if (v != expected) bad_verdict++;
                }
                verdicts[v == WORSE ? 1 : (v == UNCHECKED ? 2 : 0)]++;
                if (v == WORSE) last_worse[i] = i;
                if (v != QUIET) last_alarm[i] = i;
                if (!driftConsistent(&d0)) inconsistent++;
            }
        }

        /* When each answer arrives the second time. */
        for (i = 1; i <= n; i++) {
            uint64_t r = gate_test_rand64(&rng);
            uint32_t when = i;

            if (!truth[i]) continue;
            switch (mode) {
            case 0: when = i + lag; break;
            case 1: /* mostly soon, sometimes very late */
                switch (r % 10) {
                case 0: case 1: case 2: case 3: when = i; break;
                case 4: case 5: when = i + 1 + (uint32_t)((r >> 8) % 10); break;
                case 6: case 7: when = i + 1 + (uint32_t)((r >> 8) % 200); break;
                case 8: when = i + 1 + (uint32_t)((r >> 8) % 3000); break;
                default: when = i + 3000 + (uint32_t)((r >> 8) % 3000); break;
                }
                break;
            case 2: when = (i + batch - 1) / batch * batch; break;
            case 3: when = n; break;
            case 4: when = 0; break;    /* held back until 'burst' answers are in hand: see below */
            default: when = r % 3 ? i : i + lag; break; /* a third of them late */
            }
            ev[nev].when = when;
            ev[nev].key = (uint32_t)(r >> 40);
            ev[nev].number = i;
            ev[nev].lost = truth[i] == 2;
            nev++;
        }
        if (mode == 4) {
            /* Each answer is released when the last one of its group of
             * 'burst' was asked. */
            for (i = 0; i < nev; i++) {
                uint32_t group_end = (i / burst + 1) * burst - 1;

                ev[i].when = group_end < nev ? ev[group_end].number : n;
            }
        }
        for (i = 0; i < nev; i++) {
            if (ev[i].when > n) ev[i].when = n;     /* whatever is still out arrives at the end */
        }
        qsort(ev, nev, sizeof(ev[0]), driftEventCmp);

        gateDriftInit(&d1);
        r1.a = a;
        r1.asked = r1.errors = 0;
        r1.n = 0;
        r1.rep = rep_b;
        for (t = 1; t <= n; t++) {
            gateDriftAsk(&d1);
            r1.asked++;
            while (next < nev && ev[next].when <= t) {
                uint32_t number = ev[next].number, first = (t - 1) / 4096 * 4096 + 1;
                int lost = ev[next].lost, nr = 0, v, expected;

                v = lost ? gateDriftLost(&d1, &b, number, ll_t) : gateDriftWrong(&d1, &b, number, la_t, ll_t);
                expected = refAnswer(&r1, number, lost, la_t, ll_t, &nr);
                if (nr) near++;
                else {
                    compared++;
                    if (v != expected) bad_verdict++;
                }
                late_verdicts[v == WORSE ? 1 : (v == UNCHECKED ? 2 : 0)]++;
                /* In order, a block of the open block of 4,096 said so
                 * too, by now. */
                if (v == WORSE && !(last_worse[t] >= first)) made_up++;
                if (v == UNCHECKED && !(last_alarm[t] >= first)) made_up++;
                if (!driftConsistent(&d1)) inconsistent++;
                if (next % 64 == 0) {
                    if (!(fabs(gateDriftLogStat(&d1, &b, 0) - refStat(&r1, 0)) <= 1e-9)) bad_stat++;
                    if (!(fabs(gateDriftLogStat(&d1, &b, 1) - refStat(&r1, 1)) <= 1e-9)) bad_stat++;
                }
                next++;
            }
        }
        if (next != nev) bad_end++;
        /* The same detector at the end, bit for bit, and it is the
         * model's. */
        if (memcmp(&d0, &d1, sizeof(d0)) != 0) bad_end++;
        if (d1.asked != r1.asked || d1.errors != r1.errors) bad_end++;
        for (i = 0; i < SCALES; i++) {
            uint32_t open, wrong, lost;

            refCounts(&r1, (int)i, &open, &wrong, &lost);
            if (gateDriftOpen(&d1, (int)i) != open || d1.wrong[i] != wrong || d1.lost[i] != lost) bad_end++;
        }
        if (driftStateVerdict(&d0, &b, la_t, ll_t) != driftStateVerdict(&d1, &b, la_t, ll_t)) bad_end++;
    }
    CHECK_EQ_INT(bad_verdict, 0);
    CHECK_EQ_INT(inconsistent, 0);
    CHECK_EQ_INT(bad_end, 0);
    CHECK_EQ_INT(bad_stat, 0);
    CHECK_EQ_INT(made_up, 0);
    /* The comparison was almost never a matter of rounding, and the streams
     * exercised every verdict in both runs. */
    CHECK(compared > 100000 && near * 1000 < compared);
    CHECK(verdicts[0] > 10000 && verdicts[1] > 10000 && verdicts[2] > 1000);
    CHECK(late_verdicts[0] > 10000 && late_verdicts[1] > 1000 && late_verdicts[2] > 300);
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
    RUN(flawless_at_the_bar);
    RUN(flawless_any_threshold);
    RUN(flawless_is_the_first_n_at_the_threshold);
    RUN(flawless_fails_closed);
    RUN(budget_generation_delta);
    RUN(budget_sum_never_exceeds_total);
    RUN(budget_log_threshold);
    RUN(budget_fails_closed);
    RUN(grid_end_points_and_order);
    RUN(grid_closed_form);
    RUN(grid_other_ranges);
    RUN(grid_invalid_arguments);
    RUN(grid_count);
    RUN(drift_constants);
    RUN(drift_increments);
    RUN(drift_alpha_next_to_one);
    RUN(drift_init_and_numbering);
    RUN(drift_counts_by_block);
    RUN(drift_hand_computed_sequence);
    RUN(drift_statistic_of_a_block);
    RUN(drift_statistic_across_blocks);
    RUN(drift_right_answers_only);
    RUN(drift_alarm_counts);
    RUN(drift_alarm_boundary);
    RUN(drift_alarm_boundary_in_later_blocks);
    RUN(drift_alarm_level_is_inclusive);
    RUN(drift_lost_answers_only_in_the_second_statistic);
    RUN(drift_answers_after_their_blocks_have_ended);
    RUN(drift_verdict_precedence);
    RUN(drift_fails_closed);
    RUN(drift_counters_never_exceed_the_block);
    RUN(drift_determinism);
    RUN(drift_order_every_interleaving);
    RUN(drift_order_every_state);
    RUN(drift_order_random_streams);
    return TEST_REPORT();
}
